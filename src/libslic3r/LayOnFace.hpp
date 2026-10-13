#pragma once

#include "Point.hpp"

#include <vector>

struct indexed_triangle_set;

namespace Slic3r {

class ModelObject;
class ModelVolume;

// A face of an object's convex hull that the object can rest on. These are the faces the
// "Lay on Face" gizmo offers and the ones the CLI --ground-* options choose from.
//
// Frames: "object" coordinates have the volume transformations applied but not the instance
// transformation. "Instance" coordinates additionally have the instance rotation, scale and
// mirror applied, but not its offset.
struct LayOnFacePlane
{
    Vec3d       normal;         // outward unit normal, object coordinates
    Vec3d       center;         // centroid of the outline, object coordinates; on the face's mean plane
    float       area;           // mm², instance coordinates
    Pointf3s    outline;        // convex outline in the plane frame, where the face is horizontal
    Transform3d to_plane_frame; // rotation from instance coordinates to the plane frame
};

// Which hull faces lay_on_face_planes() offers, measured at printed size (instance coordinates).
// The defaults are the strict limits the CLI --ground-* and --inspect-mesh options have always used.
struct LayOnFaceFilter
{
    float  min_area      = 5.f; // mm²: faces at least this large are always offered
    float  min_side      = 1.f; // mm: a face made of one triangle with a shorter side is dropped
    float  min_angle_deg = 1.f; // a face whose outline has a sharper inner angle is dropped
    // When fewer than min_count faces reach min_area, the largest of the smaller ones, down to
    // floor_area, are offered as well until there are min_count. 0 offers only faces of min_area or more.
    size_t min_count     = 0;
    float  floor_area    = 0.f; // mm²

