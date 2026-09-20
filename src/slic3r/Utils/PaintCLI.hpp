#pragma once

// CLI paint-inspection primitives.
//
// Backs the --inspect-paint CLI action. Reads the per-facet enforcer /
// blocker / extruder / fuzzy-skin state that the slicer stores on every
// ModelVolume (supports, seam, MMU color, fuzzy-skin) and emits a
// structured JSON summary — facet count, surface area, and mesh-local
// bounding box per state — so CI / scripted / AI tooling can reason
// about existing paint on a .3mf without opening the GUI.
//
// Coordinates are mesh-local (each volume's own frame), matching the
// frame that the paint gizmos operate in. Paint-depth / ImageMap facet
// layers are not included: this dump is FacetsAnnotation / TriangleSelector
// only (supports, seam, mmu, fuzzy).

#include "libslic3r/Model.hpp"

#include <iosfwd>
#include <string>
#include <vector>

namespace Slic3r {
namespace PaintCLI {

// One JSON document covering every loaded model. `source_paths` lists every input file
// (the CLI usually merges those into one Model before this runs; leftover models are
// still folded into the same `objects` / `summary` so stdout is never concatenated JSON).
void inspect_to_json(const std::vector<Model> &models, const std::vector<std::string> &source_paths, std::ostream &out);

} // namespace PaintCLI
} // namespace Slic3r
