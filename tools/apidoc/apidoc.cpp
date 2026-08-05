// Writes docs/api-reference.md: what every SDK function looks like from Lua.
//
// Built from the same classification the wrappers use, so a signature here
// cannot disagree with what the binding accepts. Prose comes from upstream's own
// trailing comments in MinaModAPI.h, which cannot go stale against the pin.

#include "MinaModAPI.h"
#include "classify.hpp"
#include "describe.hpp"
#include "pod.hpp"

#include <algorithm>
#include <cstdio>
#include <map>
#include <string>
#include <type_traits>
#include <vector>

namespace
{

std::map<std::string, std::string> arg_names()
{
    std::map<std::string, std::string> m;
#define MM_ARGS( n, packed ) m[#n] = packed;
#include "api_args.inc"
#undef MM_ARGS
    return m;
}

std::map<std::string, std::string> upstream_comments()
{
    std::map<std::string, std::string> m;
#define MM_DOC( n, text ) m[#n] = text;
#include "api_comments.inc"
#undef MM_DOC
    return m;
}

// A Markdown table cell cannot contain a raw pipe or a newline. Nothing in the
// current pin does, but an upstream comment is one commit away from breaking
// every row after it.
std::string cell( const std::string& s )
{
    std::string out;
    for ( const char c : s )
    {
        if ( c == '|' )
            out += "\\|";
        else if ( c == '\n' || c == '\r' )
            out += ' ';
        else
            out += c;
    }
    return out;
}

struct Entry
{
    std::string name;
    std::string lua;
    std::string signature;
    std::string why;          // empty when bound
    bool ownsString = false;  // returns a host-allocated string
};

std::vector<Entry> collect()
{
    std::vector<Entry> all;
    const auto argn = arg_names();

#define MM_FN( n )                                                                                                     \
    {                                                                                                                  \
        constexpr mml::SigInfo s = mml::sig_of<&MinaModAPI::n>;                                                        \
        Entry e;                                                                                                       \
        e.name = #n;                                                                                                   \
        e.lua = mml::to_snake_case( #n );                                                                              \
        e.ownsString = s.owns_string;                                                                                  \
        if constexpr ( s.supported )                                                                                   \
            e.signature = mml::signature_of<&MinaModAPI::n>( argn.count( #n ) ? argn.at( #n ) : "" );                  \
        else                                                                                                           \
            e.why = s.variadic       ? "variadic; formatting is done Lua-side"                                         \
                    : s.has_callback ? "takes a callback; routed through mina.on_event"                                \
                    : s.has_opaque   ? "raw void*; needs a hand-written wrapper"                                       \
                                     : "no generic mapping";                                                             \
        all.push_back( e );                                                                                            \
    }
#include "api_list.inc"
#undef MM_FN

    return all;
}

template <typename T>
const char* element_name()
{
    if constexpr ( std::is_same_v<typename mml::PodTraits<T>::Element, float> )
        return "float";
    else
        return "integer";
}

// Field order is the one thing a mod author cannot recover from the signature:
// `MM_AABB(6 numbers)` says how many to pass, never which is which.
void write_value_types( FILE* f )
{
    fprintf( f, "\n## Value types\n\nA struct is not a table: it spreads across the listed number of Lua values, in "
                "this order.\n\n"
                "| Type | Numbers | Element | Order |\n| --- | --- | --- | --- |\n" );
#define MML_POD_ROW( TYPE, COUNT, LAYOUT )                                                                             \
    fprintf( f, "| `%s` | %zu | %s | %s |\n", mml::PodTraits<TYPE>::name, mml::PodTraits<TYPE>::count,                 \
             element_name<TYPE>(), cell( mml::PodTraits<TYPE>::layout ).c_str() );
    MML_POD_TYPES( MML_POD_ROW )
#undef MML_POD_ROW
}

}  // namespace

int main( int argc, char** argv )
{
    if ( argc < 2 )
    {
        fprintf( stderr, "usage: apidoc <output.md>\n" );
        return 2;
    }

    std::vector<Entry> all = collect();
    const auto comments = upstream_comments();

    // Tiebreak on the C name: to_snake_case is not injective, and std::sort is
    // not stable, so an equal key would make row order vary between runs and
    // the CI staleness check flap.
    std::sort( all.begin(), all.end(),
               []( const Entry& a, const Entry& b ) { return a.lua != b.lua ? a.lua < b.lua : a.name < b.name; } );

    // register_member does an unconditional lua_setfield, so two members that
    // snake_case alike would leave one silently shadowed while the host still
    // reported both as bound. Fail the build instead - this is the guarantee
    // that lets to_snake_case stay a simple transform.
    size_t collisions = 0;
    for ( size_t i = 1; i < all.size(); ++i )
    {
        if ( all[i].lua != all[i - 1].lua || !all[i].why.empty() || !all[i - 1].why.empty() ) continue;
        fprintf( stderr, "apidoc: name collision: %s and %s both map to %s\n", all[i - 1].name.c_str(),
                 all[i].name.c_str(), all[i].lua.c_str() );
        ++collisions;
    }
    if ( collisions ) return 1;

    // Binary mode: the reference is committed and diffed by CI, so it must be
    // LF on every platform or a Windows regeneration rewrites all of it.
    FILE* f = fopen( argv[1], "wb" );
    if ( !f )
    {
        fprintf( stderr, "apidoc: cannot write %s\n", argv[1] );
        return 1;
    }

    size_t bound = 0;
    for ( const Entry& e : all )
        if ( e.why.empty() ) ++bound;

    fprintf( f, "# `mina.raw` reference\n\n" );
    fprintf( f, "<!-- Generated by tools/apidoc. Run `cmake --build build --target apidoc`. -->\n\n" );
    fprintf( f, "Every MinaModAPI function as it appears from Lua, under `mina.raw`. Signatures are the "
                "Lua types you pass and receive, not the C ones: out-parameters come back as extra return "
                "values, and a struct spreads across several numbers. The same strings are available at "
                "runtime in `mina.signatures`.\n\n" );

    fprintf( f, "Conventions that apply to every function here:\n\n"
                "- A wrong argument type raises a Lua error naming the function and the argument position; "
                "it never reaches the engine.\n"
                "- A member the running game build does not export raises `<name> is not available in this "
                "game build`. Check before calling if you support more than one build.\n"
                "- Handles are opaque userdata, not tables. `nil` is accepted wherever a handle is taken and "
                "passed through as null, and returned wherever the engine gives back null - so a handle "
                "result is always worth testing. Use `.valid` to test one you have held on to.\n"
                "- `number(64-bit)` values are checked, not truncated: a value a Lua number cannot hold "
                "exactly (greater than 2^53) raises an error.\n"
                "- A struct parameter is exactly one value's worth of numbers. Where a function also takes a "
                "count, only 1 is meaningful from Lua.\n\n" );

    fprintf( f, "## Bound\n\n| Lua | C | Signature | Description |\n| --- | --- | --- | --- |\n" );
    for ( const Entry& e : all )
    {
        if ( !e.why.empty() ) continue;
        auto it = comments.find( e.name );
        std::string doc = it == comments.end() ? std::string() : cell( it->second );
        if ( e.ownsString )
        {
            if ( !doc.empty() ) doc += ' ';
            doc += "(The binding copies the string and frees the original; do not free it yourself.)";
        }
        fprintf( f, "| `%s` | `%s` | `%s` | %s |\n", e.lua.c_str(), e.name.c_str(), cell( e.signature ).c_str(),
                 doc.c_str() );
    }

    write_value_types( f );

    fprintf( f, "\n## Not bound\n\nReachable only from C++.\n\n"
                "| C name | Why |\n| --- | --- |\n" );
    for ( const Entry& e : all )
    {
        if ( e.why.empty() ) continue;
        fprintf( f, "| `%s` | %s |\n", e.name.c_str(), cell( e.why ).c_str() );
    }

    // A short write leaves a truncated reference that CI reports as "stale",
    // which is a confusing way to learn the disk filled up.
    if ( ferror( f ) )
    {
        fprintf( stderr, "apidoc: write failed on %s\n", argv[1] );
        fclose( f );
        return 1;
    }
    if ( fclose( f ) != 0 )
    {
        fprintf( stderr, "apidoc: close failed on %s\n", argv[1] );
        return 1;
    }

    printf( "apidoc: %zu bound, %zu not, written to %s\n", bound, all.size() - bound, argv[1] );
    return 0;
}
