#include "events.hpp"

#include "eventctx.hpp"
#include "eventdefs.hpp"
#include "handle.hpp"
#include "invoke.hpp"
#include "log.hpp"
#include "marshal.hpp"

#include <MinaModHooks.h>

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

constexpr int kPoolSize = 128;

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

// "This event is not cancellable" is a fact about the mod's code, not a
// recurring failure: it is worth saying once and never again. It gets its own
// per-slot latch rather than sharing g_reports, which is the budget for real
// handler errors - sharing it meant a mod setting mod_handled on fixed_update
// exhausted that budget in three frames and every genuine error after that
// vanished with no "further errors will not be reported" notice to explain why.
bool g_warnedNotCancellable[kPoolSize] = {};

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
    if ( !push_event( L, *s.def, d->ctx, &world ) )
    {
        g_slots[d->slot].disabled = true;
        log::write( "%s context failed its layout check - disabling this event. Upstream declares "
                    "these structs now, but the game build is what actually has to match.",
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

    // The sticky-mod_handled rule (see latch_handled, eventctx.hpp) is shared
    // with tests/events_test.cpp's dispatch_chain, so it is defined once.
    bool handled = false;

    // Seed from whatever push_event already placed in the table - i.e. from
    // ctx's modHandled at entry - rather than unconditionally from false.
    // Slots are keyed on (hookName, priority), so handlers registered at
    // different priorities are separate hook installations and separate
    // protected_dispatch calls. Whether the engine walks that priority chain
    // reusing one ctx pointer cannot be confirmed from the headers alone, but
    // if it does, an unseeded `handled` would let a later-priority slot's
    // empty handler list - or its first handler clearing the field - silently
    // overwrite an earlier-priority claim. Seeding costs nothing when it
    // doesn't apply: push_event never sets modHandled true out of nothing.
    if ( s.def->cancellable ) latch_handled( L, eventIndex, handled );

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

        if ( s.def->cancellable ) latch_handled( L, eventIndex, handled );
    }

    // Setting mod_handled on an event whose context has no such field would
    // otherwise fail silently, which is an afternoon lost to a typo.
    if ( !s.def->cancellable )
    {
        lua_getfield( L, eventIndex, "mod_handled" );
        if ( lua_toboolean( L, -1 ) && !g_warnedNotCancellable[d->slot] )
        {
            log::write( "%s is not cancellable - setting e.mod_handled on it does nothing", s.def->luaName );
            g_warnedNotCancellable[d->slot] = true;
        }
        lua_pop( L, 1 );
    }

    read_back( L, eventIndex, *s.def, d->ctx );

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
    // strcmp, not pointer equality: two Lua names sharing one hook installation
    // (world_update / world_update_any) is documented behaviour, and it holds by
    // pointer only where the compiler pools the two identical "WorldUpdate"
    // literals. MSVC without /GF - i.e. a Debug build - does not pool them.
    for ( int i = 0; i < kPoolSize; ++i )
        if ( g_slots[i].used && strcmp( g_slots[i].def->hookName, def->hookName ) == 0 &&
             g_slots[i].priority == priority )
            return i;

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
    for ( size_t i = 0; i < kEventCount; ++i )
        if ( strcmp( kEvents[i].luaName, name ) == 0 ) def = &kEvents[i];

    if ( !def )
    {
        std::string known;
        for ( size_t i = 0; i < kEventCount; ++i )
        {
            if ( !known.empty() ) known += ", ";
            known += kEvents[i].luaName;
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

    // YC_TOUCH_COUNT sizes ycMouseUpdateCtx::mouseDown and appears in no header
    // we receive. GetEnum* takes the full macro name and returns -1 on a miss
    // (0xFFFFFFFF unsigned), which the range check rejects; the probe found that
    // the YC_INPUT_MOUSE_* aliases do not resolve either, so a miss is expected
    // rather than exceptional. Absent beats a guessed bound: mouseDown could be
    // 3 or 7 elements and writing past it corrupts live input state every frame.
    const uint32_t touch = g_api && g_api->GetEnumUInt ? g_api->GetEnumUInt( "YC_TOUCH_COUNT" ) : 0;
    events_set_touch_count( ( touch >= 1 && touch <= 64 ) ? touch : 0 );
    if ( !events_touch_count() ) log::write( "YC_TOUCH_COUNT did not resolve; mouse_update will not expose e.touch" );

    lua_newtable( L );
    lua_setfield( L, LUA_REGISTRYINDEX, kHandlersKey );

    lua_pushcfunction( L, l_on_event );
    lua_setfield( L, -2, "on_event" );
    return true;
}

void events_shutdown()
{
    for ( int i = 0; i < kPoolSize; ++i )
    {
        Slot& s = g_slots[i];
        if ( s.used && s.handle && g_api && g_api->RemoveHook ) g_api->RemoveHook( s.handle );
        s = Slot{};
        // Both rate limits are per slot, and slots are handed out afresh on the
        // next events_open: leaving these set would give a re-initialised host a
        // slot whose error budget was already spent by whoever held it before.
        g_reports[i] = 0;
        g_warnedNotCancellable[i] = false;
    }
    g_L = nullptr;
}

}  // namespace mml
