-- mml-smoketest - a worked example of mina.on_event, and an in-game smoke
-- test for the 20-hook event surface. Run this after any game update: watch
-- the log for "first observation" lines as you play, or hit the report
-- hotkey (see REPORT_KEY below) to print a coverage snapshot on demand.
--
-- HARD CONSTRAINT: this mod is observational only. It never assigns to a
-- writable event field (no e.mod_handled, no e.result, no e.delta_x = ...),
-- and never calls a mina.raw function that mutates game state. That is what
-- makes it safe to leave installed forever, so if you extend this file, keep
-- it that way. Every self-check reads two independent sources of the same
-- fact and compares them.
--
-- Sandboxed mods get `print` for logging and `require` for reading files
-- (see src/host/sandbox.cpp). There is no mina.log or mina.read_file to
-- call. `mina.raw`, `mina.signatures` and `mina.on_event` are the rest of
-- the surface.

--------------------------------------------------------------------------
-- Tunables
--------------------------------------------------------------------------

-- YC_KEY_F10 (see MinaModEnums.h). Mina doesn't bind it and window managers
-- rarely intercept F-keys, so it is free to use as a "print the coverage
-- report now" hotkey.
local REPORT_KEY = 106

-- A full sweep of the key range is 137 keys x 2 checks x 2 cross-API calls =
-- ~548 calls per frame, on a hook that fires every frame. keyboard_update
-- measures that cost once (below), logs it, then switches to checking this
-- many keys per frame on a rotating cursor. The full range is still covered
-- every ceil(137/8) = 18 frames, under a third of a second at 60fps, so a
-- key a player is holding down never goes unchecked for long.
local KEYS_PER_SLICE = 8

-- items_on_pickup's pos_x/pos_y is the pickup's position, not the player's,
-- so it never lands on mina.raw.player_get_pos(): the player has to be
-- within pickup range, not on top of the item's origin. This is a sanity
-- bound, catching a badly wrong offset (thousands of units, or NaN), so it
-- is generous on purpose.
local PICKUP_POS_TOLERANCE = 200.0

-- fixed_update's elapsed should land near 1/60 (0.01667) or 1/120 (0.00833)
-- depending on the game's timestep mode; those two targets are ~8.3ms apart,
-- so a few ms of slack tolerates jitter without blurring the two together.
local FIXED_ELAPSED_TOLERANCE = 0.003

-- Priority-ordering demo: two handlers on the same event, registered at
-- these two priorities. See the game_init section below for what this can
-- and cannot prove.
local PRIORITY_LOW = -100
local PRIORITY_HIGH = 100

-- Diagnostic 1 (keyboard): how many distinct keys to snapshot all four
-- values for (see keyboard_diag below) once a CONFIRMED mismatch is seen.
-- Capped so a single key held through many mismatches can't fill the whole
-- list, and so the check in the hot path (keyboard_update, every frame)
-- degrades to a cheap table lookup + early return once full instead of
-- growing forever.
local KEYBOARD_DIAG_CAP = 10

-- Diagnostic 2 (mouse): how many moving-frame samples to keep (see
-- mouse_diag below). Same reasoning as KEYBOARD_DIAG_CAP - mouse_update also
-- fires every frame.
local MOUSE_DIAG_CAP = 8

--------------------------------------------------------------------------
-- Coverage bookkeeping
--------------------------------------------------------------------------

-- observed[name] = how many times that event's coverage handler has run.
local observed = {}

-- checks[key] = { pass = n, fail = n }, keyed per self-check (some events
-- have more than one, e.g. keyboard_update's key_down and key_held
-- checks are tracked separately). The two keyboard_update checks add a third
-- field, provisional = n, via record_key_check below.
local checks = {}

-- Filled in by the two game_init handlers below.
local priority_order = {}

