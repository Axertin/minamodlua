// pod.hpp - field layout for the MM_ value types.
//
// The only hand-written type descriptions in the binding: field count and
// element type cannot be recovered from C++ without reflection.
//
// Layout quirks:
//   MM_Transform is r, s, t - rotation, scale, translation. Not t/r/s.
//   MM_AABB is center/extents, not min/max.
//   MM_Color is four uint8, 0-255. Exposed as 0-255 to match the engine rather
//     than 0-1, so no rounding surprises on the way back in.
//   MM_Mtx is a flat float[16]; the struct-of-Vec4 union is commented out.

#pragma once

#include "MinaModAPI.h"

#include <stddef.h>
#include <stdint.h>

namespace mml
{

// Undefined on purpose so that a POD with no descriptor is a compile error naming the exact type
template <typename T>
struct PodTraits;

// Flat float aggregates. `count` values in, `count` values out, so a mod writes
// `local x, y, z = ...` rather than unpacking a table.
#define MML_POD_FLOATS( TYPE, COUNT )                                                                                  \
    template <>                                                                                                        \
    struct PodTraits<TYPE>                                                                                             \
    {                                                                                                                  \
        using Element = float;                                                                                         \
        static constexpr size_t count = COUNT;                                                                         \
        static constexpr const char* name = #TYPE;                                                                     \
        static float* at( TYPE& v ) { return reinterpret_cast<float*>( &v ); }                                         \
        static const float* at( const TYPE& v ) { return reinterpret_cast<const float*>( &v ); }                       \
    };                                                                                                                 \
    static_assert( sizeof( TYPE ) == COUNT * sizeof( float ),                                                          \
                   #TYPE " is not " #COUNT " tightly packed floats - its layout changed" )

MML_POD_FLOATS( MM_Vec2, 2 );
MML_POD_FLOATS( MM_Vec3, 3 );
MML_POD_FLOATS( MM_Vec4, 4 );
MML_POD_FLOATS( MM_Quat, 4 );
MML_POD_FLOATS( MM_Transform, 10 );  // r(4) + s(3) + t(3)
MML_POD_FLOATS( MM_AABB, 6 );        // center(3) + extents(3)
MML_POD_FLOATS( MM_Sphere, 4 );      // center(3) + radius
MML_POD_FLOATS( MM_Circle, 7 );      // center(3) + normal(3) + radius - a 3D circle, not a 2D one
MML_POD_FLOATS( MM_LineSeg, 6 );     // p1(3) + p2(3)
MML_POD_FLOATS( MM_Mtx, 16 );

#undef MML_POD_FLOATS

template <>
struct PodTraits<MM_Color>
{
    using Element = uint8_t;
    static constexpr size_t count = 4;
    static constexpr const char* name = "MM_Color";
    static uint8_t* at( MM_Color& v ) { return reinterpret_cast<uint8_t*>( &v ); }
    static const uint8_t* at( const MM_Color& v ) { return reinterpret_cast<const uint8_t*>( &v ); }
};
static_assert( sizeof( MM_Color ) == 4, "MM_Color is not four packed bytes - its layout changed" );

// MM_Rtti and MM_StringRef have no descriptor on purpose: neither survives being
// flattened into numbers, so invoke.hpp encodes them itself.

}  // namespace mml
