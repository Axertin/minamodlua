#include "events.hpp"

#include "handle.hpp"
#include "invoke.hpp"
#include "log.hpp"
#include "marshal.hpp"

#include <string.h>

#include <array>
#include <string>
#include <utility>
#include <vector>

extern "C"
{
#include "lauxlib.h"
#include "lua.h"
}

namespace mml
{

namespace
{

// Hook context layouts.
struct FixedUpdateCtx
{
    float elapsed;
};
struct GameStateTransitionCtx
{
    int32_t* pGameState;
};
struct WorldCtx
{
    World* world;
};
struct WorldUpdateCtx
{
    World* world;
    float elapsed;
};

enum class Shape
{
    None,          // GameInit, GameShutdown - ctx is NULL, confirmed in-game
    Elapsed,       // FixedUpdate
    World,         // WorldConstruct, WorldDestroy
    WorldElapsed,  // WorldUpdate, WorldUpdateEnd
    GameState,     // GameStateTransition - the one mutable context
};

struct EventDef
{
    const char* luaName;
    const char* hookName;
    Shape shape;
    bool playerWorldOnly;
};

// Two Lua names map to WorldUpdate. The filtered one is the default
const EventDef kEvents[] = {
    { "fixed_update", "FixedUpdate", Shape::Elapsed, false },
    { "game_state_transition", "GameStateTransition", Shape::GameState, false },
    { "game_init", "GameInit", Shape::None, false },
    { "game_shutdown", "GameShutdown", Shape::None, false },
    { "world_construct", "WorldConstruct", Shape::World, false },
    { "world_destroy", "WorldDestroy", Shape::World, false },
    { "world_update", "WorldUpdate", Shape::WorldElapsed, true },
    { "world_update_any", "WorldUpdate", Shape::WorldElapsed, false },
    { "world_update_end", "WorldUpdateEnd", Shape::WorldElapsed, true },
    { "world_update_end_any", "WorldUpdateEnd", Shape::WorldElapsed, false },
};

constexpr int kPoolSize = 64;

struct Slot
{
    bool used = false;
    const EventDef* def = nullptr;
    int32_t priority = 0;
    void* handle = nullptr;  // from InstallHook
    bool disabled = false;   // context failed its sanity check
};

std::array<Slot, kPoolSize> g_slots;
lua_State* g_L = nullptr;

const char* const kHandlersKey = "minamodlua.handlers";

// A handler erroring inside FixedUpdate does so 60 times a second, and
// debug.traceback is not cheap. Report the first few in full, then go quiet.
constexpr int kMaxReports = 3;
int g_reports[kPoolSize] = {};

void dispatch( int slot, void* ctx ) noexcept;

// One distinct function per pool index. A template cannot be extern "C", but x64
// has a single calling convention, so the implied cast is safe on every
// toolchain here.
template <int I>
void thunk( void* ctx ) noexcept
{
    dispatch( I, ctx );
}

using Thunk = void ( * )( void* );

template <size_t... I>
constexpr std::array<Thunk, sizeof...( I )> make_thunks( std::index_sequence<I...> )
{
    return { { &thunk<(int)I>... } };
}

const std::array<Thunk, kPoolSize> kThunks = make_thunks( std::make_index_sequence<kPoolSize>{} );

bool plausible_elapsed( float e ) { return e > 0.0f && e < 1.0f; }

// Builds the event table for a handler and leaves it on the stack. Returns
// false if the context failed its sanity check, in which case nothing is pushed.
bool push_event( lua_State* L, const Slot& s, void* ctx, World** outWorld )
{
    *outWorld = nullptr;

    switch ( s.def->shape )
    {
    case Shape::None:
        lua_newtable( L );
        return true;

    case Shape::Elapsed:
    {
        if ( !ctx ) return false;
        const auto* c = (const FixedUpdateCtx*)ctx;
        if ( !plausible_elapsed( c->elapsed ) ) return false;
        lua_newtable( L );
        lua_pushnumber( L, c->elapsed );
        lua_setfield( L, -2, "elapsed" );
        return true;
    }

    case Shape::World:
    {
        if ( !ctx ) return false;
        const auto* c = (const WorldCtx*)ctx;
        *outWorld = c->world;
        lua_newtable( L );
        push_handle<World>( L, c->world );
        lua_setfield( L, -2, "world" );
        return true;
    }

    case Shape::WorldElapsed:
    {
        if ( !ctx ) return false;
        const auto* c = (const WorldUpdateCtx*)ctx;
        if ( !plausible_elapsed( c->elapsed ) ) return false;
        if ( ( (uintptr_t)c->world % sizeof( void* ) ) != 0 ) return false;
        *outWorld = c->world;
        lua_newtable( L );
        push_handle<World>( L, c->world );
        lua_setfield( L, -2, "world" );
        lua_pushnumber( L, c->elapsed );
        lua_setfield( L, -2, "elapsed" );
        return true;
    }

    case Shape::GameState:
    {
        if ( !ctx ) return false;
        const auto* c = (const GameStateTransitionCtx*)ctx;
        if ( !c->pGameState ) return false;
        lua_newtable( L );
        lua_pushinteger( L, *c->pGameState );
        lua_setfield( L, -2, "new_state" );
        return true;
    }
    }
    return false;
}

struct DispatchCtx
{
    int slot;
    void* ctx;
};

int protected_dispatch( lua_State* L )
{
    auto* d = (DispatchCtx*)lua_touserdata( L, 1 );
    const Slot& s = g_slots[d->slot];

    World* world = nullptr;
    if ( !push_event( L, s, d->ctx, &world ) )
    {
        g_slots[d->slot].disabled = true;
        log::write( "%s context failed its layout check - disabling this event. The SDK does not "
                    "declare these structs, so a game update can change them silently.",
                    s.def->luaName );
        return 0;
    }
    const int eventIndex = lua_gettop( L );

    // Per handler, not per slot: world_update and world_update_any share one hook
    // installation, so a slot-level filter would apply whichever registered first.
    const bool isPlayerWorld = !g_api->PlayerGetWorld || world == g_api->PlayerGetWorld();

    lua_getfield( L, LUA_REGISTRYINDEX, kHandlersKey );
    lua_rawgeti( L, -1, d->slot + 1 );
    if ( !lua_istable( L, -1 ) )
    {
        lua_pop( L, 3 );
        return 0;
    }

    // Length is read once, so a handler registering another during dispatch does
    // not have it run this tick, and one unregistering leaves a nil we skip.
    const int count = (int)lua_objlen( L, -1 );
    for ( int i = 1; i <= count; ++i )
    {
        lua_rawgeti( L, -1, i );
        if ( !lua_istable( L, -1 ) )
        {
            lua_pop( L, 1 );
            continue;
        }

        lua_getfield( L, -1, "filter" );
        const bool wantsPlayerWorld = lua_toboolean( L, -1 ) != 0;
        lua_pop( L, 1 );
        if ( wantsPlayerWorld && !isPlayerWorld )
        {
            lua_pop( L, 1 );
            continue;
        }

        lua_getfield( L, -1, "fn" );
        lua_remove( L, -2 );  // the entry table
        lua_pushvalue( L, eventIndex );
        if ( lua_pcall( L, 1, 0, 0 ) != 0 )
        {
            if ( g_reports[d->slot] < kMaxReports )
            {
                log::write( "error in %s: %s", s.def->luaName, lua_tostring( L, -1 ) );
                if ( ++g_reports[d->slot] == kMaxReports )
                    log::write( "further errors in %s will not be reported", s.def->luaName );
            }
            lua_pop( L, 1 );
        }
    }

    // GameStateTransition is the only mutable context: a handler may assign
    // e.new_state, and the engine reads it back after we return.
    if ( s.def->shape == Shape::GameState )
    {
        lua_getfield( L, eventIndex, "new_state" );
        if ( lua_isnumber( L, -1 ) ) *( (GameStateTransitionCtx*)d->ctx )->pGameState = (int32_t)lua_tointeger( L, -1 );
        lua_pop( L, 1 );
    }

    lua_pop( L, 3 );
    return 0;
}

// An engine frame calls this, so nothing may escape it (see host.cpp).
void dispatch( int slot, void* ctx ) noexcept
{
    if ( !g_L || slot < 0 || slot >= kPoolSize ) return;
    const Slot& s = g_slots[slot];
    if ( !s.used || s.disabled ) return;

    // Handles are only valid for the invocation that produced them; a world
    // going away is the dominant case for a mod holding a stale one.
    const bool worldGone = ( strcmp( s.def->hookName, "WorldDestroy" ) == 0 );

    DispatchCtx d{ slot, ctx };
    const int top = lua_gettop( g_L );
    if ( lua_cpcall( g_L, protected_dispatch, &d ) != 0 ) lua_pop( g_L, 1 );
    lua_settop( g_L, top );

    if ( worldGone ) handles().invalidate_all();
}

int find_or_install( const EventDef* def, int32_t priority, std::string& error )
{
    for ( int i = 0; i < kPoolSize; ++i )
        if ( g_slots[i].used && g_slots[i].def->hookName == def->hookName && g_slots[i].priority == priority ) return i;

    for ( int i = 0; i < kPoolSize; ++i )
    {
        if ( g_slots[i].used ) continue;

        void* handle = g_api->InstallHook ? g_api->InstallHook( def->hookName, priority, kThunks[i] ) : nullptr;
        if ( !handle )
        {
            error = std::string( "the game refused to install a " ) + def->hookName + " hook";
            return -1;
        }

        g_slots[i] = Slot{ true, def, priority, handle, false };
        return i;
    }

    error = "out of hook slots";
    return -1;
}

int l_on_event( lua_State* L )
{
    const char* name = luaL_checkstring( L, 1 );
    luaL_checktype( L, 2, LUA_TFUNCTION );
    const int32_t priority = (int32_t)luaL_optinteger( L, 3, 0 );

    const EventDef* def = nullptr;
    for ( const EventDef& e : kEvents )
        if ( strcmp( e.luaName, name ) == 0 ) def = &e;

    if ( !def )
    {
        std::string known;
        for ( const EventDef& e : kEvents )
        {
            if ( !known.empty() ) known += ", ";
            known += e.luaName;
        }
        return luaL_error( L, "on_event: no event named '%s'. Known events: %s", name, known.c_str() );
    }

    std::string error;
    const int slot = find_or_install( def, priority, error );
    if ( slot < 0 ) return luaL_error( L, "on_event('%s'): %s", name, error.c_str() );

    lua_getfield( L, LUA_REGISTRYINDEX, kHandlersKey );
    lua_rawgeti( L, -1, slot + 1 );
    if ( !lua_istable( L, -1 ) )
    {
        lua_pop( L, 1 );
        lua_newtable( L );
        lua_pushvalue( L, -1 );
        lua_rawseti( L, -3, slot + 1 );
    }

    // { fn = handler, filter = bool } rather than a bare function, so two Lua
    // event names sharing one hook installation keep their own filtering.
    lua_newtable( L );
    lua_pushvalue( L, 2 );
    lua_setfield( L, -2, "fn" );
    lua_pushboolean( L, def->playerWorldOnly );
    lua_setfield( L, -2, "filter" );

    lua_rawseti( L, -2, (int)lua_objlen( L, -2 ) + 1 );
    lua_pop( L, 2 );
    return 0;
}

}  // namespace

bool events_open( lua_State* L )
{
    g_L = L;

    // Identical function bodies can be folded by the linker - MSVC enables
    // /OPT:ICF by default in release, and lld has --icf. Folding would collapse
    // the pool into one slot and misroute every hook.
    for ( int i = 1; i < kPoolSize; ++i )
    {
        if ( kThunks[i] != kThunks[i - 1] ) continue;
        log::write( "FATAL: hook thunks %d and %d share an address - the linker folded them "
                    "(disable /OPT:ICF or --icf)",
                    i - 1, i );
        return false;
    }

    lua_newtable( L );
    lua_setfield( L, LUA_REGISTRYINDEX, kHandlersKey );

    lua_pushcfunction( L, l_on_event );
    lua_setfield( L, -2, "on_event" );
    return true;
}

void events_shutdown()
{
    for ( Slot& s : g_slots )
    {
        if ( s.used && s.handle && g_api && g_api->RemoveHook ) g_api->RemoveHook( s.handle );
        s = Slot{};
    }
    g_L = nullptr;
}

}  // namespace mml
