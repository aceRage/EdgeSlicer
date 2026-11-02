# Attribution: Design tab CAD core

The files in this folder (the CAD document / feature tree, sketch engine, constraint solver
glue, sketch import and inference, geometry engine and thread standards) are ported from
OrcaSlicer at commit 3610d8bb (https://github.com/SoftFever/OrcaSlicer; Orca PRs #15238 and
#16019 and their follow-ups), with local changes for EdgeSlicer. The main EdgeSlicer changes:
`GeometryEngine::tessellate` uses EdgeSlicer's `BRep::brep_to_its`, OCCT 8 exception messages
go through `BRep/OcctException.hpp`, and the recipe reader uses `BoundedBinaryArchive.hpp`
(an EdgeSlicer addition that bounds every length and count in a recipe before allocating).
The tests in tests/libslic3r (test_caddocument.cpp, test_sketch*.cpp, test_slvs_constraints.cpp)
and their fixtures in tests/data come from the same port, converted to Catch2 v2.

- License: GNU Affero General Public License v3.0 (AGPL-3.0), the same as this project
  (see LICENSE.txt at the repository root).
- Authors: the original OrcaSlicer authors and contributors, as credited upstream in the
  OrcaSlicer repository and its history.
- The sketch solver links SolveSpace's libslvs (GPL-3.0), built from deps/SLVS; see
  resources/licenses/SolveSpace-libslvs-NOTICE.txt.

They are compiled only when SLIC3R_CAD is ON (src/libslic3r/CMakeLists.txt).
