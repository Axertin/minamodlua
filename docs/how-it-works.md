# How it works

Your mod is a folder of `.lua` files. The game is a C++ program that has never heard of Lua. This page is about everything in between: what runs before your `main.lua` does, what `mina.raw` is, and which Lua guarantees still hold once the engine is on the other side of a call.

You do not need to read this to write a mod. Read it when something behaves in a way plain Lua would not explain.

## The shape of the thing

*Mina the Hollower*'s modding SDK is C. A mod is a native shared library that the game loads and calls. At load, the game hands it one big struct full of function pointers, roughly 426 of them, and that struct is the entire modding API.

minamodlua is one such native mod. It implements no game behavior of its own. It embeds LuaJIT, re-exports that struct into Lua as `mina.raw`, and loads *your* folder as Lua source at runtime. From the game's point of view there is one mod, written in C++. From your point of view there is a Lua environment with the engine in it. That indirection is where the rest of this page comes from.

## Startup, in order

Everything below happens inside the game's mod-initialization call, before the game has drawn a frame.

1. The game loads `minamodlua/mod.dll` (Windows) or `mod.so` (Linux) and calls its entry point, handing over the API struct.
2. The host compares the struct's version against the version it was compiled for. A mismatch stops everything here, with a line in the log. A struct read at the wrong layout is worse than no mods at all.
3. It creates **one** LuaJIT state. Every Lua mod on the machine runs inside it. They get separate environments (see [The sandbox](#the-sandbox)) but share one VM, one garbage collector and one thread, so a mod that spends 50 ms in a handler spends it on everyone's behalf.
4. It builds `mina.raw`: one Lua function per engine function. The log line `bound 396 of 426 MinaModAPI functions` is this step finishing.
5. It builds `mina.signatures` alongside it, with the same keys: `mina.signatures.player_set_pos` is the string `(x: number, y: number)`.
6. It installs `mina.on_event` and reserves its pool of hook slots.
7. It runs the shipped Lua layer, `lua/mina/init.lua`, handing it the table built so far; whatever that returns is what mods will see as `mina`. If it is missing or raises, mods get the raw table and a log line. Today it is a stub that returns its argument unchanged.
8. It scans the folders next to its own inside the game's `mods` directory. A folder is a Lua mod if it contains **both** `main.lua` and `mod.yc`. A folder with `main.lua` and no manifest is skipped with a warning; a folder with neither is somebody else's mod and is ignored silently.
9. It sorts the mods it found: `loadPriority` descending, then id alphabetically. That order is used for everything below.
10. It builds one environment per mod, before running any of them, so a mod's environment exists whether or not it loads.
11. It runs every mod's `settings.lua`, in load order. Missing is fine and not an error.
12. It runs every mod's `main.lua`, in load order.
13. It installs its own shutdown hook, at a priority low enough that it runs after every mod's `game_shutdown` handler, and logs `ready`.

**All of `settings.lua` runs before any of `main.lua`.** That is why `settings.lua` exists: it is the phase where a mod publishes configuration another mod can read before either starts doing work.

**`main.lua` runs before the game exists.** Most engine systems are not up yet, so there is unlikely to be a world or a player to ask about. Register handlers and set up your own state there, and leave engine queries for the `game_init` event, which fires later, once most systems are up. (If you know Factorio: `main.lua` is the control stage.)

If `main.lua` raises, the host logs the error with a traceback and reports the mod as failed. That undoes nothing: any handler the mod registered before the error is still registered and still fires. A half-loaded mod is a live mod.

Only source is accepted, never precompiled bytecode. LuaJIT does not verify bytecode, so a `.lua` file containing bytecode is a native executable wearing a Lua extension.

## What `mina.raw` is

One Lua function per function in the engine's struct. `PlayerGetPos` is `mina.raw.player_get_pos`, `CombatCoreSetHealth` is `mina.raw.combat_core_set_health`.

These are **generated, not written**. A build step extracts the member *names* from the SDK header; the C++ compiler works out each function's argument and return types on its own and stamps out a wrapper for each. Nobody chose which functions to expose, or what shape or name they got.

That explains most of what is odd about `mina.raw`:

- There are ~396 of them, because that is how many the engine has, minus the ones with no mechanical translation.
- The names are transliterated, not designed. `CameraGetProjToView` becomes `camera_get_proj_to_view`, and nothing collapses `camera_*` into a `camera` table. The transform is reversible, so any name here greps against upstream's own headers.
- No grouping, no defaults, no overloads, no convenience. Arguments arrive in the engine's order, including the ones that are always the same value.
- 30 functions do not bind at all, listed under [Not bound](api-reference.md#not-bound) in the reference: variadics, ones taking a raw byte buffer, ones taking a C callback, ones taking an untyped pointer. `mina.on_event` covers the callback cases.

`mina.raw` is the floor, not the ceiling. Friendlier shapes are meant to live in the Lua layer above it (`lua/mina/`), which is currently empty. Until it fills in, `mina.raw` is all there is.

The struct is handed over at runtime, so a slot in it can be null on a particular game build. Calling such a function raises `<name> is not available in this game build` rather than jumping into nothing.

## What the argument checking buys you

This is the largest practical difference between writing a mod here and writing one in C++.

In C++, nothing sits between your call and the engine. Hand a function a pointer to an entity the engine freed two rooms ago and the call proceeds, reading whatever is at that address now. Sometimes the game crashes immediately. Often it does not: it keeps running with a corrupted object and falls over somewhere unrelated minutes later, with a stack trace pointing at innocent code. That failure mode is most of what makes native modding hard.

Every `mina.raw` function checks each of its arguments before the engine sees any of them:

```lua
mina.raw.player_set_pos(10, "north")
-- player_set_pos: bad argument #2 (number expected, got string)
```

That is an ordinary Lua error, raised at your call site, catchable with `pcall`, and it names both the function and the position. A missing argument reads as `got no value`. The engine is never entered.

Four kinds of check:

- **Types.** Numbers, booleans and strings must be what the C function declared. There is no coercion; a string that looks like a number is not accepted where a number is wanted.
- **Handle types.** Handles carry their engine type. Passing a `World` where a `ycEntity` is expected is an error naming both, not a reinterpreted pointer.
- **Stale handles.** A handle the host has invalidated is rejected by the wrapper rather than dereferenced. See below for when that happens, and for what it does not cover.
- **64-bit values.** Lua numbers are doubles, so integers above 2^53 cannot round-trip. Rather than truncate silently in either direction, the wrapper raises.

What is *not* checked is anything the type system cannot see: an out-of-range enum value, an index past the end of something, a legal-looking value the engine will reject on its own terms. Those still reach the engine and are still the engine's business. The guarantee is not "you cannot crash the game", it is "a Lua-level mistake produces a Lua-level error", which covers most of the mistakes people make.

## Handles

Many engine functions hand back a pointer to something the engine owns: an entity, a world, a texture. Those come into Lua as **userdata**: opaque values you can pass back into `mina.raw`, compare with `==`, and `tostring`, but not index for engine fields.

A table would have been a lie in both directions. It would be a snapshot of fields read at one instant, going stale silently, and assigning to it would do nothing, since the engine reads its own memory, not yours.

What is inside the userdata is not the pointer either. It is a slot number and a generation counter into a table the host keeps. Retiring a slot bumps its generation, so every handle Lua still holds for that slot fails validation from then on. That check compares two integers and dereferences nothing.

That is what `.valid` reads:

```lua
mina.on_event("world_update", function(e)
  if not e.world.valid then return end
  -- ...
end)
```

Handles go stale for one reason: **after every `world_destroy` dispatch, every outstanding handle is invalidated**, along with all of them at shutdown. A world going away takes most of what is in it, and room transitions are frequent, so a handle stashed in a global during one event and used in a later one is very likely to be dead by then. Using it raises `world_get_elapsed_time: argument #1 is a stale World; fetch it again` instead of reading freed memory.

Nothing tells the host when the engine destroys one individual object, so invalidation is wholesale, at the moments when destruction happens in bulk. A handle to something the engine disposed of on its own, between worlds, still reports `.valid` and is still passed through. `.valid` means "not invalidated", not "the engine definitely still has this". Fetch handles at the point of use rather than testing old ones.

The rest of the conventions:

- `nil` means null in both directions. A lookup that finds nothing returns `nil` rather than raising, so a handle result is always worth testing. `nil` is accepted anywhere a handle is taken.
- `==` compares identity: two lookups of the same entity produce equal handles.
- `tostring` gives `ycEntity(3:1)`, or `ycEntity(3:1) [stale]` for a dead one, which is useful in a `print`.
- `.valid` is the *only* field. There are no methods on handles: everything you do with one goes through `mina.raw`, with the handle as an argument.

## Extra return values, and structs as loose numbers

Two things in the signatures surprise people, and both come from C not having multiple returns.

**Out-parameters become extra return values.** A C function that needs to give back two numbers takes two pointers and writes through them. Lua has no pointers, so the wrapper allocates that storage itself, makes the call, and pushes whatever the engine wrote as additional returns, after the real return value, in the order the parameters were declared:

```lua
local x, y = mina.raw.player_get_pos()
local ok, cx, cy, cz, ex, ey, ez = mina.raw.combat_shape_get_aabb(shape, 0)
```

`player_get_pos` takes no arguments and returns nothing in C; both numbers are out-parameters. `combat_shape_get_aabb` returns a boolean *and* fills an `MM_AABB`, so the boolean comes first and the box follows.

**A struct is several numbers, not a table.** `MM_Vec3` is three numbers; `MM_Transform` is ten; `MM_Mtx` is sixteen. This holds in both directions: passing a vector means passing three numbers in a row, positionally, with no brackets:

```lua
mina.raw.component_move(component, 0, 1, 0)
```

The reason is cost. Building a table allocates, and these calls sit in per-frame paths; a fixed run of numbers on the stack does not allocate at all. So field *order* matters and cannot be recovered from the signature, and the reference lists it under [Value types](api-reference.md#value-types). Three worth memorizing: `MM_AABB` is center-then-extents (not min/max), `MM_Transform` is rotation-scale-position (not the usual order), and `MM_Color` channels are 0–255 (not 0–1).

Two values do not follow either rule. An engine string arrives as a proper Lua string, copied, so you never hold engine memory. A component type id arrives as an 8-byte **string** rather than a number, because it is a 64-bit value a double would mangle; compare it with `==` and pass it straight back in.

## What an event dispatch is

`mina.on_event` does not hand your Lua function to the engine. The engine can only call C.

What happens instead:

1. The host keeps a pool of 128 pre-made C functions. The first `on_event` for a given (hook, priority) pair claims one and asks the engine to install it. Every later registration for that same pair reuses it and just appends to a list on the C++ side. So the engine knows about at most 128 callbacks, no matter how many handlers Lua has.
2. When the game reaches that point in its frame, it calls that C function, passing a pointer to a context struct: live engine memory, laid out however that hook's author decided.
3. The host reads the fields it knows about out of that struct and builds **a fresh, ordinary Lua table**: numbers become numbers, engine pointers become handles, bitfields become methods on the table.
4. It calls each handler registered for that slot in turn, in registration order, each one inside a `pcall`, passing that same table.
5. After the last handler returns, it copies designated fields *back* out of the table into the engine's struct.

Steps 3 and 5 are the ones that matter when you write a handler.

**The table is a copy, not a view.** Reading `e.elapsed` does not reach into the engine; it reads a number that was put there before your handler started. Keeping a reference to `e` after your handler returns is safe but useless, because nothing writes to it again. The next dispatch builds a new one.

**Only marked fields are copied back, and only after every handler has run.** [`docs/events.md`](events.md) marks the writable ones with `*`. Assigning to any other key is not an error; the value sits in a table that is about to be discarded, so a typo'd field name looks just like a write that did nothing. The one key that gets a warning instead of silence is `mod_handled` set on an event that has no such field.

**Handles inside the table are the exception to "it is a copy."** They point at live engine objects and follow the rules in the previous section.

**Handlers on one dispatch share one table.** A handler can leave a value in `e` for a later handler on the same event to read, including handlers belonging to another mod. `e.mod_handled` is the sanctioned version of this; see the Cancellation section in [`docs/events.md`](events.md) for the rule, which is not simply last-write-wins.

There is no way to unregister a handler. A handler that should stop running has to return early on a flag of its own.

## The error boundary

Every handler call is wrapped in a `pcall`. When a handler raises:

- The error is logged, as `error in <event name>: <chunk>:<line>: <message>`. The chunk name is `<your mod id>/main.lua`, so the line says whose mod it was.
- The remaining handlers for that dispatch still run, and the write-back still happens. One mod's bug does not silence another mod's handler.
- Nothing propagates into the engine. An error escaping into the game's own frames would either be swallowed by the game's error handling and reported as success, or unwind through C++ frames that were not written to expect it.

**Repeat errors are rate-limited.** The first three per registration slot are logged in full; then one line saying further errors will not be reported, and silence. Without that, a handler that throws on `fixed_update` writes sixty lines a second for as long as the game runs. The counter resets only when the host shuts down, so after fixing a handler, restart before judging whether the log is clean.

Handlers get file and line, not a traceback; the traceback machinery is too expensive for a path that may fire every frame. Load-time errors happen once, so they do get a full traceback. While debugging a handler, calling the real body through `xpcall(body, debug.traceback)` gets you one for that handler alone.

## The sandbox

Each mod runs with its own globals table. `_G` inside your mod is your mod's environment, not the VM's, and that applies to files you `require` as well as `main.lua`.

The effects, in order of how often they come up:

- A global you set in `main.lua` is visible in your own `require`d files and nowhere else. Two mods can both have a global named `state` without noticing each other.
- `string.format = my_version` breaks only your mod. Each environment gets its own copy of the standard library tables.
- Mods cannot see each other's globals. There is no cross-mod calling channel yet; what they share is the engine, through `mina.raw`.

What is absent, and why:

- **No `io`, and `os` trimmed to clocks and dates.** A mod cannot read or write arbitrary files. `require` is the only file reader available, and it reads only `.lua` files under your own mod folder (`..` is rejected outright). Persistence goes through the engine (`mina.raw.get_active_save_slot_contents` and its setters), not the filesystem.
- **No `package`.** No LuaRocks, no C extension modules, no `package.preload`. Whatever your mod needs, it ships in its own folder. This is also what keeps `ffi` unreachable: `package.preload.ffi` exists whether or not `ffi` is a global.
- **No `ffi`.** LuaJIT's FFI can call any native code in the process, and by design it cannot tell a correct declaration from an incorrect one; a typo'd `cdef` compiles and then corrupts memory. It is the one thing in Lua that can break the game as thoroughly as C++ can. `mina.raw` being the only route to the engine is what makes the argument checking above worth anything.
- **No `loadstring`, `dofile` or `loadfile`.** `load` is available but source-only; bytecode is refused everywhere, for the reason given under startup.
- **No `jit` or `collectgarbage`.** Both are process-global. One mod calling `jit.off()` deoptimizes every other mod in the VM, and one mod collecting garbage every frame costs everyone the framerate.
- **`debug` is cut down to `traceback` and `getinfo`.** `debug.getregistry` in particular is gone, because it reaches the host's own handle metatables, and a mod that broke those would crash the game.
- **No `getfenv`/`setfenv`.** They are the way back out to the real globals.

`print` is redirected to the log, prefixed with your mod id, and runs `tostring` metamethods, so printing a handle gives `ycEntity(3:1)` rather than `userdata: 0x...`. There is no stdout to write to.

Everything else is there: the full `string`, `table`, `math`, `coroutine` and `bit` libraries, `pcall`/`xpcall`, metatables, and `require` for your own files.

## Where to look next

- [`docs/api-reference.md`](api-reference.md) — every bound function, its signature in Lua terms, and the struct field orders.
- [`docs/events.md`](events.md) — the 20 event names, what is in each event table, which fields are writable, and the caveats found by running them against a real game.
- [`examples/smoketest/main.lua`](../examples/smoketest/main.lua) — a worked mod that registers on all 20 events and checks each against an independent source.
