#include "modloader.hpp"

#include "log.hpp"
#include "modinfo.hpp"
#include "sandbox.hpp"

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

extern "C"
{
#include "lauxlib.h"
#include "lua.h"
}

namespace fs = std::filesystem;

namespace mml
{

namespace
{

const char* const kEnvsKey = "minamodlua.envs";

// debug.traceback alone is not enough: handed a non-string error it returns the
// value unchanged with no traceback at all, and `error({code = 1})` is ordinary
// Lua. Stringify first, then attach the traceback.
int error_handler( lua_State* L )
{
    if ( !lua_isstring( L, 1 ) )
    {
        if ( luaL_callmeta( L, 1, "__tostring" ) )
            lua_replace( L, 1 );
        else
            lua_pushfstring( L, "(error object is a %s)", luaL_typename( L, 1 ) ), lua_replace( L, 1 );
    }
    luaL_traceback( L, L, lua_tostring( L, 1 ), 1 );
    return 1;
}

struct RunCtx
{
    const ModInfo* mod;
    const char* filename;
    bool required;
    bool ran;
};

bool read_file( const fs::path& p, std::string& out )
{
    std::ifstream in( p, std::ios::binary );
    if ( !in ) return false;
    std::ostringstream buf;
    buf << in.rdbuf();
    out = buf.str();
    return true;
}

// Under lua_cpcall so the pushes are protected too: lua_push* and
// luaL_loadbuffer can raise, so a pcall you must push before is not a boundary.
int protected_run( lua_State* L )
{
    auto* c = (RunCtx*)lua_touserdata( L, 1 );

    const fs::path path = c->mod->dir / c->filename;
    std::string src;
    if ( !read_file( path, src ) )
    {
        if ( c->required ) log::write( "%s: cannot read %s", c->mod->id.c_str(), c->filename );
        return 0;
    }

    // "@" for readable tracebacks; "t" rejects bytecode, which LuaJIT does not
    // verify and which is therefore equivalent to shipping native code.
    const std::string chunkname = "@" + c->mod->id + "/" + c->filename;
    if ( luaL_loadbufferx( L, src.data(), src.size(), chunkname.c_str(), "t" ) != 0 )
    {
        log::write( "%s: %s", c->mod->id.c_str(), lua_tostring( L, -1 ) );
        lua_pop( L, 1 );
        return 0;
    }

    lua_getfield( L, LUA_REGISTRYINDEX, kEnvsKey );
    lua_getfield( L, -1, c->mod->id.c_str() );
    lua_setfenv( L, -3 );
    lua_pop( L, 1 );

    lua_pushcfunction( L, error_handler );
    lua_insert( L, -2 );

    if ( lua_pcall( L, 0, 0, -2 ) != 0 )
    {
        log::write( "%s: %s", c->mod->id.c_str(), lua_tostring( L, -1 ) );
        lua_pop( L, 1 );
    }
    else
    {
        c->ran = true;
    }

    lua_pop( L, 1 );  // error handler
    return 0;
}

bool run_file( lua_State* L, const ModInfo& mod, const char* filename, bool required )
{
    RunCtx ctx{ &mod, filename, required, false };

    const int top = lua_gettop( L );
    if ( lua_cpcall( L, protected_run, &ctx ) != 0 )
    {
        // Only reached for errors the inner pcall could not catch, such as
        // running out of memory building the call itself.
        log::write( "%s: %s while loading %s", mod.id.c_str(), lua_tostring( L, -1 ), filename );
        lua_pop( L, 1 );
    }
    lua_settop( L, top );
    return ctx.ran;
}

}  // namespace

int load_mods( lua_State* L, const fs::path& modsDir, uint32_t gameRevision )
{
    if ( !sandbox_init( L ) ) return 0;

    const std::vector<ModInfo> mods = discover_mods( modsDir, gameRevision );
    if ( mods.empty() )
    {
        log::write( "no Lua mods found in %s", modsDir.string().c_str() );
        return 0;
    }

    lua_newtable( L );
    lua_setfield( L, LUA_REGISTRYINDEX, kEnvsKey );

    for ( const ModInfo& mod : mods )
    {
        lua_getfield( L, LUA_REGISTRYINDEX, kEnvsKey );
        sandbox_make_env( L, mod.id.c_str(), mod.dir.generic_string().c_str() );
        lua_setfield( L, -2, mod.id.c_str() );
        lua_pop( L, 1 );
    }

    // Every settings.lua before any main.lua, so a mod can read another's
    // settings at startup.
    for ( const ModInfo& mod : mods )
        run_file( L, mod, "settings.lua", false );

    int loaded = 0;
    for ( const ModInfo& mod : mods )
    {
        if ( run_file( L, mod, "main.lua", true ) )
        {
            ++loaded;
            log::write( "loaded %s (%s)", mod.id.c_str(), mod.name.c_str() );
        }
        else
        {
            log::write( "%s failed to load and is inactive", mod.id.c_str() );
        }
    }

    log::write( "%d of %zu Lua mods loaded", loaded, mods.size() );
    return loaded;
}

}  // namespace mml
