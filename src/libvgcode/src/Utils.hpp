///|/ Copyright (c) Prusa Research 2023 Enrico Turri @enricoturri1966
///|/
///|/ libvgcode is released under the terms of the AGPLv3 or higher
///|/
#ifndef VGCODE_UTILS_HPP
#define VGCODE_UTILS_HPP

#include "../include/Types.hpp"
#include "../include/PathVertex.hpp"
#include "Settings.hpp"
#include <vector>
#include <cstdint>

#ifdef _WIN32
#define STDVEC_MEMSIZE(NAME, TYPE) NAME.capacity() * ((sizeof(TYPE) + __alignof(TYPE) - 1) / __alignof(TYPE)) * __alignof(TYPE)
#else
#define STDVEC_MEMSIZE(NAME, TYPE) NAME.capacity() * ((sizeof(TYPE) + alignof(TYPE) - 1) / alignof(TYPE)) * alignof(TYPE)
#endif // _WIN32

namespace libvgcode {

extern void add_vertex(const Vec3& position, const Vec3& normal, std::vector<float>& vertices);
extern void add_triangle(uint16_t v1, uint16_t v2, uint16_t v3, std::vector<uint16_t>& indices);
extern Vec3 normalize(const Vec3& v);
extern float dot(const Vec3& v1, const Vec3& v2);
extern float length(const Vec3& v);
extern bool operator == (const Vec3& v1, const Vec3& v2);
extern bool operator != (const Vec3& v1, const Vec3& v2);
extern Vec3 operator + (const Vec3& v1, const Vec3& v2);
extern Vec3 operator - (const Vec3& v1, const Vec3& v2);
extern Vec3 operator * (float f, const Vec3& v);

// Whether an extrusion vertex counts towards the color ranges (legend min/max) of the numeric view
// types: only extrusions whose role is currently shown in the Line Type legend do, so hiding all but
// one feature makes the ranges, and so the gradient, cover that feature alone.
extern bool counts_for_color_ranges(const PathVertex& v, const Settings& settings);

} // namespace libvgcode

#endif // VGCODE_UTILS_HPP