# Profile validator CI: run EdgeSlicer's own validator (design, 2026-11-04)

Branch `feat/own-profile-validator`. Status: **owner chose option C (self-hosted VM), 2026-11-04;
implemented in `.github/workflows/check_profiles.yml`.**

Implementation notes (differences from the sketch below):
- Source and build dirs are persistent under `C:\Dev\ci\profile_validator\{src,build}`, outside
  the runner workspace. `build_windows_selfhosted.yml` checks out other commits into the shared
  workspace and cleans it, which would turn every incremental build into a near-full rebuild.
- No sccache: Ninja's incremental build in one persistent tree is enough, and sccache with Ninja
  on the VM needs the edgebuild `/showIncludes` wrapper.
- No `concurrency` group shared with the release build: the VM has a single runner, so jobs
  never overlap. The group is per PR, so a newer push cancels the older check.
- A deps-stamp mismatch is a warning, not a rebuild: this job only reads the shared deps tree.
- The workflow also triggers on the engine files that define a valid profile (`PrintConfig.*`,
  `Preset*.*`, the validator source), so an enum change is checked even without a profile edit.
- Upstream's prebuilt binary and #854's TPU285 sed swap are dropped. #854 has to merge main
  (or this PR) to get the new workflow; its swap step then conflicts and should be deleted.

## Why

