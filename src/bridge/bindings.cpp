#include "invoke.hpp"

#include <string.h>

namespace mml
{

MinaModAPI* g_api = nullptr;

// The handle metamethods take no type parameter: everything they need is in the
// HandleRef, and Lua 5.1 only dispatches __eq when both operands share a
// metatable. They still guard against a non-handle argument, because a mod can
// pull them out of the metatable and call them on anything.

int handle_eq( lua_State* L )
{
    const auto* a = (const HandleRef*)lua_touserdata( L, 1 );
    const auto* b = (const HandleRef*)lua_touserdata( L, 2 );
    lua_pushboolean( L, a && b && a->slot == b->slot && a->generation == b->generation );
    return 1;
}

int handle_tostring( lua_State* L )
{
    const auto* h = (const HandleRef*)lua_touserdata( L, 1 );

    const char* name = "handle";
    if ( lua_getmetatable( L, 1 ) )
    {
        lua_getfield( L, -1, "__name" );
        if ( lua_isstring( L, -1 ) ) name = lua_tostring( L, -1 );
    }

    if ( !h )
        lua_pushfstring( L, "%s(?)", name );
    else
        lua_pushfstring( L, "%s(%d:%d)%s", name, (int)h->slot, (int)h->generation,
                         handles().is_live( *h ) ? "" : " [stale]" );
    return 1;
}

int handle_index( lua_State* L )
{
    const auto* h = (const HandleRef*)lua_touserdata( L, 1 );
    const char* key = lua_tostring( L, 2 );

    // `.valid` is the sanctioned way to test a handle, rather than calling
    // something and catching the stale-handle error.
    if ( key && strcmp( key, "valid" ) == 0 )
    {
        lua_pushboolean( L, h && handles().is_live( *h ) );
        return 1;
    }

    // Methods the sugar layer installs live on the metatable itself.
    if ( !lua_getmetatable( L, 1 ) )
    {
        lua_pushnil( L );
        return 1;
    }
    lua_pushvalue( L, 2 );
    lua_rawget( L, -2 );
    return 1;
}

int open_raw_api( lua_State* L )
{
    int bound = 0;
    lua_newtable( L );

#define MM_FN( name ) bound += register_member<&MinaModAPI::name>( L, #name );
#include "api_list.inc"
#undef MM_FN

    return bound;
}

}  // namespace mml
