// invoke.hpp - turn a pointer-to-data-member into a lua_CFunction.
//
//     template<auto PM> int wrap(lua_State*)
//
// The compiler deduces R and A... from &MinaModAPI::Whatever and this expands
// the marshalling for each

#pragma once

#include "describe.hpp"

#include "marshal.hpp"
#include <map>

namespace mml
{

// The live API pointer, set once in MinaMod_Init.
extern MinaModAPI* g_api;

int handle_eq( lua_State* L );
int handle_tostring( lua_State* L );
int handle_index( lua_State* L );

// Leaves the metatable on the stack for the caller.
template <typename T>
inline void ensure_handle_metatable( lua_State* L )
{
    if ( luaL_newmetatable( L, handle_mt_name<T>() ) )
    {
        lua_pushstring( L, handle_type_name<T>() );
        lua_setfield( L, -2, "__name" );

        lua_pushcfunction( L, handle_eq );
        lua_setfield( L, -2, "__eq" );

        lua_pushcfunction( L, handle_tostring );
        lua_setfield( L, -2, "__tostring" );

        // A function rather than a table, because `.valid` is computed.
        lua_pushcfunction( L, handle_index );
        lua_setfield( L, -2, "__index" );
    }
}

template <typename R>
inline int push_return( lua_State* L, R v, const char* fn )
{
    constexpr Kind k = classify_return_v<R>;

    // MM_StringRef points into engine memory and is NOT NUL-terminated, so it is
    // copied with an explicit length. Checked before the general POD path, which
    // would otherwise flatten it into two numbers.
    if constexpr ( std::is_same_v<R, MM_StringRef> )
    {
        if ( v.str )
            lua_pushlstring( L, v.str, v.len );
        else
            lua_pushnil( L );
        return 1;
    }
    // MM_Rtti's uint64 typeId cannot survive a Lua 5.1 number, so it is pushed as
    // its 8 raw bytes: Lua strings are binary-safe, so it compares with == and
    // round-trips into ComponentIsa without ever becoming a number.
    else if constexpr ( std::is_same_v<R, MM_Rtti> )
    {
        lua_pushlstring( L, (const char*)&v.typeId, sizeof( v.typeId ) );
        return 1;
    }
    else if constexpr ( k == Kind::Boolean )
    {
        lua_pushboolean( L, v ? 1 : 0 );
        return 1;
    }
    else if constexpr ( k == Kind::Integer )
    {
        lua_pushinteger( L, (lua_Integer)v );
        return 1;
    }
    else if constexpr ( k == Kind::Wide )
    {
        return push_wide( L, v, fn );
    }
    else if constexpr ( k == Kind::Number )
    {
        lua_pushnumber( L, (lua_Number)v );
        return 1;
    }
    else if constexpr ( k == Kind::CString )
    {
        if ( v )
            lua_pushstring( L, v );
        else
            lua_pushnil( L );
        return 1;
    }
    else if constexpr ( k == Kind::OwnedCString )
    {
        // Host-allocated; copying here and freeing immediately means the mod's
        // ownership obligation never reaches Lua.
        if ( v )
        {
            lua_pushstring( L, v );
            if ( g_api->Free ) g_api->Free( v );
        }
        else
            lua_pushnil( L );
        return 1;
    }
    else if constexpr ( k == Kind::Pod )
    {
        return push_pod<R>( L, v );
    }
    else if constexpr ( k == Kind::PodIn )
    {
        // A `const MM_Mtx*` return - CameraGetProj and friends - points INTO
        // engine memory, which the engine owns and will reuse. Copy the value out
        // now; never wrap the pointer.
        using Pointee = std::remove_const_t<std::remove_pointer_t<R>>;
        if ( !v )
        {
            lua_pushnil( L );
            return 1;
        }
        return push_pod<Pointee>( L, *v );
    }
    else if constexpr ( k == Kind::Handle )
    {
        return push_handle<std::remove_pointer_t<R>>( L, v );
    }
    else
    {
        static_assert( sizeof( R ) == 0, "no return rule for this type" );
        return 0;
    }
}

template <typename Fn>
struct Invoker;

template <typename R, typename... A>
struct Invoker<R ( * )( A... )>
{
    // The Storage objects are parameters of run(), so they live for the whole
    // call: an out-param is written through a pointer into one and read back
    // afterwards.
    static int call( lua_State* L, const char* fn, R ( *f )( A... ) ) { return run( L, fn, f, Storage<A>{}... ); }

    static int run( lua_State* L, const char* fn, R ( *f )( A... ), Storage<A>... a )
    {
        int argn = 1;

        // Comma fold, whose evaluation order is guaranteed left to right. Reading
        // out of order would pair each parameter with the wrong Lua stack slot.
        ( a.read( L, fn, argn ), ... );

        int pushed = 0;
        if constexpr ( std::is_void_v<R> )
            f( a.pass()... );
        else
            pushed = push_return<R>( L, f( a.pass()... ), fn );

        // Out-params follow the return value, in declaration order.
        ( ( pushed += a.push( L ) ), ... );
        return pushed;
    }
};

template <auto PM>
int wrap( lua_State* L )
{
    static_assert( sig_of<PM>.supported, "this MinaModAPI member has no generic binding - it needs a hand-written "
                                         "wrapper (variadic, callback parameter, or raw void*)" );

    // Each wrapper carries its own Lua name as upvalue 1, so errors can say
    // `player_set_pos: bad argument #2` rather than the `?` LuaJIT usually
    // recovers for a C function.
    const char* fn = lua_tostring( L, lua_upvalueindex( 1 ) );

    // A slot can be null even at a matching API version, and calling through it
    // would jump to garbage.
    auto f = g_api->*PM;
    if ( !f ) return luaL_error( L, "%s is not available in this game build", fn );

    return Invoker<decltype( f )>::call( L, fn, f );
}

// Returns 1 if the member has a generic binding, 0 if it needs a hand-written
// wrapper and was skipped.
template <auto PM>
inline int register_member( lua_State* L, const char* name, const std::map<std::string, std::string>& argNames )
{
    if constexpr ( sig_of<PM>.supported )
    {
        const std::string lua = to_snake_case( name );

        lua_pushstring( L, lua.c_str() );
        lua_pushcclosure( L, wrap<PM>, 1 );
        lua_setfield( L, -3, lua.c_str() );

        const auto it = argNames.find( name );
        const std::string sig = signature_of<PM>( it == argNames.end() ? std::string() : it->second );
        lua_pushlstring( L, sig.data(), sig.size() );
        lua_setfield( L, -2, lua.c_str() );
        return 1;
    }
    else
    {
        (void)L;
        (void)name;
        (void)argNames;
        return 0;
    }
}

// Leaves the function table and a parallel table of signature strings on the
// stack, and returns how many members bound.
int open_raw_api( lua_State* L );

}  // namespace mml
