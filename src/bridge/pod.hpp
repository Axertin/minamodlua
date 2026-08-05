// pod.hpp - field layout for the MM_ value types.
//
// The only hand-written type descriptions in the binding: field count and
// element type cannot be recovered from C++ without reflection.
//
// Field order is a `layout` string on each descriptor rather than a comment
// here, because the generated reference publishes it - a mod author passing
// MM_Transform or MM_AABB positionally has no other way to learn the order, and
// getting it wrong is silent. MM_Mtx is a flat float[16]; the struct-of-Vec4
// union is commented out upstream.

#pragma once

#include "MinaModAPI.h"

#include <stddef.h>
#include <stdint.h>

namespace mml
{

// Undefined on purpose so that a POD with no descriptor is a compile error naming the exact type
template <typename T>
struct PodTraits;

// The float aggregates, as (type, field count, field order). Kept as a list
// macro rather than loose invocations so that tools can enumerate exactly the
// set that has a descriptor: a type cannot appear here without getting a
// PodTraits specialisation, or get one without appearing in the reference.
//
// `layout` is the one thing a mod author cannot deduce from the signature. The
// count says how many numbers to pass; only this says which is which.
#define MML_POD_FLOAT_TYPES( X )                                                                                       \
    X( MM_Vec2, 2, "x, y" )                                                                                            \
    X( MM_Vec3, 3, "x, y, z" )                                                                                         \
    X( MM_Vec4, 4, "x, y, z, w" )                                                                                      \
    X( MM_Quat, 4, "x, y, z, w" )                                                                                      \
    X( MM_Transform, 10, "rot(4), scale(3), pos(3) - r/s/t, not t/r/s" )                                               \
    X( MM_AABB, 6, "center(3), extents(3) - not min/max" )                                                             \
    X( MM_Sphere, 4, "center(3), radius" )                                                                             \
    X( MM_Circle, 7, "center(3), normal(3), radius - a 3D circle, not a 2D one" )                                      \
    X( MM_LineSeg, 6, "p1(3), p2(3)" )                                                                                 \
    X( MM_Mtx, 16, "flat float[16]" )

// Flat float aggregates. `count` values in, `count` values out, so a mod writes
// `local x, y, z = ...` rather than unpacking a table.
#define MML_POD_FLOATS( TYPE, COUNT, LAYOUT )                                                                          \
    template <>                                                                                                        \
    struct PodTraits<TYPE>                                                                                             \
    {                                                                                                                  \
        using Element = float;                                                                                         \
        static constexpr size_t count = COUNT;                                                                         \
        static constexpr const char* name = #TYPE;                                                                     \
        static constexpr const char* layout = LAYOUT;                                                                  \
        static float* at( TYPE& v ) { return reinterpret_cast<float*>( &v ); }                                         \
        static const float* at( const TYPE& v ) { return reinterpret_cast<const float*>( &v ); }                       \
    };                                                                                                                 \
    static_assert( sizeof( TYPE ) == COUNT * sizeof( float ),                                                          \
                   #TYPE " is not " #COUNT " tightly packed floats - its layout changed" );

MML_POD_FLOAT_TYPES( MML_POD_FLOATS )

#undef MML_POD_FLOATS

// The range is the trap here: passing 0-1 floats yields near-black silently,
// because check_pod casts rather than rejecting.
#define MML_COLOR_LAYOUT "r, g, b, a - each 0-255, not 0-1"

template <>
struct PodTraits<MM_Color>
{
    using Element = uint8_t;
    static constexpr size_t count = 4;
    static constexpr const char* name = "MM_Color";
    static constexpr const char* layout = MML_COLOR_LAYOUT;
    static uint8_t* at( MM_Color& v ) { return reinterpret_cast<uint8_t*>( &v ); }
    static const uint8_t* at( const MM_Color& v ) { return reinterpret_cast<const uint8_t*>( &v ); }
};
static_assert( sizeof( MM_Color ) == 4, "MM_Color is not four packed bytes - its layout changed" );

// Every type with a descriptor, for tools that need to enumerate them. Tools
// should read count/layout off PodTraits rather than these arguments, which are
// here only to keep the list readable.
#define MML_POD_TYPES( X )                                                                                             \
    MML_POD_FLOAT_TYPES( X )                                                                                           \
    X( MM_Color, 4, MML_COLOR_LAYOUT )

// MM_Rtti and MM_StringRef have no descriptor on purpose: neither survives being
// flattened into numbers, so invoke.hpp encodes them itself.

}  // namespace mml
