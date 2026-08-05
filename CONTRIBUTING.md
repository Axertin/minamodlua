# Contributing

## Two halves

The **C++ half** binds MinaModAPI and hosts LuaJIT. See the [README](README.md) for how to build it.

The **Lua half** is an ergonomic layer that sits on top of the C++ mod and provides helpers and other niceties in pure lua. 

## The seam

What the C++ side has in place before any mod code runs:

|                               |                                                                                                                                                                                                        |
| ----------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `mina.raw.*`                  | All 372 SDK functions, snake_cased. Arguments type- and range-checked, out-params returned as extra values, 64-bit values never crossing as doubles. Errors surface as Lua errors rather than crashes. |
| Receiver methods              | Most of those also live on their receiver's metatable, so `entity:get_children()` works.                                                                                                               |
| `mina.on_event(evt, handler)` | Event registration and dispatch, with per-mod error isolation and tracebacks.                                                                                                                          |
| Handle userdata               | `.valid`, `__eq`, `__tostring`, and generation-stamped invalidation to avoid UAF errors                                                                                                                |
| `defines`                     | Populated at init from the running engine rather than from header constants.                                                                                                                           |
| Per-mod environment           | Whitelist sandbox, scoped `require`, `print` routed to the mod log.                                                                                                                                    |

Everything above that is the Lua layer's territory. Three things constrain it:

- It has `mina.raw.*`, `defines`, and the whitelisted stdlib. No `ffi`, no C extensions, no `package`. It runs in the same sandbox as mods, with no extra privileges.
- A `mina.raw.*` call costs roughly 22 ns against roughly 1 ns for pure Lua, so the layer exists to
  reduce boundary crossings rather than add them. Vector math in particular wants to stay pure Lua.
- If it needs something the C++ side doesn't expose, that's worth an issue.

Naming is snake_case throughout, matching how Factorio does it. The transform from the C names is 1:1 and reversible, so grepping against the SDK's own naming still works.

## License

Contributions are MIT, same as the project.
