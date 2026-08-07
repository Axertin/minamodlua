// Round-trip tests for the event context marshalling. No game required, and no
// MinaModAPI either: main() sets g_api to nullptr, because push_event and
// read_back only ever touch the context blob and the Lua stack.
//
// That is also this suite's main structural limit. events.cpp's
// protected_dispatch - the real dispatcher, which owns the error rate limit,
// the player-world filter and the disable-on-failed-check path - calls through
// g_api and is therefore unreachable from here. What it shares with these tests
// is push_event, read_back and latch_handled; everything else about it is
// exercised only by running the mod in the game.

#include "eventctx.hpp"
#include "eventdefs.hpp"

#include "invoke.hpp"

// Must precede MinaModHooks.h; see the note in eventdefs.cpp.
#include <stddef.h>
#include <stdint.h>

#include <MinaModHooks.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C"
{
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

static int g_failures = 0;

#define CHECK( cond )                                                                                                  \
    do                                                                                                                 \
    {                                                                                                                  \
        if ( !( cond ) )                                                                                               \
        {                                                                                                              \
            printf( "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond );                                                   \
            ++g_failures;                                                                                              \
        }                                                                                                              \
    } while ( 0 )

namespace
{

const mml::EventDef& find_event( const char* name )
{
    for ( size_t i = 0; i < mml::kEventCount; ++i )
        if ( strcmp( mml::kEvents[i].luaName, name ) == 0 ) return mml::kEvents[i];
    printf( "FATAL: no event named %s\n", name );
    exit( 2 );
}

// Runs `src` with the event table bound to the global `e`, then reads back.
void dispatch_lua( lua_State* L, const mml::EventDef& def, void* ctx, const char* src )
{
    World* world = nullptr;
    CHECK( mml::push_event( L, def, ctx, &world ) );
    const int eventIndex = lua_gettop( L );

    lua_pushvalue( L, eventIndex );
    lua_setglobal( L, "e" );

    if ( luaL_dostring( L, src ) != 0 )
    {
        printf( "FAIL lua: %s\n", lua_tostring( L, -1 ) );
        ++g_failures;
        lua_pop( L, 1 );
    }

    mml::read_back( L, eventIndex, def, ctx );
    lua_settop( L, eventIndex - 1 );
}

// --- context fixtures --------------------------------------------------------
//
// Every scalar in a fabricated context gets a value distinct from every other
// scalar in the same context, and every readable row is asserted from Lua. A
// row descriptor names a member; naming the wrong one compiles clean and is
// invisible in C++, so the only thing that can catch two rows having been
// swapped - or one pointing at its neighbour - is the values being telling
// apart. Zeros everywhere made five of ycControllerUpdateCtx's six sticks
// interchangeable.
//
// Pointers all address members of the fixture itself, i.e. real, aligned,
// stack-or-static addresses, which is what push_event's pointer-plausibility
// check expects to see.

struct ItemsOnPickupFixture
{
    int32_t collectionIndex = 11;
    int32_t itemType = 22;
    int32_t subweaponUseBonus = 33;
    int32_t shop = 44;
    uint32_t cheatFlags = 55;
    bool presentAllowed = true;
    bool subweaponBonusSlot = false;
    MM_Vec3 pos{ 1.5f, 2.5f, 3.5f };
    // A Player is never dereferenced - only its address is compared and handed
    // to push_handle - so a fake, non-null, pointer-aligned address is enough,
    // and non-null is what makes the row distinguishable from an absent one.
    void* playerStorage = nullptr;
    Player* player = nullptr;
    ItemsOnPickupCtx ctx{};

    ItemsOnPickupFixture()
    {
        player = reinterpret_cast<Player*>( &playerStorage );
        ctx.collectionIndex = &collectionIndex;
        ctx.itemType = &itemType;
        ctx.ppPlayer = &player;
        ctx.pos = &pos;
        ctx.presentAllowed = &presentAllowed;
        ctx.subweaponUseBonus = &subweaponUseBonus;
        ctx.shop = &shop;
        ctx.cheatFlags = &cheatFlags;
        ctx.subweaponBonusSlot = &subweaponBonusSlot;
        ctx.modHandled = false;
    }
};

// Deliberately its own type rather than a reuse of the one above:
// ItemsOnPickupDoneCtx is a *different* layout - no presentAllowed - so a row
// in kItemsOnPickupDone naming a member of the wrong struct is exactly the
// mistake this fixture exists to expose. The sentinels differ from
// ItemsOnPickupFixture's for the same reason.
struct ItemsOnPickupDoneFixture
{
    int32_t collectionIndex = 61;
    int32_t itemType = 62;
    int32_t subweaponUseBonus = 63;
    int32_t shop = 64;
    uint32_t cheatFlags = 65;
    bool subweaponBonusSlot = true;
    MM_Vec3 pos{ 7.5f, 8.5f, 9.5f };
    void* playerStorage = nullptr;  // see ItemsOnPickupFixture
    Player* player = nullptr;
    ItemsOnPickupDoneCtx ctx{};

    ItemsOnPickupDoneFixture()
    {
        player = reinterpret_cast<Player*>( &playerStorage );
        ctx.collectionIndex = &collectionIndex;
        ctx.itemType = &itemType;
        ctx.ppPlayer = &player;
        ctx.pos = &pos;
        ctx.subweaponUseBonus = &subweaponUseBonus;
        ctx.shop = &shop;
        ctx.cheatFlags = &cheatFlags;
        ctx.subweaponBonusSlot = &subweaponBonusSlot;
        ctx.modHandled = false;
    }
};

struct ControllerFixture
{
    bool exists = true;
    uint64_t buttonDown = 0;
    // Six distinct values, none of them zero and none a negation of another:
    // any two of the six rows swapped shows up as a wrong number.
    int16_t lx = 101, ly = 202, rx = 303, ry = 404, tl = 505, tr = 606;
    ycControllerUpdateCtx ctx{};

    ControllerFixture()
    {
        ctx.channel = 0;
        ctx.exists = &exists;
        ctx.buttonDown = &buttonDown;
        ctx.leftStickX = &lx;
        ctx.leftStickY = &ly;
        ctx.rightStickX = &rx;
        ctx.rightStickY = &ry;
        ctx.triggerLeft = &tl;
        ctx.triggerRight = &tr;
    }
};

void test_fixed_update_is_read_only( lua_State* L )
{
    FixedUpdateCtx ctx{};
    ctx.elapsed = 1.0f / 120.0f;

    dispatch_lua( L, find_event( "fixed_update" ), &ctx, "assert(math.abs(e.elapsed - 1/120) < 1e-6); e.elapsed = 99" );

    // FIELD_VALUE has no read function, so the assignment must not land.
    CHECK( ctx.elapsed == 1.0f / 120.0f );
}

void test_game_state_writes_back( lua_State* L )
{
    int32_t state = 3;
    GameStateTransitionCtx ctx{};
    ctx.pGameState = &state;

    dispatch_lua( L, find_event( "game_state_transition" ), &ctx, "assert(e.new_state == 3); e.new_state = 7" );

    CHECK( state == 7 );
}

// A FIELD_PTR<int32_t> row must ignore anything that isn't a Lua number,
// matching the old switch-based code's `lua_isnumber` guard exactly. Without
// it, lua_tonumber coerces false/a string/a table to 0 and writes that,
// turning a mod typo into a forced game-state-0 transition instead of a
// no-op.
void test_bad_write_is_ignored( lua_State* L )
{
    int32_t state = 3;
    GameStateTransitionCtx ctx{};
    ctx.pGameState = &state;

    dispatch_lua( L, find_event( "game_state_transition" ), &ctx, "e.new_state = false" );
    CHECK( state == 3 );

    dispatch_lua( L, find_event( "game_state_transition" ), &ctx, "e.new_state = 'menu'" );
    CHECK( state == 3 );

    dispatch_lua( L, find_event( "game_state_transition" ), &ctx, "e.new_state = {}" );
    CHECK( state == 3 );
}

void test_layout_check_rejects_garbage( lua_State* L )
{
    FixedUpdateCtx ctx{};
    ctx.elapsed = 5000.0f;  // implausible: the check must reject it

    World* world = nullptr;
    CHECK( !mml::push_event( L, find_event( "fixed_update" ), &ctx, &world ) );
    CHECK( lua_gettop( L ) == 0 );  // nothing pushed on rejection
}

void test_null_context_survives( lua_State* L )
{
    World* world = nullptr;
    // GameInit has no fields, so a null context is expected, not a failure.
    CHECK( mml::push_event( L, find_event( "game_init" ), nullptr, &world ) );
    lua_settop( L, 0 );

    // An event with fields must refuse a null context rather than deref it.
    CHECK( !mml::push_event( L, find_event( "world_update" ), nullptr, &world ) );

    // keyboard_update has fieldCount == 0 but an installer, and install_keyboard
    // dereferences the context immediately. The guard is keyed on "fieldCount
    // or install", so this is refused by the rule itself rather than by an
    // event-specific check bolted on to cover the gap.
    CHECK( !mml::push_event( L, find_event( "keyboard_update" ), nullptr, &world ) );
}

// A context's pointers are the one thing the compile-time size asserts cannot
// vouch for: they prove agreement with upstream's header, not with the running
// game. If the game's layout differs, every pointer row holds a wild address
// that push_ptr would read and read_back would *write* - an arbitrary
// read/write in the game process, inside a lua_cpcall that cannot recover from
// a segfault. push_event walks the descriptor rows and refuses the context
// instead, which is what disables the event in protected_dispatch.
//
// ItemsOnPickup is the strongest case in the table: nine pointer rows.
void test_implausible_pointer_is_refused( lua_State* L )
{
    World* world = nullptr;

    {
        // A small integer where a pointer should be - the shape a shifted
        // layout or a misread field produces. 0x10 is below the first page on
        // every platform this runs on.
        ItemsOnPickupFixture f;
        f.ctx.itemType = (int32_t*)0x10;
        CHECK( !mml::push_event( L, find_event( "items_on_pickup" ), &f.ctx, &world ) );
        CHECK( lua_gettop( L ) == 0 );  // nothing pushed on rejection
    }

    {
        // Above the floor but misaligned for its pointee: the axis a
        // one-or-two-byte layout shift trips even when the address is mapped.
        ItemsOnPickupFixture f;
        f.ctx.cheatFlags = (uint32_t*)0x100001;
        CHECK( !mml::push_event( L, find_event( "items_on_pickup" ), &f.ctx, &world ) );
        CHECK( lua_gettop( L ) == 0 );
    }

    {
        // A handle row (Player** here) is checked too, at pointer alignment.
        ItemsOnPickupFixture f;
        f.ctx.ppPlayer = (Player**)0x100004;
        CHECK( !mml::push_event( L, find_event( "items_on_pickup" ), &f.ctx, &world ) );
        CHECK( lua_gettop( L ) == 0 );
    }

    {
        // FIELD_HANDLE (a `T* m` slot holding an opaque engine handle) is
        // checked at alignof(void*), not just the low-address floor: the
        // pointee's own alignment is unknowable, but the pointer *value* is
        // still a real one, and every pointer the engine hands out is at least
        // pointer-aligned. 0x100001 is above the floor but not 8-aligned.
        // chest_construct has no event-specific check (def.check == nullptr),
        // so this isolates the generic row walk rather than being redirected
        // through, say, check_world's own (also alignment-based) test.
        ChestConstructCtx cc{};
        cc.chest = (Chest*)0x100001;
        CHECK( !mml::push_event( L, find_event( "chest_construct" ), &cc, &world ) );
        CHECK( lua_gettop( L ) == 0 );
    }

    {
        // Null is a legitimate value for any of these - every accessor already
        // checks for it - so the check must not reject a context for it.
        ItemsOnPickupFixture f;
        f.ctx.itemType = nullptr;
        f.ctx.cheatFlags = nullptr;
        CHECK( mml::push_event( L, find_event( "items_on_pickup" ), &f.ctx, &world ) );
        lua_settop( L, 0 );
    }

    {
        // The input contexts hand their arrays to an installer rather than to a
        // row, so the row walk cannot see them: keyboard_update, mouse_update
        // and controller_update carry an event-specific check for exactly that.
        uint32_t first[5] = { 0, 0, 0, 0, 0 };
        ycKeyboardUpdateCtx kb{};
        kb.keysDown = (uint32_t*)0x10;
        kb.keysDownFirstFrame = first;
        CHECK( !mml::push_event( L, find_event( "keyboard_update" ), &kb, &world ) );
        CHECK( lua_gettop( L ) == 0 );

        uint64_t buttons = 0;
        ycControllerUpdateCtx cc{};
        cc.buttonDown = &buttons;
        CHECK( mml::push_event( L, find_event( "controller_update" ), &cc, &world ) );
        lua_settop( L, 0 );
        cc.buttonDown = (uint64_t*)0x100004;  // above the floor, misaligned for a uint64_t
        CHECK( !mml::push_event( L, find_event( "controller_update" ), &cc, &world ) );
        CHECK( lua_gettop( L ) == 0 );

        // check_mouse guards mouseDown the same way check_keyboard/check_controller
        // guard their arrays. The other four rows (double_click/scroll/delta) are
        // ordinary FIELD_PTR/FIELD_PTR_COMP rows and stay legitimately aligned, so
        // this isolates check_mouse's own test rather than the generic row walk.
        // Nothing exercised this before - check_mouse could be neutered outright
        // and the suite would stay green.
        bool doubleClick = false;
        MM_Vec2 scroll{ 0.0f, 0.0f };
        MM_Vec2 delta{ 0.0f, 0.0f };
        float downArr[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        ycMouseUpdateCtx mm{};
        mm.doubleClick = &doubleClick;
        mm.mouseScroll = &scroll;
        mm.mouseDelta = &delta;
        mm.mouseDown = downArr;
        CHECK( mml::push_event( L, find_event( "mouse_update" ), &mm, &world ) );
        lua_settop( L, 0 );
        mm.mouseDown = (float*)0x100001;  // above the floor, misaligned for a float
        CHECK( !mml::push_event( L, find_event( "mouse_update" ), &mm, &world ) );
        CHECK( lua_gettop( L ) == 0 );
    }
}

// Exercises the "world" field end to end: the offsetof push_event uses to
// extract *outWorld, and the offsetof the "elapsed" row next to it in the same
// struct uses to read its own field. A wrong offsetof on either row is silent
// in C++ - nothing throws, it just reads whatever bytes happen to sit at the
// miscomputed address - so this is the one test that would catch one row's
// offset having been swapped for its neighbour's.
void test_world_update_round_trip( lua_State* L )
{
    // Nothing ever dereferences this as an actual World: check_world_update
    // only checks alignment, and push_handle only stores the address in the
    // handle table. A `void*` local's own address is guaranteed
    // pointer-aligned (unlike, say, an `int`'s, which check_world_update's
    // alignment check would then be free to reject).
    void* fakeWorldStorage = nullptr;
    World* const fakeWorld = reinterpret_cast<World*>( &fakeWorldStorage );

    WorldUpdateCtx ctx{};
    ctx.world = fakeWorld;
    ctx.elapsed = 1.0f / 60.0f;

    World* outWorld = nullptr;
    CHECK( mml::push_event( L, find_event( "world_update" ), &ctx, &outWorld ) );
    // *outWorld comes from push_event re-reading the "world" field's offset
    // directly out of ctx, independently of the field-push that filled e.world.
    CHECK( outWorld == fakeWorld );
    const int eventIndex = lua_gettop( L );

    lua_pushvalue( L, eventIndex );
    lua_setglobal( L, "e" );

    if ( luaL_dostring( L, "assert(e.world ~= nil); assert(math.abs(e.elapsed - 1/60) < 1e-6)" ) != 0 )
    {
        printf( "FAIL lua: %s\n", lua_tostring( L, -1 ) );
        ++g_failures;
        lua_pop( L, 1 );
    }

    mml::read_back( L, eventIndex, find_event( "world_update" ), &ctx );
    lua_settop( L, eventIndex - 1 );
}

// The descriptor tables (eventdefs.cpp) are data, not logic: nothing here
// exercises the actual luaName/playerWorldOnly/fieldCount values for eight of
// the ten events, or that kEventCount agrees with the row count. A dropped
// row, a flipped flag, or a field silently added/removed to one of the
// four-events-worth of shared tables (kWorld, kWorldUpdate) would pass every
// other test in this file.
void test_event_table_matches_expected( lua_State* )
{
    struct Expected
    {
        const char* luaName;
        const char* hookName;
        bool playerWorldOnly;
        uint8_t fieldCount;
        bool cancellable;
        // The check functions live in eventdefs.cpp's anonymous namespace and
        // cannot be named from here, so this pins whether the event carries an
        // event-specific one. It is *not* "is this event checked at all":
        // push_event applies the pointer-plausibility walk to every event's
        // rows regardless (see test_implausible_pointer_is_refused).
        bool hasCheck;
        // install/readExtra are declared in eventdefs.hpp, so these are pinned
        // by exact function identity: a row wired to the wrong installer, or an
        // install with no matching readExtra (writes would never reach the
        // engine), is a mismatch here rather than a silent behaviour change.
        void ( *install )( lua_State*, const void* );
        void ( *readExtra )( lua_State*, int, void* );
    };
    // clang-format off
    static const Expected kExpected[] = {
        { "fixed_update",           "FixedUpdate",        false, 1,  false, true,  nullptr,                   nullptr },
        { "game_state_transition",  "GameStateTransition",false, 1,  false, true,  nullptr,                   nullptr },
        { "game_init",              "GameInit",           false, 0,  false, false, nullptr,                   nullptr },
        { "game_shutdown",          "GameShutdown",       false, 0,  false, false, nullptr,                   nullptr },
        { "world_construct",        "WorldConstruct",     false, 1,  false, true,  nullptr,                   nullptr },
        { "world_destroy",          "WorldDestroy",       false, 1,  false, true,  nullptr,                   nullptr },
        { "world_update",           "WorldUpdate",        true,  2,  false, true,  nullptr,                   nullptr },
        { "world_update_any",       "WorldUpdate",        false, 2,  false, true,  nullptr,                   nullptr },
        { "world_update_end",       "WorldUpdateEnd",     true,  2,  false, true,  nullptr,                   nullptr },
        { "world_update_end_any",   "WorldUpdateEnd",     false, 2,  false, true,  nullptr,                   nullptr },
        { "items_on_pickup",        "ItemsOnPickup",      false, 12, true,  false, nullptr,                   nullptr },
        { "items_on_pickup_done",   "ItemsOnPickupDone",  false, 11, true,  false, nullptr,                   nullptr },
        { "is_item_collected",      "IsItemCollected",    false, 7,  true,  false, nullptr,                   nullptr },
        { "pickup_on_pickup",       "PickupOnPickup",     false, 4,  true,  false, nullptr,                   nullptr },
        { "shop_item_refresh",      "ShopItemRefresh",    false, 2,  true,  false, nullptr,                   nullptr },
        { "area_manager_new_area",  "AreaManagerNewArea", false, 3,  true,  false, nullptr,                   nullptr },
        { "chest_construct",        "ChestConstruct",     false, 1,  false, false, nullptr,                   nullptr },
        { "keyboard_update",        "ycKeyboardUpdate",   false, 0,  false, true,  mml::install_keyboard,     mml::read_keyboard },
        { "mouse_update",           "ycMouseUpdate",      false, 5,  false, true,  mml::install_mouse_touch,  mml::read_mouse_touch },
        { "controller_update",      "ycControllerUpdate", false, 8,  false, true,  mml::install_controller,   mml::read_controller },
    };
    // clang-format on
    constexpr size_t kExpectedCount = sizeof( kExpected ) / sizeof( kExpected[0] );

    CHECK( mml::kEventCount == kExpectedCount );

    for ( size_t i = 0; i < kExpectedCount; ++i )
    {
        const mml::EventDef& def = find_event( kExpected[i].luaName );
        CHECK( strcmp( def.hookName, kExpected[i].hookName ) == 0 );
        CHECK( def.playerWorldOnly == kExpected[i].playerWorldOnly );
        CHECK( def.fieldCount == kExpected[i].fieldCount );
        CHECK( def.cancellable == kExpected[i].cancellable );
        CHECK( ( def.check != nullptr ) == kExpected[i].hasCheck );
        CHECK( def.install == kExpected[i].install );
        CHECK( def.readExtra == kExpected[i].readExtra );
    }

    // Two Lua names sharing one hook must be the same event in every respect
    // except the player-world filter - that is what makes one hook installation
    // legitimately serve both (events.cpp's find_or_install). Pinned by
    // identity, not by the columns above, so a table entry that merely *looks*
    // the same but points at a second copy of the rows is still caught.
    for ( size_t i = 0; i < mml::kEventCount; ++i )
        for ( size_t j = i + 1; j < mml::kEventCount; ++j )
        {
            const mml::EventDef& a = mml::kEvents[i];
            const mml::EventDef& b = mml::kEvents[j];
            if ( strcmp( a.hookName, b.hookName ) != 0 ) continue;
            CHECK( a.fields == b.fields );
            CHECK( a.fieldCount == b.fieldCount );
            CHECK( a.check == b.check );
            CHECK( a.cancellable == b.cancellable );
            CHECK( a.install == b.install );
            CHECK( a.readExtra == b.readExtra );
        }
}

// Dispatches a chain of Lua handler bodies against one context, applying the
// sticky-mod_handled rule between them via the same latch_handled that
// protected_dispatch calls (eventctx.hpp) - not a second copy of the rule
// that could silently drift from the real one.
//
// `expectedErrors` pins how many of the bodies are allowed to error: a
// mismatch - in either direction - is a CHECK failure. Without this, a body
// deliberately calling error() (to prove the chain survives a handler error)
// would be indistinguishable from a body that errors by accident, and
// swallowing every error unconditionally would make it impossible to notice
// an unrelated typo in a test body.
void dispatch_chain( lua_State* L, const mml::EventDef& def, void* ctx, const char* const* bodies, int count,
                     int expectedErrors = 0 )
{
    World* world = nullptr;
    CHECK( mml::push_event( L, def, ctx, &world ) );
    const int eventIndex = lua_gettop( L );

    bool handled = false;
    // Seed from ctx's modHandled, exactly as protected_dispatch does, before
    // any handler in this chain runs.
    if ( def.cancellable ) mml::latch_handled( L, eventIndex, handled );

    int errors = 0;
    for ( int i = 0; i < count; ++i )
    {
        lua_pushvalue( L, eventIndex );
        lua_setglobal( L, "e" );
        // protected_dispatch's lua_pcall swallows a handler error and moves on to
        // the next handler without failing anything; it does not propagate out of
        // dispatch. Mirrored here - the error is counted, not printed - so an
        // *unexpected* error still fails the test via the CHECK below, but an
        // expected one (test_erroring_handler_does_not_skip_read_back's deliberate
        // error('boom')) does not register as a spurious failure.
        if ( luaL_dostring( L, bodies[i] ) != 0 )
        {
            ++errors;
            lua_pop( L, 1 );
        }
        if ( def.cancellable ) mml::latch_handled( L, eventIndex, handled );
    }
    CHECK( errors == expectedErrors );

    mml::read_back( L, eventIndex, def, ctx );
    lua_settop( L, eventIndex - 1 );
}

// Fetches a handle field off the event table at `eventIndex` and resolves it
// through the same HandleTable check_handle uses. This is what actually
// distinguishes two same-typed handle rows: "e.a ~= e.b" only proves the two
// values differ from each other, which stays true whether or not the rows
// were swapped in the descriptor table - a swap just exchanges which value
// lands in which field, and the two are still mutually distinct either way.
// Comparing the resolved pointer against the exact address placed in the
// context is the only thing a row swap actually fails.
template <typename T>
T* resolve_handle_field( lua_State* L, int eventIndex, const char* field )
{
    lua_getfield( L, eventIndex, field );
    T* const p = mml::check_handle<T>( L, "resolve_handle_field", lua_gettop( L ) );
    lua_pop( L, 1 );
    return p;
}

void test_mod_handled_is_sticky( lua_State* L )
{
    ItemsOnPickupFixture f;

    // Handler A claims it; handler B tries to clear it and must not succeed.
    const char* bodies[] = {
        "e.mod_handled = true",
        "assert(e.mod_handled == true, 'B must observe the claim'); e.mod_handled = false",
    };
    dispatch_chain( L, find_event( "items_on_pickup" ), &f.ctx, bodies, 2 );

    CHECK( f.ctx.modHandled == true );
}

void test_items_on_pickup_writes_back( lua_State* L )
{
    ItemsOnPickupFixture f;

    // Every readable row is asserted, each against its own sentinel: this is
    // the only thing that can catch two rows in kItemsOnPickup having been
    // swapped or one naming its neighbour's member.
    dispatch_lua( L, find_event( "items_on_pickup" ), &f.ctx,
                  "assert(e.collection_index == 11)\n"
                  "assert(e.item_type == 22)\n"
                  "assert(e.subweapon_use_bonus == 33)\n"
                  "assert(e.shop == 44)\n"
                  "assert(e.cheat_flags == 55)\n"
                  "assert(e.present_allowed == true)\n"
                  "assert(e.subweapon_bonus_slot == false)\n"
                  "assert(e.player ~= nil)\n"
                  "assert(math.abs(e.pos_x - 1.5) < 1e-6)\n"
                  "assert(math.abs(e.pos_y - 2.5) < 1e-6)\n"
                  "assert(math.abs(e.pos_z - 3.5) < 1e-6)\n"
                  "assert(e.mod_handled == false)\n"
                  "e.item_type = 42; e.present_allowed = false; e.pos_y = 99" );

    CHECK( f.itemType == 42 );
    CHECK( f.presentAllowed == false );
    CHECK( f.collectionIndex == 11 );  // untouched fields stay put
    CHECK( f.pos.y == 2.5f );          // FIELD_POD_IN is read-only
}

// ItemsOnPickupDoneCtx is a separate struct from ItemsOnPickupCtx with one
// fewer member, so kItemsOnPickupDone's rows have their own offsets that
// nothing else in this file exercises. Until this test existed, a row here
// naming the wrong member - or the table having drifted a field out of step
// with the struct - was invisible.
void test_items_on_pickup_done_round_trip( lua_State* L )
{
    ItemsOnPickupDoneFixture f;

    dispatch_lua( L, find_event( "items_on_pickup_done" ), &f.ctx,
                  "assert(e.collection_index == 61)\n"
                  "assert(e.item_type == 62)\n"
                  "assert(e.subweapon_use_bonus == 63)\n"
                  "assert(e.shop == 64)\n"
                  "assert(e.cheat_flags == 65)\n"
                  "assert(e.subweapon_bonus_slot == true)\n"
                  "assert(e.player ~= nil)\n"
                  "assert(math.abs(e.pos_x - 7.5) < 1e-6)\n"
                  "assert(math.abs(e.pos_y - 8.5) < 1e-6)\n"
                  "assert(math.abs(e.pos_z - 9.5) < 1e-6)\n"
                  "assert(e.mod_handled == false)\n"
                  // The layout difference from ItemsOnPickupCtx: this context
                  // has no presentAllowed, so copying that row across would
                  // read whatever member sits at its offset in the other struct.
                  "assert(e.present_allowed == nil, 'ItemsOnPickupDone has no presentAllowed')\n"
                  "e.collection_index = 99; e.cheat_flags = 7; e.subweapon_bonus_slot = false\n"
                  "e.pos_z = 1234; e.mod_handled = true" );

    CHECK( f.collectionIndex == 99 );
    CHECK( f.cheatFlags == 7u );
    CHECK( f.subweaponBonusSlot == false );
    CHECK( f.itemType == 62 );  // untouched fields stay put
    CHECK( f.pos.z == 9.5f );   // FIELD_POD_IN is read-only
    CHECK( f.ctx.modHandled == true );
}

// is_item_collected's two handle rows (collection/save_slot) are the same
// shape, sitting next to each other in kIsItemCollected. This does not use
// dispatch_chain: catching a row swap needs the event table's fields resolved
// through the handle table while it is still on the stack, before
// dispatch_chain's own push_event/pop would run.
void test_result_last_write_wins( lua_State* L )
{
    // Two distinct fake pointers of two different handle types: the two handle
    // rows are then telling apart from Lua, which a pair of nulls was not.
    void* collectionStorage = nullptr;
    void* saveSlotStorage = nullptr;

    IsItemCollectedCtx ctx{};
    ctx.collection = reinterpret_cast<ItemCollection*>( &collectionStorage );
    ctx.saveSlot = reinterpret_cast<SaveSlot*>( &saveSlotStorage );
    ctx.index = 7;
    ctx.includePawnShop = true;
    ctx.includeEarlyCollected = false;  // opposite of includePawnShop, so a swap shows
    ctx.modHandled = false;
    ctx.modRetVal = false;

    const mml::EventDef& def = find_event( "is_item_collected" );
    World* world = nullptr;
    CHECK( mml::push_event( L, def, &ctx, &world ) );
    const int eventIndex = lua_gettop( L );

    // "e.collection ~= e.save_slot" in the handler body below only proves the
    // two handles differ from each other - true whether or not kIsItemCollected's
    // "collection"/"save_slot" rows were exchanged, since a swap just moves
    // which pointer lands in which field. Resolving each through the handle
    // table and comparing it to the exact pointer placed in the context is what
    // actually catches a swap.
    CHECK( resolve_handle_field<ItemCollection>( L, eventIndex, "collection" ) ==
           reinterpret_cast<ItemCollection*>( &collectionStorage ) );
    CHECK( resolve_handle_field<SaveSlot>( L, eventIndex, "save_slot" ) ==
           reinterpret_cast<SaveSlot*>( &saveSlotStorage ) );

    bool handled = false;
    mml::latch_handled( L, eventIndex, handled );

    // Every one of the five read-only rows is asserted here too; before this,
    // only mod_handled and result were exercised and the other five were free
    // to be wrong.
    const char* bodies[] = {
        "assert(e.collection ~= nil); assert(e.save_slot ~= nil)\n"
        "assert(e.collection ~= e.save_slot, 'the two handle rows must not be the same object')\n"
        "assert(e.index == 7)\n"
        "assert(e.include_pawn_shop == true)\n"
        "assert(e.include_early_collected == false)\n"
        "e.mod_handled = true; e.result = true",
        "e.result = false",
    };
    for ( const char* body : bodies )
    {
        lua_pushvalue( L, eventIndex );
        lua_setglobal( L, "e" );
        if ( luaL_dostring( L, body ) != 0 )
        {
            printf( "FAIL lua: %s\n", lua_tostring( L, -1 ) );
            ++g_failures;
            lua_pop( L, 1 );
        }
        mml::latch_handled( L, eventIndex, handled );
    }

    mml::read_back( L, eventIndex, def, &ctx );
    lua_settop( L, eventIndex - 1 );

    CHECK( ctx.modHandled == true );
    CHECK( ctx.modRetVal == false );  // last writer wins
    CHECK( ctx.index == 7 );          // FIELD_VALUE rows are read-only
    CHECK( ctx.includePawnShop == true );
}

// PickupOnPickupCtx's two handle rows (pickup/listener) sit next to each other
// and are the same shape. Like test_result_last_write_wins above, this does
// not use dispatch_chain: catching a row swap needs the fields resolved
// through the handle table while the event table is still on the stack.
void test_pickup_on_pickup_round_trip( lua_State* L )
{
    void* pickupStorage = nullptr;
    void* listenerStorage = nullptr;

    PickupOnPickupCtx ctx{};
    ctx.pickup = reinterpret_cast<Pickup*>( &pickupStorage );
    ctx.listener = reinterpret_cast<PickupListener*>( &listenerStorage );
    ctx.modHandled = false;
    ctx.modRetVal = false;

    const mml::EventDef& def = find_event( "pickup_on_pickup" );
    World* world = nullptr;
    CHECK( mml::push_event( L, def, &ctx, &world ) );
    const int eventIndex = lua_gettop( L );

    // "e.pickup ~= e.listener" in the handler body below cannot catch
    // kPickupOnPickup's "pickup"/"listener" rows being swapped, for the same
    // reason as test_result_last_write_wins above: both handles stay non-nil
    // and mutually distinct either way. Resolve-and-compare-to-the-exact-
    // pointer does catch it.
    CHECK( resolve_handle_field<Pickup>( L, eventIndex, "pickup" ) == reinterpret_cast<Pickup*>( &pickupStorage ) );
    CHECK( resolve_handle_field<PickupListener>( L, eventIndex, "listener" ) ==
           reinterpret_cast<PickupListener*>( &listenerStorage ) );

    bool handled = false;
    mml::latch_handled( L, eventIndex, handled );

    lua_pushvalue( L, eventIndex );
    lua_setglobal( L, "e" );
    const char* body = "assert(e.pickup ~= nil); assert(e.listener ~= nil)\n"
                       "assert(e.pickup ~= e.listener, 'pickup and listener must be distinct handles')\n"
                       "e.pickup = nil; e.listener = nil\n"  // FIELD_HANDLE is read-only
                       "e.mod_handled = true; e.result = true";
    if ( luaL_dostring( L, body ) != 0 )
    {
        printf( "FAIL lua: %s\n", lua_tostring( L, -1 ) );
        ++g_failures;
        lua_pop( L, 1 );
    }
    mml::latch_handled( L, eventIndex, handled );

    mml::read_back( L, eventIndex, def, &ctx );
    lua_settop( L, eventIndex - 1 );

    CHECK( ctx.modHandled == true );
    CHECK( ctx.modRetVal == true );
    CHECK( ctx.pickup == reinterpret_cast<Pickup*>( &pickupStorage ) );
    CHECK( ctx.listener == reinterpret_cast<PickupListener*>( &listenerStorage ) );
}

void test_shop_item_refresh_round_trip( lua_State* L )
{
    void* shopItemStorage = nullptr;

    ShopItemRefreshCtx ctx{};
    ctx.shopItem = reinterpret_cast<ShopItem*>( &shopItemStorage );
    ctx.modHandled = false;

    const char* bodies[] = {
        "assert(e.shop_item ~= nil)\n"
        "assert(e.result == nil, 'ShopItemRefresh has no modRetVal')\n"
        "e.shop_item = nil\n"  // FIELD_HANDLE is read-only
        "e.mod_handled = true",
    };
    dispatch_chain( L, find_event( "shop_item_refresh" ), &ctx, bodies, 1 );

    CHECK( ctx.modHandled == true );
    CHECK( ctx.shopItem == reinterpret_cast<ShopItem*>( &shopItemStorage ) );
}

// One row, no writable fields and no modHandled: the test is that the handle
// arrives, that assigning to it is inert, and that mod_handled is genuinely
// absent (chest_construct is the one new hook that is not cancellable, and
// pushing a mod_handled row onto a context with no such member would write a
// byte past the end of an 8-byte struct).
void test_chest_construct_round_trip( lua_State* L )
{
    void* chestStorage = nullptr;

    ChestConstructCtx ctx{};
    ctx.chest = reinterpret_cast<Chest*>( &chestStorage );

    dispatch_lua( L, find_event( "chest_construct" ), &ctx,
                  "assert(e.chest ~= nil)\n"
                  "assert(e.mod_handled == nil, 'chest_construct is not cancellable')\n"
                  "e.chest = nil" );

    CHECK( ctx.chest == reinterpret_cast<Chest*>( &chestStorage ) );
}

void test_erroring_handler_does_not_skip_read_back( lua_State* L )
{
    int32_t oldArea = 1, newArea = 2;
    AreaManagerNewAreaCtx ctx{};
    ctx.oldArea = &oldArea;
    ctx.newArea = &newArea;

    // B writes a *different* field than A rather than asserting on A's: an
    // assert inside a dispatch_chain body cannot fail anything on its own (its
    // error is just counted, see dispatch_chain), so "B still ran" has to be
    // pinned by a C++ CHECK against something only B could have written. A
    // dispatch_chain mutated to stop the chain on the first error (rather than
    // swallowing and continuing, as protected_dispatch's lua_pcall does) would
    // leave errors == expectedErrors == 1 - the count alone would not catch
    // it - but oldArea would stay at its initial value instead of becoming 99.
    const char* bodies[] = {
        "e.new_area = 5; error('boom')",
        "e.old_area = 99",
    };
    dispatch_chain( L, find_event( "area_manager_new_area" ), &ctx, bodies, 2, /*expectedErrors=*/1 );

    CHECK( newArea == 5 );   // A's write survives despite A erroring afterward
    CHECK( oldArea == 99 );  // B still ran: the chain did not stop at A's error
}

// `handled` must start from whatever ctx already carries, not from false.
// Slots are keyed on (hookName, priority): handlers registered at different
// priorities are separate protected_dispatch calls, and if the engine walks
// that priority chain reusing one ctx pointer, an earlier-priority claim must
// survive a later-priority handler that clears the field - or a later slot
// with no handlers at all, which would otherwise force the field back to an
// unseeded false. This is the scenario latch_handled's seed call (before the
// loop, both here and in protected_dispatch) exists for.
void test_mod_handled_seeds_from_incoming_ctx( lua_State* L )
{
    int32_t oldArea = 1, newArea = 2;
    AreaManagerNewAreaCtx ctx{};
    ctx.oldArea = &oldArea;
    ctx.newArea = &newArea;
    ctx.modHandled = true;  // already claimed before this chain runs

    const char* bodies[] = {
        "e.mod_handled = false",
    };
    dispatch_chain( L, find_event( "area_manager_new_area" ), &ctx, bodies, 1 );

    CHECK( ctx.modHandled == true );  // the incoming claim must survive
}

// FIELD_PTR_HANDLE is exercised nowhere else: items_on_pickup's "player" row is
// the first row to use it. A Player* is never dereferenced here - only its
// address is compared - so fake, non-dereferenceable pointers are fine, exactly
// like test_world_update_round_trip's fakeWorld.
void test_ptr_handle_round_trips_player( lua_State* L )
{
    void* fakePlayerAStorage = nullptr;
    Player* const fakePlayerA = reinterpret_cast<Player*>( &fakePlayerAStorage );
    void* fakePlayerBStorage = nullptr;
    Player* const fakePlayerB = reinterpret_cast<Player*>( &fakePlayerBStorage );

    ItemsOnPickupFixture f;
    f.player = fakePlayerA;
    ItemsOnPickupCtx& ctx = f.ctx;

    // e.player must read as the live handle for A (pushed by push_event from
    // ctx), and assigning a *different* handle through it must actually take:
    // reassigning the same value back (the original version of this test)
    // would pass just as easily against a read_ptr_handle turned into a
    // total no-op.
    mml::push_handle<Player>( L, fakePlayerB );
    lua_setglobal( L, "other_player" );
    dispatch_lua( L, find_event( "items_on_pickup" ), &ctx, "assert(e.player ~= nil); e.player = other_player" );
    CHECK( f.player == fakePlayerB );

    // Assigning a wrong-typed handle (a World, not a Player) must be silently
    // ignored: read_ptr_handle never raises, and the pointer must be left
    // exactly as it was. It also must not stop read_back from reaching the
    // fields after "player" in kItemsOnPickup - present_allowed is the next
    // row - which is the entire reason read_ptr_handle is non-raising instead
    // of calling check_handle and erroring on a mismatch.
    void* fakeWorldStorage = nullptr;
    World* const fakeWorld = reinterpret_cast<World*>( &fakeWorldStorage );
    mml::push_handle<World>( L, fakeWorld );
    lua_setglobal( L, "bogus_world_handle" );

    dispatch_lua( L, find_event( "items_on_pickup" ), &ctx,
                  "e.player = bogus_world_handle; e.present_allowed = false" );
    CHECK( f.player == fakePlayerB );    // wrong-typed handle: assignment must be a no-op
    CHECK( f.presentAllowed == false );  // read_back still reached the field after it
}

void test_keyboard_bitfield_round_trip( lua_State* L )
{
    // (YC_KEY_COUNT+31)/32 with YC_KEY_COUNT >= 137 is at least 5 words.
    uint32_t down[5] = { 0, 0, 0, 0, 0 };
    uint32_t first[5] = { 0, 0, 0, 0, 0 };
    down[136 / 32] |= 1u << ( 136 % 32 );  // the highest key we can name

    ycKeyboardUpdateCtx ctx{};
    ctx.keysDown = down;
    ctx.keysDownFirstFrame = first;

    // key_held (not key_down) is what reads back keysDown/`down`: keysDown is
    // the level ("held") signal, and key_held is its Lua-facing name (see the
    // mapping comment in eventctx.cpp above l_key_held/l_key_down).
    dispatch_lua( L, find_event( "keyboard_update" ), &ctx,
                  "assert(e:key_held(136) == true, 'high key reads back')\n"
                  "assert(e:key_held(0) == false)\n"
                  "e:set_key_held(136, false)\n"
                  "e:set_key_held(5, true)" );

    CHECK( ( down[136 / 32] & ( 1u << ( 136 % 32 ) ) ) == 0 );
    CHECK( ( down[0] & ( 1u << 5 ) ) != 0 );
}

void test_keyboard_rejects_out_of_range( lua_State* L )
{
    uint32_t down[5] = { 0, 0, 0, 0, 0 };
    uint32_t first[5] = { 0, 0, 0, 0, 0 };
    ycKeyboardUpdateCtx ctx{};
    ctx.keysDown = down;
    ctx.keysDownFirstFrame = first;

    // 137 is past the last named key, so it must error rather than touch word 4.
    dispatch_lua( L, find_event( "keyboard_update" ), &ctx,
                  "assert(pcall(function() e:key_down(137) end) == false)\n"
                  "assert(pcall(function() e:set_key_down(-1, true) end) == false)" );

    // 4294967301 == 2^32 + 5. LuaJIT hands luaL_checkinteger a 64-bit
    // lua_Integer; if the range check ran on that value narrowed to `int`
    // first, this would truncate to 5 and pass the check, silently aliasing
    // an out-of-range index onto key 5 instead of raising. set_key_down backs
    // keysDownFirstFrame (`first`), not keysDown (`down`) - see eventctx.cpp.
    dispatch_lua( L, find_event( "keyboard_update" ), &ctx,
                  "assert(pcall(function() e:set_key_down(4294967301, true) end) == false)" );
    CHECK( first[0] == 0 );  // must not have silently set bit 5
}

// bit_get/bit_set format the bounds-check message with luaL_error, which routes
// through LuaJIT's own formatter rather than the C library's - its conversion
// table has no 'l' entry (no printf length modifiers at all, just %s/%d/%c/%f/
// %p/%%), so "%lld" used to abort formatting on the spot: the message a mod
// actually saw was "key_down: index ?", with the offending index and the valid
// range silently dropped. This asserts the live text, not merely that the call
// errors - pcall alone (as in test_keyboard_rejects_out_of_range) cannot tell a
// truncated message from a complete one.
void test_bounds_error_message_reports_the_index( lua_State* L )
{
    uint32_t down[5] = { 0, 0, 0, 0, 0 };
    uint32_t first[5] = { 0, 0, 0, 0, 0 };
    ycKeyboardUpdateCtx ctx{};
    ctx.keysDown = down;
    ctx.keysDownFirstFrame = first;

    dispatch_lua( L, find_event( "keyboard_update" ), &ctx,
                  "local ok, err = pcall(function() e:key_down(137) end)\n"
                  "assert(ok == false)\n"
                  "err = tostring(err)\n"
                  "assert(err:find('key_down', 1, true), err)\n"
                  "assert(err:find('137', 1, true), err)\n"
                  "assert(err:find('0..136', 1, true), err)\n"
                  // A value outside `int`'s range must appear in the message in
                  // full, not narrowed to `int` before formatting (which would
                  // both misreport the value and, for this one, wrap to
                  // something that might look in-range).
                  "local ok2, err2 = pcall(function() e:set_key_down(4294967301, true) end)\n"
                  "assert(ok2 == false)\n"
                  "err2 = tostring(err2)\n"
                  "assert(err2:find('set_key_down', 1, true), err2)\n"
                  "assert(err2:find('4294967301', 1, true), err2)" );
}

void test_controller_scalars_write_back( lua_State* L )
{
    ControllerFixture f;
    f.ctx.channel = 3;  // distinct from every stick value and from 0

    // All six sticks/triggers carry different values and all six are asserted:
    // with five of them at zero, swapping any two rows was undetectable.
    dispatch_lua( L, find_event( "controller_update" ), &f.ctx,
                  "assert(e.channel == 3)\n"
                  "assert(e.exists == true)\n"
                  "assert(e.left_stick_x == 101)\n"
                  "assert(e.left_stick_y == 202)\n"
                  "assert(e.right_stick_x == 303)\n"
                  "assert(e.right_stick_y == 404)\n"
                  "assert(e.trigger_left == 505)\n"
                  "assert(e.trigger_right == 606)\n"
                  "e.left_stick_x = -200\n"
                  "e.trigger_right = 707\n"
                  "e.exists = false\n"
                  "e.channel = 99\n"         // FIELD_VALUE: read-only
                  "e:set_button(8, true)\n"  // YC_INPUT_L1
                  "assert(e:button(8) == true)" );

    CHECK( f.lx == -200 );
    CHECK( f.tr == 707 );
    CHECK( f.exists == false );
    CHECK( f.ctx.channel == 3 );  // read-only row: the assignment must not land
    CHECK( f.ly == 202 );         // untouched rows stay put
    CHECK( f.rx == 303 );
    CHECK( f.ry == 404 );
    CHECK( f.tl == 505 );
    CHECK( ( f.buttonDown & ( 1ull << 8 ) ) != 0 );
}

// Lua hands every number back as a double, and converting one that does not fit
// the destination is undefined behaviour in C++ - not a wrap - so an assignment
// out of range must be dropped before the cast. Each of these is reachable from
// plain mod Lua and each hits a different conversion: the scalar row path
// (read_ptr/assign_scalar), the POD component path (read_ptr_comp), the array
// paths (copy_words_out, read_controller, read_mouse_touch) and the bit
// accessors' word read. The branch's UBSan lane flags every one of them if the
// range test is removed.
void test_out_of_range_writes_are_dropped( lua_State* L )
{
    {
        // uint32_t from a negative number, and int16_t from far past its range.
        ItemsOnPickupFixture f;
        dispatch_lua( L, find_event( "items_on_pickup" ), &f.ctx,
                      "e.cheat_flags = -1\n"
                      "e.collection_index = 1e300\n"
                      "e.item_type = 0/0" );  // NaN fails both comparisons
        CHECK( f.cheatFlags == 55u );
        CHECK( f.collectionIndex == 11 );
        CHECK( f.itemType == 22 );

        // In range, including the exact endpoints, still lands.
        dispatch_lua( L, find_event( "items_on_pickup" ), &f.ctx,
                      "e.cheat_flags = 4294967295\n"
                      "e.collection_index = -2147483648" );
        CHECK( f.cheatFlags == 4294967295u );
        CHECK( f.collectionIndex == -2147483647 - 1 );
    }

    {
        ControllerFixture f;
        dispatch_lua( L, find_event( "controller_update" ), &f.ctx,
                      "e.left_stick_x = 99999\n"
                      "e.trigger_left = -99999\n"
                      "e._buttons[2] = -1\n" );  // an out-of-range word half
        CHECK( f.lx == 101 );
        CHECK( f.tl == 505 );
        CHECK( f.buttonDown == 0 );  // the bad half left the engine's value alone

        dispatch_lua( L, find_event( "controller_update" ), &f.ctx,
                      "e.left_stick_x = 32767\n"
                      "e.right_stick_y = -32768" );
        CHECK( f.lx == 32767 );
        CHECK( f.ry == -32768 );
    }

    {
        // The MM_Vec2 component path: float, not an integer, and 1e300 does not
        // fit a float either.
        bool doubleClick = false;
        float downArr[4] = { 0.25f, 0.5f, 0.75f, 1.0f };
        MM_Vec2 scroll{ 4.0f, 5.0f };
        MM_Vec2 delta{ 6.0f, 7.0f };

        ycMouseUpdateCtx ctx{};
        ctx.mouseDown = downArr;
        ctx.doubleClick = &doubleClick;
        ctx.mouseScroll = &scroll;
        ctx.mouseDelta = &delta;

        mml::events_set_touch_count( 4 );
        dispatch_lua( L, find_event( "mouse_update" ), &ctx,
                      "assert(e.scroll_x == 4); assert(e.scroll_y == 5)\n"
                      "assert(e.delta_x == 6); assert(e.delta_y == 7)\n"
                      "e.scroll_x = 1e300\n"
                      "e.delta_y = -1e300\n"
                      "e.touch[1] = 1e300" );
        CHECK( scroll.x == 4.0f );
        CHECK( delta.y == 7.0f );
        CHECK( downArr[0] == 0.25f );
        mml::events_set_touch_count( 0 );
    }

    {
        // copy_words_out and the bit accessors' word read. Word 0 holds a value
        // no uint32_t can represent; the accessor must read it as "no bits set"
        // and the copy-out must leave the engine's word alone.
        uint32_t down[5] = { 0xAAAAAAAAu, 0, 0, 0, 0 };
        uint32_t first[5] = { 0, 0, 0, 0, 0 };
        ycKeyboardUpdateCtx ctx{};
        ctx.keysDown = down;
        ctx.keysDownFirstFrame = first;

        // key_held reads _keys_down; key_down reads _keys_first. See the
        // mapping comment in eventctx.cpp above l_key_held/l_key_down.
        dispatch_lua( L, find_event( "keyboard_update" ), &ctx,
                      "e._keys_down[1] = -1\n"
                      "assert(e:key_held(1) == false, 'an unrepresentable word reads as empty')\n"
                      "e._keys_first[1] = 1e300" );
        CHECK( down[0] == 0xAAAAAAAAu );
        CHECK( first[0] == 0u );
    }
}

// install_controller/read_controller split the 64-bit buttonDown into a
// 2-element _buttons table (lo, hi) so bit_get/bit_set never see a 64-bit
// value. test_controller_scalars_write_back only ever sets a low-half bit
// with the high half at zero, which cannot catch the halves being swapped,
// the high half being dropped on write-back, a wrong shift amount recombining
// them, or the high half never being installed at all. 0x8000010180000001 has
// bits 0 and 31 in the low word and 0, 8 and 31 in the high word (i.e. bits
// 0/31/32/40/63 overall), chosen so a lo/hi swap is not symmetric and shows up
// as a wrong bit rather than an accidental match.
void test_controller_buttons_round_trip_both_halves( lua_State* L )
{
    bool exists = true;
    uint64_t buttonDown = 0x8000010180000001ull;
    int16_t lx = 0, ly = 0, rx = 0, ry = 0, tl = 0, tr = 0;

    ycControllerUpdateCtx ctx{};
    ctx.channel = 0;
    ctx.exists = &exists;
    ctx.buttonDown = &buttonDown;
    ctx.leftStickX = &lx;
    ctx.leftStickY = &ly;
    ctx.rightStickX = &rx;
    ctx.rightStickY = &ry;
    ctx.triggerLeft = &tl;
    ctx.triggerRight = &tr;

    dispatch_lua( L, find_event( "controller_update" ), &ctx,
                  "assert(e:button(0) == true)\n"
                  "assert(e:button(31) == true)\n"
                  "assert(e:button(32) == true)\n"
                  "assert(e:button(40) == true)\n"
                  "assert(e:button(63) == true, 'bit 63 specifically must survive')\n"
                  "assert(e:button(1) == false)\n"
                  "assert(e:button(30) == false)\n"
                  "assert(e:button(33) == false)\n"
                  "assert(e:button(62) == false)" );

    // Nothing was written, so read_controller's unconditional recombination
    // (it runs every dispatch, not just when a handler calls set_button) must
    // reproduce the original value byte-identical.
    CHECK( buttonDown == 0x8000010180000001ull );
}

void test_controller_rejects_out_of_range( lua_State* L )
{
    bool exists = true;
    uint64_t buttonDown = 0;
    int16_t lx = 0, ly = 0, rx = 0, ry = 0, tl = 0, tr = 0;

    ycControllerUpdateCtx ctx{};
    ctx.channel = 0;
    ctx.exists = &exists;
    ctx.buttonDown = &buttonDown;
    ctx.leftStickX = &lx;
    ctx.leftStickY = &ly;
    ctx.rightStickX = &rx;
    ctx.rightStickY = &ry;
    ctx.triggerLeft = &tl;
    ctx.triggerRight = &tr;

    // buttonDown is one uint64_t, so 63 is the highest valid index; 64 must
    // raise rather than wrap or read/write word 2 (which does not exist).
    dispatch_lua( L, find_event( "controller_update" ), &ctx,
                  "assert(pcall(function() e:button(64) end) == false)\n"
                  "assert(pcall(function() e:set_button(64, true) end) == false)" );
    CHECK( buttonDown == 0 );
}

// A handler can reassign a private field (_keys_down, _keys_first, _buttons)
// to anything, or call an accessor method with the wrong self. LuaJIT's
// api_check is a no-op in release builds, so before the lua_istable guards
// added to bit_get/bit_set/copy_words_out/read_controller, every one of these
// would segfault the whole process (SIGSEGV, not a catchable Lua error) from
// plain mod Lua - lua_cpcall cannot catch a segfault. This test's job is
// simply that the process is still alive and the real arrays are untouched
// when it finishes.
void test_reassigned_private_field_does_not_crash( lua_State* L )
{
    uint32_t down[5] = { 0, 0, 0, 0, 0 };
    uint32_t first[5] = { 0, 0, 0, 0, 0 };
    down[0] = 0xFFFFFFFFu;  // a stray write would visibly clobber this

    ycKeyboardUpdateCtx ctx{};
    ctx.keysDown = down;
    ctx.keysDownFirstFrame = first;

    // key_held is the accessor that reads/writes _keys_down (down); key_down
    // reads/writes _keys_first. See the mapping comment in eventctx.cpp above
    // l_key_held/l_key_down.
    dispatch_lua( L, find_event( "keyboard_update" ), &ctx,
                  "e._keys_down = 5\n"
                  "assert(e:key_held(0) == false)\n"
                  "e:set_key_held(0, false)" );
    CHECK( down[0] == 0xFFFFFFFFu );  // bit_set on a non-table field must not touch it

    down[0] = 0xFFFFFFFFu;
    dispatch_lua( L, find_event( "keyboard_update" ), &ctx, "e._keys_down = nil" );
    CHECK( down[0] == 0xFFFFFFFFu );  // copy_words_out must skip a non-table field

    down[0] = 0xFFFFFFFFu;
    dispatch_lua( L, find_event( "keyboard_update" ), &ctx, "e._keys_down = 'astring'" );
    CHECK( down[0] == 0xFFFFFFFFu );

    // A wrong-typed self (not the event table at all) must be treated the
    // same as a missing field, not indexed.
    dispatch_lua( L, find_event( "keyboard_update" ), &ctx,
                  "local f = e.key_held\n"
                  "assert(f( {}, 0 ) == false)" );

    bool exists = true;
    uint64_t buttonDown = 0xFFFFFFFFFFFFFFFFull;
    int16_t lx = 0, ly = 0, rx = 0, ry = 0, tl = 0, tr = 0;
    ycControllerUpdateCtx cctx{};
    cctx.channel = 0;
    cctx.exists = &exists;
    cctx.buttonDown = &buttonDown;
    cctx.leftStickX = &lx;
    cctx.leftStickY = &ly;
    cctx.rightStickX = &rx;
    cctx.rightStickY = &ry;
    cctx.triggerLeft = &tl;
    cctx.triggerRight = &tr;

    dispatch_lua( L, find_event( "controller_update" ), &cctx, "e._buttons = nil" );
    CHECK( buttonDown == 0xFFFFFFFFFFFFFFFFull );  // read_controller must skip a non-table field
}

// key_held/set_key_held exercise keysDown; key_down/set_key_down exercise
// keysDownFirstFrame - the swapped mapping from the old key_down/key_pressed
// names (see the comment in eventctx.cpp above l_key_held/l_key_down). That
// swap is exactly what makes this test worth having: renaming is easy to
// half-apply, e.g. leaving both names bound to the same array, swapping a
// getter but not its setter, or swapping the set_method registration but not
// the field string a helper reads. Without a dedicated test, nothing else
// here reads or writes one pair independently of the other, so any of those
// mistakes would compile clean and pass every other test in this file. This
// test is built to catch each of them specifically:
//   - the two arrays aliased to each other (install_keyboard pointing both
//     _keys_down and _keys_first at the same source table, or both accessor
//     pairs resolving to the same field string)
//   - an accessor reading or writing the wrong array (key_held ending up
//     backed by _keys_first, or key_down by _keys_down)
//   - a getter and its own setter disagreeing about which array they use
//     (e.g. key_held reading _keys_down but set_key_held writing _keys_first)
//     - the specific failure mode a half-applied swap produces, since the
//     four `l_key_*`/`l_set_key_*` definitions are separate lines that must
//     all be edited together.
// The two arrays start with different bits set so an aliased install is
// visible immediately, not just on write-back.
void test_keyboard_arrays_are_independent( lua_State* L )
{
    uint32_t down[5] = { 0, 0, 0, 0, 0 };
    uint32_t first[5] = { 0, 0, 0, 0, 0 };
    down[0] = 1u << 20;   // a "held" bit, distinct from the "just pressed" bit below
    first[0] = 1u << 15;  // a "just pressed" bit not present in keysDown

    ycKeyboardUpdateCtx ctx{};
    ctx.keysDown = down;
    ctx.keysDownFirstFrame = first;

    dispatch_lua( L, find_event( "keyboard_update" ), &ctx,
                  // Reads must come from the right source array, not each other's.
                  "assert(e:key_held(20) == true)\n"
                  "assert(e:key_down(20) == false)\n"
                  "assert(e:key_down(15) == true)\n"
                  "assert(e:key_held(15) == false)\n"
                  // Writes must land in the right array too, and each setter
                  // must agree with its own getter about which array that is.
                  "e:set_key_down(7, true)\n"
                  "assert(e:key_down(7) == true, 'set_key_down must write the array key_down reads')\n"
                  "assert(e:key_held(7) == false, 'set_key_down must not alias key_held')\n"
                  "e:set_key_held(9, true)\n"
                  "assert(e:key_held(9) == true, 'set_key_held must write the array key_held reads')\n"
                  "assert(e:key_down(9) == false, 'set_key_held must not alias key_down')" );

    CHECK( ( first[0] & ( 1u << 7 ) ) != 0 );   // set_key_down reached keysDownFirstFrame...
    CHECK( ( down[0] & ( 1u << 7 ) ) == 0 );    // ...and not keysDown
    CHECK( ( down[0] & ( 1u << 9 ) ) != 0 );    // set_key_held reached keysDown...
    CHECK( ( first[0] & ( 1u << 9 ) ) == 0 );   // ...and not keysDownFirstFrame
    CHECK( ( down[0] & ( 1u << 20 ) ) != 0 );   // keysDown otherwise untouched
    CHECK( ( first[0] & ( 1u << 15 ) ) != 0 );  // keysDownFirstFrame otherwise untouched
}

void test_mouse_without_touch_count( lua_State* L )
{
    bool doubleClick = false;
    float downArr[8] = { 0 };
    MM_Vec2 scroll{ 0.0f, 1.0f };
    MM_Vec2 delta{ 2.0f, 3.0f };

    ycMouseUpdateCtx ctx{};
    ctx.mouseDown = downArr;
    ctx.doubleClick = &doubleClick;
    ctx.mouseScroll = &scroll;
    ctx.mouseDelta = &delta;

    mml::events_set_touch_count( 0 );  // unresolved: the field must be absent

    dispatch_lua( L, find_event( "mouse_update" ), &ctx,
                  "assert(e.touch == nil, 'absent when YC_TOUCH_COUNT is unknown')\n"
                  "assert(e.scroll_y == 1); assert(e.delta_x == 2)\n"
                  "e.scroll_y = 9; e.double_click = true" );

    CHECK( scroll.y == 9.0f );
    CHECK( doubleClick == true );
}

void test_mouse_with_touch_count( lua_State* L )
{
    bool doubleClick = false;
    float downArr[4] = { 0.0f, 0.5f, 0.0f, 0.0f };
    MM_Vec2 scroll{ 0.0f, 0.0f };
    MM_Vec2 delta{ 0.0f, 0.0f };

    ycMouseUpdateCtx ctx{};
    ctx.mouseDown = downArr;
    ctx.doubleClick = &doubleClick;
    ctx.mouseScroll = &scroll;
    ctx.mouseDelta = &delta;

    mml::events_set_touch_count( 4 );

    dispatch_lua( L, find_event( "mouse_update" ), &ctx,
                  "assert(#e.touch == 4)\n"
                  "assert(math.abs(e.touch[2] - 0.5) < 1e-6)\n"
                  "e.touch[1] = 1.0" );

    CHECK( downArr[0] == 1.0f );

    mml::events_set_touch_count( 0 );  // leave the global as the tests found it
}

}  // namespace

int main()
{
    // push_handle (marshal.hpp:119) only touches the handle table and the Lua
    // stack, so a null g_api is safe here and makes any accidental engine call
    // segfault loudly rather than read garbage.
    mml::g_api = nullptr;

    lua_State* L = luaL_newstate();
    luaL_openlibs( L );

    test_fixed_update_is_read_only( L );
    test_game_state_writes_back( L );
    test_bad_write_is_ignored( L );
    test_layout_check_rejects_garbage( L );
    test_null_context_survives( L );
    test_implausible_pointer_is_refused( L );
    test_world_update_round_trip( L );
    test_event_table_matches_expected( L );
    test_mod_handled_is_sticky( L );
    test_items_on_pickup_writes_back( L );
    test_items_on_pickup_done_round_trip( L );
    test_result_last_write_wins( L );
    test_pickup_on_pickup_round_trip( L );
    test_shop_item_refresh_round_trip( L );
    test_chest_construct_round_trip( L );
    test_out_of_range_writes_are_dropped( L );
    test_erroring_handler_does_not_skip_read_back( L );
    test_mod_handled_seeds_from_incoming_ctx( L );
    test_ptr_handle_round_trips_player( L );
    test_keyboard_bitfield_round_trip( L );
    test_keyboard_rejects_out_of_range( L );
    test_bounds_error_message_reports_the_index( L );
    test_controller_scalars_write_back( L );
    test_controller_buttons_round_trip_both_halves( L );
    test_controller_rejects_out_of_range( L );
    test_reassigned_private_field_does_not_crash( L );
    test_keyboard_arrays_are_independent( L );
    test_mouse_without_touch_count( L );
    test_mouse_with_touch_count( L );

    lua_close( L );

    printf( g_failures ? "FAILED (%d)\n" : "ok\n", g_failures );
    return g_failures ? 1 : 0;
}
