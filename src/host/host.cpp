// host.cpp - the mod entry point. One native mod embeds LuaJIT and loads Lua
// mods from sibling mods/<id>/ folders.

#include "MinaModAPI.h"
#include "events.hpp"
#include "handle.hpp"
#include "invoke.hpp"
#include "log.hpp"
#include "modloader.hpp"

#include <filesystem>

#if defined( _WIN32 )
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
#else
    #include <dlfcn.h>
#endif

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

// Where this module was actually loaded from, rather than a path rebuilt from
// %APPDATA%: the game can be pointed at a different folder, and a guess would
// silently find nothing. The layout is <mods>/minamodlua/mod.{dll,so}, so the
// mods folder is the parent and the shipped Lua sits in ./lua.
std::filesystem::path module_dir()
{
#if defined( _WIN32 )
    HMODULE self = nullptr;
    if ( !GetModuleHandleExA( GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              (LPCSTR)&module_dir, &self ) )
        return {};

    char buf[MAX_PATH];
    const DWORD n = GetModuleFileNameA( self, buf, sizeof buf );
    if ( n == 0 || n == sizeof buf ) return {};
    return std::filesystem::path( buf ).parent_path();
#else
    Dl_info info{};
    if ( !dladdr( (void*)&module_dir, &info ) || !info.dli_fname ) return {};
    return std::filesystem::path( info.dli_fname ).parent_path();
#endif
}

// Every function the engine calls is noexcept. On x64 LuaJIT unwinds for real
// (SEH on Windows, DWARF on Linux) rather than longjmp'ing, so an escaping Lua
// error would unwind through the game's own C++ frames, where a catch(...) in
// its frame loop swallows it and the enclosing pcall then reports success.
extern "C" void on_game_shutdown( void* ) noexcept
{
    // Hooks come out first: after lua_close a thunk that still fired would be
    // dispatching into a dead state.
    mml::events_shutdown();
    mml::handles().invalidate_all();

    if ( g_L )
    {
        lua_close( g_L );
        g_L = nullptr;
    }

    mml::log::write( "shut down" );
    mml::log::shutdown();
}

bool start_lua( uint32_t gameRevision )
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

    // Reaches mods as mina.raw, under whatever the Lua layer wraps around it.
    const int bound = mml::open_raw_api( g_L );
    mml::log::write( "bound %d of %d MinaModAPI functions", bound, MINAMODLUA_API_COUNT );

    // open_raw_api leaves the functions and their signatures; both hang off mina
    // so a mod can list what exists and what it takes.
    lua_newtable( g_L );
    lua_insert( g_L, -3 );
    lua_setfield( g_L, -3, "signatures" );
    lua_setfield( g_L, -2, "raw" );

    if ( !mml::events_open( g_L ) )
    {
        lua_pop( g_L, 1 );
        return false;
    }

    const std::filesystem::path home = module_dir();
    if ( home.empty() )
    {
        mml::log::write( "FATAL: cannot locate this module's own folder" );
        lua_pop( g_L, 1 );
        return false;
    }

    mml::load_lua_layer( g_L, home / "lua" );

    // Runs during MinaMod_Init, before most engine systems exist, so mods only
    // register handlers here - the same shape as Factorio's control stage.
    mml::load_mods( g_L, home.parent_path(), gameRevision );
    return true;
}

}  // namespace

extern "C" MM_EXPORT void MinaMod_Init( MinaModAPI* mm ) noexcept
{
    if ( !mm ) return;

    mml::g_api = mm;
    mml::log::init( mm );
    mml::log::write( "minamodlua %s starting", MINAMODLUA_VERSION );

    // There is no struct size field, only APIVersion, so a shorter struct from a
    // future build would be read past its end with nothing to detect it.
    if ( (int)mm->APIVersion != (int)MinaModAPI_Version )
    {
        mml::log::write( "FATAL: MinaModAPI version mismatch - game reports %llu, built against %d",
                         (unsigned long long)mm->APIVersion, (int)MinaModAPI_Version );
        mml::log::shutdown();
        return;
    }

    const uint32_t revision = mm->GetGameRevision ? mm->GetGameRevision() : 0u;
    mml::log::write( "MinaModAPI v%llu, game revision %u", (unsigned long long)mm->APIVersion, revision );

    if ( !start_lua( revision ) ) return;

    if ( mm->InstallHook ) mm->InstallHook( "GameShutdown", 0, on_game_shutdown );

    mml::log::write( "ready" );
}
