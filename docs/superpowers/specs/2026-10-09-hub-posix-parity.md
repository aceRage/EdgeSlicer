# Remote hub on macOS and Linux: what was Windows-only, and what the packaging needs (2026-10-09)

Branch `fix/hub-posix-parity`, stacked on `fix/hub-tailscale-posix` (PR 417).

## Camera video: go2rtc and ffmpeg

Windows ships `resources/tools/go2rtc/go2rtc.exe` (checked into the repo, MIT, go2rtc v1.9.14, see
`LICENSE-NOTE.txt`) and, when the build is given `-DFFMPEG_BIN_DIR`, an LGPL `ffmpeg.exe` from
BtbN/FFmpeg-Builds (see `2026-09-12-bundled-ffmpeg.md`). Neither exists for macOS or Linux in the
repo, and nothing here downloads one.

`RemoteHub.cpp` now looks, in order, at `<resources>/tools/go2rtc/{go2rtc,ffmpeg}`, then
`/opt/homebrew/bin` (macOS only), `/usr/local/bin`, `/usr/bin`, then PATH. The hub works with any of
them; with none, camera video is relayed through the hub as before. A system ffmpeg is asked once
(`ffmpeg -hide_banner -encoders`): libopenh264 uses the hub's own encoder template (written for the
bundled LGPL build); libx264 leaves go2rtc's built-in template in place (it is x264's); an ffmpeg
with neither is ignored.

### What the owner or CI has to provide to bundle them

Stage one folder per tool, outside the repo (same convention as ULTRANET_BIN_DIR / FFMPEG_BIN_DIR),
holding the unmodified binary named exactly `go2rtc` / `ffmpeg`:

| | macOS | Linux |
|---|---|---|
| go2rtc | release asset `go2rtc_mac_arm64.zip` / `go2rtc_mac_amd64.zip` of github.com/AlexxIT/go2rtc, tag v1.9.14 (the version Windows ships) | `go2rtc_linux_amd64` / `go2rtc_linux_arm64`, same tag |
| ffmpeg | **no source decided.** BtbN builds Windows and Linux only. A macOS LGPL build (static, libopenh264) needs a source the repo does not use yet; until that is decided macOS relies on a Homebrew ffmpeg or none | BtbN `ffmpeg-N-...-linux64-lgpl` (same project and licence handling as the Windows build) |

Pinned sha256 values must be recorded here when those files are first staged (not done in this
change: nothing was downloaded). Then:

- macOS: `GO2RTC_BIN_DIR=... FFMPEG_BIN_DIR=... GO2RTC_SHA256=<hex> FFMPEG_SHA256=<hex> ./build_release_macos.sh ...`
  (`build_release_macos.sh` copies them into `Contents/Resources/tools/go2rtc`, verifies the sha256 if
  given, and `scripts/macos_sign_app.sh` signs every nested Mach-O for the Developer ID build).
- Linux: `-DGO2RTC_BIN_DIR=... -DFFMPEG_BIN_DIR=...` to CMake; they install under the resources dir.

The build-time copy and CMake rules are untested here (no macOS, no staged binaries). go2rtc's
LICENSE-NOTE.txt and an ffmpeg `FFMPEG-LICENSE.txt` in the same folder are installed beside them.

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
