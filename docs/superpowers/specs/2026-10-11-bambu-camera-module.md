# Bambu camera module: shipped, and kept apart from UltraNet's storage module

Owner decision, 2026-10-11: EdgeSlicer ships Bambu's own camera files for Bambu printer live view
until our go2rtc-based desktop live view is proven on every platform (it is not yet on macOS). This is
an exception for the camera files only. Bambu's network plug-in (`bambu_networking`) is still never
shipped or downloaded, and UltraNet stays the network plug-in.

## The problem

Two different libraries carry Bambu's one file name (`BambuSource.dll`, `libBambuSource.dylib`):

| | UltraNet's storage module | Bambu's camera module |
|---|---|---|
| What it does | the storage browser's LAN tunnel (`Bambu_Open` & co., `EdgeSlicer_TunnelLanOnly`) | live video: the DirectShow source filter on Windows, the `BambuPlayer` Objective-C class on macOS |
| Loaded by | `PrinterFileSystem` through `NetworkAgent::get_bambu_source_entry()` | Windows Media Player (COM, by registered path) / `wxMediaCtrl2.mm` |
| Needs | nothing | `live555` next to it for RTSP/RTSPS cameras (X1, H2D, H2C); the Agora libraries only for cloud P2P, which we do not use |
| Comes from | UltraNet (our sidecar) | Bambu (signed by Bambu) |

On macOS both features loaded **the same file**, `<data_dir>/plugins/libBambuSource.dylib`
(`wxMediaCtrl2.mm` and `PrinterFileSystem.cpp`). 2.4.5.4 put UltraNet's there, so every Bambu camera
failed ("LAN Connection Failed (Failed to start liveview)", no BambuTunnel activity). The owner proved
by hand that Bambu's `libBambuSource.dylib` + `liblive555.dylib` (universal, team T3UBR9Y3B2,
June 2025) give working video there - and that storage/timelapse then breaks.

## How Windows live view worked before this change

Not through `cameratools`. On Windows the Device-tab player is `wxMediaCtrl2.cpp`: Windows Media
Player / DirectShow opens the `bambu:` URL through the source filter **registered** under
`HKCR\bambu\Source Filter` = `{233E64FB-2041-4A6C-AFAB-FF9BCF83E7AA}`, whose
`CLSID\{...}\InprocServer32` names the DLL. EdgeSlicer never loaded the filter itself.

On the owner's PC that registration (in `HKCU\Software\Classes\CLSID`) points at
`%APPDATA%\OrcaSlicer\plugins\BambuSource.dll` - Bambu's 5.5 MB filter that OrcaSlicer downloaded,
with `live555.dll` beside it - so live view works although `%APPDATA%\EdgeSlicer\cameratools` does
not exist and `%APPDATA%\EdgeSlicer\plugins\BambuSource.dll` is UltraNet's 2.2 MB storage module.
(The owner's logs from the last days hold no live-view session; the registry is what shows the
path.) `<data_dir>\cameratools` was only ever used by the **virtual camera** (`bambu_source.exe` +
ffmpeg, `MediaPlayCtrl::start_stream_service`), which EdgeSlicer does not ship.

So a fresh Windows install with no OrcaSlicer/Bambu Studio had no working live view either: it fell
into the "download Bambu's camera component" offer. **Windows needs the files shipped too.**

## Layout after this change

