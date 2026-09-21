# Catch2 lockstep smoke (#62–#71)

Unpaid EdgeSlicer lockstep PRs. **Do not merge.** Leave draft/open.

Windows Catch2 in this repo is run against the test **exe** with a Catch2 tag filter (not `ctest -R <tag>` for `libslic3r_tests`, which is registered as a single `add_test`). After a VS2022 Release build:

```bat
cmake --build build --config Release --target <target>
build\tests\<dir>\Release\<target>.exe "[tag]"
```

`slic3rutils_tests` and `fff_print_tests` also use `catch_discover_tests()`, so `ctest -C Release -R <prefix>` works there. `libslic3r_tests` does **not** (`add_test(libslic3r_tests …)` of the whole binary).

| PR | Catch2? | CMake target | Windows one-liner | Notes |
|---|---|---|---|---|
| **#62** profiles | No | — | — | JSON / `scripts/orca_extra_profile_check.py` only (`ctest -R profile` on a configured tree). |
| **#63** correctness 3a | Yes | `fff_print_tests`, `libslic3r_tests` | `fff_print_tests.exe "[SupportMaterial][TreeHybrid]"`<br>`libslic3r_tests.exe "[Config][SAFC]"` | Cases live in already-registered `test_support_material.cpp` / `test_config.cpp`. |
| **#64** OpenSSL 3.5.7 | Yes | `libslic3r_tests` | `libslic3r_tests.exe "[MD5]"` | `TestMD5.cpp` is on **#64 only** (`cursor/openssl-357-infra-bd61`); wired in that branch’s `tests/libslic3r/CMakeLists.txt`. |
| **#65** Flutter web | No | — | — | Resource / HttpServer load-path; no Catch2. |
| **#66** import / MM crashes 3b | Yes | `libslic3r_tests` | `libslic3r_tests.exe "[ShortestPath],[GCodeImport],[OOB]"` | `test_shortest_path.cpp`, `test_gcode_import.cpp`, plus `[ToolOrdering][OOB]` in `test_toolordering_cyclic.cpp`. Wired on **#66** CMakeLists. |
| **#67** selection highlight | No | — | — | GL/UI; no in-tree Catch2. |
| **#68** nozzle/paint/large-model 3c | Yes | `libslic3r_tests` | `libslic3r_tests.exe "[EdgeGrid][PaintOverflow]"` | `test_edgegrid.cpp` wired on **#68** CMakeLists. |
| **#69** Full Spectrum Auto Match | Yes | `libslic3r_tests` | `libslic3r_tests.exe "[MixedFilament][FilamentColor]"` | +7 palette cases in already-registered `test_mixed_filament.cpp`. |
| **#70** High-Flow Hot End | Partial | `libslic3r_tests` | `libslic3r_tests.exe "[Config]"` | No dedicated HF tag. `inner_wall_speed` accessors in `"Config accessor functions perform as expected."` now `ConfigOptionFloats`. No `is_perimeter_compatible` Catch2 case. |
| **#71** Both-mode + copy (this tip) | Yes | `slic3rutils_tests` | `slic3rutils_tests.exe "[FlowVariantEdit]"` | `tests/slic3rutils/test_flow_variant_edit.cpp` is in `tests/slic3rutils/CMakeLists.txt`. Tag filter, or `ctest -C Release -R FlowVariantEdit`. |

## #71 cases (`[FlowVariantEdit]`)

- `flow_variant_slots_differ detects Standard vs High flow diverge` — diverge-detect for Both-enter / Copy confirm
- `copy_flow_variant_slot copies domain keys Standard to High flow`
- `copy_flow_variant_slot copies process keys and leaves filament slots alone`
- `copy_flow_variant_slot is a no-op without a High-flow support entry`
- `replicate_flow_variant_value dual-writes one key across mode indices`
- `replicate_flow_variant_value leaves indices past modes.size() intact`
- `replicate_flow_variant_value is a no-op for a single mode`
- `copy_flow_variant_slot visits every filament_flow_variant_options key`

CMake target name: **`slic3rutils_tests`**. CTest prefix from `catch_discover_tests`: `slic3rutils: `.

## Wiring status (other tips)

Only **#71** was checked/fixed on this branch. Sibling tips that already ship new `.cpp` files also list them in that branch’s `tests/libslic3r/CMakeLists.txt` (#64 `TestMD5.cpp`, #66 `test_shortest_path.cpp` + `test_gcode_import.cpp`, #68 `test_edgegrid.cpp`). No extra one-line CMake push was needed on those open branches.

## How this environment verified #71

CMake wiring is already present on this tip:

```
tests/slic3rutils/CMakeLists.txt → add_executable(slic3rutils_tests … test_flow_variant_edit.cpp)
src/slic3r/CMakeLists.txt        → GUI/FlowVariantEdit.cpp / .hpp in SLIC3R_GUI_SOURCES
```

No `deps/build` or configured `build/` tree (and no system Boost). Attempted TU compile:

```
$ g++ -std=c++17 -fsyntax-only -I/workspace/src -I/workspace/tests \
    src/slic3r/GUI/FlowVariantEdit.cpp tests/slic3rutils/test_flow_variant_edit.cpp
In file included from src/libslic3r/libslic3r.h:37,
                 from src/libslic3r/PrintConfig.hpp:19,
                 from src/slic3r/GUI/FlowVariantEdit.hpp:10,
                 from tests/slic3rutils/test_flow_variant_edit.cpp:3:
src/libslic3r/Semver.hpp:8:10: fatal error: boost/optional.hpp: No such file or directory
```

`slic3rutils_tests` still needs `libslic3r_gui` + wx. Run the Windows one-liner above on a machine with `BUILD_TESTS=ON`.
