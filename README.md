# minamodlua

Lua bindings for [MinaModAPI](https://github.com/YachtClubGames/MinaModAPI), so *Mina the Hollower*
mods can be written in Lua instead of C++.

> **Status: early.** The host loads, embeds LuaJIT and binds 351 of the 372 MinaModAPI functions,
> but it does not discover or run Lua mods yet.

## Why

The game's modding SDK is C: 372 function pointers in one struct, handed to a mod at load time. That
is a fine interface if you write C++ and own a toolchain. This project puts LuaJIT in front of it so
that people who know Lua can write mods without compiling anything.

[Factorio](https://lua-api.factorio.com/) is the model for conventions: per-mod environments, a
`defines` namespace, `remote` interfaces between mods, and a declarative settings system.

## Requirements

- CMake 3.19+
- GCC, Clang, or MSVC with C++17.
- *Mina the Hollower* on the experimental modding branch, to run anything in-game

MinaModAPI and LuaJIT are fetched automatically at configure time, each pinned to an exact commit.

## Build

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Mods land in `build/mods/<id>/` as `mod.so` (Linux) or `mod.dll` (Windows) beside a generated
`mod.yc`. `cmake --build build --target deploy` copies them into the host's own game mods folder;
launch the game with `-mod -mod-allow-code`.

The mod exports exactly one symbol, `MinaMod_Init`. That is enforced by the link line, because
another loaded mod may embed its own LuaJIT and interposing two VMs crashes immediately.

On Windows, run that from a Visual Studio developer prompt — LuaJIT builds via its own
`msvcbuild.bat`, which needs `cl` and the Windows SDK on `PATH`.

### Cross-compiling for Windows from Linux

Useful when the game is easier to run on Windows than the machine you build on. Requires
`mingw-w64`:

```sh
cmake -B build-win -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw64.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-win
```

The result is self-contained — it imports only `KERNEL32.dll` and `msvcrt.dll`, so there are no
runtime DLLs to ship beside it. Copy `build-win/mods/<id>/` into
`%APPDATA%\Yacht Club Games\Mina the Hollower\mods\` by hand; the `deploy` target only knows the
host's own mods folder.

## Design

- **LuaJIT**, bound through the Lua C API rather than the FFI. The SDK headers
  only compile as C++, the FFI cannot detect misdeclarations, and argument type-checking costs
  essentially nothing.
- **Bindings are deduced, not parsed.** A generator extracts only the 372 member *names*; C++
  pointer-to-data-member and parameter-pack deduction do all type handling. 351 bind with no
  per-function code; the remaining 21 (variadics, callback parameters, raw `void*`) get hand-written
  wrappers. Anything unhandled is a compile error rather than a silent gap.
- **Mod bugs must not crash the game.** Every argument is checked, every call into Lua is protected
  so an error can never unwind into engine frames, and engine handles carry a generation stamp so a
  stale reference is a Lua error rather than a use-after-free.
- **Lua mods are ordinary mods**: `mods/<id>/` with `mod.yc` + `main.lua` and no binary.

## Contributing

The Lua-facing ergonomics are deliberately written in **pure Lua**, are the part most worth outside help, and need
neither the game nor a C++ toolchain. See [CONTRIBUTING.md](CONTRIBUTING.md).

## License

MIT — see [LICENSE](LICENSE).
