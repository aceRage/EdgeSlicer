# Attribution: Design tab GUI

The files in this folder, and src/slic3r/GUI/AuiMgr.hpp/.cpp (CadAuiMgr), are ported from
OrcaSlicer at commit 3610d8bb (https://github.com/SoftFever/OrcaSlicer), with local changes
for EdgeSlicer.

- License: GNU Affero General Public License v3.0 (AGPL-3.0), the same as this project
  (see LICENSE.txt at the repository root).
- Authors: the original OrcaSlicer authors and contributors, as credited upstream in the
  OrcaSlicer repository and its history.

They are compiled only when SLIC3R_CAD is ON (src/slic3r/CMakeLists.txt).

Prepare-toolbar Sketch / Primitive gizmos (P4) live next to the other gizmos:

- src/slic3r/GUI/Gizmos/GLGizmoSketch.hpp/.cpp
- src/slic3r/GUI/Gizmos/GLGizmoPrimitive.hpp/.cpp

They are adapted from Orca PRs #15762 / #15238. Orca's ImGui/OCCT canvas gizmos
(GeometryEngine PrimitiveParams / make_primitive) were removed upstream before
3610d8bb; these files are Design-tab launchers at that pin (Sketch opens a
sketch session; Primitive adds a 20 mm box). Same AGPL-3.0 license as above.
