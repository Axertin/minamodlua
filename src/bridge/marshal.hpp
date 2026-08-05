// marshal.hpp - moving values across the Lua boundary.

#pragma once

#include "classify.hpp"
#include "handle.hpp"
#include "pod.hpp"

#include <stdlib.h>
#include <string.h>

#include <string>

extern "C"
{
#include "lauxlib.h"
#include "lua.h"
}

namespace mml
{

// Dense runtime id per handle type, so HandleTable can reject a World* handed to
// something expecting a ycEntity*.
inline uint32_t next_type_id()
{
    static uint32_t n = 0;
    return ++n;
}

template <typename T>
inline uint32_t type_id()
{
    static const uint32_t id = next_type_id();
    return id;
}

// Registry key. Prefixed so it cannot collide with a key another embedder puts
// in the same registry. Owns its storage because type_name's view is not
// NUL-terminated at the right place.
template <typename T>
inline const char* handle_mt_name()
{
    static const std::string s = "minamodlua." + std::string( type_name<T>() );
    return s.c_str();
}

// What a mod sees: plain `ycEntity`, not the registry key. Error messages and
// tostring read as the engine's own type names.
template <typename T>
inline const char* handle_type_name()
{
    static const std::string s( type_name<T>() );
    return s.c_str();
}

// Defined in invoke.hpp. Registers the per-type metatable and leaves it on the
// stack; called once per handle type, lazily.
template <typename T>
inline void ensure_handle_metatable( lua_State* L );

// Level 1 so the message points at the mod's call site, not into the binding.
// LuaJIT often cannot recover a C function's name, so it is passed explicitly.
// luaL_error does not return, but its declaration does not say so.
[[noreturn]] inline void arg_error( lua_State* L, const char* fn, int argn, const char* expected )
{
    luaL_error( L, "%s: bad argument #%d (%s expected, got %s)", fn, argn, expected, luaL_typename( L, argn ) );
    abort();
}

// The runtime half of Kind::Wide: rejects anything a double cannot hold exactly.
template <typename T>
inline T check_wide( lua_State* L, const char* fn, int argn )
{
    const lua_Number n = luaL_checknumber( L, argn );
    const T v = (T)n;
    if ( (lua_Number)v != n )
        luaL_error( L, "%s: argument #%d does not fit a Lua number exactly; pass a name, not a raw 64-bit value", fn,
                    argn );
    return v;
}

template <typename T>
inline int push_wide( lua_State* L, T v, const char* fn )
{
    const lua_Number n = (lua_Number)v;
    if ( (T)n != v )
        luaL_error( L, "%s returned 0x%llx, which does not fit a Lua number exactly", fn, (unsigned long long)v );
    lua_pushnumber( L, n );
    return 1;
}

template <typename T>
inline int push_pod( lua_State* L, const T& v )
{
    using P = PodTraits<T>;
    const auto* f = P::at( v );
    for ( size_t i = 0; i < P::count; ++i )
        lua_pushnumber( L, (lua_Number)f[i] );
    return (int)P::count;
}

template <typename T>
inline T check_pod( lua_State* L, const char* fn, int& argn )
{
    using P = PodTraits<T>;
    T out{};
    auto* const f = P::at( out );
    for ( size_t i = 0; i < P::count; ++i )
    {
        if ( !lua_isnumber( L, argn ) ) arg_error( L, fn, argn, P::name );
        f[i] = (typename P::Element)lua_tonumber( L, argn );
        ++argn;
    }
    return out;
}

template <typename T>
inline int push_handle( lua_State* L, T* ptr )
{
    if ( !ptr )
    {
        lua_pushnil( L );  // "no such entity" is nil, not an error
        return 1;
    }

    const HandleRef ref = handles().acquire( (void*)ptr, type_id<T>() );

    auto* ud = (HandleRef*)lua_newuserdata( L, sizeof( HandleRef ) );
    *ud = ref;
    ensure_handle_metatable<T>( L );
    lua_setmetatable( L, -2 );
    return 1;
}

template <typename T>
inline T* check_handle( lua_State* L, const char* fn, int argn )
{
    if ( lua_isnoneornil( L, argn ) ) return nullptr;  // nil is a legitimate "none"

    void* raw = lua_touserdata( L, argn );
    if ( !raw || !lua_getmetatable( L, argn ) ) arg_error( L, fn, argn, handle_type_name<T>() );

    ensure_handle_metatable<T>( L );
    const bool same = lua_rawequal( L, -1, -2 );
    lua_pop( L, 2 );
    if ( !same ) arg_error( L, fn, argn, handle_type_name<T>() );

    const HandleRef ref = *(const HandleRef*)raw;
    T* const p = (T*)handles().resolve( ref, type_id<T>() );
    if ( !p ) luaL_error( L, "%s: argument #%d is a stale %s; fetch it again", fn, argn, handle_type_name<T>() );
    return p;
}

// Holds one parameter across the call. Out-params live here too: the pointer the
// engine writes through points into this object, and its value is pushed as an
// extra Lua return afterwards. Without ffi Lua cannot allocate a buffer, so
// synthesising it on this side is required.
template <typename T>
struct Storage
{
    static constexpr Kind kind = classify_v<T>;
    static_assert( kind != Kind::Unsupported, "no marshalling rule for this parameter type" );

