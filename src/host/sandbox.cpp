#include "sandbox.hpp"

#include "log.hpp"

#include <string.h>

#include <fstream>
#include <sstream>
#include <string>

extern "C"
{
#include "lauxlib.h"
#include "lua.h"
}

namespace mml
{

namespace
{

const char* const kMakeEnvKey = "minamodlua.make_env";

int host_read_file( lua_State* L )
{
    const char* path = luaL_checkstring( L, 1 );
    std::ifstream in( path, std::ios::binary );
    if ( !in )
    {
        lua_pushnil( L );
        return 1;
    }

    std::ostringstream buf;
    buf << in.rdbuf();
    const std::string s = buf.str();
    lua_pushlstring( L, s.data(), s.size() );
    return 1;
}

// Backs `print` inside a mod. Builds the line with std::string rather than a
// fixed buffer, so no length of mod output can overrun anything.
int host_log( lua_State* L )
{
    const int n = lua_gettop( L );
    std::string line = luaL_checkstring( L, 1 );
    line += ": ";

    for ( int i = 2; i <= n; ++i )
    {
        if ( i > 2 ) line += '\t';

        // tostring rather than lua_tostring, so __tostring metamethods run and
        // a handle prints as "ycEntity(3:1)" instead of "userdata".
        lua_getglobal( L, "tostring" );
        lua_pushvalue( L, i );
        lua_call( L, 1, 1 );
        size_t len = 0;
        const char* s = lua_tolstring( L, -1, &len );
        line.append( s ? s : "?", s ? len : 1 );
        lua_pop( L, 1 );
    }

    log::write( "%s", line.c_str() );
    return 0;
}

// Everything a mod may see. Notably absent, each for a reason:
//   getfenv/setfenv    the escape hatch back to the real globals
//   package/require    `package.preload.ffi` is reachable without ffi ever
//                      being a global; require is replaced per-mod below
//   loadstring/dofile/loadfile  load arbitrary code, including bytecode
//   io                 no legitimate use; the SDK has its own file API
//   os.execute/exit/remove/rename/getenv
//   debug.getregistry  reaches our handle metatables, so a mod could break
//                      __index on ycEntity and crash the game
//   newproxy           5.1's backdoor to userdata with __gc
//   jit                jit.off() is process-global: one mod would deoptimise
//                      every other mod in the VM
//   collectgarbage     a mod calling it per frame tanks the framerate
const char* const kBootstrap = R"LUA(
local host = ...

local function shallow(t)
  local c = {}
  for k, v in pairs(t) do c[k] = v end
  return c
end

-- One string metatable is shared by the whole VM, so without this a mod
-- replacing string.format would replace it for every mod.
do
  local mt = getmetatable("")
  if mt then mt.__metatable = "locked" end
end

local function make_require(id, dir, env)
  local loaded = {}
  return function(name)
    if type(name) ~= "string" then error("require expects a module name", 2) end
    if name:find("%.%.", 1, true) then error("require: '..' is not allowed", 2) end

    local cached = loaded[name]
    if cached ~= nil then
      if cached == false then error("circular require of '" .. name .. "'", 2) end
      return cached
    end

    local rel = name:gsub("%.", "/") .. ".lua"
    local src = host.read_file(dir .. "/" .. rel)
    if not src then error("no module '" .. name .. "' in mod '" .. id .. "'", 2) end

    -- "@" and "t" for the same reasons as the mod chunks themselves.
    local chunk, err = load(src, "@" .. id .. "/" .. rel, "t")
    if not chunk then error(err, 2) end
    -- Required files are sandboxed too. Forgetting this is the classic bug: it
    -- leaves every file but main.lua running against the real globals.
    setfenv(chunk, env)

    loaded[name] = false          -- marks in-progress, so a cycle is an error
    local result = chunk(name)
    if result == nil then result = true end
    loaded[name] = result
    return result
  end
end

return function(id, dir)
  local env = {
    assert = assert, error = error, ipairs = ipairs, next = next, pairs = pairs,
    pcall = pcall, xpcall = xpcall, select = select, tonumber = tonumber,
    tostring = tostring, type = type, unpack = unpack,
    rawequal = rawequal, rawget = rawget, rawset = rawset,
    setmetatable = setmetatable, getmetatable = getmetatable,
    _VERSION = _VERSION,

    -- Per-mod copies: `string.format = mine` stays inside the mod that did it.
    string = shallow(string),
    table = shallow(table),
    math = shallow(math),
    coroutine = shallow(coroutine),
    bit = shallow(bit),

    os = { clock = os.clock, time = os.time, date = os.date, difftime = os.difftime },
    debug = { traceback = debug.traceback, getinfo = debug.getinfo },
  }

  env._G = env
  env.print = function(...) host.log(id, ...) end
  env.load = function(src, name) return load(src, name, "t") end
  env.require = make_require(id, dir, env)
  env.mina = host.mina

  return env
end
)LUA";

}  // namespace

bool sandbox_init( lua_State* L )
{
    // Expects the mina table on top; builds host = { read_file, log, mina }.
    lua_newtable( L );

    lua_pushcfunction( L, host_read_file );
    lua_setfield( L, -2, "read_file" );

    lua_pushcfunction( L, host_log );
    lua_setfield( L, -2, "log" );

    lua_pushvalue( L, -2 );  // the mina table, below the host table
    lua_setfield( L, -2, "mina" );

    if ( luaL_loadbuffer( L, kBootstrap, strlen( kBootstrap ), "@minamodlua:sandbox" ) != 0 )
    {
        log::write( "FATAL: sandbox bootstrap failed to compile: %s", lua_tostring( L, -1 ) );
        lua_pop( L, 3 );
        return false;
    }

    lua_insert( L, -2 );  // chunk, host
    if ( lua_pcall( L, 1, 1, 0 ) != 0 )
    {
        log::write( "FATAL: sandbox bootstrap failed to run: %s", lua_tostring( L, -1 ) );
        lua_pop( L, 2 );
        return false;
    }

    lua_setfield( L, LUA_REGISTRYINDEX, kMakeEnvKey );
    lua_pop( L, 1 );  // the mina table
    return true;
}

void sandbox_make_env( lua_State* L, const char* id, const char* dir )
{
    lua_getfield( L, LUA_REGISTRYINDEX, kMakeEnvKey );
    lua_pushstring( L, id );
    lua_pushstring( L, dir );
    lua_call( L, 2, 1 );
}

}  // namespace mml
