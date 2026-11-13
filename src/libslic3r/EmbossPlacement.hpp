#ifndef slic3r_EmbossPlacement_hpp_
#define slic3r_EmbossPlacement_hpp_

// Which side of the surface an embossed volume (text, SVG, code, traced image) sits on, and how far
// it reaches, by its volume type. Shared by the emboss jobs (GUI) and the tests.
//
// A part ("Join") is raised: it stands on the surface and reaches `depth` out of it. Every other
// type - a negative volume ("Cut"), a modifier - is sunk: it reaches `depth` into the object, so a
// modifier colours (or otherwise changes) the object right under the surface. Both keep a hair
// (`safe`) on the other side of the surface, so the volume always meets the surface it was cut from.
// With "Use surface" the shell follows the surface (it is the surface patch moved along the
// projection); without it the shell is flat, on the plane through the placement point.

#include "Model.hpp"

namespace Slic3r {
namespace Emboss {

// Hair kept on the other side of the surface, mm (EmbossJob's SAFE_SURFACE_OFFSET).
constexpr float SURFACE_SAFE_OFFSET = 0.015f;

inline bool is_raised(ModelVolumeType type) { return type == ModelVolumeType::MODEL_PART; }

// A change of type moves the volume to the other side of the surface: its mesh must be made again
// (otherwise a raised SVG turned into a modifier still sits on top of the object and covers nothing).
inline bool type_change_needs_new_mesh(ModelVolumeType from, ModelVolumeType to) { return is_raised(from) != is_raised(to); }

// The shell of a surface cut volume, along the projection's local +Z (out of the object):
// from `back` (<= 0, into the object) to `front` (>= 0, out of it), in mm from the surface.
struct SurfaceShell
{
    float front = 0.f;
    float back  = 0.f;
};
inline SurfaceShell surface_shell(bool raised, float depth, float safe = SURFACE_SAFE_OFFSET)
{
    return raised ? SurfaceShell{ depth, -safe } : SurfaceShell{ safe, -depth };
}

// A flat volume: where its back face sits, along local Z from the placement point.
inline float flat_back_offset(bool raised, float depth, float safe = SURFACE_SAFE_OFFSET)
{
    return raised ? -safe : safe - depth;
}

} // namespace Emboss
} // namespace Slic3r

#endif // slic3r_EmbossPlacement_hpp_
