# Build and Porting Notes (Windows)

Practical notes for building EdgeSlicer on the maintainer's Windows machine and for porting upstream OrcaSlicer pull requests. For the generic build instructions, see [How-to-build](../How-to-build.md).

## Toolchain locations on this machine

- **CMake**: `C:\dev\tools\cmake-3.31.8-windows-x86_64\bin\cmake.exe`. It is *not* on `PATH`.
- Do **not** use the CMake bundled with VS2019 BuildTools (`C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\...`): it predates the Visual Studio 2022 generator and fails with "Does not match the generator used previously" against the existing `build\` cache.
- The existing `build\` directory is configured for **Visual Studio 17 2022**, Release, install prefix `./Snapmaker_Orca`. Reuse it: `cmake .` in `build\` reconfigures incrementally; no need to rebuild deps.

## Build tips

- The main app target produces **`EdgeSlicer.exe` / `EdgeSlicer.dll`** (the product was renamed; a stale `Snapmaker_Orca.dll` from older builds may still sit in `build\src\Release\` — check timestamps, the renamed files are the live ones).
- Object files land in a nonstandard location, e.g. `build\src\slic3r\libslic3r_gui.dir\Release\*.obj` (not under `CMakeFiles`). When scripting progress checks, glob there.
- To build just one test runner: `cmake --build . --config Release --target libslic3r_tests` (also `fff_print_tests`, `slic3rutils_tests`).
- Long builds: run them detached so host-shell timeouts cannot kill them mid-compile, then poll the log:

  ```powershell
  Start-Process -FilePath 'C:\dev\tools\cmake-3.31.8-windows-x86_64\bin\cmake.exe' `
      -ArgumentList '--build','.','--config','Release','--target','ALL_BUILD','--','-m' `
      -WorkingDirectory 'C:\Dev\SnapmakerOrca\build' `
      -RedirectStandardOutput "$env:TEMP\edgebuild.log" -WindowStyle Hidden
  ```

  After such a build, check for **orphaned `cl.exe` / `MSBuild` node processes** (they survive the parent and keep CPU busy); stop them with `Get-Process cl,MSBuild | Stop-Process -Force`.
- Killing a build mid-compile leaves no `.obj` for the in-flight files, so the next build restarts those files — expect the same filenames to reappear at the log tail; that is normal.
- Full install into the live-testing folder: `cmake --build . --target install --config Release` (installs to `build\Snapmaker_Orca` per the cache prefix).

## Documentation CI (Validate Documentation)

- Workflow: `.github/workflows/validate-documentation.yml`, job name "Check Documentation". Runs on PRs that touch `src/slic3r/GUI/Tab.cpp` or `doc/**/*.md`.
- It validates: markdown links (file existence by basename + heading anchors, normalized to lowercase hyphenated), `Tab.cpp` `append_single_option_line(option, "doc_key#anchor")` references, and image rules (alt text must equal the image filename for the SoftFever raw-URL pattern).
- The script needs **PowerShell 7 (`pwsh`)** syntax (ternary operators); on machines with only Windows PowerShell 5.1, run an adapted copy. The anchors/fragment comparison is case-insensitive via hashtable keys, but heading normalization strips punctuation — keep headings plain.
- When you add a print/filament option with a doc reference, add the matching heading anchor to the target page in the same PR, or CI fails.

## Porting upstream OrcaSlicer PRs — known fork differences

The fork diverges from upstream in ways that break naive ports. Checklist compiled from real port failures (PR #41, Sep 2026):

| Upstream (OrcaSlicer) | EdgeSlicer | Fix when porting |
|---|---|---|
| `def->mode = comExpert;` | No expert tier in the `ConfigOptionMode` enum | Use `comAdvanced` (existing ported options carry the comment `// upstream uses comExpert; this fork has no such tier`) |
| `Point3(x, y, z)` | No `Point3` alias | Use `Vec3crd`; and check the consumer — `ExtrusionPath::polyline` is a 2D `Polyline` here, so appending `Point3(..., 0)` becomes `Point(x, y)` |
| `Point3::to_point()` (drop Z) | Not present | Usually a no-op on already-2D points — just drop the call; otherwise build a `Point(x(), y())` |
| `Polyline::reset_to_linear_move()` (public) | **Private** (BBS arc-fitting state) | Operate on the public `fitting_result` member instead: `clear()`, then `emplace_back(PathFittingData{0, points.size()-1, EMovePathType::Linear_move, ArcSegment()})` |
| `wrapping_detection`, `wtd.bbx`, `rib_offset` | Not present | Omit from sparse-skip rules / footprint estimates (see PR #51 for the pattern) |

General porting workflow that worked well:

1. Merge the upstream diff onto a branch off **current** `main` (both feature branches and `feat/ultra-preferences` move; verify with `git merge-tree --write-tree` before pushing anything).
2. Build `libslic3r` first to shake out type/mode differences, then the test targets — upstream tests often use identifiers that do not exist here.
3. Run the touched Catch2 suites (`ctest -R` or the test exe with a tag filter) before asking for print validation.
4. If the PR adds `Tab.cpp` option lines with doc references, add the doc anchors in the same branch so "Check Documentation" stays green.
