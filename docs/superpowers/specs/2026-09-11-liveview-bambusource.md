# Live view offers to register a DLL that cannot be registered

Branch `fix/liveview-bambusource`, from `feat/ultra-preferences` at `621bc91496`.

Windows, Bambu X2D, fresh EdgeSlicer install. The user opens the Device tab and presses **Play** on
the live view. They get:

> BambuSource has not correctly been registered for media playing! Press Yes to re-register it.
> You will be promoted twice

They press Yes, approve **two** UAC prompts, and regsvr32 finishes with:

> The module `...\plugins\BambuSource.dll` was loaded but the entry-point DllRegisterServer was not
> found.

Nothing is fixed, and pressing Play again repeats the whole dance.

## Cause

Two different components share one file name, and the code treats them as the same thing.

**The network plug-in** is `bambu_networking.dll`. EdgeSlicer ships its own clean-room replacement
("UltraNet") in `<data_dir>\plugins`, identified by the `ultranet.txt` marker beside it. Because
Bambu's CDN package would overwrite ours, every download path is deliberately shut off while the
marker is present (`GUI_App.cpp`: the `m_ultranet_plugin_installed` guards in
`check_networking_version`, `install_plugin`, the download dialog, the home banner).

**The camera component** is `BambuSource.dll`. It is a proprietary DirectShow *source filter* - a
COM server registered under CLSID `{233E64FB-2041-4A6C-AFAB-FF9BCF83E7AA}`, which is what live view
actually plays through. It is not something UltraNet replaces and we cannot clean-room it. It only
ever arrives inside Bambu's network plug-in package - the package we just disabled the download of.

So that the agent's `LoadLibrary` probe of the plug-ins folder still succeeds, EdgeSlicer ships a
**stub** named `BambuSource.dll`. Measured on the reporter's machine:

| file | size | exports `DllRegisterServer` |
| --- | --- | --- |
| EdgeSlicer `plugins\BambuSource.dll` (our stub) | 9,728 B | no - **no export directory at all** |
| Bambu Studio `plugins\BambuSource.dll` (the real filter) | 5,521,440 B | yes (139 exported names) |

The stub has no `DllRegisterServer`, so `regsvr32` on it can only ever produce the reported error.
Two concrete breakages followed:

1. **`src/slic3r/GUI/wxMediaCtrl2.cpp` (~80-140).** When the CLSID is unregistered, the code offers
   to `regsvr32 <plugins>\BambuSource.dll` as long as *some* file exists at that path. On EdgeSlicer
   that file is always the stub, so the offer is guaranteed to fail. A comment already at line ~140
   acknowledged the stub cannot be registered, but only suppressed a *different* warning.

2. **`src/slic3r/GUI/MediaPlayCtrl.cpp` (~770).** `start_stream_service()` copies
   `<plugins>\BambuSource.dll` over `<data_dir>\cameratools\` whenever the timestamps differ. A user
   who obtained the real filter and put it in `cameratools` gets it **overwritten by the stub** on
   the next Play - so even a correct manual fix does not survive.

## Change

### 1. Telling the stub from the real filter

`src/slic3r/GUI/PluginGuard.{hpp,cpp}` - the existing home of the "whose plug-in is this?"
decisions - gains the camera-component equivalents.

```cpp
const char *bambu_source_library_name();                                // per platform
bool exports_dll_register_server(const boost::filesystem::path &dll);
bool is_ultranet_bambusource_stub(const boost::filesystem::path &dll);
```

`exports_dll_register_server()` parses the **PE export directory off the file on disk** - DOS
header, PE signature, optional header (PE32 vs PE32+ change where data directory 0 lives), section
table, then the exported-name array. It deliberately does *not* use `LoadLibrary` +
`GetProcAddress`: that would run `DllMain` of a binary we have already decided we do not trust, and
would pin the file so a later install could not replace it. Every read is bounds-checked and the
name walk is capped, so a malformed or hostile image answers "not a real filter" rather than
misbehaving - which is the safe direction, since it only ever suppresses a registration attempt.

`is_ultranet_bambusource_stub()` requires **both** halves: the `ultranet.txt` marker in the same
folder *and* the absence of `DllRegisterServer`. Both matter. A real filter dropped into our
plug-ins folder still has the marker beside it and must be recognised as real; an unexporting DLL in
a folder with no marker is not ours to judge.

Two pure decisions were factored out so they can be unit-tested without a filesystem:

```cpp
CameraToolsCopy camera_tools_copy_decision(bool plugins_copy_is_stub,
                                           bool cameratools_has_real_filter,
                                           bool cameratools_up_to_date);