`.github/workflows/check_profiles.yml` downloads the prebuilt upstream binary
`SoftFever/Orca_tools` release `1` (`OrcaSlicer_profile_validator`) and runs it on
`resources/profiles`. That binary links upstream's libslic3r, so any config value EdgeSlicer's
engine adds (PR #854's `TPU285` nozzle volume type, `nvtTPU285`) is an error to it. #854 works
around that with a scratch copy that sed-swaps `"TPU285"` to `"Standard"`. The fix is to run
our own `Snapmaker_Orca_profile_validator` (`src/dev-utils/`, `-DORCA_TOOLS=ON`), which links
our libslic3r and therefore knows every enum the app knows.

## Closing the two known differences (done on this branch, commit 281aa1e15a)

1. **printer_variant check: ported.** Upstream added it in OrcaSlicer 10a1b17381 (#14084) inside
   `PresetBundle` (not the validator .cpp, which is otherwise the same as ours). Ported the
   engine half into `load_vendor_configs_from_json`'s printer branch: validation mode only, for
   `instantiation == "true"` printers, the `+`-split numeric prefixes of `printer_variant` must
   equal the set of `nozzle_diameter` values. Counts as an error (`++m_errors`), doesn't abort
   the vendor, same as upstream. The commit's profile fixes were already done here
   (housekeeping, 2026-09-12).
2. **OrcaFilamentLibrary empty-manifest fallback: disable it in validation mode.**
   Recommendation: **yes, disable it**, and make an empty `filament_list` a validation error.
   - The manifest is what sets load order (parents before children, see the profile-validators
     memory, Trap 2/6). A disk scan hides an ordering or listing regression that upstream
     loaders (and an upstream re-sync that overwrites the json) would trip over.
   - Our manifest is full today (986 files on disk, 986 entries), so the change has no effect
     now: it only turns a future regression red instead of silently green.
   - The app keeps the fallback (validation-only branch), so users are not affected.
   - Not covered by this (pre-existing gap, same as upstream): a file on disk that is *missing*
     from a non-empty list. If wanted, that is a cheap check to add to
     `scripts/orca_extra_profile_check.py` later; not part of this change.

The Linux-only failure mode (case-only path mismatches, Trap 4) stays covered because the
Python checker keeps running on hosted Ubuntu, where `Path.exists` is case-exact.

## CI cost: options

Data: hosted `ubuntu-24.04` full-app build with a deps-cache hit takes 74-99 min (runs
40614449597, 41156094612). libslic3r is most of that compile. The Ubuntu deps cache on `main`
(`...-3b96bf...`, 109 MiB) predates the 2026-11-01 GMP deps change, so the next hosted run
would rebuild deps first (about 60 min more). `check_profiles.yml` ran 5 times on 2026-11-04
alone.

| Option | Hosted minutes | Validator is built from | Notes |
|---|---|---|---|
| A. Hosted Ubuntu, deps cache + build libslic3r per run | ~50-70 min **per PR run** (+~60 min whenever deps change); ~4-6 h/day at today's rate | PR head | Simple, but the most expensive. ccache via actions/cache could cut repeat runs but the 10 GB cache budget is already ~5.6 GB of flatpak caches. |
| B. Build once on `main`, publish the binary, PRs download it | ~50-70 min per libslic3r-touching main push; PR runs ~2 min | `main` | Does **not** fix the #854 case: a PR that adds an enum and uses it in profiles is checked by a validator that doesn't know the enum yet. |
| C. **Self-hosted Windows VM (`devwin11-lance`, label `edgeslicer-build`)** | **0** (the Python job stays hosted, ~1 min) | PR head | Recommended. Reuses the VM's pre-built deps tree (`C:\Dev\EdgeSlicer\deps\build`) and sccache. |
| D. Self-hosted Mac (`MacMighty`) | 0 | PR head | Fast, but it's the owner's personal Mac, and it's already used as an edgebuild slot. Keep it as the fallback. |

## Recommended: option C

Split `check_profiles.yml` into two jobs:

1. `python_checks` on `ubuntu-24.04` (hosted, unchanged): `orca_extra_profile_check.py`.
2. `validate` on `[self-hosted, Windows, edgeslicer-build]`:
   - `if: github.event.pull_request.head.repo.full_name == github.repository || github.event_name == 'workflow_dispatch'`.
     The repo is **public**, so a fork PR must never run on the VM. Fork PRs get the Python
     job only (plus a notice). Also confirm the repo setting "Require approval for all outside
     collaborators" is on.
   - `concurrency: group: profile-validator-vm` (its own group, so it doesn't queue behind the
     25-min `build-windows-selfhosted` release builds; the VM still runs one at a time per group).
   - Persistent build dir outside the workspace, e.g. `C:\Dev\ci\validator\build`, configured
     once with `-G Ninja -DORCA_TOOLS=ON -DCMAKE_PREFIX_PATH=<shared deps>` and the sccache
     launcher, the same way `build_windows_selfhosted.yml` sets up its env (explicit tool paths,
     `powershell -ExecutionPolicy Bypass`, `-Encoding ascii` for GITHUB_ENV).
   - `ninja Snapmaker_Orca_profile_validator` only (libslic3r plus one .cpp; no GUI).
   - Run with the deps `bin` and `bin/occt` dirs on PATH (OCCT DLLs, `TKernel.dll`), on a
     scratch copy of `resources/profiles` (the validator writes `user/` next to the profiles):
     `Snapmaker_Orca_profile_validator.exe -p <tmp>\profiles -l 2`.
   - Then remove #854's TPU285 swap and the download step.

Expected VM time per run: the first build is libslic3r from scratch (~30 min, from the local
cold build below; less where sccache already holds main's objects from edgebuild VM jobs). After that, profile-only PRs
(most of `check_profiles` traffic) recompile only what `src/` changed between runs, and sccache
covers most of that: a few minutes. Hosted minutes: unchanged ~1 min for the Python job.

Costs and risks of C:
- VM CPU contention with the edgebuild VM slot and release builds. Validator runs are short
  and queue; worst case a PR check waits behind one release build.
- Validator becomes Windows-only. Upstream's Linux binary also caught case mismatches; the
  hosted Python job keeps that coverage (see above).
- If the VM is offline, the `validate` job sits queued. Option: keep the old downloaded
  upstream binary as a non-required, `continue-on-error` hosted job for a release or two as a
  comparison, then delete it.

## Steps after the owner says go

1. Owner: confirm the fork-PR approval setting; OK option C (or pick another).
2. Write the workflow change on this branch (no hosted dispatch needed: the VM job runs on
   the PR itself, and the Python job is about 1 min).
3. Compare the new validator's output with the upstream binary on main's profiles once, locally
   (expect identical results except for EdgeSlicer-only enums).
4. After #854 merges, merge main here and drop its TPU285 swap step.
5. Make `validate` a required check in place of the old job, if branch protection lists it.

## Local verification on this branch

- edgebuild job 1195 (`--cmake-define ORCA_TOOLS=ON --targets Snapmaker_Orca_profile_validator`):
  cold local build, 182 steps, **30m28s** at -j16. On a 4-vCPU hosted runner, expect 2-3x that.
  This is what option A pays on every run without a compiler cache.
- A validator run over all of `resources/profiles` takes **12 s**.
- main's profiles: ours and upstream's release-1 binary both give **59 vendors, 0 errors, pass**.
- #854's profiles (`feat/bbl-h3c-profiles`): ours, built from main, fails on
  `default_nozzle_volume_type ... TPU285`. That's expected (main has no `nvtTPU285`). It shows the
  validator has to be built from the PR head, which rules out option B.
- Negative: X1 Carbon 0.4 machine given `printer_variant "0.6"` -> new error, exit 1.
- Negative: `OrcaFilamentLibrary.json` with `"filament_list": []` -> new error, exit 1.