    // The looser limits of the Lay on face gizmo, scaled to the printed size of what is laid
    // (`size_diagonal`, its bounding box diagonal in mm): at least the 12 largest hull faces down to
    // max(0.01 mm², 1e-4 * diagonal²), and a minimum side of min(1 mm, 5% of the diagonal), so tiny and
    // scaled-down parts still offer faces. Objects with 12 or more faces of 5 mm² get the same faces as before.
    static LayOnFaceFilter for_gizmo(double size_diagonal);
};

// Candidate faces of the object's model parts, largest first. The instance transformation
// (without offset) is applied before measuring, so faces too small to rest on are dropped
// by their printed size. With the default filter: under 5 mm², a side under 1 mm, or an inner angle under 1°.
std::vector<LayOnFacePlane> lay_on_face_planes(const ModelObject &object, const Transform3d &instance_matrix_no_offset,
                                               const LayOnFaceFilter &filter = {});

// Candidate faces of a single model part, measured the same way. Normals are still in object coordinates,
// so the planes can be passed to the functions below like the object's.
std::vector<LayOnFacePlane> lay_on_face_planes(const ModelVolume &volume, const Transform3d &instance_matrix_no_offset,
                                               const LayOnFaceFilter &filter = {});

// Bounding box diagonal (mm) of the object's model parts, or of the part `volume_idx` when it is not -1,
// with the instance transformation (without offset) applied. What LayOnFaceFilter::for_gizmo() scales by.
double lay_on_face_size_diagonal(const ModelObject &object, int volume_idx, const Transform3d &instance_matrix_no_offset);

// ---- Face under cursor ---------------------------------------------------------------------------------
// The gizmo's second mode lays the object on any face the user points at, concave ones included. These are
// the pure parts of it, kept here so they can be tested without the GUI.

// The flat face of `its` around the facet `seed`: the facets reachable from it over shared edges
// (`face_neighbors` from its_face_neighbors()) whose normal is within `max_angle_deg` of the seed's.
// Each facet is compared with the seed rather than with its neighbour, so the face cannot creep round
// a curve. Facets without area are taken (they do not tilt the face) so that slivers do not split it.
// The seed comes first; the rest are in no particular order. Empty when `seed` is out of range.
std::vector<int> grow_flat_face(const indexed_triangle_set &its, const std::vector<Vec3i32> &face_neighbors, int seed,
                                double max_angle_deg = 0.5);

// Area-weighted mean of the facets' normals, in mesh coordinates: exactly the normal of a true plane, and
// steadier than one skinny facet on a noisy one. Unit length, or zero when the facets have no area.
Vec3d flat_face_normal(const indexed_triangle_set &its, const std::vector<int> &facets);

// A mesh normal of a part placed by `volume_matrix`, in object coordinates: what lay_on_face(),
// lay_part_on_face() and the gizmo's Selection::flattening_rotate*() take. Normals map through the inverse
// transpose of the linear part (never the full affine matrix, whose translation would leak in), so this is
// right under a non-uniform scale and a mirror too. Unit length, or zero for a zero normal.
Vec3d mesh_normal_to_object(const Transform3d &volume_matrix, const Vec3d &mesh_normal);

// Vertices of the convex hulls of the object's model parts (or of the part `volume_idx` when it is not -1),
// in object coordinates: what the object rests on, for lay_on_face_recess_depth().
std::vector<Vec3d> lay_on_face_hull_points(const ModelObject &object, int volume_idx);

// How far, in mm at printed size, a face lies inside the convex hull along its outward `normal`.
// `hull_points`, `face_points` and `normal` are in object coordinates. 0 for a face on the hull: laid on it,
// the face rests on the bed. More than 0 for a recessed face (a pocket floor, the inside of a bracket): laid
// on it, the face points down that far above the bed while the object rests on its outer points.
double lay_on_face_recess_depth(const std::vector<Vec3d> &hull_points, const std::vector<Vec3d> &face_points, const Vec3d &normal,
                                const Transform3d &instance_matrix_no_offset);

// Index of the largest plane, or -1 if `planes` is empty. Of planes with the same area, such as
// the top and bottom of a box, the one already facing down the most wins, so flat parts stay put.
int find_largest_plane(const std::vector<LayOnFacePlane> &planes);

// Index of the plane whose normal is closest to `direction` (object coordinates),
// or -1 if `planes` is empty.
int find_plane_by_normal(const std::vector<LayOnFacePlane> &planes, const Vec3d &direction);

// Index of the plane whose face contains `point` (object coordinates) within `tolerance` mm, or -1
// if there is none. `instance_matrix_no_offset` is the one the planes were computed with.
int find_plane_at_point(const std::vector<LayOnFacePlane> &planes, const Transform3d &instance_matrix_no_offset,
                        const Vec3d &point, double tolerance);

// Rotates the instance so that `normal` (object coordinates) points down, the same rotation as
// the gizmo applies, then drops the instance so its lowest point is at z = 0.
void lay_on_face(ModelObject &object, size_t instance_idx, const Vec3d &normal);

// The part's new volume matrix after turning it about its own center so that `normal` (object coordinates,
// from the part's planes) points down in the world, and then sliding it along world Z so that face lies at the
// bed level z = 0 (its XY position is unchanged). `volume_matrix` is the part's current volume matrix and
// `instance_matrix` (with its offset) places the object in the world; only the part moves.
Transform3d lay_part_on_face_matrix(const ModelVolume &volume, const Transform3d &volume_matrix, const Transform3d &instance_matrix,
                                    const Vec3d &normal);

// The z translation that puts the lowest point of the object's instance on the bed (z = 0), measured on the
// parts' exact geometry like lay_on_face() does. Zero when it is on the bed already (within a nanometre).
double bed_drop_shift(const ModelObject &object, size_t instance_idx);

// Puts the lowest point of every instance of the object on the bed. Used after a part has been turned: the
// parts share the instances, so all of them drop.
void drop_object_to_bed(ModelObject &object);

// Rotates one part of the object with lay_part_on_face_matrix(), which puts the picked face on the bed, then raises
// the object if another part is left below z = 0. The other parts and the instance rotation stay as they were.
void lay_part_on_face(ModelObject &object, size_t instance_idx, size_t volume_idx, const Vec3d &normal);

} // namespace Slic3r
