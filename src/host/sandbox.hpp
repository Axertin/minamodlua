#pragma once

struct lua_State;

namespace mml
{

// Prepares the per-mod environment factory. Expects the bound API table on top
// of the stack and pops it.
//
// `setfenv` with an __index fallthrough to _G is not a sandbox, and neither is
// this - a mod can still reach the real globals through getfenv on any C
// function, and mods are native-code peers anyway. What this does prevent is
// accidents: one mod assigning string.format, or leaking a global that silently
// breaks another. That is the failure mode shared-state modding actually hits.
bool sandbox_init( lua_State* L );

// Pushes a fresh environment table for one mod.
void sandbox_make_env( lua_State* L, const char* id, const char* dir );

}  // namespace mml
