// describe.hpp - renders a bound signature as the Lua types a mod actually
// passes and receives.
//
// Used by both the generated reference and the runtime `mina.signatures` table,
// so the two cannot disagree. Derived from the same classification the wrappers
// use, so neither can drift from what the binding really accepts.

#pragma once

#include "classify.hpp"
#include "pod.hpp"

#include <cctype>
#include <string>
#include <vector>

namespace mml
{

namespace detail
{

template <typename...>
inline constexpr bool always_false = false;

inline void join( std::string& s, const std::string& part )
{
    if ( part.empty() ) return;
    if ( !s.empty() ) s += ", ";
    s += part;
}

// "x|y" -> {"x", "y"}; an empty string is no names at all.
inline std::vector<std::string> split_names( const std::string& packed )
{
    std::vector<std::string> out;
    if ( packed.empty() ) return out;
    size_t b = 0;
    for ( size_t i = 0; i <= packed.size(); ++i )
        if ( i == packed.size() || packed[i] == '|' )
        {
            out.push_back( packed.substr( b, i - b ) );
            b = i + 1;
        }
    return out;
}

inline std::string label( const std::vector<std::string>& names, size_t i, const std::string& type )
{
    if ( i < names.size() && !names[i].empty() ) return names[i] + ": " + type;
    return type;
}

}  // namespace detail

// The Lua type(s) one value occupies. A POD spreads across several; a scalar
// out-param pointer (PlayerGetPos takes two float*) is a single number.
template <typename T, Kind K>
inline std::string describe_as()
{
    using Bare = std::remove_const_t<std::remove_pointer_t<T>>;

    if constexpr ( std::is_same_v<Bare, MM_Rtti> )
        return "typeid";
    else if constexpr ( std::is_same_v<Bare, MM_StringRef> )
        return "string";
    else if constexpr ( K == Kind::Boolean )
        return "boolean";
    else if constexpr ( K == Kind::Integer || K == Kind::Number )
        return "number";
    // Spelled out because the wrapper *errors* on any 64-bit value a double
    // cannot hold exactly, so "number" alone would hide the failure mode.
    else if constexpr ( K == Kind::Wide )
        return "number(64-bit)";
    else if constexpr ( K == Kind::CString || K == Kind::OwnedCString )
        return "string";
    else if constexpr ( K == Kind::Handle )
        return std::string( type_name<Bare>() );
    else if constexpr ( K == Kind::Pod || K == Kind::PodIn || K == Kind::PodOut )
    {
        if constexpr ( std::is_class_v<Bare> )
            return std::string( type_name<Bare>() ) + "(" + std::to_string( PodTraits<Bare>::count ) + " numbers)";
        else
            return "number";
    }
    else if constexpr ( K == Kind::Void )
        return "";
    // Not a fallback: a Kind with no rule must break the build, because
    // join() drops an empty part and would silently shorten the signature
    // rather than mislabel it - a wrong arity in the docs with a green build.
    else
    {
        static_assert( detail::always_false<T>, "describe_as: no description rule for this Kind" );
        return "";
    }
}

template <typename T>
inline std::string describe_type()
{
    return describe_as<T, classify_v<T>>();
}

// `char*` means an owned string when returned, so return position classifies
// differently from parameter position.
template <typename R>
inline std::string describe_return()
{
    return describe_as<R, classify_return_v<R>>();
}

template <typename Fn>
struct describe_sig;

template <typename R, typename... A>
struct describe_sig<R ( * )( A... )>
{
    // Names only apply when there is one per C parameter; anything else means the
    // header parse disagreed with the compiler, and bare types beat wrong labels.
    static bool usable( const std::vector<std::string>& names ) { return names.size() == sizeof...( A ); }

    // Out-params take no argument and come back as extra return values, so each
    // parameter lands in exactly one of these two.
    static std::string args( const std::vector<std::string>& names )
    {
        std::string s;
        size_t i = 0;
        ( ( classify_v<A> == Kind::PodOut ? (void)++i
                                          : (void)detail::join( s, detail::label( names, i++, describe_type<A>() ) ) ),
          ... );
        return s;
    }

    static std::string returns( const std::vector<std::string>& names )
    {
        std::string s;
        if constexpr ( !std::is_void_v<R> ) detail::join( s, describe_return<R>() );
        size_t i = 0;
        ( ( classify_v<A> == Kind::PodOut ? (void)detail::join( s, detail::label( names, i++, describe_type<A>() ) )
                                          : (void)++i ),
          ... );
        return s;
    }
};

// PascalCase to snake_case, so grepping the SDK's own naming still works. A
// capital starts a word unless it is inside an acronym: HUDRootEntity ->
// hud_root_entity.
//
// Not injective, despite looking it: names differing only in case collide
// (FooGetHP and FooGetHp both give foo_get_hp), and an underscore already in
// the C name yields a doubled one (PlayerGetWeapon_ItemType ->
// player_get_weapon__item_type). Neither occurs in the current pin. apidoc
// fails the build on a collision rather than letting one binding shadow
// another, which is the guarantee that actually matters here.
inline std::string to_snake_case( const std::string& pascal )
{
    std::string out;
    for ( size_t i = 0; i < pascal.size(); ++i )
    {
        const unsigned char c = (unsigned char)pascal[i];
        if ( i && std::isupper( c ) &&
             ( !std::isupper( (unsigned char)pascal[i - 1] ) ||
               ( i + 1 < pascal.size() && std::islower( (unsigned char)pascal[i + 1] ) ) ) )
            out += '_';
        out += (char)std::tolower( c );
    }
    return out;
}

// e.g. "(ycEntity, number) -> number, number, number"
template <auto PM>
inline std::string signature_of( const std::string& packedNames = {} )
{
    using Sig = describe_sig<member_type_t<decltype( PM )>>;

    std::vector<std::string> names = detail::split_names( packedNames );
    if ( !Sig::usable( names ) ) names.clear();

    const std::string r = Sig::returns( names );
    return "(" + Sig::args( names ) + ")" + ( r.empty() ? "" : " -> " + r );
}

}  // namespace mml
