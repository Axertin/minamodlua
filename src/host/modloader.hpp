#pragma once

#include <stdint.h>

#include <filesystem>

struct lua_State;

namespace mml
{

// Discovers, orders and runs every Lua mod under `modsDir`.
//
// Expects the bound API table on top of the stack and pops it. Returns how many
// mods ran without error. A mod that fails is logged and skipped; the rest load.
int load_mods( lua_State* L, const std::filesystem::path& modsDir, uint32_t gameRevision );

}  // namespace mml
