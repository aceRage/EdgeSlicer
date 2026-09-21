# Repository Guidelines

## Project Structure & Module Organization
Snapmaker_Orca’s C++17 sources live in `src/`, split by feature modules and platform adapters. User assets, icons, and printer presets are in `resources/`; translations stay in `localization/`. Tests sit in `tests/`, grouped by domain (`libslic3r/`, `sla_print/`, etc.) with fixtures under `tests/data/`. CMake helpers reside in `cmake/`, and longer references in `doc/` and `SoftFever_doc/`. Automation scripts belong in `scripts/` and `tools/`. Treat everything in `deps/` and `deps_src/` as vendored snapshots—do not modify without mirroring upstream tags.

## Build, Test, and Development Commands
Use out-of-source builds:
- `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release` configures dependencies and generates build files.
- `cmake --build build --target Snapmaker_Orca --config Release` compiles the app; add `--parallel` to speed up.
- `cmake --build build --target tests` then `ctest --test-dir build --output-on-failure` runs automated suites.
Platform helpers such as `build_linux.sh`, `build_release_macos.sh`, and `build_release_vs2022.bat` wrap the same flow with toolchain flags. Use `build_release_macos.sh -sx` when reproducing macOS build issues, and `scripts/DockerBuild.sh` for reproducible container builds.

## Coding Style & Naming Conventions
`.clang-format` enforces 4-space indents, a 140-column limit, aligned initializers, and brace wrapping for classes and functions. Run `clang-format -i <file>` before committing; the CMake `clang-format` target is available when LLVM tools are on your PATH. Prefer `CamelCase` for classes, `snake_case` for functions and locals, and `SCREAMING_CASE` for constants, matching conventions in `src/`. Keep headers self-contained and align include order with the IWYU pragmas.

## Testing Guidelines
Unit tests rely on Catch2 (`tests/catch2/`). Name specs after the component under test—for example `tests/libslic3r/TestPlanarHole.cpp`—and tag long-running cases so `ctest -L fast` remains useful. Cover new algorithms with deterministic fixtures or sample G-code stored in `tests/data/`. Document manual printer validation or regression slicer checks in your PR when automated coverage is insufficient.

## Commit & Pull Request Guidelines
The history favors concise, sentence-style subject lines with optional issue references, e.g., `Fix grid lines origin for multiple plates (#10724)`. Squash fixups locally before opening a PR. Complete `.github/pull_request_template.md`, include reproduction steps or screenshots for UI changes, and mention impacted presets or translations. Link issues via `Closes #NNNN` when applicable, and call out dependency bumps or profile migrations for maintainer review.

## Security & Configuration Tips
Follow `SECURITY.md` for vulnerability reporting. Keep API tokens and printer credentials out of tracked configs; use `sandboxes/` for experimental settings. When touching third-party code in `deps_src/`, record the upstream commit or release in your PR description and run the relevant platform build script to confirm integration.

## Mesh Fixture & Draw-Cut Test Gotchas
- `its_make_sphere(radius, fa)`'s second argument is a **facet angle in radians**, not a length. The common `0.6` gives ~34° facets; points computed on the analytic sphere then float millimetres above the faceted surface. Use `M_PI / 180.0` for ~1° facets when samples must sit on the mesh.
- `its_make_cube(x, y, z)` is corner-origin (spans `0..x, 0..y, 0..z`); `its_make_cylinder(r, h, fa)` stands on `z == 0`. Translate manually to center fixtures.
- Resampled draw-cut stroke points sit a few microns inside the faceted skin (resample chord sagitta plus float32 vertex quantization). Geometric hit tests fired from stroke samples need a hit floor well above that noise (0.05 mm in `DrawCut.cpp`'s `ray_first_hit`), or a ray leaving the body clamps back onto the skin.
- On draw-cut changes, run `libslic3r_tests.exe "[DrawCut]"` from `build/tests/libslic3r/Release`; the suite is the fast gate (~92 cases), a full Snapmaker_Orca rebuild is only needed for the deliverable.
- From Git Bash, MSBuild switches must be dash-style (`-- -m -v:m`); slash-style `/m /v:m` gets path-mangled into `M:/`.
