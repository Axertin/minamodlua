# Contributing

## Two halves

The C++ half binds MinaModAPI and hosts LuaJIT. See the [README](README.md) for how to build it.

The Lua half is pure-Lua helpers on top of the C++ mod, in [`lua/mina/`](lua/mina/). `init.lua` is handed the table the C++ side built and returns whatever mods see as `mina`; add modules beside it and require them from there:

```lua
-- lua/mina/init.lua
local mina = ...
mina.vec = require("mina.vec")
return mina
```

It ships as files next to the binary, so changing it takes a redeploy and a game restart, with no C++ rebuild. If it errors or is missing, mods still load with the raw bindings.

## The seam

What the C++ side has in place before any mod code runs. [`docs/how-it-works.md`](docs/how-it-works.md) covers the same machinery from a mod author's side, in more detail.

| | |
| --- | --- |
| `mina.raw.*` | 396 of the SDK's 426 functions, snake_cased, generated rather than hand-written. Arguments type-checked, 64-bit values rejected rather than truncated, out-params returned as extra values. Errors surface as Lua errors rather than crashes. The remaining 30 need hand-written wrappers; see "Not bound" in [`docs/raw-api-reference.md`](docs/raw-api-reference.md). |
| `mina.signatures.*` | The signature strings the reference lists, keyed by Lua name, available at runtime. |
| `mina.on_event(evt, handler)` | Event registration and dispatch. Every handler call is its own `pcall`, so an error reaches neither the engine nor another mod's handler. The log line names the event; the Lua message carries the chunk name, which is `<mod id>/main.lua`. Reports are rate-limited to 3 per `(hook, priority)` slot. Tracebacks are attached at load time only, since dispatch is too hot for `debug.traceback`. |
| Handle userdata | `.valid`, `__eq`, `__tostring`, and generation-stamped invalidation to avoid UAF errors. |
| Per-mod environment | Whitelist sandbox, scoped `require`, `print` routed to the mod log. |

Two pieces of that seam are **specified but not built**. They are still the intended direction, but nothing depends on them yet, and nothing (code, docs or examples) should be written as though they exist:

| | |
| --- | --- |
| Receiver methods | The plan (design spec §7.1) is for most functions to also live on their receiver's metatable, so `entity:get_children()` works. Nothing installs any today. `handle_index` in `src/bridge/bindings.cpp` answers `.valid` and then falls back to a `rawget` on the metatable, which is where such a method would hang. But each type's metatable is created lazily, on the first handle of that type, long after the Lua layer has run, so this is C++ work rather than Lua work. `EntityGetChildren` does not bind at all ("no generic mapping") and needs its own wrapper before the sugar is worth anything. |
| `defines` | The plan (design spec §7.4) is a table of engine constants resolved through `GetEnum*` at init rather than transcribed from headers. There is no such table anywhere in `src/`. Mods currently pass raw integers; see `REPORT_KEY` in `examples/smoketest/main.lua`. |

Everything above that is the Lua layer's territory. Three rules constrain it:

- It may use only what a mod can: `mina.raw.*` and the whitelisted stdlib. Nothing enforces this. The layer loads before the sandbox is built, against the real globals and the stock `require`, so it can still reach `io`, `package` or `ffi`. Keeping to the rule anyway is what lets a helper move into a mod later, and what stops mods depending on a privilege they do not have.
- A `mina.raw.*` call costs roughly 22 ns against roughly 1 ns for pure Lua, so the layer exists to reduce boundary crossings rather than add them. Vector math in particular wants to stay pure Lua.
- If it needs something the C++ side doesn't expose, that's worth an issue.

Naming is snake_case throughout, matching how Factorio does it. The transform from the C names is 1:1 and reversible, so grepping against the SDK's own naming still works.

## License

Contributions are MIT, same as the project.
