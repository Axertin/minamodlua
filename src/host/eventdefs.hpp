#pragma once

#include <stddef.h>
#include <stdint.h>

#include <type_traits>

struct lua_State;
class World;

namespace mml
{

// A push/read pair per field. `ctx` is the engine's context struct; `off` is the
// field's offsetof within it. read == nullptr means the field is read-only.
using PushFn = void ( * )( lua_State* L, const void* ctx, uint16_t off );
using ReadFn = void ( * )( lua_State* L, int idx, void* ctx, uint16_t off );

struct Field
{
    const char* name;
    uint16_t offset;
    PushFn push;
    ReadFn read;
    // Non-zero when the context slot at `offset` holds a *pointer* the accessors
    // will dereference: the alignment its pointee requires, which is the only
    // property of the pointee a row-walking plausibility check can test. 1
    // means "no alignment requirement beyond the low-address floor" - which is
    // what a genuine alignof(pointee)==1 gives (a bool* row), not a stand-in
    // for "unknowable". An opaque engine handle (FIELD_HANDLE) still carries a
    // real pointer *value*, even though its pointee's alignment cannot be
    // named, so it gets alignof(void*) rather than 1. 0 means the row is a
    // plain value. push_event uses this to reject a context whose pointers
    // cannot be real before any accessor dereferences one - see
    // check_pointer_fields.
    uint8_t ptrAlign;
};

struct EventDef
{
    const char* luaName;
    const char* hookName;
    const Field* fields;
    uint8_t fieldCount;
    // Layout plausibility. Returns false to permanently disable the event rather
    // than dispatch garbage into Lua. nullptr means "nothing to check".
    bool ( *check )( const void* ctx );
    bool playerWorldOnly;
    bool cancellable;  // context carries modHandled
    // Installs the fields that do not fit the one-value-per-row model (bitfield
    // accessors, the mouse touch array). Called by push_event with the event
    // table on top of the stack, i.e. at index -1, and required to leave the
    // stack exactly as it found it - push_event records the table's index
    // *before* calling this and every later step indexes off that. Unlike
    // readExtra, which is handed an explicit `eventIndex` because read_back may
    // have pushed by then, install has no such parameter: -1 is the contract.
    // A null ctx is never passed (push_event refuses one when install is set),
    // so an installer may dereference it immediately.
    void ( *install )( lua_State* L, const void* ctx );
    void ( *readExtra )( lua_State* L, int eventIndex, void* ctx );
};

extern const EventDef kEvents[];
extern const size_t kEventCount;

// --- field implementations -------------------------------------------------
//
// Each takes the context base plus an offset rather than a typed pointer, so one
// instantiation per (kind, type) serves every context that has such a field.

template <typename T>
void push_ptr( lua_State* L, const void* ctx, uint16_t off );
template <typename T>
void read_ptr( lua_State* L, int idx, void* ctx, uint16_t off );

// A single component of a POD reached through a pointer (MM_Vec2::x etc). The
// element type is not a parameter: it comes from PodTraits<S> (bridge/pod.hpp),
// the same descriptor mina.raw marshals these types through, which also carries
// the component count Comp is bounds-checked against. Naming the element type
// here instead would re-open the hole PodTraits closes - MM_Vec2/MM_Vec3 become
// the engine's own ycVec2/ycVec3 under MM_USE_YC_TYPES (MinaModTypes.h), so
// "three floats, tightly packed" is an assumption that has to be asserted, not
// spelled into a macro argument.
template <typename S, size_t Comp>
void push_ptr_comp( lua_State* L, const void* ctx, uint16_t off );
template <typename S, size_t Comp>
void read_ptr_comp( lua_State* L, int idx, void* ctx, uint16_t off );

template <typename T>
void push_ptr_handle( lua_State* L, const void* ctx, uint16_t off );
template <typename T>
void read_ptr_handle( lua_State* L, int idx, void* ctx, uint16_t off );

template <typename T>
void push_value( lua_State* L, const void* ctx, uint16_t off );

template <typename T>
void push_handle_field( lua_State* L, const void* ctx, uint16_t off );

void push_inline_bool( lua_State* L, const void* ctx, uint16_t off );
void read_inline_bool( lua_State* L, int idx, void* ctx, uint16_t off );

// Bitfield and array fields do not fit the one-value-per-row model: they need
// accessors, and the mouse array's length is only known at runtime. Each gets a
// hook that installs what it needs onto the event table.
void install_keyboard( lua_State* L, const void* ctx );
void install_controller( lua_State* L, const void* ctx );
void install_mouse_touch( lua_State* L, const void* ctx );
void read_keyboard( lua_State* L, int eventIndex, void* ctx );
void read_controller( lua_State* L, int eventIndex, void* ctx );
void read_mouse_touch( lua_State* L, int eventIndex, void* ctx );

// YC_TOUCH_COUNT is not published in any header; events_open resolves it via
// GetEnumUInt. 0 means unresolved, and the touch array is then absent.
void events_set_touch_count( uint32_t n );
uint32_t events_touch_count();

// --- row-macro compile-time guards ------------------------------------------
//
// FIELD_PTR_COMP and FIELD_POD_IN take the same arguments and differ only in
// whether they install a read function, so picking the wrong one compiles
// silently - and picking FIELD_PTR_COMP for a `const MM_Vec3*` member would
// write through a pointer the engine declared const. The member type already
// says which is correct, so make the macros assert it rather than trust the
// author. Both are constexpr functions, not bare static_asserts, because a row
// is a braced initializer: there is nowhere to put a statement, but the offset
// is an expression.

template <typename P>
constexpr uint16_t offset_of_const_ptr( size_t off )
{
    static_assert( std::is_pointer_v<P>, "FIELD_POD_IN's member is not a pointer" );
    static_assert( std::is_const_v<std::remove_pointer_t<P>>,
                   "FIELD_POD_IN is for a const pointee; this member is writable - use FIELD_PTR_COMP" );
    return (uint16_t)off;
}

template <typename P>
constexpr uint16_t offset_of_mutable_ptr( size_t off )
{
    static_assert( std::is_pointer_v<P>, "FIELD_PTR_COMP's member is not a pointer" );
    static_assert( !std::is_const_v<std::remove_pointer_t<P>>,
                   "FIELD_PTR_COMP writes through this pointer, but the engine declared the pointee "
                   "const - use FIELD_POD_IN" );
    return (uint16_t)off;
}

}  // namespace mml

