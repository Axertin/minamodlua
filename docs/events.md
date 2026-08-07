# Event reference

[`docs/api-reference.md`](api-reference.md) covers `mina.raw`, the functions you call *into* the engine. This page covers the other direction: `mina.on_event`, the 20 points at which the engine calls into your Lua, and the event table your handler gets at each one.

A dispatch works like this. The game reaches one of these points and calls into C. The host reads the engine's context struct and builds a **fresh, ordinary Lua table** from it, calls each registered handler in turn with that table, then copies designated fields back out into the engine.

So the table is a copy, not a live view. Reading it never touches the engine, and holding onto it after your handler returns is safe but pointless. Handles inside it are the exception: they refer to live engine objects. [`docs/how-it-works.md`](how-it-works.md) has the longer version, including the error boundary and why handles behave the way they do.

This page is hand-maintained, not generated. If it and the code disagree, the code is right: `src/host/eventdefs.cpp` (the `kEvents` table), `src/host/eventctx.cpp` (the field accessors) and `src/host/events.cpp` (registration and dispatch). You do not need to read any of that to use this page.

## Worked example

`examples/smoketest/main.lua` registers a handler on all 20 events below, and cross-checks several of them against an independent `mina.raw` call rather than just logging that they fired (`keyboard_update`'s `e:key_down(k)` against `mina.raw.is_key_down(k)`, for instance). It builds and stages automatically as `mml-smoketest` (see `examples/smoketest/CMakeLists.txt`), and it is safe to leave installed permanently, since it never assigns to a writable field or calls a mutating `mina.raw` function. Press F10 in-game for a coverage summary of which events have fired and what their self-checks found, or read the same report off `game_shutdown`.

## `mina.on_event(name, fn, priority)`

```lua
mina.on_event("world_update", function(e)
  -- e is one event table, valid only for the duration of this call
end, priority)
```

- `name` is one of the 20 Lua event names below. Registering an unknown name raises an error that lists every known name.
- `fn` receives exactly one argument: the event table for this dispatch.
- `priority` is an optional signed 32-bit integer (default `0`), passed straight through to the engine's `InstallHook(hookName, priority, callback)`. This project does not interpret it. The engine uses it to order **every** hook installed against that hook name, Lua or native. Two `on_event` calls for the same underlying hook at the same priority share a single engine-level hook installation; their Lua handlers then run in the order they were registered.
- **Higher priority runs first.** A handler registered at priority 100 fires before one at -100 on the same hook. This is **observed behavior at revision 149150** (`mml-smoketest`'s `game_init` priority-ordering demo, `examples/smoketest/main.lua`), not a documented guarantee. Upstream's headers don't say which direction the engine sorts, so treat it as true on the build it was checked against, not as a contract.
- **Reserved floor.** `GameShutdown` is the one hook this project installs its own handler on (`src/host/host.cpp`), and it must run **last**: it tears down the Lua state, and any mod `game_shutdown` thunk running after that would find `g_L` already null. Since higher runs first, "last" means the lowest priority in play, so the host installs its teardown hook at a reserved floor, `kTeardownPriorityFloor = -1000000` in `host.cpp`, well below any priority a mod should pick. A mod that registers `game_shutdown` at or below that floor loses the race and never sees its handler run. Don't use priorities anywhere near `-1000000` on `game_shutdown`, or, to be safe, on anything else.
- Each handler call runs in its own `pcall` on the C side, so an error is caught and logged. The remaining handlers for that dispatch still run, and nothing propagates into the engine. Errors are logged in full only for the first 3 occurrences per `(hook, priority)` registration slot, then silenced, so a handler that errors every frame (on `fixed_update`, say) doesn't flood the log. The counter resets only when the game exits, so restart before judging whether a fix worked.
- **There is no way to unregister.** `on_event` returns nothing, and nothing removes a handler once it is in. A handler that should stop doing work has to return early on a flag of its own.
- Registration is process-wide, not per mod. Every mod's handlers for one `(hook, priority)` slot go into a single list and run in the order they registered, which between mods is mod load order. They all get the same event table, so what one handler leaves in it, the next one reads.
- Hook installation can fail: the engine can refuse a hook, and there's a fixed pool of 128 `(hook, priority)` registration slots process-wide, shared by every Lua event name. Both raise a Lua error from `on_event` itself.

Two pairs of Lua names share one underlying hook: `world_update` / `world_update_any` both install `WorldUpdate`, and `world_update_end` / `world_update_end_any` both install `WorldUpdateEnd`. The player-world filter (see the table below) is applied per *handler*, not per hook installation, so registering both names at the same priority still filters each handler correctly regardless of which one happened to install the shared slot first.

## Events

Fields marked `*` are **writable**: whatever the last handler leaves in that key is copied back into the engine's context after every handler for this dispatch has run. Unmarked fields are read-only. Assigning to them is silently discarded, not an error, so a typo'd field name looks just like a write that did nothing. (Which fields are which follows from the field's descriptor in `eventdefs.cpp`: `FIELD_PTR`, `FIELD_PTR_COMP`, `FIELD_PTR_HANDLE`, `FIELD_HANDLED` and `FIELD_RESULT` rows have a read function and are writable; `FIELD_VALUE`, `FIELD_HANDLE` and `FIELD_POD_IN` rows do not and are read-only.)

`world`, `player`, `pickup`, `listener`, `shop_item`, `chest`, `collection` and `save_slot` are handles (opaque userdata), not tables, the same convention as `mina.raw`.

| Event | Hook | Fields | Notes |
| --- | --- | --- | --- |
| `fixed_update` | `FixedUpdate` | `elapsed` | Runs every fixed-timestep tick (1/60 or 1/120s). |
| `game_state_transition` | `GameStateTransition` | `new_state*` | `new_state` is an integer game-state id. |
| `game_init` | `GameInit` | *(none)* | Fires a bit after `MinaMod_Init`, once most systems are up. |
| `game_shutdown` | `GameShutdown` | *(none)* | |
| `world_construct` | `WorldConstruct` | `world` | |
| `world_destroy` | `WorldDestroy` | `world` | All live handles are invalidated immediately after this dispatch; see Handle lifetime below. |
| `world_update` | `WorldUpdate` | `world`, `elapsed` | Only dispatches to handlers registered under **this** name when `world` is the local player's world (`PlayerGetWorld()`). The default of the two `WorldUpdate` names. |
| `world_update_any` | `WorldUpdate` | `world`, `elapsed` | Same hook as `world_update`, but every handler runs regardless of which world it is. |
| `world_update_end` | `WorldUpdateEnd` | `world`, `elapsed` | Same context shape as `world_update`; fires later in the same frame, after other objects/systems have updated. Player-world filtered, like `world_update`. |
| `world_update_end_any` | `WorldUpdateEnd` | `world`, `elapsed` | Unfiltered counterpart of `world_update_end`. |
| `items_on_pickup` | `ItemsOnPickup` | `collection_index*`, `item_type*`, `player*`, `pos_x`, `pos_y`, `pos_z`, `present_allowed*`, `subweapon_use_bonus*`, `shop*`, `cheat_flags*`, `subweapon_bonus_slot*`, `mod_handled*` | Cancellable. Runs at the start of `Items::OnPickup`. `pos_*` is the pickup position, read-only. |
| `items_on_pickup_done` | `ItemsOnPickupDone` | `collection_index*`, `item_type*`, `player*`, `pos_x`, `pos_y`, `pos_z`, `subweapon_use_bonus*`, `shop*`, `cheat_flags*`, `subweapon_bonus_slot*`, `mod_handled*` | Cancellable. Runs at the start of `Items::OnPickupDone`. |
| `is_item_collected` | `IsItemCollected` | `collection`, `save_slot`, `index`, `include_pawn_shop`, `include_early_collected`, `mod_handled*`, `result*` | Cancellable, and has `result`. Runs at the start of `Items::IsItemCollected`. |
| `pickup_on_pickup` | `PickupOnPickup` | `pickup`, `listener`, `mod_handled*`, `result*` | Cancellable, and has `result`. Runs at the start of `Pickup::OnPickup`. |
| `shop_item_refresh` | `ShopItemRefresh` | `shop_item`, `mod_handled*` | Cancellable. Runs at the start of `ShopItem::Refresh`. |
| `area_manager_new_area` | `AreaManagerNewArea` | `old_area*`, `new_area*`, `mod_handled*` | Cancellable. Both area ids are integers. |
| `chest_construct` | `ChestConstruct` | `chest` | Not cancellable. Fires at the **end** of the `Chest` constructor; see below. |
| `keyboard_update` | `ycKeyboardUpdate` | *(none; see Input hooks)* | Not cancellable. |
| `mouse_update` | `ycMouseUpdate` | `double_click*`, `scroll_x*`, `scroll_y*`, `delta_x*`, `delta_y*` *(plus `touch`, conditionally; see Input hooks)* | Not cancellable. `delta_x`/`delta_y` are pixels with Y increasing downward; see Input hooks for how that relates to `mina.raw.mouse_get_pos()`. |
| `controller_update` | `ycControllerUpdate` | `channel`, `exists*`, `left_stick_x*`, `left_stick_y*`, `right_stick_x*`, `right_stick_y*`, `trigger_left*`, `trigger_right*` *(plus button methods; see Input hooks)* | Not cancellable. Only fires for controller channel 0. |

## Cancellation

`e.mod_handled` exists only on the six cancellable events: `items_on_pickup`, `items_on_pickup_done`, `is_item_collected`, `pickup_on_pickup`, `shop_item_refresh` and `area_manager_new_area`. Setting it on any other event does nothing but log a warning, since there's no field for it to land in. That warning is logged **once** per registration slot and never again. It does not consume the handler-error budget above, so a mod that sets `mod_handled` on `fixed_update` still gets its first three real handler errors reported.

**Claiming an event suppresses the engine's own behavior. It does not stop the other handlers.** Any handler may claim an event by setting `e.mod_handled = true`, and the claim is sticky:

- Once any handler in a dispatch sets it `true`, it stays `true` for the rest of that dispatch, even across handlers registered at different priorities that happen to observe the same underlying context.
- Handlers registered after the claim still run; nothing is skipped. They can read `e.mod_handled` to see it has been claimed, but they cannot un-claim it. Assigning `e.mod_handled = false` after a `true` claim is inert.
- If the context already arrives with `mod_handled` set (from the engine, or conceivably from an earlier priority tier reusing the same context), that incoming claim is seeded before the first handler runs and survives the same way.

`e.result` exists only on `is_item_collected` and `pickup_on_pickup`, the two events whose underlying function returns a value; `result` supplies that value when `mod_handled` suppresses the original. Unlike `mod_handled`, `result` is plain **last-write-wins**: whichever handler assigns it last is what the engine sees.

## `is_item_collected`: reentrancy and cost

`is_item_collected` wraps `Items::IsItemCollected`, and `mina.raw.items_is_item_collected()` calls that same function. **Calling `mina.raw.items_is_item_collected()` from inside an `is_item_collected` handler re-enters the `is_item_collected` hook.** This was measured, not guessed: at revision 149150, `mml-smoketest`'s self-check (which makes that call, to validate its own fields against the raw one) recorded 159,288 handler invocations against 79,644 logged checks. That 2:1 ratio is only possible if every top-level dispatch triggers one nested re-dispatch. The mod's own reentrancy guard is what held it at 2:1 rather than an unbounded stack.

Without a guard this is unbounded recursion inside an engine frame: each nested call re-enters the handler, which calls the raw function again, until the stack overflows. The guard idiom, from `examples/smoketest/main.lua`:

```lua
local is_item_collected_in_progress = false

mina.on_event("is_item_collected", function(e)
  if is_item_collected_in_progress then return end
  is_item_collected_in_progress = true
  local ok = pcall(mina.raw.items_is_item_collected, e.index, e.collection, e.save_slot,
    e.include_pawn_shop, e.include_early_collected)
  is_item_collected_in_progress = false
  -- ... use ok/the result ...
end)
```

A plain boolean flag is enough: `dispatch()` (`src/host/events.cpp`) only re-enters synchronously on the same Lua state and thread, so there's no concurrent access to race against.

**`is_item_collected` is also a very hot path.** That same session logged its 159,288 invocations in a single sitting, an order of magnitude more than any other non-per-frame event. Anything expensive in this handler (allocation, string formatting, I/O, wide `mina.raw` sweeps) runs at that frequency. Keep it cheap. If you need to do real work in response to a collection check, set a flag and do the work on the next `fixed_update` instead.

## Handle lifetime

The event table is an ordinary Lua table, built fresh for each dispatch. Keeping a reference to it after your handler returns is harmless but pointless, since nothing refills it. Field values are copied in at dispatch time and copied back out (for writable fields) once every handler has run; there's no live link to the engine's context in between.

Handles (`world`, `player`, `pickup`, and so on) inside that table are valid **only** for the call that produced them. After every `world_destroy` dispatch, all outstanding handles are invalidated process-wide, since a world going away is the dominant way a handle goes stale. If you need a handle from a previous event later, re-fetch it from `mina.raw` or from the next dispatch rather than holding the one you were handed.

## Input hooks

`keyboard_update`, `mouse_update` and `controller_update` cover per-frame input state. Bitfields and the mouse's touch array don't fit the one-value-per-field model the rest of the table uses, so they expose **methods** on the event table instead of plain fields:

- `e:key_down(k)` / `e:set_key_down(k, v)` — the engine's "went down this frame" bitfield: an **edge** signal, a separate array from `key_held`, not derived from it. Corresponds to `mina.raw.is_key_down(k)`, but see the caveat below: at hook time it does not reliably hold edge data.
- `e:key_held(k)` / `e:set_key_held(k, v)` — true for every frame key index `k` is held down (a **level** signal). Matches `mina.raw.is_key_held(k)`. This is a separate underlying array from `key_down`, not derived from it.
- `e:button(i)` / `e:set_button(i, v)` — controller button state for button index `i`.

Key indices are bounds-checked to **0–136**; button indices to **0–63**. An out-of-range index raises a Lua error rather than silently aliasing (this includes values like `2^32 + 5`, which are checked before any narrowing that could wrap them into range).

The mapping from these methods onto the engine's own fields crosses over, so it is worth spelling out. ("ctx" here and below is the engine's context struct, the C-side thing the event table is built from and copied back into. You never see it from Lua, but its field names are the ones upstream's headers use.)

- `e:key_down` and `e:set_key_down` back onto ctx `keysDownFirstFrame`. Despite its name, that field is the edge signal.
- `e:key_held` and `e:set_key_held` back onto ctx `keysDown`. Despite its name, that field is the level signal.

The Lua names deliberately follow the engine's own `IsKeyDown`/`IsKeyHeld` vocabulary (`mina.raw.is_key_down`/`is_key_held`) rather than upstream's struct field names. Unlike most of this reference, the mapping was **verified against a running game** rather than transcribed from `MinaModHooks.h` alone. An earlier revision of this project had it backwards, pairing `key_down` with the level bitfield and a `key_pressed` accessor with the edge one, which surfaced as every held movement key (S/W/SPACE) disagreeing with `mina.raw.is_key_down`/`is_key_held` in the pattern a crossed pairing produces. See the mapping comment above `l_key_held`/`l_key_down` in `src/host/eventctx.cpp`, and `examples/smoketest/main.lua`'s keyboard diagnostic, which is what caught it.

**Caveat: `e:key_down` is not a dependable edge signal when read from the hook.** The same in-game session that settled the mapping measured each accessor against its `mina.raw` counterpart across ~135,000 samples. `key_held` agreed with `is_key_held` on all 134,710 samples, with zero disagreements. `key_down` agreed with `is_key_down` on 134,685 samples and disagreed on 6. Every captured disagreement looked like this:

```
key 66 (W)  key_down=true  key_held=true  is_key_down=false  is_key_held=true
```

That is a key being *held* with the edge bit reading true anyway, which a clean edge signal should never do mid-hold. `ycKeyboardUpdate` fires at the *start* of the keyboard update, and the evidence is that the engine computes `keysDownFirstFrame` later in that same update, after the hook has run, so what the hook sees is not yet the current frame's edge data. At 6 in 135,000 it is rare rather than systematic, so the field is not simply a copy of `keysDown` either. The behavior was not characterized further.

So: to *detect* a key press, use `mina.raw.is_key_down(k)`, which the engine computes and gets right. Use `e:key_held` for reading state from the hook, and the setters for injecting or suppressing input; the memory mapping is confirmed correct in both directions, which is what the accessors exist for. Deriving your own edge from `key_held` across frames is also reliable.

These methods back onto private fields on the event table: `_keys_down` and `_keys_first` (both on `keyboard_update`) and `_buttons` (on `controller_update`). They are internal representation, not documented surface. Reassigning one to something other than the table it started as (`nil`, a number, a string) is a guarded no-op: reads through the accessor methods report "not set" instead of crashing, and writes are dropped. Don't rely on their shape. Their names follow the *struct* fields, not the Lua method names above, so `_keys_down` (ctx `keysDown`) is read and written by `key_held`, and `_keys_first` (ctx `keysDownFirstFrame`) is read and written by `key_down`.

`mouse_update`'s `e.delta_x`/`e.delta_y` and `mina.raw.mouse_get_pos()` use different coordinate systems, not just different units, and combining them without correcting for that silently gets Y backwards. `delta_x`/`delta_y` are pixels with Y increasing **downward**, ordinary screen space. `mouse_get_pos()` is documented as "top left is (-1,1), bottom right is (1,-1)" (see [`docs/api-reference.md`](api-reference.md)): normalized, with Y increasing **upward**. To compare the two, say a frame-over-frame change in `mouse_get_pos()` against `delta_x`/`delta_y`, negate the normalized Y and account for the scale difference (pixels against a 2-unit normalized span; a 16:9 window gives an x:y scale ratio near 1.84).

This was confirmed in-game, not inferred: `examples/smoketest/main.lua`'s mouse diagnostic captured samples where X agreed in sign on every sample and Y was inverted on every sample, with magnitudes consistent with that scale ratio. Both APIs are behaving correctly; it is a trap only for someone who assumes they share an axis convention. Separately, `mouseDelta` (the ctx field behind `delta_x`/`delta_y`) is populated in practice in this project's testing, despite upstream's header commenting that it is "only used in relative mouse mode". That session was not running in relative mouse mode.

`mouse_update`'s `e.touch`, an array of per-touch-point values, is **absent unless `YC_TOUCH_COUNT` resolves** at startup via the engine's `GetEnumUInt`. That count isn't published in any header this project receives, and a miss from `GetEnumUInt` is expected rather than exceptional; some related enum aliases don't resolve either. Guessing a size would mean writing past the real array and corrupting live input state every frame, so when it doesn't resolve, `mouse_update` never sets `e.touch` at all. Check `e.touch ~= nil` before indexing it. When present, `e.touch[i]` is writable, with assignments copied back into the underlying array like any other writable field.

## `chest_construct` timing

`chest_construct` fires at the **end** of the `Chest` constructor. The object is fully built by the time your handler sees it, but it has not been docked into a world yet, so don't assume it is reachable from world queries inside this handler.

## Provenance and stability

**All 20 events have been observed firing against a running game, at revision 149150, via `mml-smoketest` (`examples/smoketest/main.lua`).** Every event's coverage handler fired at least once, and every self-check with an independent `mina.raw` call to compare against reported agreement, after two rounds of correcting the *checks* rather than the code they were checking:

- `key_held` vs `is_key_held` — 134,710 samples, **zero** disagreements.
- `key_down` vs `is_key_down` — 134,685 agreeing, 6 disagreeing; see the caveat under "Input hooks", which explains why that residual is a property of when the engine populates the field rather than a mapping error.
- `world_update`'s `e.world` vs `player_get_world()`, `fixed_update`'s `elapsed`, `items_on_pickup`'s position, `is_item_collected` against `items_is_item_collected()`, and controller stick ranges — all zero disagreements.
- Mouse delta sign vs `mouse_get_pos()` change — 540 of 552 *moving* frames, once the check accounted for the two APIs' opposite Y conventions.

Two earlier sessions reported far worse figures for the keyboard and mouse checks. Both times the checks were wrong, not the bindings: the keyboard pairing was crossed, and the mouse check compared incompatible coordinate systems while counting stationary frames as passes. If you rerun this and get alarming numbers, read the diagnostics rather than the totals.

`YC_TOUCH_COUNT` did not resolve on that build, so `mouse_update`'s `e.touch` was absent throughout. That is the documented fallback, not a failure.

Read that claim narrowly. It is **one build, on one platform, in one session**. It is not a guarantee that these layouts are correct on every build, OS or architecture, and it is not a substitute for the two runtime checks below, which stay on regardless. Upstream has already announced breaking changes to some of these hooks (see the renames at the end of this section), so "observed agreeing at 149150" does not mean "will keep agreeing". It is more confidence than the previous state, which was transcribed from a header and never run, but it is not validation. Confirm again after any game update, which is what `mml-smoketest` is built for.

These context layouts (field names, types and offsets) are transcribed from upstream's `MinaModHooks.h`, which is fetched and pinned alongside the rest of MinaModAPI (see the top-level README). They are not hand-guessed: every context struct backing a field in the table above has a `static_assert` on its size in `eventdefs.cpp`. 15 asserts cover all 18 events that carry any fields (`game_init` and `game_shutdown` have none), because `WorldUpdateCtx` alone backs all four `world_update`/`world_update_end` variants. What those asserts prove is that this project's row offsets agree with the header it compiled against. The *game* is a separate binary that was never checked against either.

Two runtime checks stand between a layout mismatch and a wild pointer dereference inside a game frame:

- **Every** event's pointer-carrying fields are checked on every dispatch: each must be null, or else land above the first page and be correctly aligned for what it points at. This is derived from the field table itself rather than hand-written per event, so `items_on_pickup`'s nine pointers are all covered. A shifted layout almost always trips it on the first dispatch.
- **11 of the 20** events additionally carry an event-specific plausibility `check`: `fixed_update`, `game_state_transition`, `world_construct`, `world_destroy`, the four `world_update`/`world_update_end` variants, and the three input events (bounds on `elapsed`, pointer alignment on `world`, plausibility of the input arrays that aren't ordinary fields).

If either check fails, that event is **permanently disabled for the process** (not the whole mod, not the whole event system) the first time it's dispatched, with a line logged to that effect. Neither check can prove a layout is *right*; they can only reject one that is visibly wrong.

Upstream has announced pending renames to the underlying functions these hooks wrap, notably `IsItemCollected` → `ItemsIsItemCollected`. That will change the corresponding hook string and, most likely, the Lua event name with it. This project tracks upstream's hook strings directly rather than maintaining a compatibility shim, so a future MinaModAPI bump can rename `is_item_collected` out from under existing mods.
