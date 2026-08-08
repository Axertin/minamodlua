#include "eventctx.hpp"

#include "handle.hpp"
#include "invoke.hpp"
#include "marshal.hpp"

// Must precede MinaModHooks.h; see the note in eventdefs.cpp.
#include <stddef.h>
#include <stdint.h>

#include <MinaModEnums.h>
#include <MinaModHooks.h>

#include <stdio.h>
#include <string.h>

extern "C"
{
#include "lauxlib.h"
#include "lua.h"
}

namespace mml
{

void push_inline_bool( lua_State* L, const void* ctx, uint16_t off )
{
    lua_pushboolean( L, *detail::at<bool>( ctx, off ) );
}

void read_inline_bool( lua_State* L, int idx, void* ctx, uint16_t off )
{
    *detail::at<bool>( ctx, off ) = lua_toboolean( L, idx ) != 0;
}

namespace
{
// The whole safety argument for writing kKeyWords words into an engine-owned
// array rests on 136 being the highest YC_KEY_*, which is a compile-time fact
// nothing was checking. YC_KEY_META is the last entry in MinaModEnums.h's key
// block; if a later key is added above it this assert fires and both constants
// have to be re-derived rather than silently overrunning keysDown, which the
// engine sizes [(YC_KEY_COUNT+31)/32].
constexpr int kKeyMax = 136;  // highest YC_KEY_*
static_assert( YC_KEY_META == kKeyMax, "highest YC_KEY_* moved; re-derive kKeyMax/kKeyWords" );

constexpr int kKeyWords = ( kKeyMax / 32 ) + 1;  // words needed to hold bits 0..kKeyMax

// buttonDown is one uint64_t, so the highest bit index is its width minus one.
constexpr int kButtonMax = 8 * (int)sizeof( uint64_t ) - 1;

uint32_t g_touchCount = 0;

// A word read back out of a Lua table. Casting a lua_Number outside uint32_t's
// range to uint32_t is undefined behaviour (`e._keys_down[1] = -1` from mod
// Lua reaches this), so the range is tested first and the caller decides what
// an out-of-range value means.
bool to_word( lua_State* L, int idx, uint32_t& out )
{
    const lua_Number n = lua_tonumber( L, idx );
    if ( !detail::in_range<uint32_t>( n ) ) return false;
    out = (uint32_t)n;
    return true;
}

// luaL_error routes its format string through LuaJIT's own formatter
// (lj_strfmt_parse), not the C library's: its conversion table has no 'l'
// entry at all - it never supported the printf length modifiers, only a fixed
// set of one-letter conversions (%s, %d, %c, %f, %p, %%). "%lld" hits the
// unrecognised 'l' and formatting aborts on the spot, so the message a mod
// actually saw was "key_down: index ?" - the offending index and the valid
// range silently dropped, not merely misformatted. Confirmed live by running
// the suite with the old "%lld" call reinstated (see the mutation note this
// accompanies).
//
// Casting `raw` to int first is not the fix: the whole point of this check is
// that `raw` is the full 64-bit lua_Integer LuaJIT hands back, which may be
// out of int range, and narrowing it before reporting would misreport the
// very value the error is about. Format it as a string instead and hand
// luaL_error a %s, which lj_strfmt_parse does support.
int bounds_error( lua_State* L, const char* what, lua_Integer raw, int maxIndex )
{
    char buf[32];
    snprintf( buf, sizeof buf, "%lld", (long long)raw );
    return luaL_error( L, "%s: index %s out of range 0..%d", what, buf, maxIndex );
}

// Shared by key_held/key_down and button: `field` names which copied array
// to use, and `what` names the calling Lua method so a bounds error says which
// of the six entry points raised it (spec 6.1). idx is range-checked as the
// full lua_Integer LuaJIT hands back, before any narrowing to int: narrowing
// first would let e.g. 2^32+5 alias index 5 silently instead of raising, since
// the truncated value passes the check.
int bit_get( lua_State* L, const char* what, const char* field, int maxIndex )
{
    const lua_Integer raw = luaL_checkinteger( L, 2 );
    if ( raw < 0 || raw > maxIndex ) return bounds_error( L, what, raw, maxIndex );
    const int idx = (int)raw;
    lua_getfield( L, 1, field );
    // A handler can reassign _keys_down/_keys_first/_buttons to anything -
    // nil, a number, a string. LuaJIT's api_check is compiled out in release,
    // so lua_rawgeti on a non-table would read through whatever the value
    // actually holds instead of raising. Treat it the same as read_mouse_touch
    // treats a non-table `touch`: silently absent rather than a crash.
    if ( !lua_istable( L, -1 ) )
    {
        lua_pop( L, 1 );
        lua_pushboolean( L, false );
        return 1;
    }
    lua_rawgeti( L, -1, ( idx / 32 ) + 1 );
    // A handler can have put anything in the word slot; an out-of-range number
    // reads as "no bits set" rather than being cast out of range.
    uint32_t word = 0;
    to_word( L, -1, word );
    lua_pop( L, 2 );
    lua_pushboolean( L, ( word >> ( idx % 32 ) ) & 1u );
    return 1;
}

int bit_set( lua_State* L, const char* what, const char* field, int maxIndex )
{
    const lua_Integer raw = luaL_checkinteger( L, 2 );
    if ( raw < 0 || raw > maxIndex ) return bounds_error( L, what, raw, maxIndex );
    const int idx = (int)raw;
    const bool on = lua_toboolean( L, 3 ) != 0;
    lua_getfield( L, 1, field );
    if ( !lua_istable( L, -1 ) )
    {
        lua_pop( L, 1 );
        return 0;
    }
    lua_rawgeti( L, -1, ( idx / 32 ) + 1 );
    uint32_t word = 0;
    to_word( L, -1, word );  // see bit_get: an out-of-range word starts from 0
    lua_pop( L, 1 );
    word = on ? ( word | ( 1u << ( idx % 32 ) ) ) : ( word & ~( 1u << ( idx % 32 ) ) );
    lua_pushnumber( L, (lua_Number)word );
    lua_rawseti( L, -2, ( idx / 32 ) + 1 );
    lua_pop( L, 1 );
    return 0;
}

// The two Lua-facing names below deliberately do NOT match the ctx struct
// field each one reads. They follow the engine's own IsKeyDown/IsKeyHeld
// vocabulary (mina.raw.is_key_down/is_key_held, docs/raw-api-reference.md)
// instead of upstream's keysDown/keysDownFirstFrame field names, because
// upstream's names describe the *storage*, not the *semantics*, and the
// semantics are what a mod actually needs to reason about:
//   - ctx keysDown is a LEVEL signal - true for every frame a key is held -
//     which is what IsKeyHeld reports. It backs key_held/set_key_held here,
//     even though its own field name says "down".
//   - ctx keysDownFirstFrame is an EDGE signal - true only on the frame a key
//     goes down - which is what IsKeyDown reports. It backs key_down/
//     set_key_down here.
// This was mapped the other way around (key_down -> keysDown, key_pressed ->
// keysDownFirstFrame) until an in-game run caught it: every key whose
// key_down/key_pressed disagreed with is_key_down/is_key_held was a movement
// key (S/W/SPACE) being held, which is only explainable if keysDown tracks
// "held" and keysDownFirstFrame tracks "just pressed" - i.e. the reverse of
// the old names. Verified against the running engine, not inferred from the
// header alone; see docs/events.md. The private table fields below
// (_keys_down/_keys_first) keep the struct's own names rather than being
// renamed to match, so mind the crossed mapping: "_keys_down" backs
// key_held, and "_keys_first" backs key_down.
int l_key_held( lua_State* L ) { return bit_get( L, "key_held", "_keys_down", kKeyMax ); }
int l_key_down( lua_State* L ) { return bit_get( L, "key_down", "_keys_first", kKeyMax ); }
int l_set_key_held( lua_State* L ) { return bit_set( L, "set_key_held", "_keys_down", kKeyMax ); }
int l_set_key_down( lua_State* L ) { return bit_set( L, "set_key_down", "_keys_first", kKeyMax ); }
int l_button( lua_State* L ) { return bit_get( L, "button", "_buttons", kButtonMax ); }
int l_set_button( lua_State* L ) { return bit_set( L, "set_button", "_buttons", kButtonMax ); }

void copy_words_in( lua_State* L, const char* field, const uint32_t* src, int words )
{
    lua_newtable( L );
    for ( int i = 0; i < words; ++i )
    {
        lua_pushnumber( L, src ? (lua_Number)src[i] : 0 );
        lua_rawseti( L, -2, i + 1 );
    }
    lua_setfield( L, -2, field );
}

void copy_words_out( lua_State* L, int eventIndex, const char* field, uint32_t* dst, int words )
{
    if ( !dst ) return;
    lua_getfield( L, eventIndex, field );
    // See bit_get: a reassigned non-table field must be skipped, not indexed.
    if ( lua_istable( L, -1 ) )
        for ( int i = 0; i < words; ++i )
        {
            lua_rawgeti( L, -1, i + 1 );
            // An out-of-range word leaves the engine's word as it was, matching
            // how every other writable field treats a value it cannot represent.
            uint32_t word = 0;
            if ( to_word( L, -1, word ) ) dst[i] = word;
            lua_pop( L, 1 );
        }
    lua_pop( L, 1 );
}

void set_method( lua_State* L, const char* name, lua_CFunction fn )
{
    lua_pushcfunction( L, fn );
    lua_setfield( L, -2, name );
}

// The pointers in a context are the one thing a compile-time size assert cannot
// vouch for: the asserts in eventdefs.cpp prove agreement with upstream's
// header, not with the running game, and a layout mismatch turns every pointer
// row into a wild address. push_ptr and friends dereference those immediately,
// and read_back later *writes* through them - an arbitrary read/write in the
// game process, inside a lua_cpcall that cannot recover from a segfault.
//
// Nothing here can prove a pointer is the object the header says it is. What it
// can do is reject the addresses no real pointer to that object could hold:
// below the first page (0x10000 is unmapped on Windows and Linux alike, so this
// catches a small integer or a misread field), or misaligned for the pointee -
// which is the axis a shifted layout trips first, since a struct field read one
// or two bytes off is almost never still aligned. A row whose pointee alignment
// is unknowable (an opaque engine handle, FIELD_HANDLE) still gets checked at
// alignof(void*): the pointee's shape is unknown, but the pointer *value* is a
// real one the engine handed out, and any real pointer is at least
// pointer-aligned. Only a row whose pointee's own alignment is genuinely 1 (a
// bool* row) gets the floor check alone.
constexpr uintptr_t kLowAddressFloor = 0x10000;

bool plausible_ptr( const void* p, uint8_t align )
{
    if ( !p ) return true;  // null is a legitimate value; every accessor checks for it
    const uintptr_t v = (uintptr_t)p;
    return v >= kLowAddressFloor && ( align <= 1 || ( v % align ) == 0 );
}

// Walks the descriptor table and applies plausible_ptr to every pointer-bearing
// row. Every event gets this, unconditionally - it is derived from the rows
// themselves, so an event acquires the coverage by having rows rather than by
// someone remembering to write a check. def.check stays what it always was: an
// event-specific plausibility test on top of this one (bounds on `elapsed`, a
// pointer the descriptor table does not cover, ...).
bool check_pointer_fields( const EventDef& def, const void* ctx )
{
    for ( uint8_t i = 0; i < def.fieldCount; ++i )
    {
        const Field& f = def.fields[i];
        if ( !f.ptrAlign ) continue;
        // Not *(const void* const*)(...): reinterpreting arbitrary context
        // bytes as a pointer through a cast-and-deref is a strict-aliasing
        // violation (the byte range's effective type was never "pointer to
        // void"), technically undefined behaviour even though every platform
        // this runs on happens to have trivial pointer representation. memcpy
        // into a local sidesteps the aliasing rule entirely and is the
        // standard-sanctioned way to reinterpret bytes.
        const void* p;
        memcpy( &p, (const unsigned char*)ctx + f.offset, sizeof( p ) );
        if ( !plausible_ptr( p, f.ptrAlign ) ) return false;
    }
    return true;
}
}  // namespace

bool plausible_context_pointer( const void* p, uint8_t align ) { return plausible_ptr( p, align ); }

void events_set_touch_count( uint32_t n ) { g_touchCount = n; }
uint32_t events_touch_count() { return g_touchCount; }

void install_keyboard( lua_State* L, const void* ctx )
{
    const auto* c = (const ycKeyboardUpdateCtx*)ctx;
    copy_words_in( L, "_keys_down", c->keysDown, kKeyWords );
    copy_words_in( L, "_keys_first", c->keysDownFirstFrame, kKeyWords );
    set_method( L, "key_held", l_key_held );
    set_method( L, "set_key_held", l_set_key_held );
    set_method( L, "key_down", l_key_down );
    set_method( L, "set_key_down", l_set_key_down );
}

void read_keyboard( lua_State* L, int eventIndex, void* ctx )
{
    auto* c = (ycKeyboardUpdateCtx*)ctx;
    copy_words_out( L, eventIndex, "_keys_down", c->keysDown, kKeyWords );
    copy_words_out( L, eventIndex, "_keys_first", c->keysDownFirstFrame, kKeyWords );
}

void install_controller( lua_State* L, const void* ctx )
{
    const auto* c = (const ycControllerUpdateCtx*)ctx;
    const uint64_t v = c->buttonDown ? *c->buttonDown : 0;
    // Two 32-bit halves, so the bit helpers are shared with the keyboard and no
    // 64-bit value ever reaches Lua as a number.
    lua_newtable( L );
    lua_pushnumber( L, (lua_Number)(uint32_t)( v & 0xFFFFFFFFull ) );
    lua_rawseti( L, -2, 1 );
    lua_pushnumber( L, (lua_Number)(uint32_t)( v >> 32 ) );
    lua_rawseti( L, -2, 2 );
    lua_setfield( L, -2, "_buttons" );
    set_method( L, "button", l_button );
    set_method( L, "set_button", l_set_button );
}

void read_controller( lua_State* L, int eventIndex, void* ctx )
{
    auto* c = (ycControllerUpdateCtx*)ctx;
    if ( !c->buttonDown ) return;
    lua_getfield( L, eventIndex, "_buttons" );
    // See bit_get: a reassigned non-table field must be skipped, not indexed.
    if ( !lua_istable( L, -1 ) )
    {
        lua_pop( L, 1 );
        return;
    }
    // Each half starts from the value the engine handed over, so a half a
    // handler put out of uint32_t range (which cannot be cast without undefined
    // behaviour) is left alone rather than forced to zero.
    uint32_t lo = (uint32_t)( *c->buttonDown & 0xFFFFFFFFull );
    uint32_t hi = (uint32_t)( *c->buttonDown >> 32 );
    lua_rawgeti( L, -1, 1 );
    to_word( L, -1, lo );
    lua_pop( L, 1 );
    lua_rawgeti( L, -1, 2 );
    to_word( L, -1, hi );
    lua_pop( L, 2 );
    *c->buttonDown = (uint64_t)lo | ( (uint64_t)hi << 32 );
}

void install_mouse_touch( lua_State* L, const void* ctx )
{
    const auto* c = (const ycMouseUpdateCtx*)ctx;
    if ( !g_touchCount || !c->mouseDown ) return;  // absent, never guessed
    lua_newtable( L );
    for ( uint32_t i = 0; i < g_touchCount; ++i )
    {
        lua_pushnumber( L, c->mouseDown[i] );
        lua_rawseti( L, -2, (int)i + 1 );
    }
    lua_setfield( L, -2, "touch" );
}

void read_mouse_touch( lua_State* L, int eventIndex, void* ctx )
{
    auto* c = (ycMouseUpdateCtx*)ctx;
    if ( !g_touchCount || !c->mouseDown ) return;
    lua_getfield( L, eventIndex, "touch" );
    if ( lua_istable( L, -1 ) )
        for ( uint32_t i = 0; i < g_touchCount; ++i )
        {
            lua_rawgeti( L, -1, (int)i + 1 );
            // assign_scalar, not a bare cast: a lua_Number past float's range
            // (1e300) is undefined behaviour to convert, so it is skipped.
            if ( lua_isnumber( L, -1 ) ) detail::assign_scalar( L, lua_gettop( L ), &c->mouseDown[i] );
            lua_pop( L, 1 );
        }
    lua_pop( L, 1 );
}

bool push_event( lua_State* L, const EventDef& def, const void* ctx, World** outWorld )
{
    *outWorld = nullptr;

    // A context is required whenever anything is going to touch it: the rows
    // (fieldCount) *or* an installer, which every one of the three dereferences
    // immediately. Keying only on fieldCount left keyboard_update - zero rows,
    // one installer - relying on a hand-written non-null check to cover the gap.
    if ( ( def.fieldCount || def.install ) && !ctx ) return false;
    if ( ctx && !check_pointer_fields( def, ctx ) ) return false;
    if ( def.check && !def.check( ctx ) ) return false;

    lua_newtable( L );
    for ( uint8_t i = 0; i < def.fieldCount; ++i )
    {
        const Field& f = def.fields[i];
        f.push( L, ctx, f.offset );
        lua_setfield( L, -2, f.name );
    }
    if ( def.install ) def.install( L, ctx );

    // The player-world filter needs the world before any handler runs. Matched
    // by field name rather than by comparing function pointers: only the world
    // events have one, and a name is stable across template instantiations.
    for ( uint8_t i = 0; i < def.fieldCount; ++i )
        if ( strcmp( def.fields[i].name, "world" ) == 0 )
            *outWorld = *(World* const*)( (const unsigned char*)ctx + def.fields[i].offset );

    return true;
}

void latch_handled( lua_State* L, int eventIndex, bool& handled )
{
    lua_getfield( L, eventIndex, "mod_handled" );
    if ( lua_toboolean( L, -1 ) ) handled = true;
    lua_pop( L, 1 );
    lua_pushboolean( L, handled );
    lua_setfield( L, eventIndex, "mod_handled" );
}

void read_back( lua_State* L, int eventIndex, const EventDef& def, void* ctx )
{
    if ( !ctx ) return;
    for ( uint8_t i = 0; i < def.fieldCount; ++i )
    {
        const Field& f = def.fields[i];
        if ( !f.read ) continue;
        lua_getfield( L, eventIndex, f.name );
        f.read( L, lua_gettop( L ), ctx, f.offset );
        lua_pop( L, 1 );
    }
    if ( def.readExtra ) def.readExtra( L, eventIndex, ctx );
}

}  // namespace mml
