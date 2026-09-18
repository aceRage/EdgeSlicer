# EdgeSlicer Flatpak

Originally a copy of [com.bambulab.BambuStudio](https://github.com/flathub/com.bambulab.BambuStudio),
since diverged. The app id is `io.github.acerage.EdgeSlicer`. The binary and the installed
paths are still named `Snapmaker_Orca` internally — that is deliberate, only the app id,
the bundle name and the user-visible metadata are EdgeSlicer.

## Files

| File | Purpose |
| --- | --- |
| `io.github.acerage.EdgeSlicer.yml` | The flatpak-builder manifest (7 modules). |
| `io.github.acerage.EdgeSlicer.metainfo.xml` | AppStream metadata for GNOME Software et al. |
| `entrypoint` | Launcher: NVIDIA/WebKit DMA-BUF workaround, opt-in Zink, then `exec /app/bin/EdgeSlicer`. |
| `umount` | Redirects umount calls to UDisks2 over D-Bus. |
| `images/` | Screenshots referenced by the metainfo. |

## CI

`.github/workflows/build_all.yml` has a `flatpak` job, matrix `x86_64` (ubuntu-24.04) and
`aarch64` (ubuntu-24.04-arm), in the `ghcr.io/flathub-infra/flatpak-github-actions:gnome-49`
container. It produces:

```
EdgeSlicer_Linux_flatpak_V<ver>_<arch>.flatpak
```

The artifact name matches the bundle name. Three things keep this job from taking the rest
of the workflow down with it:

- `continue-on-error: true` on the job — a red Flatpak never marks the run failed, so the
  mac/Linux/Windows artifacts still publish. The job has no `needs:`, so it was already
  independent; this only stops it from colouring the run.
- `fail-fast: false` on the matrix — the two arches no longer kill each other. On
  2026-09-18 x86_64 hit a flaky 504 and fail-fast cancelled aarch64 *mid-compile*, after it
  had already fetched every source successfully.
- `concurrency: flatpak-<arch>-<ref>` with `cancel-in-progress: false` — a new push queues
  behind a running Flatpak build per arch rather than cancelling it.

### Flaky source downloads

The build fetches ~25 archives before it compiles anything, and one bad response kills the
whole run ("Failed to download sources: module X: The requested URL returned error: 5xx",
surfaced as `The process '/app/bin/xvfb-run' failed with exit code 1` — the xvfb-run message
is just the wrapper, the real error is the line above it).

Sources on single-host CDNs now carry `mirror-urls`, which flatpak-builder tries in order
when the primary fails. The sha256 is unchanged and still enforced, so a mirror cannot
substitute different content. If a new source starts flaking, add a `mirror-urls:` list to
it rather than retrying the job.

### Adding a dependency (the easy one to get wrong)

Module builds run in a sandbox with **no network**. Anything `deps/` downloads must also be
listed as a `type: file` source on the `orca_deps` module with `dest: external-packages/<X>`,
where `<X>` is the project name passed to `Snapmaker_Orca_add_cmake_project` — that is the
`DOWNLOAD_DIR` CMake looks in. Miss it and the build runs for ~20-60 minutes and then dies
with `Could not resolve host: github.com`, which reads like a network blip but is not.

Under `FLATPAK=ON`, `deps/CMakeLists.txt` `find_package()`s ZLIB, PNG, EXPAT, CURL, JPEG,
Freetype and OpenSSL from the SDK, so only those may be left out. Every other dep needs an
entry whose url and sha256 match its `deps/<X>/<X>.cmake`. To check them all:

```sh
python - <<'PY'
import re,yaml,glob
man=yaml.safe_load(open('scripts/flatpak/io.github.acerage.EdgeSlicer.yml',encoding='utf-8'))
dests={s['dest'].split('/',1)[1].lower() for m in man['modules'] for s in m.get('sources',[])
       if str(s.get('dest','')).startswith('external-packages/')}
sdk={'zlib','png','expat','curl','jpeg','freetype','openssl'}
proj=[re.search(r'add_cmake_project\(\s*(\w+)',open(f,encoding='utf-8',errors='ignore').read())
      for f in glob.glob('deps/*/*.cmake')]
miss=[m.group(1) for m in proj if m and m.group(1).lower() not in dests|sdk]
print("UNRESOLVED:", miss or "none")
PY
```

### Known blockers (2026-09-18)

The Flatpak layer itself is fixed — sources all resolve, all 23 deps are accounted for, and
both arches build for an hour before failing. What remains are pre-existing source bugs
that are not Flatpak-specific and are owned elsewhere:

- **QuadriFlow does not compile on GCC 13** (`loader.cpp`, missing `<cstdint>`, which
  cascades into `'it' was not declared` and `VertexMap {aka 'int'}`). Being fixed on
  `fix/quadriflow-gcc13-cstdint`. This is what x86_64 hits, ~35 min in.
- **`src/libslic3r/FillBedPack.hpp` is not self-contained**: it uses `Polygon`,
  `ExPolygon` and `BoundingBox` but only includes `Point.hpp`. aarch64 gets *past*
  `orca_deps` entirely and dies here, ~62 min in, while compiling the slicer.

Both need to land before a Flatpak bundle can be produced. Run 35358030365 is the reference:
x86_64 = QuadriFlow, aarch64 = FillBedPack.

## Attaching bundles to a release

The Flatpak jobs take 60-90 minutes, far longer than the other builds, so they are a second
pass over a release that already has its other assets:

```sh
scripts/release/upload_flatpak_assets.sh <run-id> <version> [repo] [tag]

# e.g. after the build_all.yml run for 2.3.8.5 finishes:
scripts/release/upload_flatpak_assets.sh 35400000000 2.3.8.5
```

Defaults are `aceRage/EdgeSlicer` and tag `v<version>-edge`. It downloads both `.flatpak`
artifacts from the run, uploads them with `--clobber`, and *appends* their sha256 lines to
the release's existing `SHA256SUMS.txt` (replacing any prior line for the same file name)
rather than rewriting it — the main release script owns the other entries. Safe to re-run.
If only one arch went green it uploads that one and reports the other as `UFA_MISSING`.

## Flathub: what it would take

Nothing here is submitted to Flathub, and the manifest as it stands cannot be. Flathub
builds on its own infrastructure with **no network access during the build**, so every
source must be independently fetchable and pinned. Current blockers:

1. **`orca_deps` and `Snapmaker_Orca` use `type: dir` with `path: ../../`.** This bakes the
   local checkout in. Flathub needs a `type: git` source with an explicit `commit:` (a tag
   is not enough — it must be an immutable sha) or a release tarball with a `sha256`. This
   is the big one: it means cutting a real source tarball or tagged commit per release and
   updating the manifest each time.
2. **`orca_wxwidgets` uses `branch: master`.** A moving branch is not reproducible; it has
   to become a pinned `commit:`. Note it also uses `path: ../`, which has the same problem
   as (1).
3. **Bundled dependency downloads.** `orca_deps` runs the project's own CMake dep fetcher.
   The archives are already listed as `type: file` sources with sha256 into
   `external-packages/`, which is the right shape — this part is mostly fine, but it needs
   verifying that the CMake scripts never reach the network for anything not listed.
4. **Metainfo.** Screenshots must be reachable and stable (they now point at this repo's
   `scripts/flatpak/images/`, which works), and `<releases>` should carry a real entry per
   released version. `appstreamcli validate` must pass clean, including OARS content rating
   and a description longer than the current single sentence.
5. **The submission itself.** Fork `flathub/flathub`, open a PR on the `new-pr` branch with
   the manifest, respond to the reviewer, then maintain the repo Flathub creates for the
   app (each release becomes a PR bumping the pinned commit).
6. **Licensing/branding review.** Flathub checks that the app id matches a domain or repo
   you control (`io.github.acerage` matches `github.com/aceRage`, so that is fine) and that
   bundled artwork is properly licensed — worth a look given the imported vendor renders.

**Rough effort:** 2-4 days of focused work to get an offline-buildable manifest (most of it
on items 1 and 2, since pinning `orca_deps` means restructuring how the source tree reaches
the build), plus a 1-3 week review turnaround that is mostly waiting, and then ongoing
per-release maintenance of roughly an hour to bump and verify the pinned commits. The
GitHub-release Flatpak bundles this CI produces are the cheaper path and stay useful either
way; Flathub buys discoverability and automatic updates, not a better bundle.