bool may_overwrite_bambusource(bool dest_exists, bool dest_is_real_filter);
```

### 2. `start_stream_service()` never copies the stub

`MediaPlayCtrl.cpp`. The blind timestamp copy now asks `camera_tools_copy_decision()` for
`BambuSource` specifically (`live555.dll` keeps the stock timestamp rule):

* `cameratools` already holds a real filter → **KeepExisting**. Never overwritten, by anything.
  This is the fix for breakage 2.
* the plug-ins copy is our stub and `cameratools` has nothing usable →
  **SkipStubMissingComponent**: no copy, and `start_stream_service()` returns false with
  `need_install = false`, which `ToggleStream()` now turns into the component offer below instead
  of a mute return.
* otherwise → stock behaviour.

### 3. The registration prompt becomes a component offer

`wxMediaCtrl2::Load()` now branches three ways when the CLSID is not registered:

* the plug-ins DLL **is our stub** → do *not* offer regsvr32. Show
  `GUI_App::offer_bambu_camera_component()` instead.
* a real filter is there but unregistered → the stock re-register prompt, which now routes through
  `GUI_App::register_bambu_source_filter()`.
* nothing there at all → the same component offer. On a fresh EdgeSlicer install this is the same
  story as the stub, so telling the user to "re-install BambuStudio" (the old message) was wrong.

`GUI_App::register_bambu_source_filter()` is the regedit + regsvr32 pair lifted out of
`wxMediaCtrl2.cpp`, with the guard that was missing: **it refuses outright unless the DLL exports
`DllRegisterServer`.** The two UAC prompts can no longer be spent on a file that cannot use them.

### 4. Fetching the component

`GUI_App::install_bambu_camera_component()` is a dedicated, deliberately narrow exception to the
UltraNet download guard.

* It reuses `download_plugin("plugins", ...)` - which only *fetches*, and so needs no guard change -
  with the existing `get_plugin_url()` version/country logic. It does **not** call
  `install_plugin()`, which unzips the whole package over `plugins/` and stays refused.
* From the zip it extracts **only** `BambuSource` and `live555`, matched on the bare filename.
  `bambu_networking.dll` and `ultranet.txt` are never touched.
* Each file is extracted to a temp path first and the `BambuSource` candidate is re-checked with
  `exports_dll_register_server()` before installation. A package that somehow yields an
  unregistrable filter is refused rather than recreating this very bug.
* On success both `plugins\` and `cameratools\` get the file, then the filter is registered.

`offer_bambu_camera_component()` shows the one dialog the user should see:

> Live view needs Bambu's camera component (BambuSource.dll), which is not part of EdgeSlicer's
> network plug-in. Download it from Bambu Lab now?

Yes → busy-cursor download, then registration (the UAC prompts, now meaningful), then the caller
retries playback. Failure → a plain error naming the CDN result code. The
`DownloadProgressDialog`/`UpgradeNetworkJob` pair was *not* reused, because its job routes through
`install_plugin()`, which must stay refused here.

### 5. An upgrade keeps a component the user downloaded

The first-run copier in `GUI_App.cpp` (~3055) copies the sidecar `ultranet\BambuSource.dll` into
`plugins\`. It now consults `may_overwrite_bambusource()` and **skips** when a real filter is
already there, so the component survives EdgeSlicer upgrades. (The neighbouring "unmarked plug-in
byte-identical to sidecar" path only writes the marker file and never copies `BambuSource`, so it
needed no change.)

### 6. macOS / Linux

The "never copy the stub over cameratools" rule is platform-independent and applies as written; the
library name comes from `bambu_source_library_name()` (`libBambuSource.dylib` / `.so`). The
**download path is Windows-only for now** - `offer_bambu_camera_component()` says so plainly on
other platforms rather than pretending to fetch something. There is no registrable DirectShow filter
outside Windows, and the CDN package layout for the other platforms was not verified (see below).

## Package layout: not verified

The one permitted read-only probe,
`https://api.bambulab.com/v1/iot-service/api/slicer/resource?slicer/plugins/cloud=02.01.01.00`,
returned **HTTP 402 Payment Required** - the endpoint is account/region gated. The zip was therefore
never fetched and its internal layout is **unconfirmed**.

