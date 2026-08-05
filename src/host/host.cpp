// host.cpp - the mod entry point.
//
// One native mod embeds LuaJIT and loads Lua mods from sibling mods/<id>/
// folders. This is the skeleton: version gate, logging, and the VM. Mod
// discovery, the per-mod sandbox, and the event dispatch land on top of it.

#include "MinaModAPI.h"
#include "handle.hpp"
#include "invoke.hpp"
#include "log.hpp"

extern "C"
{
#include "lauxlib.h"
#include "lua.h"
#include "luajit.h"
#include "lualib.h"
}

namespace
{

lua_State* g_L = nullptr;

// On x64 LuaJIT unwinds for real (SEH on Windows, DWARF on Linux) rather than
// longjmp'ing, so an unprotected Lua error would propagate through the game's
// C++ frames - where a catch(...) in its frame loop would swallow it and the
// enclosing pcall would report success. noexcept turns any escape into a loud
// terminate instead, and every callback the engine invokes must carry it.
extern "C" void on_game_shutdown( void* ) noexcept
{
    mml::handles().invalidate_all();

    if ( g_L )
    {
        lua_close( g_L );
        g_L = nullptr;
    }

    mml::log::write( "shut down" );
    mml::log::shutdown();
}

bool start_lua()
{
    g_L = luaL_newstate();
    if ( !g_L )
    {
        mml::log::write( "FATAL: could not create a Lua state" );
        return false;
    }

    luaL_openlibs( g_L );

    const bool jit = luaJIT_setmode( g_L, 0, LUAJIT_MODE_ENGINE | LUAJIT_MODE_ON );
    lua_getglobal( g_L, "_VERSION" );
    mml::log::write( "%s (%s), JIT %s", LUAJIT_VERSION, lua_tostring( g_L, -1 ), jit ? "on" : "off" );
    lua_pop( g_L, 1 );

    // Deliberately parked under a name mods are not pointed at: the ergonomic
    // layer sits in front of it, and `raw` is the escape hatch for anything that
    // layer has not covered yet.
    const int bound = mml::open_raw_api( g_L );
    lua_setglobal( g_L, "__mina_raw" );

    mml::log::write( "bound %d of %d MinaModAPI functions", bound, MINAMODLUA_API_COUNT );
    return true;
}

}  // namespace

extern "C" MM_EXPORT void MinaMod_Init( MinaModAPI* mm ) noexcept
{
    if ( !mm ) return;

    mml::g_api = mm;
    mml::log::init( mm );
    mml::log::write( "minamodlua %s starting", MINAMODLUA_VERSION );

    // MinaModAPI carries no struct size field, only APIVersion. A future game
    // build shipping a shorter struct would be read past the end into a garbage
    // function pointer with nothing to detect it, so a mismatch is refused
    // outright rather than warned about.
    if ( (int)mm->APIVersion != (int)MinaModAPI_Version )
    {
        mml::log::write( "FATAL: MinaModAPI version mismatch - game reports %llu, built against %d",
                         (unsigned long long)mm->APIVersion, (int)MinaModAPI_Version );
        mml::log::shutdown();
        return;
    }

    mml::log::write( "MinaModAPI v%llu, game revision %u", (unsigned long long)mm->APIVersion,
                     mm->GetGameRevision ? mm->GetGameRevision() : 0u );

    if ( !start_lua() ) return;

    if ( mm->InstallHook ) mm->InstallHook( "GameShutdown", 0, on_game_shutdown );

    mml::log::write( "ready" );
}