    // PodIn and PodOut hold the pointee; everything else holds the value.
    using Held = std::conditional_t<kind == Kind::PodOut || kind == Kind::PodIn,
                                    std::remove_const_t<std::remove_pointer_t<T>>, T>;
    Held held{};

    void read( lua_State* L, const char* fn, int& argn )
    {
        // Read side of the 8-byte-string encoding in invoke.hpp. Ahead of the
        // general POD path, which would flatten it into numbers.
        if constexpr ( std::is_same_v<Held, MM_Rtti> )
        {
            size_t len = 0;
            const char* s = lua_tolstring( L, argn, &len );
            if ( !s || len != sizeof( uint64_t ) )
                arg_error( L, fn, argn, "type id (8-byte string from ComponentGetType)" );
            memcpy( &held.typeId, s, sizeof( uint64_t ) );
            ++argn;
        }
        // MM_StringRef is ptr+len. Pointing it straight at the Lua string is
        // safe only because that string is an argument on the stack, so it
        // cannot be collected before the call returns.
        else if constexpr ( std::is_same_v<Held, MM_StringRef> )
        {
            size_t len = 0;
            const char* s = lua_tolstring( L, argn, &len );
            if ( !s ) arg_error( L, fn, argn, "string" );
            held.str = s;
            held.len = len;
            ++argn;
        }
        else if constexpr ( kind == Kind::PodOut )
        {
            // Consumes no Lua argument; produces one (or more) on the way out.
        }
        else if constexpr ( kind == Kind::Boolean )
        {
            if ( !lua_isboolean( L, argn ) ) arg_error( L, fn, argn, "boolean" );
            held = (Held)lua_toboolean( L, argn++ );
        }
        else if constexpr ( kind == Kind::Number )
        {
            if ( !lua_isnumber( L, argn ) ) arg_error( L, fn, argn, "number" );
            held = (Held)lua_tonumber( L, argn++ );
        }
        else if constexpr ( kind == Kind::Integer )
        {
            if ( !lua_isnumber( L, argn ) ) arg_error( L, fn, argn, "number" );
            held = (Held)lua_tointeger( L, argn++ );
        }
        else if constexpr ( kind == Kind::Wide )
        {
            held = check_wide<Held>( L, fn, argn++ );
        }
        else if constexpr ( kind == Kind::CString )
        {
            if ( !lua_isstring( L, argn ) ) arg_error( L, fn, argn, "string" );
            held = lua_tostring( L, argn++ );
        }
        else if constexpr ( kind == Kind::Pod || kind == Kind::PodIn )
        {
            held = check_pod<Held>( L, fn, argn );
        }
        else if constexpr ( kind == Kind::Handle )
        {
            held = check_handle<std::remove_pointer_t<T>>( L, fn, argn++ );
        }
        else
        {
            static_assert( sizeof( T ) == 0, "no read rule for this parameter kind" );
        }
    }

    // What actually goes to the engine.
    T pass()
    {
        if constexpr ( kind == Kind::PodOut || kind == Kind::PodIn )
            return &held;
        else
            return held;
    }

    // Out-params become extra Lua return values, in declaration order.
    int push( lua_State* L )
    {
        if constexpr ( kind == Kind::PodOut )
        {
            using Pointee = std::remove_pointer_t<T>;
            if constexpr ( std::is_class_v<Pointee> )
            {
                return push_pod<Pointee>( L, held );
            }
            else
            {
                lua_pushnumber( L, (lua_Number)held );
                return 1;
            }
        }
        else
        {
            (void)L;
            return 0;
        }
    }
};

}  // namespace mml