-- How many mouse_update dispatches saw mina.raw.mouse_get_pos() change
-- frame-over-frame. Reported alongside the delta-sign check below so a
-- near-zero count (mouse never moved during the session) makes a
-- vacuously-passing check visible instead of reading as a clean result.
local mouse_moving_frames = 0

-- Diagnostic 1 (keyboard): keyboard_diag[i] = { k, key_down, key_held,
-- is_key_down, is_key_held }. The moment a CONFIRMED mismatch (see
-- record_key_check) is seen for key k, all four values that went into both
-- checks are captured together. Reading them side by side in the report
-- shows a crossed pairing directly (e:key_down tracking is_key_held rather
-- than is_key_down, say) without needing another run. This diagnostic is
-- what confirmed the pairing was crossed that way: key_down and key_held
-- used to be named key_down and key_pressed, and ctx keysDown/
-- keysDownFirstFrame backed them the other way around (see the mapping
-- comment in src/host/eventctx.cpp). It stays wired in to catch a future
-- regression, not just as a record of the one it caught. keyboard_diag_seen
-- enforces "first KEYBOARD_DIAG_CAP distinct keys", not "first N events", so
-- one held key disagreeing every frame can't fill the list by itself.
local keyboard_diag = {}
local keyboard_diag_seen = {}

-- A handful of common key indices, so the diagnostic output doesn't need
-- MinaModEnums.h cross-referenced by hand for the keys a player is most
-- likely holding (movement and modifiers). Not exhaustive; the raw index is
-- always printed too, and is enough on its own.
local KEY_NAMES = {
  [3] = "RETURN", [5] = "ESCAPE", [6] = "SPACE",
  [44] = "A", [47] = "D", [62] = "S", [66] = "W",
  [88] = "UP", [89] = "DOWN", [90] = "RIGHT", [91] = "LEFT",
  [115] = "RSHIFT", [116] = "LSHIFT", [117] = "RCTRL", [118] = "LCTRL",
  [119] = "RALT", [120] = "LALT",
  [132] = "SHIFT", [133] = "CTRL", [134] = "ALT",
}

-- Diagnostic 2 (mouse): mouse_diag[i] = { delta_x, delta_y, frame_dx,
-- frame_dy } - a handful of moving-frame samples (same gate as
-- mouse_moving_frames: only frames where mouse_get_pos() actually changed),
-- with e.delta_x/delta_y next to the frame-over-frame mouse_get_pos() change
-- they're being compared against, at full precision.
local mouse_diag = {}

local function mark_observed(name)
  local first_time = observed[name] == nil
  observed[name] = ( observed[name] or 0 ) + 1
  -- One line per event the first time it fires, not every time - fixed_update
  -- and the three input hooks would otherwise flood the log at 60-120Hz.
  if first_time then print( "first observation: " .. name ) end
end

local function record_check( key, ok )
  local c = checks[key]
  if not c then
    c = { pass = 0, fail = 0 }
    checks[key] = c
  end
  if ok then
    c.pass = c.pass + 1
  else
    c.fail = c.fail + 1
  end
end

-- Per-key latch for the keyboard cross-checks: state[k] is true if key k's
-- check last disagreed. keyboard_update fires every frame, but after the
-- one-time full sweep (see key_sweep_measured below) only KEYS_PER_SLICE
-- keys are checked per frame, on a rotating cursor, so the next observation
-- of a given key k is ~ceil(137/KEYS_PER_SLICE) = 18 frames later, not next
-- frame. A latch keyed by key index survives however many frames elapse
-- between observations, since it only compares this observation to that
-- key's previous one.
--
-- The hook fires at the START of the keyboard update, so on a frame where a
-- key's state changes, the event context already holds the new value while
-- mina.raw.is_key_down()/is_key_held() (queried later, after this handler
-- runs) still report the old one. That is a legitimate one-frame
-- disagreement, not a layout bug. Calling a mismatch "provisional" until the
-- SAME key disagrees again on its next check filters that noise out: a real
-- layout error reproduces every time that key is checked and quickly becomes
-- "confirmed", while a transition-timing mismatch is a one-off the next
-- check (18 frames later) almost never repeats.
local function record_key_check( key, k, ok, state )
  local c = checks[key]
  if not c then
    c = { pass = 0, fail = 0, provisional = 0 }
    checks[key] = c
  end
  if ok then
    c.pass = c.pass + 1
  elseif state[k] then
    c.fail = c.fail + 1  -- same key disagreed last time it was checked too: confirmed
  else
    c.provisional = c.provisional + 1  -- first time this key has disagreed: could be transition noise
  end
  state[k] = not ok
