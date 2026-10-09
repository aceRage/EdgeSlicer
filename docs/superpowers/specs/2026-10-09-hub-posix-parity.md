# Remote hub on macOS and Linux: what was Windows-only, and how go2rtc / ffmpeg get there (2026-10-09)

Branch `fix/hub-posix-parity`, stacked on `fix/hub-tailscale-posix` (PR 417).

## Camera video: go2rtc and ffmpeg

Windows ships `resources/tools/go2rtc/go2rtc.exe`, checked into the repo (MIT, AlexxIT/go2rtc; see
`LICENSE-NOTE.txt`), and, when the build is given `-DFFMPEG_BIN_DIR`, an LGPL `ffmpeg.exe` from
BtbN/FFmpeg-Builds (see `2026-09-12-bundled-ffmpeg.md`).

`RemoteHub.cpp` looks, in order, at `<resources>/tools/go2rtc/{go2rtc,ffmpeg}`, then
`/opt/homebrew/bin` (macOS only), `/usr/local/bin`, `/usr/bin`, then PATH. With neither tool found,
camera video is relayed through the hub as before.

### go2rtc on macOS: pinned, fetched in CI, never in the repo

The committed `go2rtc.exe` is go2rtc **v1.9.14**: `LICENSE-NOTE.txt` says so and the version string
`1.9.14` is in the binary (found by searching the file's bytes; it was not run). Its sha256 is
`923d57252e8139a69c52e4acc1e399a640244a8ef457fd9b7267a25847d68f8c`. The macOS assets below are the
same upstream and the same tag, so there is no version mismatch between platforms.

| Asset (github.com/AlexxIT/go2rtc/releases/download/v1.9.14/) | sha256 of the zip |
|---|---|
| `go2rtc_mac_arm64.zip` | `919b78adc759d6b3883d1e1b2ac915ac0985bb903ff1897b4d228527bd64690c` |
| `go2rtc_mac_amd64.zip` | `9b0b9a27a4dc3a5b8b93376e7e8fc2787c6af624a512842622be84aec0171c7a` |

The arm64 binary inside the zip is `6e5039d56d652d2d325cb3ce3f7cc20248a34db42fe8abf9bb13488494ac023e`
(`go2rtc version 1.9.14 (b5948cf) darwin/arm64`, verified by the owner). The amd64 inner digest has
not been recorded.

`.github/workflows/build_orca.yml` (macOS job only, before `build_release_macos.sh`) downloads the
two zips with curl, checks both zip digests and the arm64 inner digest, unzips, joins the slices with
`lipo -create` into one universal `go2rtc` (the Mac build is always universal), writes a
LICENSE-NOTE.txt beside it, and exports `GO2RTC_BIN_DIR`. `build_release_macos.sh` copies it to
`Contents/Resources/tools/go2rtc`, and the existing Developer ID signing step signs it with every
other nested Mach-O. A later step fails the build if go2rtc is not in the bundle.

`GO2RTC_SHA256` is **not** exported by CI: the universal binary is a new file whose digest is not one
of the pinned values, so the optional binary check in `build_release_macos.sh` is skipped there and
the trust sits in the zip digests (and the arm64 inner digest). The script's check is for a local
build with a single-arch binary, e.g. the owner's:
`GO2RTC_BIN_DIR=~/Dev/go2rtc GO2RTC_SHA256=6e5039d56d652d2d325cb3ce3f7cc20248a34db42fe8abf9bb13488494ac023e ./build_release_macos.sh ...`.

Nothing in this change downloaded or ran any of these binaries; the CI step has not been run.

For Linux, `-DGO2RTC_BIN_DIR=<folder with go2rtc>` installs the file under the resources dir; there
is no Linux CI step and no pinned digest yet. go2rtc is not in Homebrew, so there is no package-manager
fallback on macOS (a hand-installed one in `/usr/local/bin` is still found).

### ffmpeg on macOS: system fallback only, never bundled

Owner decision: no bundled ffmpeg on the Mac. The hub uses an ffmpeg it finds (Homebrew's, normally
`/opt/homebrew/bin/ffmpeg`), after asking it once for its encoders (`ffmpeg -hide_banner -encoders`):

| macOS preference | Encoder | go2rtc `ffmpeg: h264:` |
|---|---|---|
| 1 | `h264_videotoolbox` (hardware) | `-codec:v h264_videotoolbox -profile:v main -realtime 1 -bf 0` |
| 2 | `libopenh264` | the hub's template (written for the bundled Windows build) |
| 3 | `libx264` | none written: go2rtc's built-in (x264) template |

Off macOS hardware encoders are left alone (openh264, then x264). An ffmpeg with none of these is
treated as no ffmpeg. The VideoToolbox template is explicit rather than go2rtc's `#hardware` selector
so that what runs is stated in one place; the per-variant `-b:v` / `-g:v` the hub appends are honoured by
VideoToolbox. It is unverified on hardware: if a Mac shows a black tile for the Medium/Low variants,
this template is the first suspect (a one-line change to `h264_template_override`).

For Linux, a bundled LGPL build (BtbN `linux64-lgpl`, same project as the Windows one) would go through
`-DFFMPEG_BIN_DIR`; nothing is staged and no digest is pinned.

## Process handling

go2rtc is a child the hub owns (`HubPlatform::spawn_child_posix`: fork + exec, no shell, own process
group, output to `<hub dir>/go2rtc.log`). `HubServer::loop()` reaps it if it ends; `shutdown()`
sends SIGTERM, then SIGKILL, and reaps. A hub killed outright leaves go2rtc running (Windows has the
kill-on-close job object); `go2rtc.pid` records it and the next hub stops it only if that pid's
executable really is this go2rtc.

## Not changed

Windows paths, wording and behaviour, apart from two shared fixes: a `tailscale` run that times out
is now "did not answer" instead of "not installed", and `tailscale serve` that prints the
admin-console link it is waiting on is stopped at once with the instruction to enable Serve/HTTPS.