The extraction code is written not to depend on it: entries are matched on the **bare filename**
(`boost::iequals` on `path.filename()`), so a flat zip and one that nests the DLLs under a folder
both work. The layout claim that *is* evidence-backed is that the real filter and `live555.dll` ship
together in the network plug-in package - both are present in a stock Bambu Studio
`plugins\` folder on this machine, alongside `agora_rtc_sdk.dll`, `libagora-*.dll` and `libaosl.dll`.

**Owner check before release:** confirm the package really contains `BambuSource.dll`, and confirm
whether live view needs any of the `agora_*` / `libaosl` DLLs (the RTC path) as well as `live555`.
If it does, add them to the extract list in `install_bambu_camera_component()`.

## Tests

`tests/slic3rutils/plugin_guard_tests.cpp` - four new cases, no wx, no filesystem, no network:

* the stub is never copied over cameratools (including when timestamps coincidentally match)
* a real filter in cameratools is never overwritten, by the stub or by anything else
* stock copy behaviour survives for a real plug-ins filter
* an upgrade keeps a camera component the user downloaded

The PE probe itself is not unit-tested - fabricating a valid PE export table in a test fixture is
more machinery than it is worth - so it was validated by hand against real binaries:

| file | probe answer | expected |
| --- | --- | --- |
| EdgeSlicer stub `BambuSource.dll` (9,728 B) | no | no |
| Bambu Studio `BambuSource.dll` (5,521,440 B) | **yes** | yes |
| `live555.dll` | no | no (not a COM server) |
| `C:\Windows\System32\shell32.dll` | **yes** | yes (self-registering COM) |
| `C:\Windows\System32\kernel32.dll` | no | no (1,697 exports, none matching) |

## Build

`libslic3r_gui` (VS 2022 x64, Release, cmake 3.31.8), configured against the same deps tree as
`C:\Dev\SnapmakerOrcaPhone\build`: **Build succeeded, 0 errors** ->
`build\src\slic3r\Release\libslic3r_gui.lib`. All four changed translation units compile clean, and
the repo's `encoding-check` gate passes on the new and modified files.

One trap worth recording: `wxMediaCtrl2.cpp` sits **outside** `namespace Slic3r::GUI` (it has no
namespace block at all - note the pre-existing `Slic3r::data_dir()` call), so `wxGetApp()` and the
new `PluginGuard` helpers must be written fully qualified as `Slic3r::GUI::...` there. The first
build failed with four `C3861: identifier not found` for exactly that reason.

The worktree's `build\` directory was deleted afterwards, as instructed. The worktree itself
(`C:\Dev\wt_liveview`, branch `fix/liveview-bambusource`) is kept.

## Click-tests for the owner

The build in this worktree is `libslic3r_gui` only - not a runnable slicer - so all of these need a
full build and install.

1. **The reported bug is gone.** Fresh install (UltraNet stub in `plugins`, no `cameratools`).
   Device tab → Play. Expect the *"Live view needs Bambu's camera component"* dialog. Expect **no**
   "press Yes to re-register" message, and **no** UAC prompt before you answer.
2. **Decline is clean.** Answer No. Expect no UAC prompt, no regsvr32 error, and the player to sit
   idle. Press Play again - the same offer, not a different error.
3. **Accept installs and plays.** Answer Yes. Expect a busy cursor, then two UAC prompts (regedit,
   then regsvr32), then live view to start. Check `<data_dir>\plugins` and
   `<data_dir>\cameratools` both now hold a multi-megabyte `BambuSource.dll`, and that
   `bambu_networking.dll` and `ultranet.txt` are **unchanged** (compare size and timestamp).
4. **The component survives Play.** Press Stop, then Play again. `cameratools\BambuSource.dll` must
   still be the large file - this is breakage 2 and the stub must not come back.
5. **The component survives an upgrade.** Install a newer EdgeSlicer build over the top. After
   first start, `plugins\BambuSource.dll` must still be the large file, and the log should carry
   `[UltraNet] keeping the installed Bambu camera component`.
6. **Sign-in still works.** Account → Login. The UltraNet guards are untouched, so the Bambu sign-in
   flow must behave exactly as before - and no plug-in download must ever be offered.
7. **Virtual camera.** Device tab → the virtual-camera toggle on a fresh install. Expect the same
   component offer rather than a silent no-op.
8. **A Bambu-original install is unaffected.** On a machine with Bambu Studio's plug-in (no
   `ultranet.txt`), the stock re-register prompt must still appear and still work.

## Files

* `src/slic3r/GUI/PluginGuard.hpp` / `.cpp` - stub detection, PE export probe, copy decisions
* `src/slic3r/GUI/MediaPlayCtrl.cpp` - never copy the stub over `cameratools`; offer the component
* `src/slic3r/GUI/wxMediaCtrl2.cpp` - stub-aware branch instead of a doomed regsvr32 offer
* `src/slic3r/GUI/GUI_App.hpp` / `.cpp` - `install_bambu_camera_component`,
  `offer_bambu_camera_component`, `has_bambu_camera_component`, `register_bambu_source_filter`,
  first-run copier guard
* `tests/slic3rutils/plugin_guard_tests.cpp` - the four new decision cases