end

-- Diagnostic 1 capture: see keyboard_diag above. Called from check_key
-- (below, next to the keyboard_update handler) only when a CONFIRMED
-- mismatch was just observed for key k; does nothing once KEYBOARD_DIAG_CAP
-- distinct keys have been captured, or if k was already captured.
local function record_keyboard_diag( k, key_down, key_held, is_key_down, is_key_held )
  if keyboard_diag_seen[k] or #keyboard_diag >= KEYBOARD_DIAG_CAP then return end
  keyboard_diag_seen[k] = true
  table.insert( keyboard_diag,
    { k = k, key_down = key_down, key_held = key_held, is_key_down = is_key_down, is_key_held = is_key_held } )
end

local function near( a, b, tolerance ) return math.abs( a - b ) <= tolerance end

local function in_int16( v ) return v ~= nil and v >= -32768 and v <= 32767 end

--------------------------------------------------------------------------
-- Coverage report
--------------------------------------------------------------------------

-- Static metadata for the report: one entry per Lua event name in kEvents
-- order (src/host/eventdefs.cpp), with what a player has to do to exercise
-- it if it hasn't fired yet, and which self-checks apply. This list is the
-- second place event names are spelled out (the on_event calls are the
-- first). A typo here shows up as a silently blank report line, never an
-- error, so check it against kEvents when editing this file.
local EVENT_INFO = {
  { name = "fixed_update", checks = { { key = "fixed_update", label = "elapsed near 1/60 or 1/120" } } },
  { name = "game_state_transition" },
  { name = "game_init" },
  { name = "game_shutdown" },
  { name = "world_construct", action = "load a save or enter an area (constructs a World)" },
  { name = "world_destroy", action = "leave the current area, or return to the main menu (tears down the World)" },
  { name = "world_update", checks = { { key = "world_update", label = "e.world == player_get_world()" } } },
  { name = "world_update_any" },
  { name = "world_update_end" },
  { name = "world_update_end_any" },
  { name = "items_on_pickup", action = "pick up any item",
    checks = { { key = "items_on_pickup", label = "pos_x/pos_y within tolerance of player_get_pos()" } } },
  { name = "items_on_pickup_done", action = "pick up any item" },
  { name = "is_item_collected", action = "open the map/collection screen, or approach a pickup whose item is already collected",
    checks = { { key = "is_item_collected", label = "items_is_item_collected(event's own fields) succeeds" } } },
  { name = "pickup_on_pickup", action = "pick up any item" },
  { name = "shop_item_refresh", action = "visit a shop and let its item list refresh" },
  { name = "area_manager_new_area", action = "walk from one area/zone into another" },
  { name = "chest_construct", action = "enter (or re-enter) an area containing a chest" },
  { name = "keyboard_update", checks = {
      { key = "keyboard_update.key_down", label = "key_down(k) == is_key_down(k)" },
      { key = "keyboard_update.key_held", label = "key_held(k) == is_key_held(k)" },
    } },
  { name = "mouse_update", checks = { { key = "mouse_update", label = "delta sign agrees with frame-over-frame mouse_get_pos() change" } },
    extra = function() return string.format( "%d moving frame(s) observed (checked only when mouse_get_pos() actually changed)", mouse_moving_frames ) end },
  { name = "controller_update", action = "connect a controller (channel 0) and move a stick or trigger",
    checks = { { key = "controller_update", label = "stick/trigger values within int16 range" } } },
}

