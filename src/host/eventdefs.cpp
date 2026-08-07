#include "eventdefs.hpp"

#include "eventctx.hpp"

// Must precede MinaModHooks.h: it pulls in MinaModTypes.h, which uses uint8_t
// and size_t without including the headers that declare them.
#include <stddef.h>
#include <stdint.h>

#include <MinaModHooks.h>

namespace mml
{

// Tripwires, not validation: the sizes come from the same header the offsets do,
// so these cannot prove the game agrees. They fail the build if upstream changes
// a context's shape, which is the moment a human needs to re-check the rows
// below and re-run the probe. Pointer-and-int only, so identical on all lanes.
static_assert( sizeof( FixedUpdateCtx ) == 4, "FixedUpdateCtx changed shape" );
static_assert( sizeof( GameStateTransitionCtx ) == 8, "GameStateTransitionCtx changed shape" );
static_assert( sizeof( WorldConstructCtx ) == 8, "WorldConstructCtx changed shape" );
static_assert( sizeof( WorldDestroyCtx ) == 8, "WorldDestroyCtx changed shape" );
static_assert( sizeof( WorldUpdateCtx ) == 16, "WorldUpdateCtx changed shape" );
static_assert( sizeof( ItemsOnPickupCtx ) == 80, "ItemsOnPickupCtx changed shape" );
static_assert( sizeof( ItemsOnPickupDoneCtx ) == 72, "ItemsOnPickupDoneCtx changed shape" );
static_assert( sizeof( IsItemCollectedCtx ) == 24, "IsItemCollectedCtx changed shape" );
static_assert( sizeof( PickupOnPickupCtx ) == 24, "PickupOnPickupCtx changed shape" );
static_assert( sizeof( ShopItemRefreshCtx ) == 16, "ShopItemRefreshCtx changed shape" );
static_assert( sizeof( AreaManagerNewAreaCtx ) == 24, "AreaManagerNewAreaCtx changed shape" );
static_assert( sizeof( ChestConstructCtx ) == 8, "ChestConstructCtx changed shape" );
static_assert( sizeof( ycKeyboardUpdateCtx ) == 16, "ycKeyboardUpdateCtx changed shape" );
static_assert( sizeof( ycMouseUpdateCtx ) == 32, "ycMouseUpdateCtx changed shape" );
static_assert( sizeof( ycControllerUpdateCtx ) == 72, "ycControllerUpdateCtx changed shape" );

namespace
{

bool plausible_elapsed( float e ) { return e > 0.0f && e < 1.0f; }

bool check_fixed_update( const void* ctx ) { return plausible_elapsed( ( (const FixedUpdateCtx*)ctx )->elapsed ); }

bool check_world( const void* ctx )
{
    World* const w = ( (const WorldConstructCtx*)ctx )->world;
    return ( (uintptr_t)w % sizeof( void* ) ) == 0;
}

bool check_world_update( const void* ctx )
{
    const auto* c = (const WorldUpdateCtx*)ctx;
    return plausible_elapsed( c->elapsed ) && ( (uintptr_t)c->world % sizeof( void* ) ) == 0;
}

bool check_game_state( const void* ctx ) { return ( (const GameStateTransitionCtx*)ctx )->pGameState != nullptr; }

// push_event applies plausible_context_pointer to every pointer-bearing row on
// its own, so most events need nothing here. The three input contexts are the
// exception: their arrays are not rows at all - install_keyboard and friends
// take them straight off the context - so a row walk never sees them, and a
// garbage keysDown would be read *and written* a word at a time every frame.
bool check_keyboard( const void* ctx )
{
    const auto* c = (const ycKeyboardUpdateCtx*)ctx;
    return plausible_context_pointer( c->keysDown, alignof( uint32_t ) ) &&
           plausible_context_pointer( c->keysDownFirstFrame, alignof( uint32_t ) );
}

bool check_mouse( const void* ctx )
{
    return plausible_context_pointer( ( (const ycMouseUpdateCtx*)ctx )->mouseDown, alignof( float ) );
}

bool check_controller( const void* ctx )
{
    return plausible_context_pointer( ( (const ycControllerUpdateCtx*)ctx )->buttonDown, alignof( uint64_t ) );
}

const Field kFixedUpdate[] = {
    FIELD_VALUE( "elapsed", FixedUpdateCtx, elapsed, float ),
};

const Field kGameState[] = {
    FIELD_PTR( "new_state", GameStateTransitionCtx, pGameState, int32_t ),
};

// WorldConstructCtx and WorldDestroyCtx are both { World* }; one row set serves
// both, and offsetof is 0 in either case.
const Field kWorld[] = {
    FIELD_HANDLE( "world", WorldConstructCtx, world, World ),
};

const Field kWorldUpdate[] = {
    FIELD_HANDLE( "world", WorldUpdateCtx, world, World ),
    FIELD_VALUE( "elapsed", WorldUpdateCtx, elapsed, float ),
};

const Field kItemsOnPickup[] = {
    FIELD_PTR( "collection_index", ItemsOnPickupCtx, collectionIndex, int32_t ),
    FIELD_PTR( "item_type", ItemsOnPickupCtx, itemType, int32_t ),
    FIELD_PTR_HANDLE( "player", ItemsOnPickupCtx, ppPlayer, Player ),
    FIELD_POD_IN( "pos_x", ItemsOnPickupCtx, pos, MM_Vec3, 0 ),
    FIELD_POD_IN( "pos_y", ItemsOnPickupCtx, pos, MM_Vec3, 1 ),
    FIELD_POD_IN( "pos_z", ItemsOnPickupCtx, pos, MM_Vec3, 2 ),
    FIELD_PTR( "present_allowed", ItemsOnPickupCtx, presentAllowed, bool ),
    FIELD_PTR( "subweapon_use_bonus", ItemsOnPickupCtx, subweaponUseBonus, int32_t ),
    FIELD_PTR( "shop", ItemsOnPickupCtx, shop, int32_t ),
    FIELD_PTR( "cheat_flags", ItemsOnPickupCtx, cheatFlags, uint32_t ),
    FIELD_PTR( "subweapon_bonus_slot", ItemsOnPickupCtx, subweaponBonusSlot, bool ),
    FIELD_HANDLED( ItemsOnPickupCtx ),
};

const Field kItemsOnPickupDone[] = {
    FIELD_PTR( "collection_index", ItemsOnPickupDoneCtx, collectionIndex, int32_t ),
    FIELD_PTR( "item_type", ItemsOnPickupDoneCtx, itemType, int32_t ),
    FIELD_PTR_HANDLE( "player", ItemsOnPickupDoneCtx, ppPlayer, Player ),
    FIELD_POD_IN( "pos_x", ItemsOnPickupDoneCtx, pos, MM_Vec3, 0 ),
    FIELD_POD_IN( "pos_y", ItemsOnPickupDoneCtx, pos, MM_Vec3, 1 ),
    FIELD_POD_IN( "pos_z", ItemsOnPickupDoneCtx, pos, MM_Vec3, 2 ),
    FIELD_PTR( "subweapon_use_bonus", ItemsOnPickupDoneCtx, subweaponUseBonus, int32_t ),
    FIELD_PTR( "shop", ItemsOnPickupDoneCtx, shop, int32_t ),
    FIELD_PTR( "cheat_flags", ItemsOnPickupDoneCtx, cheatFlags, uint32_t ),
    FIELD_PTR( "subweapon_bonus_slot", ItemsOnPickupDoneCtx, subweaponBonusSlot, bool ),
    FIELD_HANDLED( ItemsOnPickupDoneCtx ),
};

const Field kIsItemCollected[] = {
    FIELD_HANDLE( "collection", IsItemCollectedCtx, collection, ItemCollection ),
    FIELD_HANDLE( "save_slot", IsItemCollectedCtx, saveSlot, SaveSlot ),
    FIELD_VALUE( "index", IsItemCollectedCtx, index, int32_t ),
    FIELD_VALUE( "include_pawn_shop", IsItemCollectedCtx, includePawnShop, bool ),
    FIELD_VALUE( "include_early_collected", IsItemCollectedCtx, includeEarlyCollected, bool ),
    FIELD_HANDLED( IsItemCollectedCtx ),
    FIELD_RESULT( IsItemCollectedCtx ),
};

const Field kPickupOnPickup[] = {
    FIELD_HANDLE( "pickup", PickupOnPickupCtx, pickup, Pickup ),
    FIELD_HANDLE( "listener", PickupOnPickupCtx, listener, PickupListener ),
    FIELD_HANDLED( PickupOnPickupCtx ),
    FIELD_RESULT( PickupOnPickupCtx ),
};

const Field kShopItemRefresh[] = {
    FIELD_HANDLE( "shop_item", ShopItemRefreshCtx, shopItem, ShopItem ),
    FIELD_HANDLED( ShopItemRefreshCtx ),
};

const Field kAreaManagerNewArea[] = {
    FIELD_PTR( "old_area", AreaManagerNewAreaCtx, oldArea, int32_t ),
    FIELD_PTR( "new_area", AreaManagerNewAreaCtx, newArea, int32_t ),
    FIELD_HANDLED( AreaManagerNewAreaCtx ),
};

const Field kChestConstruct[] = {
    FIELD_HANDLE( "chest", ChestConstructCtx, chest, Chest ),
};

const Field kMouseUpdate[] = {
    FIELD_PTR( "double_click", ycMouseUpdateCtx, doubleClick, bool ),
    FIELD_PTR_COMP( "scroll_x", ycMouseUpdateCtx, mouseScroll, MM_Vec2, 0 ),
    FIELD_PTR_COMP( "scroll_y", ycMouseUpdateCtx, mouseScroll, MM_Vec2, 1 ),
    FIELD_PTR_COMP( "delta_x", ycMouseUpdateCtx, mouseDelta, MM_Vec2, 0 ),
    FIELD_PTR_COMP( "delta_y", ycMouseUpdateCtx, mouseDelta, MM_Vec2, 1 ),
};

const Field kControllerUpdate[] = {
    FIELD_VALUE( "channel", ycControllerUpdateCtx, channel, int32_t ),
    FIELD_PTR( "exists", ycControllerUpdateCtx, exists, bool ),
    FIELD_PTR( "left_stick_x", ycControllerUpdateCtx, leftStickX, int16_t ),
    FIELD_PTR( "left_stick_y", ycControllerUpdateCtx, leftStickY, int16_t ),
    FIELD_PTR( "right_stick_x", ycControllerUpdateCtx, rightStickX, int16_t ),
    FIELD_PTR( "right_stick_y", ycControllerUpdateCtx, rightStickY, int16_t ),
    FIELD_PTR( "trigger_left", ycControllerUpdateCtx, triggerLeft, int16_t ),
    FIELD_PTR( "trigger_right", ycControllerUpdateCtx, triggerRight, int16_t ),
};

// ycKeyboardUpdateCtx has no row fields at all - both members are bitfields
// handled by install_keyboard/read_keyboard - so it uses nullptr/0 for
// fields/fieldCount. A null context is still refused: push_event requires one
// whenever fieldCount is non-zero *or* an installer is set.

}  // namespace

#define N( a ) (uint8_t)( sizeof( a ) / sizeof( ( a )[0] ) )

// Two Lua names map to WorldUpdate. The filtered one is the default.
//
// A nullptr `check` does not mean "unchecked": push_event applies
// check_pointer_fields to every event's pointer-bearing rows regardless. A
// `check` here is the event-specific test on top of that.
const EventDef kEvents[] = {
    { "fixed_update", "FixedUpdate", kFixedUpdate, N( kFixedUpdate ), check_fixed_update, false, false, nullptr,
      nullptr },
    { "game_state_transition", "GameStateTransition", kGameState, N( kGameState ), check_game_state, false, false,
      nullptr, nullptr },
    { "game_init", "GameInit", nullptr, 0, nullptr, false, false, nullptr, nullptr },
    { "game_shutdown", "GameShutdown", nullptr, 0, nullptr, false, false, nullptr, nullptr },
    { "world_construct", "WorldConstruct", kWorld, N( kWorld ), check_world, false, false, nullptr, nullptr },
    { "world_destroy", "WorldDestroy", kWorld, N( kWorld ), check_world, false, false, nullptr, nullptr },
    { "world_update", "WorldUpdate", kWorldUpdate, N( kWorldUpdate ), check_world_update, true, false, nullptr,
      nullptr },
    { "world_update_any", "WorldUpdate", kWorldUpdate, N( kWorldUpdate ), check_world_update, false, false, nullptr,
      nullptr },
    { "world_update_end", "WorldUpdateEnd", kWorldUpdate, N( kWorldUpdate ), check_world_update, true, false, nullptr,
      nullptr },
    { "world_update_end_any", "WorldUpdateEnd", kWorldUpdate, N( kWorldUpdate ), check_world_update, false, false,
      nullptr, nullptr },
    { "items_on_pickup", "ItemsOnPickup", kItemsOnPickup, N( kItemsOnPickup ), nullptr, false, true, nullptr, nullptr },
    { "items_on_pickup_done", "ItemsOnPickupDone", kItemsOnPickupDone, N( kItemsOnPickupDone ), nullptr, false, true,
      nullptr, nullptr },
    { "is_item_collected", "IsItemCollected", kIsItemCollected, N( kIsItemCollected ), nullptr, false, true, nullptr,
      nullptr },
    { "pickup_on_pickup", "PickupOnPickup", kPickupOnPickup, N( kPickupOnPickup ), nullptr, false, true, nullptr,
      nullptr },
    { "shop_item_refresh", "ShopItemRefresh", kShopItemRefresh, N( kShopItemRefresh ), nullptr, false, true, nullptr,
      nullptr },
    { "area_manager_new_area", "AreaManagerNewArea", kAreaManagerNewArea, N( kAreaManagerNewArea ), nullptr, false,
      true, nullptr, nullptr },
    { "chest_construct", "ChestConstruct", kChestConstruct, N( kChestConstruct ), nullptr, false, false, nullptr,
      nullptr },
    { "keyboard_update", "ycKeyboardUpdate", nullptr, 0, check_keyboard, false, false, install_keyboard,
      read_keyboard },
    { "mouse_update", "ycMouseUpdate", kMouseUpdate, N( kMouseUpdate ), check_mouse, false, false, install_mouse_touch,
      read_mouse_touch },
    { "controller_update", "ycControllerUpdate", kControllerUpdate, N( kControllerUpdate ), check_controller, false,
      false, install_controller, read_controller },
};

const size_t kEventCount = sizeof( kEvents ) / sizeof( kEvents[0] );

#undef N

}  // namespace mml
