# minamodlua

Lua bindings for [MinaModAPI](https://github.com/YachtClubGames/MinaModAPI), so *Mina the Hollower* mods can be written in Lua as well as C/C++.

## Why

The game's modding SDK is C, and hands a mod a big function table at load time. This project puts LuaJIT in front of it so people who know Lua can write mods against that API directly.

You get the whole engine API, no build step, and argument checking on every call. Pass the wrong thing and you get a Lua error naming the function and the argument, instead of a crash or a corrupted game that falls over ten minutes later. [`docs/how-it-works.md`](docs/how-it-works.md) explains what the C++ layer does on your behalf; read it once you have a mod loading.

[Factorio](https://lua-api.factorio.com/) is referenced heavily for structure and conventions.

## Installation

Modding is not in the default build. You need *Mina the Hollower* on Steam's `experimental-modding` branch.

1. Download the archive for your platform from [Releases](../../releases): `minamodlua-<version>-windows-x86_64.zip` or `minamodlua-<version>-linux-x86_64.zip`. `SHA256SUMS.txt` is published alongside them.
2. Extract it into the game's mods folder, so you end up with a `minamodlua/` folder inside it:

   | Windows | `%APPDATA%\Yacht Club Games\Mina the Hollower\mods` |
   |---|---|
   | **Linux** | `~/.local/share/Yacht Club Games/Mina the Hollower/mods` |

3. Launch the game with `-mod -mod-allow-code`.

minamodlua is a host. On its own it does nothing visible. It writes its own log beside the game's `mod.log`, in the folder one level above `mods/`, and a line reading `bound N of M MinaModAPI functions` means it started cleanly. That log is also where your `print` output and any error your mod raises will appear. There is no console.

Lua mods install the same way: one folder each, alongside `minamodlua/`.

## Making a Lua Mod

A Lua mod is a folder in the game's mods directory. No binary, no build step:

```
mods/my-mod/
  mod.yc      manifest, in the game's own format
  main.lua    entry point
```

```
[YCD Version: 1]
MinaModDef
{
	id: "my-mod",
	name: "My Mod",
	modVersion: 1,
	minGameVersion: 0,
	maxGameVersion: 0,
	loadPriority: 0,
}
```

`0` for `minGameVersion`/`maxGameVersion` means unconstrained. `loadPriority` orders mods against each other, highest first. `modVersion` is yours to bump; the host logs it at load, and other mods can read it with `mina.raw.get_mod_version("my-mod")`.

`main.lua` runs at load. If a `settings.lua` is present it runs first, and *every* mod's `settings.lua` runs before *any* mod's `main.lua`, so one mod can read another's configuration before it starts.

```lua
print("hello from my-mod")

mina.on_event("world_update", function(e)
  -- e.world, e.elapsed
end)
```

Load is early. `main.lua` runs while the game is still assembling itself, so most engine systems are not up yet and there is unlikely to be a world or a player to ask about. Register handlers and set up your own state here. Leave engine queries for the `game_init` event, which fires once most systems are up.

### What a mod can see

Each mod gets its own environment, so a mod that reassigns `string.format` breaks only itself, and two mods can use the same global name without colliding.

- `mina.raw` — every bound engine function ([reference](docs/raw-api-reference.md))
- `mina.on_event` — the 20 events the engine dispatches into Lua ([reference](docs/events.md))
- `mina.signatures` — the same signatures the reference lists, available at runtime
- `print` — writes to the log, tagged with your mod id
- `require` — loads `.lua` files from your own mod folder, and nowhere else
- The Lua standard library, minus anything that reaches outside the game: no `io`, no `package`, no `os.exit`, and no `ffi`

The missing `ffi` is what people notice first. `mina.raw` is the only route to the engine, and being the only route is what lets it check every argument. [`docs/how-it-works.md`](docs/how-it-works.md) has the rest of the list and the reasoning.

[`examples/smoketest`](examples/smoketest) is a worked example. It registers a handler on all 20 events and checks what each one reports against an independent source.

## Compiling The C++ Host Locally

### Requirements

- CMake 3.19+
- GCC, Clang, or MSVC with C++17
- *Mina the Hollower* on the experimental modding branch, to run anything in-game

MinaModAPI and LuaJIT are fetched automatically at configure time, each pinned to an exact commit.

### Build

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Mods land in `build/mods/<id>/` as `mod.so` (Linux) or `mod.dll` (Windows), beside a generated `mod.yc`. `cmake --build build --target deploy` copies them into the host's own game mods folder; launch the game with `-mod -mod-allow-code`.

On Windows, run that from a Visual Studio developer prompt (LuaJIT builds via its own `msvcbuild.bat`, which needs `cl` and the Windows SDK on `PATH`).

### Cross-compiling for Windows from Linux

Useful when the game is easier to run on Windows than on the machine you build on. Requires `mingw-w64`:

```sh
cmake -B build-win -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw64.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-win
```

## Design

- LuaJIT, bound through the Lua C API rather than the FFI. The SDK headers only compile as C++, the FFI cannot detect misdeclarations, and argument type-checking costs almost nothing.
- Bindings are deduced, not parsed. A generator extracts only the member *names*; C++ pointer-to-data-member and parameter-pack deduction do all the type handling. Anything unhandled is a compile error.
- Mod bugs must not crash the game. Every argument is checked, and every call into Lua is protected so an error can never unwind into engine frames.
- Lua mods are ordinary mods: `mods/<id>/` with `mod.yc` + `main.lua` and no binary. In theory the game's own resource replacement should work on them too.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md).

## License

[MIT](LICENSE)