local function print_coverage_report()
  print( "==== mml-smoketest coverage report ====" )
  for _, info in ipairs( EVENT_INFO ) do
    local count = observed[info.name]
    if count then
      local line = string.format( "  [x] %-24s fired %d time(s)", info.name, count )
      if info.checks then
        for _, c in ipairs( info.checks ) do
          local r = checks[c.key]
          if r then
            if r.provisional ~= nil then
              -- Keyboard-style checks: mismatches are split into confirmed
              -- (same key disagreed on two consecutive checks, a real
              -- layout problem) and provisional (disagreed once, possibly
              -- one-frame transition timing; see record_key_check).
              line = line .. string.format( "  | %s: %d ok / %d confirmed mismatch / %d provisional",
                c.label, r.pass, r.fail, r.provisional )
            else
              line = line .. string.format( "  | %s: %d ok / %d mismatch", c.label, r.pass, r.fail )
            end
          end
        end
      end
      if info.extra then line = line .. "  | " .. info.extra() end
      print( line )
    else
      print( string.format( "  [ ] %-24s not yet observed - %s", info.name,
        info.action or "fires automatically; give it a moment" ) )
    end
  end
  print( string.format( "  game_init priority order (%d then %d, as registered): %s", PRIORITY_LOW, PRIORITY_HIGH,
    #priority_order > 0 and table.concat( priority_order, " -> " ) or "not yet observed" ) )

  -- Diagnostic 1 (keyboard) - see keyboard_diag above and check_key below.
  -- If key_down tracks is_key_held instead of is_key_down (or key_held
  -- tracks is_key_down), that shows up here as key_down == is_key_held and
  -- key_down ~= is_key_down across every row, with no further run required.
  -- That is how the previous key_down/key_pressed naming was confirmed
  -- crossed; see the mapping comment above l_key_held/l_key_down in
  -- src/host/eventctx.cpp.
  print( "  ---- diagnostic 1: keyboard confirmed-mismatch snapshots (up to " .. KEYBOARD_DIAG_CAP .. " distinct keys) ----" )
  if #keyboard_diag == 0 then
    print( "    no confirmed mismatches recorded" )
  else
    for _, s in ipairs( keyboard_diag ) do
      local label = KEY_NAMES[s.k] and string.format( "%d (%s)", s.k, KEY_NAMES[s.k] ) or tostring( s.k )
      print( string.format( "    key %-14s key_down=%-5s key_held=%-5s is_key_down=%-5s is_key_held=%-5s",
        label, tostring( s.key_down ), tostring( s.key_held ), tostring( s.is_key_down ), tostring( s.is_key_held ) ) )
    end
  end

  -- Diagnostic 2 (mouse) - see mouse_diag above. delta_x/delta_y near zero
  -- while pos_change is clearly nonzero points at "mouseDelta isn't
  -- populated outside relative mouse mode". A plausible-magnitude delta with
  -- the wrong sign, or the axes swapped against pos_change, points at an
  -- offset problem instead. Full precision (%.9g) so "near zero" isn't an
  -- artifact of rounding.
  print( "  ---- diagnostic 2: mouse delta vs frame-over-frame mouse_get_pos() change (up to " .. MOUSE_DIAG_CAP .. " moving frames) ----" )
  if #mouse_diag == 0 then
    print( "    no moving frames recorded" )
  else
    for _, s in ipairs( mouse_diag ) do
      print( string.format( "    delta=(%.9g, %.9g)  pos_change=(%.9g, %.9g)", s.delta_x, s.delta_y, s.frame_dx,
        s.frame_dy ) )
    end
  end

  print( "========================================" )
end

--------------------------------------------------------------------------
-- fixed_update - runs every fixed-timestep tick (1/60 or 1/120s).
--------------------------------------------------------------------------

mina.on_event( "fixed_update", function( e )
  mark_observed( "fixed_update" )
  -- e.elapsed is a plain value row (FIELD_VALUE), never nil.
  local ok = near( e.elapsed, 1 / 60, FIXED_ELAPSED_TOLERANCE ) or near( e.elapsed, 1 / 120, FIXED_ELAPSED_TOLERANCE )
  record_check( "fixed_update", ok )
end )

--------------------------------------------------------------------------
-- game_state_transition - fires whenever the game's top-level state changes
-- (entering/leaving menus, loading a save, ...).
--------------------------------------------------------------------------

mina.on_event( "game_state_transition", function( e )
  mark_observed( "game_state_transition" )
  -- No independent API reports "the current game state" for a fresh
  -- comparison, so this event has no self-check - just coverage.
end )

--------------------------------------------------------------------------
-- game_init - fires once, a bit after MinaMod_Init. No fields.
--------------------------------------------------------------------------

-- Priority-ordering demo. Two handlers on the *same* event name at two
-- different priorities land in two different (hookName, priority) slots
-- (src/host/events.cpp:find_or_install). Each is its own hook installation,
-- and the engine decides how installations at different priorities
-- interleave. Registering them here records what order they ran in. It
-- proves nothing about whether two handlers at different priorities observe
-- the same underlying context object, only that both fired and in what
-- order. Don't cite it as evidence for shared-context behavior.
mina.on_event( "game_init", function( e ) table.insert( priority_order, "priority " .. PRIORITY_LOW ) end, PRIORITY_LOW )
mina.on_event( "game_init", function( e ) table.insert( priority_order, "priority " .. PRIORITY_HIGH ) end, PRIORITY_HIGH )

-- Separate handler, default priority, purely for coverage bookkeeping - kept
-- apart from the two above so registering the priority demo doesn't double
-- -count game_init as "fired twice" in the report.
mina.on_event( "game_init", function( e ) mark_observed( "game_init" ) end )

--------------------------------------------------------------------------
-- game_shutdown - fires once, as the game closes. No fields.
--------------------------------------------------------------------------

mina.on_event( "game_shutdown", function( e )
  mark_observed( "game_shutdown" )
  print_coverage_report()
end )

--------------------------------------------------------------------------
-- world_construct / world_destroy - world is a handle, valid only for this
-- call (see "Handle lifetime" in docs/events.md). We don't hold onto it.
--------------------------------------------------------------------------

mina.on_event( "world_construct", function( e ) mark_observed( "world_construct" ) end )
mina.on_event( "world_destroy", function( e ) mark_observed( "world_destroy" ) end )

--------------------------------------------------------------------------
-- world_update - the player-world-filtered variant: only dispatches to this
-- handler when e.world is the local player's world.
--------------------------------------------------------------------------

mina.on_event( "world_update", function( e )
  mark_observed( "world_update" )
  -- Both sides can legitimately be nil (no world at all, e.g. a menu) and
  -- Lua's == treats nil == nil as true, so this is correct without an
  -- explicit nil check on either side.
  record_check( "world_update", e.world == mina.raw.player_get_world() )
end )

--------------------------------------------------------------------------
-- world_update_any / world_update_end / world_update_end_any - same context
-- shape as world_update (all four share WorldUpdateCtx), so the world-vs-
-- player_get_world() check above already exercises that comparison; these
-- three just add coverage rather than repeating it.
--------------------------------------------------------------------------

mina.on_event( "world_update_any", function( e ) mark_observed( "world_update_any" ) end )
mina.on_event( "world_update_end", function( e ) mark_observed( "world_update_end" ) end )
mina.on_event( "world_update_end_any", function( e ) mark_observed( "world_update_end_any" ) end )

--------------------------------------------------------------------------
-- items_on_pickup - cancellable, but we never touch mod_handled. Runs at
-- the start of Items::OnPickup, so pos_x/pos_y/pos_z is the pickup's
-- position, arriving flattened: three separate fields, not one e.pos
-- table or vector. Struct fields always arrive this way over the event
-- boundary.
--------------------------------------------------------------------------

mina.on_event( "items_on_pickup", function( e )
  mark_observed( "items_on_pickup" )
  local px, py = mina.raw.player_get_pos()
  local ok = px ~= nil and near( e.pos_x, px, PICKUP_POS_TOLERANCE ) and near( e.pos_y, py, PICKUP_POS_TOLERANCE )
  record_check( "items_on_pickup", ok )
end )

mina.on_event( "items_on_pickup_done", function( e ) mark_observed( "items_on_pickup_done" ) end )

--------------------------------------------------------------------------
-- is_item_collected - cancellable, and has e.result. Runs at the start of
-- Items::IsItemCollected, so e.result does not yet hold "the real answer"
-- when a handler sees it. That is the point of a cancellable pre-dispatch:
-- a mod SETS result to override, it doesn't read the outcome from it. So
-- the check here calls the raw function ourselves with the event's own
-- field values and confirms it doesn't error, rather than comparing against
-- e.result.
--
-- Caution for anyone extending this: this hook wraps the very function we
-- are calling, and calling it from inside the handler DOES re-enter this
-- same handler - measured in-game at revision 149150, see docs/events.md.
-- in_progress is what keeps that from becoming unbounded recursion.
--------------------------------------------------------------------------

local is_item_collected_in_progress = false

mina.on_event( "is_item_collected", function( e )
  mark_observed( "is_item_collected" )
  if is_item_collected_in_progress then return end
  is_item_collected_in_progress = true
  -- Argument order is NOT the same as the event's field order - index comes
  -- first here, collection and save_slot after (see docs/raw-api-reference.md).
  -- nil handles pass through as null, which is fine: collection/save_slot
  -- can legitimately be nil.
  local ok = pcall( mina.raw.items_is_item_collected, e.index, e.collection, e.save_slot, e.include_pawn_shop,
    e.include_early_collected )
  is_item_collected_in_progress = false
  record_check( "is_item_collected", ok )
end )

mina.on_event( "pickup_on_pickup", function( e ) mark_observed( "pickup_on_pickup" ) end )
mina.on_event( "shop_item_refresh", function( e ) mark_observed( "shop_item_refresh" ) end )
mina.on_event( "area_manager_new_area", function( e ) mark_observed( "area_manager_new_area" ) end )

--------------------------------------------------------------------------
-- chest_construct - fires at the END of Chest's constructor (see
-- docs/events.md). The chest exists but isn't docked into a world yet, so
-- there is nothing further to safely query about it from here.
--------------------------------------------------------------------------

mina.on_event( "chest_construct", function( e ) mark_observed( "chest_construct" ) end )

--------------------------------------------------------------------------
-- keyboard_update - no plain fields; key state comes through methods
-- (e:key_down(k), e:key_held(k)) backed by private array fields on the
-- event table. That's method-call syntax (colon), not e.key_down(k): the
-- method needs `self` (the event table) to find its backing array.
--
-- key_down is an EDGE signal (true only on the frame a key goes down) and
-- key_held is a LEVEL signal (true for every frame a key is held), matching
-- mina.raw.is_key_down/is_key_held below, which is what check_key
-- cross-checks. This pairing was confirmed in-game (see the mapping comment
-- above l_key_held/l_key_down in src/host/eventctx.cpp); it used to be
-- crossed under the old key_down/key_pressed names, which is what diagnostic
-- 1 below exists to catch. Read from the hook, key_down is not a dependable
-- edge signal - see the caveat under "Input hooks" in docs/events.md.
--------------------------------------------------------------------------

local key_cursor = 0
local key_sweep_measured = false

-- Per-key latches for record_key_check (see its definition above). Separate
-- tables for the two checks, since key_down and key_held are independent
-- underlying arrays and can disagree independently of each other.
local key_down_mismatched = {}
local key_held_mismatched = {}

-- Runs both keyboard cross-checks for key k, then feeds diagnostic 1 if
-- either check just went from "flagged last time" to "still disagreeing",
-- which is what record_key_check treats as a CONFIRMED mismatch. The latch
-- is read here BEFORE record_key_check overwrites it, since record_key_check
-- doesn't hand that fact back. Shared by the one-time full sweep and the
-- per-frame amortized sweep below, so the check logic appears once.
local function check_key( e, k )
  local kd, kh = e:key_down( k ), e:key_held( k )
  local ikd, ikh = mina.raw.is_key_down( k ), mina.raw.is_key_held( k )

  local down_was_flagged = key_down_mismatched[k]
  local down_ok = kd == ikd
  record_key_check( "keyboard_update.key_down", k, down_ok, key_down_mismatched )

  local held_was_flagged = key_held_mismatched[k]
  local held_ok = kh == ikh
  record_key_check( "keyboard_update.key_held", k, held_ok, key_held_mismatched )

  if ( down_was_flagged and not down_ok ) or ( held_was_flagged and not held_ok ) then
    record_keyboard_diag( k, kd, kh, ikd, ikh )
  end
end

mina.on_event( "keyboard_update", function( e )
  mark_observed( "keyboard_update" )

  if not key_sweep_measured then
    -- One-time full sweep, timed, so the amortization above is a measured
    -- decision rather than a guess. This is the only frame that pays the
    -- full 137-key cost.
    key_sweep_measured = true
    local start = os.clock()
    for k = 0, 136 do
      check_key( e, k )
    end
    print( string.format( "keyboard_update: one-time full 137-key sweep took %.3fms; switching to %d keys/frame",
      ( os.clock() - start ) * 1000, KEYS_PER_SLICE ) )
  else
    -- Amortized sweep: KEYS_PER_SLICE keys this frame, rotating through the
    -- full 0..136 range over successive frames.
    for i = 0, KEYS_PER_SLICE - 1 do
      local k = ( key_cursor + i ) % 137
      check_key( e, k )
    end
    key_cursor = ( key_cursor + KEYS_PER_SLICE ) % 137
  end

  -- Checked directly every frame, outside the slice above: the report
  -- hotkey has to be responsive regardless of where the rotating cursor
  -- currently is, or pressing it could take up to 18 frames to register.
  -- key_down (the edge signal, true only on the frame the key goes down) is
  -- what belongs here, not key_held, or one press would print the report
  -- once per frame for as long as the key stayed held.
  if e:key_down( REPORT_KEY ) then print_coverage_report() end
end )

--------------------------------------------------------------------------
-- mouse_update - double_click/scroll_*/delta_* are plain fields (field
-- access, not methods) but are still pointer-backed rows, so any of them
-- can legitimately be nil. Guard before doing arithmetic on them.
--------------------------------------------------------------------------

local prev_mouse_x, prev_mouse_y = nil, nil
local touch_reported = false

mina.on_event( "mouse_update", function( e )
  mark_observed( "mouse_update" )

  -- e.touch only exists if YC_TOUCH_COUNT resolved at startup (see "Input
  -- hooks" in docs/events.md). Report once which case this process landed
  -- in, since nothing else can observe that from outside the host.
  if not touch_reported then
    touch_reported = true
    print( "mouse_update: e.touch is " .. ( e.touch ~= nil and "present" or "absent" ) ..
      " (YC_TOUCH_COUNT " .. ( e.touch ~= nil and "resolved" or "did not resolve" ) .. " at startup)" )
  end

  local mx, my = mina.raw.mouse_get_pos()
  if prev_mouse_x ~= nil and e.delta_x ~= nil and e.delta_y ~= nil then
    local frame_dx, frame_dy = mx - prev_mouse_x, my - prev_mouse_y
    -- Only counts as a check on a frame where mouse_get_pos() changed.
    -- When the mouse is stationary, frame_dx/frame_dy are both 0, and 0
    -- "agrees" with anything under signs_agree below by definition, so on a
    -- mostly-stationary mouse (the common case while playing) nearly every
    -- dispatch would pass without the sign check having exercised anything.
    -- Gating on movement means a mismatch count is only measured against
    -- frames where there was something real to disagree about, and reporting
    -- mouse_moving_frames alongside it (see print_coverage_report) makes
    -- that denominator visible instead of letting a vacuous 0-mismatch
    -- result read as a clean pass.
    if frame_dx ~= 0 or frame_dy ~= 0 then
      mouse_moving_frames = mouse_moving_frames + 1
      -- Diagnostic 2 capture: see mouse_diag above. e.delta_x/delta_y are
      -- already known non-nil here (guarded above), so nothing further to
      -- check before recording. Capped at MOUSE_DIAG_CAP; once full this is
      -- a length check and an early-out, not a growing table.
      if #mouse_diag < MOUSE_DIAG_CAP then
        table.insert( mouse_diag, { delta_x = e.delta_x, delta_y = e.delta_y, frame_dx = frame_dx, frame_dy = frame_dy } )
      end
      -- e.delta_x/delta_y and frame_dx/frame_dy use different coordinate
      -- systems, not just different units. mouse_get_pos() is documented as
      -- "top left is (-1,1), bottom right is (1,-1)" (see
      -- mina.raw.mouse_get_pos in docs/raw-api-reference.md): normalized, with Y
      -- increasing UPWARD. delta_x/delta_y are screen-space pixels with Y
      -- increasing DOWNWARD, like every other screen-space delta this
      -- project exposes. X needs no correction; Y must be negated before
      -- comparing, or every real Y movement reads as a sign mismatch.
      -- Confirmed in-game: X agreed in sign on every sample captured, Y was
      -- inverted on every sample (see mouse_diag above and docs/events.md).
      -- Not a bug, just the two APIs' Y axes pointing opposite ways.
      -- delta_x/delta_y's units still aren't documented (and mouseDelta is
      -- nominally "only used in relative mouse mode" per upstream's header,
      -- though it is populated here regardless - see docs/events.md), so
      -- comparing magnitudes would mean guessing at a conversion factor we
      -- don't have. Sign agreement is unit-independent and still a real
      -- cross-check: did the mouse move the direction we independently think
      -- it did. Zero on either side is treated as vacuously consistent
      -- (absolute mode legitimately reports 0 deltas even on a moving
      -- frame).
      local function signs_agree( a, b ) return a == 0 or b == 0 or ( a < 0 ) == ( b < 0 ) end
      record_check( "mouse_update", signs_agree( e.delta_x, frame_dx ) and signs_agree( e.delta_y, -frame_dy ) )
    end
  end
  prev_mouse_x, prev_mouse_y = mx, my
end )

--------------------------------------------------------------------------
-- controller_update - only fires for channel 0 (docs/events.md). exists and
-- the stick/trigger fields are all pointer-backed rows and can be nil.
--------------------------------------------------------------------------

mina.on_event( "controller_update", function( e )
  mark_observed( "controller_update" )
  -- exists is false (or nil) when no controller is connected on this
  -- channel; the stick/trigger fields still arrive as whatever the engine's
  -- memory happens to hold in that case, so there's nothing meaningful to
  -- range-check yet.
  if not e.exists then return end
  local ok = in_int16( e.left_stick_x ) and in_int16( e.left_stick_y ) and in_int16( e.right_stick_x ) and
    in_int16( e.right_stick_y ) and in_int16( e.trigger_left ) and in_int16( e.trigger_right )
  record_check( "controller_update", ok )
end )

print( "mml-smoketest loaded - 20 event handlers registered, press F10 in-game for a coverage report" )
