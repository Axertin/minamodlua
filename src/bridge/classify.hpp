// classify.hpp - decide, at compile time, how each MinaModAPI parameter and
// return type crosses the Lua boundary.
//
// Types are deduced from a pointer-to-data-member rather than parsed out of the
// header, so a shape no rule covers is a compile error naming the exact member.
// No Lua dependency here on purpose: classification is pure type introspection.

#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string_view>
#include <type_traits>

namespace mml
{

// Engine handles (ycEntity, World, SpawnPoint, ...) are forward-declared and
// never defined, so sizeof fails in a SFINAE context. Value types are complete.
// That one bit separates "opaque pointer the mod passes back and forth" from
// "pointer to a POD the mod reads or writes".
template <typename T, typename = void>
struct is_complete : std::false_type
{
};

template <typename T>
struct is_complete<T, std::void_t<decltype( sizeof( T ) )>> : std::true_type
{
};

template <typename T>
inline constexpr bool is_complete_v = is_complete<T>::value;

// Handle types are incomplete, so there is nothing to hang a trait on and no way
// to name one without a macro list that would drift from the header.
// __PRETTY_FUNCTION__ / __FUNCSIG__ work regardless, because they only name the
// template argument.
template <typename T>
constexpr std::string_view type_name()
{
#if defined( __clang__ ) || defined( __GNUC__ )
    constexpr std::string_view p = __PRETTY_FUNCTION__;
    constexpr size_t b = p.find( "T = " ) + 4;
    constexpr size_t e = p.find_first_of( ";]", b );
    return p.substr( b, e - b );
#elif defined( _MSC_VER )
    constexpr std::string_view p = __FUNCSIG__;
    constexpr size_t b = p.find( "type_name<" ) + 10;
    constexpr size_t e = p.rfind( ">(" );
    std::string_view s = p.substr( b, e - b );
    // MSVC spells it "struct ycEntity" / "class World".
    if ( s.rfind( "struct ", 0 ) == 0 ) s.remove_prefix( 7 );
    if ( s.rfind( "class ", 0 ) == 0 ) s.remove_prefix( 6 );
    return s;
#else
    return "?";
#endif
}

enum class Kind
{
    Unsupported,  // no rule matches - a build error naming the function
    Void,         // return only
    Boolean,
    Integer,  // fits a Lua 5.1 double exactly (<= 32 bits)
    // 64-bit. On x86-64 `size_t` IS `uint64_t`, so no compile-time test can tell
    // EntityGetChildren's element count from Hash64's hash. Both bind, and the
    // wrapper rejects at runtime any value a double cannot hold exactly.
    Wide,
    Number,   // float / double
    CString,  // const char*
    // char* returned: host-allocated, the mod owns it and must Free(). Only ever
    // valid in return position. The wrapper pushes a copy and Frees the
    // original, so the obligation never reaches Lua.
    OwnedCString,
    Pod,       // small POD by value (MM_Vec3, MM_Rtti, ...)
    PodIn,     // const POD* - an input the mod supplies
    PodOut,    // non-const POD* - an out-param, returned as an extra value
    Handle,    // opaque engine pointer - userdata
    Callback,  // function pointer - needs a trampoline, always hand-written
    Opaque,    // void* - context-dependent, hand-written
};

template <typename T>
struct classify
{
    static constexpr Kind value = std::is_same_v<T, bool>       ? Kind::Boolean
                                  : std::is_floating_point_v<T> ? Kind::Number
                                  // 64-bit integers may exceed a Lua 5.1 double's 53-bit mantissa.
                                  : ( std::is_integral_v<T> && sizeof( T ) == 8 )  ? Kind::Wide
                                  : ( std::is_integral_v<T> || std::is_enum_v<T> ) ? Kind::Integer
                                  : std::is_class_v<T>                             ? Kind::Pod
                                                                                   : Kind::Unsupported;
};

// Specialised so the primary never instantiates sizeof(void).
template <>
struct classify<void>
{
    static constexpr Kind value = Kind::Void;
};

// Pointers to complete non-char types: input if const, out-param if not.
// Pointers to incomplete types are engine handles.
template <typename T>
struct classify<T*>
{
    static constexpr Kind value = std::is_function_v<T> ? Kind::Callback
                                  // A pointer type is complete even when it points at an incomplete type,
                                  // so without this ycComponent** would classify as PodOut and marshal a
                                  // pointer as a number. Only EntityGetChildren has this shape.
                                  : std::is_pointer_v<std::remove_const_t<T>> ? Kind::Unsupported
                                  : !is_complete_v<T>                         ? Kind::Handle
                                  // `const uint8_t*` is a byte buffer whose length is a separate parameter
                                  // (PaletteWrite and friends); flattening it would read one byte and stop.
                                  // Non-const scalar pointers are genuine out-params - PlayerGetPos takes
                                  // two float* - and stay generic.
                                  : ( std::is_const_v<T> && !std::is_class_v<std::remove_const_t<T>> )
                                      ? Kind::Unsupported
                                  : std::is_const_v<T> ? Kind::PodIn
                                                       : Kind::PodOut;
};

template <>
struct classify<const char*>
{
    static constexpr Kind value = Kind::CString;
};

template <>
struct classify<void*>
{
    static constexpr Kind value = Kind::Opaque;
};

template <>
struct classify<const void*>
{
    static constexpr Kind value = Kind::Opaque;
};

template <typename T>
inline constexpr Kind classify_v = classify<T>::value;

// A bare `char*` is the one shape whose meaning depends on position: an
// out-buffer as a parameter, an owned string as a return.
template <typename R>
inline constexpr Kind classify_return_v = std::is_same_v<R, char*> ? Kind::OwnedCString : classify_v<R>;

// Peel MinaModAPI:: off a pointer-to-data-member to get the function-pointer type.
template <typename T>
struct member_type;

template <typename C, typename M>
struct member_type<M C::*>
{
    using type = M;
};

template <typename T>
using member_type_t = typename member_type<T>::type;

// The flags beyond `supported` are not consumed by the binding; they exist so
// tools can report *why* a member needs a hand-written wrapper.
struct SigInfo
{
    bool variadic = false;      // printf-style: Log, Assert
    bool has_callback = false;  // needs a trampoline
    bool has_wide = false;      // 64-bit crosses the boundary
    bool has_out = false;       // out-params become extra returns
    bool has_opaque = false;    // raw void*, in either direction
    bool owns_string = false;   // returns char*: wrapper copies and Frees it
    bool supported = false;     // fully handled by the generic path
};

// Undefined primary: anything that is not a function pointer is a compile error.
template <typename Fn>
struct signature;

template <typename R, typename... A>
struct signature<R ( * )( A... )>
{
    static constexpr SigInfo compute()
    {
        SigInfo s;
        const Kind ret = classify_return_v<R>;
        const Kind args[] = { Kind::Void, classify_v<A>... };  // leading pad keeps [] legal at arity 0

        // A PodOut return is a bare `char*` (owned string), a void* return is
        // Alloc, and neither has a generic meaning worth inventing.
        bool ok = ( ret != Kind::Unsupported ) && ( ret != Kind::PodOut ) && ( ret != Kind::Callback ) &&
                  ( ret != Kind::Opaque );
        if ( ret == Kind::Wide ) s.has_wide = true;
        // Alloc and WeakPtrGet are refused for their void* *return*; without
        // this they report "no generic mapping" instead of the real reason.
        if ( ret == Kind::Opaque ) s.has_opaque = true;
        if ( ret == Kind::OwnedCString ) s.owns_string = true;

        for ( size_t i = 1; i < sizeof( args ) / sizeof( args[0] ); ++i )
        {
            const Kind a = args[i];
            s.has_callback = s.has_callback || a == Kind::Callback;
            s.has_opaque = s.has_opaque || a == Kind::Opaque;
            s.has_out = s.has_out || a == Kind::PodOut;
            s.has_wide = s.has_wide || a == Kind::Wide;  // guarded at runtime, not refused
            if ( a == Kind::Unsupported || a == Kind::Callback || a == Kind::Opaque ) ok = false;
        }

        s.supported = ok;
        return s;
    }

    static constexpr SigInfo info = compute();
};

// C variadics (Log, Assert), matched separately so they are never mistaken for
// the fixed-arity form.
template <typename R, typename... A>
struct signature<R ( * )( A..., ... )>
{
    static constexpr SigInfo info = { /*variadic=*/true };
};

template <auto PM>
inline constexpr SigInfo sig_of = signature<member_type_t<decltype( PM )>>::info;

}  // namespace mml