// --- row macros ------------------------------------------------------------

// The declared type of CTX::MEMBER, in an unevaluated operand so no object of
// either type is ever created.
#define MML_MEMBER_T( CTX, MEMBER ) decltype( ( (CTX*)0 )->MEMBER )

// `T* m` -- writable out-parameter the original function will read back.
#define FIELD_PTR( NAME, CTX, MEMBER, T )                                                                              \
    { NAME, (uint16_t)offsetof( CTX, MEMBER ), &mml::push_ptr<T>, &mml::read_ptr<T>, (uint8_t)alignof( T ) }

// One component of a POD reached through a writable `S* m`.
#define FIELD_PTR_COMP( NAME, CTX, MEMBER, S, COMP )                                                                   \
    { NAME, mml::offset_of_mutable_ptr<MML_MEMBER_T( CTX, MEMBER )>( offsetof( CTX, MEMBER ) ),                        \
      &mml::push_ptr_comp<S, COMP>, &mml::read_ptr_comp<S, COMP>, (uint8_t)alignof( S ) }

// `T** m` -- a handle the caller may substitute. The context slot holds a `T**`,
// so what it points at is a pointer: alignof(T*), not alignof(T) (which T,
// being an incomplete engine type, does not have).
#define FIELD_PTR_HANDLE( NAME, CTX, MEMBER, T )                                                                       \
    { NAME, (uint16_t)offsetof( CTX, MEMBER ), &mml::push_ptr_handle<T>, &mml::read_ptr_handle<T>,                     \
      (uint8_t)alignof( T* ) }

// `const S* m` -- read-only POD, one row per component.
#define FIELD_POD_IN( NAME, CTX, MEMBER, S, COMP )                                                                     \
    { NAME, mml::offset_of_const_ptr<MML_MEMBER_T( CTX, MEMBER )>( offsetof( CTX, MEMBER ) ),                          \
      &mml::push_ptr_comp<S, COMP>, nullptr, (uint8_t)alignof( S ) }

// `T m` -- a value sitting in the context, read-only. Not a pointer row.
#define FIELD_VALUE( NAME, CTX, MEMBER, T ) { NAME, (uint16_t)offsetof( CTX, MEMBER ), &mml::push_value<T>, nullptr, 0 }

// `T* m` -- a handle the caller may not replace. T is an incomplete engine type,
// so its alignment is unknowable, but the slot still holds a real T* value: any
// pointer the engine hands out is at least pointer-aligned, so the row is
// checked at alignof(void*) rather than left at "floor only".
#define FIELD_HANDLE( NAME, CTX, MEMBER, T )                                                                           \
    { NAME, (uint16_t)offsetof( CTX, MEMBER ), &mml::push_handle_field<T>, nullptr, (uint8_t)alignof( void* ) }

// `bool modHandled` -- sticky; see eventctx.cpp. Not a pointer row.
#define FIELD_HANDLED( CTX )                                                                                           \
    { "mod_handled", (uint16_t)offsetof( CTX, modHandled ), &mml::push_inline_bool, &mml::read_inline_bool, 0 }

// `bool modRetVal` -- last write wins. Not a pointer row.
#define FIELD_RESULT( CTX )                                                                                            \
    { "result", (uint16_t)offsetof( CTX, modRetVal ), &mml::push_inline_bool, &mml::read_inline_bool, 0 }
