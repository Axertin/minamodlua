#pragma once

struct lua_State;

namespace mml
{

// Installs mina.on_event onto the table on top of the stack, which it leaves in
// place.
//
// MM_HookCallback is void(*)(void*) where the void* is the hook's own context,
// so there is no channel to carry a Lua closure through. The way round it is a
// compile-time pool of distinct thunk functions, one handed out per distinct
// (hook name, priority) pair; each knows its own slot and dispatches from there.
bool events_open( lua_State* L );

// Drops every registered handler. Called before lua_close so no thunk can reach
// a dead state if the engine fires a hook during shutdown.
void events_shutdown();

}  // namespace mml
