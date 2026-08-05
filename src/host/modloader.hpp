#pragma once

#include <stdint.h>

#include <filesystem>

struct lua_State;

namespace mml
{

// Runs the shipped Lua layer in `luaDir` against the mina table on top of the
// stack, replacing it with whatever the layer returns. Leaves the original in
// place if the layer is absent or fails, so mods still get the raw bindings.
void load_lua_layer( lua_State* L, const std::filesystem::path& luaDir );

// Discovers, orders and runs every Lua mod under `modsDir`.
//
// Expects the mina table on top of the stack and pops it. Returns how many mods
// ran without error. A mod that fails is logged and skipped; the rest load.
int load_mods( lua_State* L, const std::filesystem::path& modsDir, uint32_t gameRevision );

}  // namespace mml
