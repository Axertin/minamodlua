# minamodlua

Lua bindings for [MinaModAPI](https://github.com/YachtClubGames/MinaModAPI), so *Mina the Hollower* mods can be written in Lua as well as C/C++.

## Why

The game's modding SDK is C, and hands a mod a big function table at load time. This project puts LuaJIT in front of it so that people who know Lua can write mods directly against this API.

[Factorio](https://lua-api.factorio.com/) is referenced heavily for structure and conventions.

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
`mod.yc`. `cmake --build build --target deploy` copies them into the host's own game mods folder; launch the game with `-mod -mod-allow-code`.

On Windows, run that from a Visual Studio developer prompt (LuaJIT builds via its own `msvcbuild.bat`, which needs `cl` and the Windows SDK on `PATH`).

### Cross-compiling for Windows from Linux

Useful when the game is easier to run on Windows than the machine you build on. Requires
`mingw-w64`:

```sh
cmake -B build-win -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw64.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-win
```

## Design

- **LuaJIT**, bound through the Lua C API rather than the FFI. The SDK headers only compile as C++, the FFI cannot detect misdeclarations, and argument type-checking costs essentially nothing.
- **Bindings are deduced, not parsed.** A generator extracts only the member *names*; C++ pointer-to-data-member and parameter-pack deduction do all type handling. Anything unhandled is a compile error.
- **Mod bugs must not crash the game.** Every argument is checked, every call into Lua is protected so an error can never unwind into engine frames.
- **Lua mods are ordinary mods**: `mods/<id>/` with `mod.yc` + `main.lua` and no binary. This means that in theory, the game's own resource replacement will work just fine.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md).

## License

[MIT](LICENSE)
