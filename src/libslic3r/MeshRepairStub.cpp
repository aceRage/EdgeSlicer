// The no-OpenVDB half of MeshRepair.hpp.
//
// OpenVDBUtils.cpp is only compiled when the OpenVDB target exists (see
// src/libslic3r/CMakeLists.txt). This file is the other side of that switch:
// when OpenVDB is absent it supplies the same symbols, each returning "nothing
// happened", and voxel_ops_available() answers false.
//
// It is never compiled at the same time as OpenVDBUtils.cpp.

#include "MeshRepair.hpp"

namespace Slic3r {

indexed_triangle_set remesh_by_voxels(const indexed_triangle_set &, double) { return {}; }

bool voxel_ops_available() { return false; }

} // namespace Slic3r