| | Windows | macOS |
|---|---|---|
| App bundle | `<exe dir>\cameratools\BambuSource.dll`, `live555.dll` | `EdgeSlicer.app/Contents/Resources/cameratools/libBambuSource.dylib`, `liblive555.dylib` |
| Used from | `<data_dir>\cameratools\` (seeded at start-up) | `<data_dir>/cameratools/` (seeded at start-up), falling back to the bundle's folder |
| Storage module | `<data_dir>\plugins\BambuSource.dll` (UltraNet, unchanged) | `<data_dir>/plugins/libBambuSource.dylib` (UltraNet, unchanged) |

Linux is unchanged (one module, as before).

### Start-up (`GUI_App::sync_bambu_camera_module` → `PluginGuard::sync_camera_module_files`)

Runs on Windows and macOS before the network plug-in loads, logs every step as `[camera module] ...`:

1. **plugins/ repair.** If `plugins/<name>` is Bambu's camera module (it exports `DllRegisterServer`
   / the `BambuPlayer` class), it moves to `cameratools/` - or is dropped when `cameratools/` already
   has the identical file, or parked as `cameratools/<name>.from-plugins` when `cameratools/` has a
   different camera module. `plugins/<live555>` always moves out the same way (UltraNet ships none).
2. **Storage module back.** If `plugins/<name>` is now missing, UltraNet's module is copied back
   from the app's sidecar; with no sidecar (a developer build) the hand-kept
   `plugins/<name>.ultranet` is renamed back - only if it really is UltraNet's module.
3. **Seed cameratools** from the app's bundled folder: install when missing, refresh a copy an
   earlier EdgeSlicer installed (`cameratools/edgeslicer_bundled.txt` records size + FNV-1a hash of
   what we put there), replace junk, **never overwrite a real camera module the user supplied**, and
   never install a bundled file that is not Bambu's camera module (a packaging mistake is logged).

The owner's current Mac state - Bambu's `libBambuSource.dylib` + `liblive555.dylib` in `plugins/`,
UltraNet's kept as `libBambuSource.dylib.ultranet` - therefore resolves on the first start of this
build: the two Bambu files move to `cameratools/`, UltraNet's module comes back from the app's
sidecar, the `.ultranet` copy is left in place (logged), and live view and storage both work. The
`ultranet_keep_foreign_plugin` escape hatch skips steps 1-2.

### Telling the files apart (no loading)

`PluginGuard::classify_camera_module()` reads the file:
- PE: camera when it exports `DllRegisterServer`; UltraNet's when it exports `EdgeSlicer_TunnelLanOnly`.
- Mach-O (thin or universal): camera when it exports `_OBJC_CLASS_$_BambuPlayer`, looked up in the
  symbol table and in the dyld export trie (`LC_DYLD_INFO(_ONLY)` / `LC_DYLD_EXPORTS_TRIE`);
  UltraNet's when it exports `_EdgeSlicer_TunnelLanOnly`.
- Anything carrying the `UltraNet-module` tag is UltraNet's, whatever else it exports.

This replaces the PE-only `DllRegisterServer` check for macOS (`is_real_camera_component`).

### macOS player (`wxMediaCtrl2.mm`)

`load_bambu_camera_module()` loads `libBambuSource.dylib` from `<data_dir>/cameratools`, else the
bundle's `Contents/Resources/cameratools`, only if it is Bambu's camera module. `liblive555.dylib`
from the same folder is `dlopen`ed first by full path (`RTLD_NOW | RTLD_GLOBAL`), so the module's
own run-time load of live555 finds it whatever search path it uses (the owner's hand test shows it
resolves live555 relative to its own folder: both files in one folder worked with the app's cwd at
`/`). `PrinterFileSystem` keeps loading `plugins/libBambuSource.dylib` (UltraNet). Two images of the
same name load side by side from different full paths.

### Windows player (`wxMediaCtrl2.cpp`)

`filter_registration_decision`: a registered filter that works (CLSID matches and the path is a
real filter) is used as it is - including another app's, so OrcaSlicer/Bambu Studio and EdgeSlicer
do not fight over the registration. A missing registration, a dangling path, or one pointing at a
non-filter (UltraNet's module after the repair) is replaced by **ours**, registered **per user**
without UAC: `HKCU\Software\Classes\CLSID\{233E...}\InprocServer32` = `<data_dir>\cameratools\BambuSource.dll`,
`ThreadingModel=Both`, and `HKCU\Software\Classes\bambu\Source Filter` = the CLSID (exactly what the
filter's own registration writes, observed on a working install). Only with no camera files at all
does the old "download Bambu's camera component" offer remain; that download now also installs into
`cameratools` only, never `plugins`.

## Packaging

`CAMERA_BIN_DIR`, parallel to `ULTRANET_BIN_DIR`, names a folder holding the two files:

- Windows: `BambuSource.dll` + `live555.dll` → `CMakeLists.txt` installs them to `./cameratools`
  (`build_release_vs2022.bat` passes `-DCAMERA_BIN_DIR`). The VM workflow probes
  `C:\Dev\ship\cameratools` like the other payloads and fails the run if a found file is missing from
  the package.
- macOS: `libBambuSource.dylib` + `liblive555.dylib` → `build_release_macos.sh` copies them to
  `Contents/Resources/cameratools` and refuses a `libBambuSource.dylib` without the `BambuPlayer`
  export (UltraNet's by mistake). Universal dylibs for a universal app (`lipo -archs` is printed).
- Only those named files: never `bambu_networking`, never the Agora cloud libraries.
- **Bambu's files are never committed to this public repo** and never downloaded by a build or a
  test (the tests use synthetic PE/Mach-O images).

### Signing

- **Windows.** `windows_sign.ps1` leaves validly signed files alone, so Bambu's DLLs keep Bambu's
  Authenticode signature (CN "Shenzhen Tuozhu Technology Co., Ltd." or "Shanghai Lunkuo Technology
  Co., Ltd" - both seen on Bambu's camera DLLs). `windows_verify_signatures.ps1 -AllowBambuCameraFiles`
  accepts exactly that signer on exactly `cameratools\BambuSource.dll` and `cameratools\live555.dll`
  (Valid, embedded, timestamped; reported `OK-3P`). Not "unsigned allowed". Passed by both verify
  steps of `build_windows_selfhosted.yml`, the CPack staging check and the release publisher
  template. Tested against Bambu-signed DLLs: without the switch they FAIL "unexpected signer"; with
  it they pass in `cameratools\` (folder and zip) and still FAIL anywhere else.
- **macOS.** The app is notarized with the hardened runtime and
  `com.apple.security.cs.disable-library-validation = true`. CI signs with
  `scripts/macos_sign_app.sh`, which already signs **every** nested Mach-O (any folder, including
  `Contents/Resources/cameratools`) with our Developer ID, `--options runtime --timestamp`, before
  sealing the bundle - so Bambu's dylibs are re-signed with ours. That is what notarization needs:
  every Mach-O in the submission must carry a Developer ID signature with a secure timestamp, and
  re-signing guarantees it rather than depending on how Bambu signed a given build. Loading needs
  nothing more: disable-library-validation admits them either way. `scripts/sign_and_package.sh`
  (the manual path) did **not** sign anything under `Contents/Resources` (neither UltraNet's sidecar
  nor these files) and now does (step 2.2b). Local `build_release_macos.sh` builds keep Bambu's
  signature (no hardened runtime there, so nothing to satisfy).

## CI source for the files (proposed; the owner creates these)

Same pattern as UltraNet:

1. **Private repo `aceRage/edgeslicer-camera`** with
   - `macos/libBambuSource.dylib`, `macos/liblive555.dylib` (universal, Bambu's, unmodified)
   - `windows/BambuSource.dll`, `windows/live555.dll` (Bambu-signed, unmodified)
   - `SHA256SUMS` at the root listing all four (`sha256sum macos/* windows/* > SHA256SUMS`), which
     CI checks before staging.
2. **Read-only deploy key** on that repo, private half in the EdgeSlicer secret
   **`CAMERA_DEPLOY_KEY`**. `build_orca.yml` ("Camera files - ..." steps) then fetches the repo,
   moves it out of the workspace, verifies `SHA256SUMS`, stages the platform's two files and sets
   `CAMERA_BIN_DIR` for the macOS and hosted Windows builds; without the secret it skips, as before.
3. **VM folder `C:\Dev\ship\cameratools`** holding `BambuSource.dll` + `live555.dll` for the
   self-hosted Windows release build (`build_windows_selfhosted.yml` probes it like
   `C:\Dev\ship\ultranet`).
4. Publisher (`snorca_hubtest/rel_signed_template.sh`): verifies with `-AllowBambuCameraFiles` and,
   with `SHIP_CAMERA=1` (default), aborts when the portable zip lacks `cameratools/BambuSource.dll`.

## Owner's local Mac test

1. Copy Bambu's two dylibs into a folder, e.g. `~/edge-camera/` (`libBambuSource.dylib`,
   `liblive555.dylib`).
2. Build:
   `git checkout -B mac-test origin/feat/bambu-camera-module`
   `CAMERA_BIN_DIR=~/edge-camera ULTRANET_BIN_DIR=<ultranet out> ./build_release_macos.sh -s -x -a arm64`
3. Run it on a scratch data dir first:
   `build/arm64/Snapmaker_Orca/EdgeSlicer.app/Contents/MacOS/EdgeSlicer --datadir ~/edge-test-data`
   and look for `[camera module]` lines in that data dir's `log/`.
4. Check live view on H2D, H2C and X1C **and** that the storage browser / timelapse still work.
5. Then run it once on the real data dir (the mixed `plugins/` state): the log should show the two
   files moved to `cameratools/` and the storage module restored from the sidecar.
