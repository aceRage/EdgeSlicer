#ifndef slic3r_MeshPreview_hpp_
#define slic3r_MeshPreview_hpp_

// Small picture of meshes drawn without the graphics card: flat shaded triangles with a depth
// buffer, parallel projection. For previews inside of dialogs ("Image to object").

#include <array>
#include <cstdint>
#include <vector>

#include "Point.hpp"
#include "TriangleMesh.hpp"

namespace Slic3r {

struct MeshPreviewItem
{
    const indexed_triangle_set *mesh = nullptr;
    std::array<uint8_t, 3>      color{180, 190, 205};
    // Added to every vertex [in the units of the mesh]
    Vec3f offset = Vec3f::Zero();
};

struct MeshPreviewImage
{
    int                  width  = 0;
    int                  height = 0;
    std::vector<uint8_t> rgba; // width * height * 4, alpha 0 where nothing is drawn

    bool empty() const { return width <= 0 || height <= 0 || rgba.size() != size_t(width) * size_t(height) * 4; }
};

/// <summary>
/// Draw the meshes so they fill the picture. Z of the meshes is up.
/// </summary>
/// <param name="yaw_deg">The scene is turned about Z by this angle</param>
/// <param name="pitch_deg">Height of the viewer above the horizon: 0 = from the front, 90 = from above</param>
MeshPreviewImage render_mesh_preview(const std::vector<MeshPreviewItem> &items, int width, int height, double yaw_deg, double pitch_deg);

} // namespace Slic3r

#endif // slic3r_MeshPreview_hpp_
