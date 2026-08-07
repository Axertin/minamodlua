#pragma once

#include "eventdefs.hpp"
// marshal.hpp declares push_handle<T>/check_handle<T> but only declares
// ensure_handle_metatable<T> - its body lives in invoke.hpp. A TU that
// instantiates push_handle<T>/check_handle<T> (below) without also seeing that
// body emits a call to an external symbol that nothing else in the program is
// guaranteed to define: the old switch-based events.cpp got away with this
// only because it happened to call push_handle<World> directly in a TU that
// also included invoke.hpp. Pull it in explicitly rather than rely on that
// coincidence again.
#include "invoke.hpp"
// PodTraits: push_ptr_comp/read_ptr_comp reach a component through it rather
// than casting the pointer to float* themselves. Reached transitively via
// invoke.hpp -> marshal.hpp today; named here because this header uses it
// directly.
#include "pod.hpp"

#include <limits>

class World;

namespace mml
{

// Builds the event table and leaves it on the stack. Returns false if the
// context failed its layout check, in which case nothing is pushed.
bool push_event( lua_State* L, const EventDef& def, const void* ctx, World** outWorld );

// Copies writable fields from the event table at `eventIndex` back into `ctx`.
void read_back( lua_State* L, int eventIndex, const EventDef& def, void* ctx );

// The same pointer-plausibility test push_event applies to every pointer-bearing
// descriptor row (null, or above the first page and correctly aligned), exposed
// for the pointers no row covers: the input contexts hand their arrays to an
// installer instead, so eventdefs.cpp's checks for them call this directly.
// `align` of 0 or 1 means "alignment unknowable", i.e. floor check only.
bool plausible_context_pointer( const void* p, uint8_t align );

// mod_handled is preventDefault, not stopPropagation: any handler may claim
// it, the claim is sticky (a later handler assigning false must not erase an
// earlier one's claim), and later handlers keep running and can observe it.
// It cannot be read once at the end, because the sticky rule requires
// latching after every write, not just the last one.
//
// Reads e.mod_handled off the table at `eventIndex`, ORs it into `handled`,
// then writes `handled` back so any later read of the field - by another
// handler, or by read_back - sees the accumulated claim rather than whatever
// the most recent assignment happened to be.
//
// Shared by events.cpp's protected_dispatch and tests/events_test.cpp's
// dispatch_chain, so the sticky rule is one piece of code exercised by the
// test suite, not a real copy plus a test copy that can silently drift apart.
void latch_handled( lua_State* L, int eventIndex, bool& handled );

// --- field template definitions ---------------------------------------------
//
// Declared in eventdefs.hpp (so a FIELD_* row can take their address without
// including this header) but defined here. eventdefs.cpp names a fresh (kind,
// type) instantiation every time it adds a row, and it already includes this
// header; a definition sitting only in eventctx.cpp - one translation unit
// over - would never be visible at that point of use; the compiler emits a
// call to an external symbol and the link fails, because nothing anywhere
// instantiates that exact specialization. Keeping the bodies here means a new
// FIELD_* row is enough on its own - no second file to remember to touch.

namespace detail
{
// The context is a byte blob at dispatch time; every accessor works from its
// base plus a recorded offsetof.
template <typename T>
T* at( void* ctx, uint16_t off )
{
    return (T*)( (unsigned char*)ctx + off );
}
template <typename T>
const T* at( const void* ctx, uint16_t off )
{
    return (const T*)( (const unsigned char*)ctx + off );
}

inline void push_scalar( lua_State* L, bool v ) { lua_pushboolean( L, v ); }
inline void push_scalar( lua_State* L, float v ) { lua_pushnumber( L, v ); }
template <typename T>
void push_scalar( lua_State* L, T v )
{
    lua_pushnumber( L, (lua_Number)v );
}

// Every value Lua hands back is a lua_Number (a double). Converting one that
// does not fit the destination type is *undefined behaviour* in C++ - not a
// wrap, not a clamp - for both the floating-to-integral case (`e.cheat_flags =
// -1` on a uint32_t, `e.left_stick_x = 99999` on an int16_t) and the
// floating-to-floating one (`e.scroll_y = 1e300` on a float). All of those are
// reachable from ordinary mod Lua, so the range has to be tested before the
// cast, not assumed.
//
// lowest()/max() are exact in a double for every type used here (nothing wider
// than 32 bits reaches this path), so the comparison is exact rather than
// merely close. NaN fails both comparisons and is rejected with the rest.
//
// That exactness is only true up to 32 bits: a double's 53-bit significand
// represents every int32_t/uint32_t bound exactly, but not every int64_t/
// uint64_t one - (double)UINT64_MAX rounds up past the true maximum, which
// would silently admit an out-of-range value through this comparison. No such
// row exists today (see the comment above), so this is enforced rather than
// merely documented: if a 64-bit row is ever added, in_range needs a real
// integer-domain range check instead of this cast-to-double comparison.
template <typename T>
bool in_range( lua_Number n )
{
    static_assert( sizeof( T ) <= 4,
                   "in_range<T> compares via double, which is only exact up to 32-bit integer types; a 64-bit row "
                   "needs its own integer-domain range check, not this function" );
    return n >= (lua_Number)std::numeric_limits<T>::lowest() && n <= (lua_Number)std::numeric_limits<T>::max();
}

// Assigns only if the value fits; an out-of-range assignment leaves the field
// exactly as the engine handed it over, which is the same "silently ignored"
// treatment a non-number already gets (see read_ok).
inline void assign_scalar( lua_State* L, int idx, bool* p ) { *p = lua_toboolean( L, idx ) != 0; }
template <typename T>
void assign_scalar( lua_State* L, int idx, T* p )
{
    const lua_Number n = lua_tonumber( L, idx );
    if ( in_range<T>( n ) ) *p = (T)n;
}

// Whether read_ptr should write at all. bool has no prior behaviour to match,
// so any non-nil value is accepted and read via Lua truthiness. Every numeric T
// matches the old switch-based code, which only ever wrote from an
// `lua_isnumber` value (via lua_tointeger) and silently ignored anything else -
// including a bare `lua_isnil` check, that old code would also have coerced a
// string, a table or `false` into a number and written it.
inline bool read_ok( lua_State* L, int idx, bool* ) { return !lua_isnil( L, idx ); }
template <typename T>
bool read_ok( lua_State* L, int idx, T* )
{
    return lua_isnumber( L, idx ) != 0;
}
}  // namespace detail

template <typename T>
void push_ptr( lua_State* L, const void* ctx, uint16_t off )
{
    T* const p = *detail::at<T*>( ctx, off );
    if ( p )
        detail::push_scalar( L, *p );
    else
        lua_pushnil( L );
}

template <typename T>
void read_ptr( lua_State* L, int idx, void* ctx, uint16_t off )
{
    T* const p = *detail::at<T*>( ctx, off );
    if ( !p || !detail::read_ok( L, idx, (T*)nullptr ) ) return;
    detail::assign_scalar( L, idx, p );
}

template <typename S, size_t Comp>
void push_ptr_comp( lua_State* L, const void* ctx, uint16_t off )
{
    static_assert( Comp < PodTraits<S>::count, "component index is past the end of this POD" );
    const S* const p = *detail::at<const S*>( ctx, off );
    if ( p )
        detail::push_scalar( L, PodTraits<S>::at( *p )[Comp] );
    else
        lua_pushnil( L );
}

template <typename S, size_t Comp>
void read_ptr_comp( lua_State* L, int idx, void* ctx, uint16_t off )
{
    static_assert( Comp < PodTraits<S>::count, "component index is past the end of this POD" );
    S* const p = *detail::at<S*>( ctx, off );
    if ( !p || !lua_isnumber( L, idx ) ) return;
    detail::assign_scalar( L, idx, &PodTraits<S>::at( *p )[Comp] );
}

template <typename T>
void push_ptr_handle( lua_State* L, const void* ctx, uint16_t off )
{
    T** const pp = *detail::at<T**>( ctx, off );
    push_handle<T>( L, pp ? *pp : nullptr );
}

template <typename T>
void read_ptr_handle( lua_State* L, int idx, void* ctx, uint16_t off )
{
    T** const pp = *detail::at<T**>( ctx, off );
    if ( !pp ) return;
    if ( lua_isnil( L, idx ) )
    {
        *pp = nullptr;
        return;
    }
    // check_handle raises on a bad value, and lua_isuserdata alone accepts any
    // full/light userdata - including a handle of the wrong type - so it would
    // reach check_handle and raise. Read-back runs after every handler has
    // finished, and dispatch() (events.cpp) swallows a raised error without
    // logging it, so that would silently skip every field after this one. Do
    // the same metatable check that check_handle does, but never raise: on any
    // mismatch (wrong type, not a handle at all, or a stale one) leave the
    // field untouched instead.
    if ( !lua_isuserdata( L, idx ) || !lua_getmetatable( L, idx ) ) return;
    ensure_handle_metatable<T>( L );
    const bool sameType = lua_rawequal( L, -1, -2 );
    lua_pop( L, 2 );
    if ( !sameType ) return;

    const HandleRef ref = *(const HandleRef*)lua_touserdata( L, idx );
    T* const resolved = (T*)handles().resolve( ref, type_id<T>() );
    if ( !resolved ) return;  // stale handle - ignore rather than raise
    *pp = resolved;
}

template <typename T>
void push_value( lua_State* L, const void* ctx, uint16_t off )
{
    detail::push_scalar( L, *detail::at<T>( ctx, off ) );
}

template <typename T>
void push_handle_field( lua_State* L, const void* ctx, uint16_t off )
{
    push_handle<T>( L, *detail::at<T*>( ctx, off ) );
}

}  // namespace mml
