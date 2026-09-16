# Network plug-in guards: sign-in without a plug-in, and never overwriting UltraNet

Branch `fix/plugin-guards`, off `feat/ultra-preferences`.

## Cause

EdgeSlicer ships its own clean-room network plug-in, **UltraNet**, a drop-in for Bambu's
`bambu_networking.dll`. The host loads the plug-in from `<data_dir>/plugins`, not from the app
folder, so a released build carries the DLLs in `<exe dir>/ultranet` and copies them in on first run
(`GUI_App.cpp`, the "Ultra Net: first-run install" block). A self-heal right below it forces
`installed_networking=true` whenever a plug-in is present.

Two things were still wrong.

**Sign-in with no plug-in is a dead end.** Bambu cloud sign-in with Google goes: login webview ->
system browser -> our loopback on 13650 -> ticket exchange *through the plug-in*
(`HttpServer::bbl_auth_handle_request`). With no agent loaded there is nothing to hand the ticket
to. `ZUserLogin` already had a "plug-in not detected" branch, but every other way in - the menu and
topbar Login action (`GUI_App::request_login`) and every Device-tab cloud action
(`GUI_App::check_login`) - went straight to `ShowUserLogin()`, so a fresh install could open a
sign-in page that could never complete.

**Nothing stopped Bambu's CDN package from replacing UltraNet.** `GUI_App::install_plugin()` unzips
the downloaded package straight over `<data_dir>/plugins`. Every download entry point funnels into
it: the home-page banner command `begin_network_plugin_download`, the Device-tab "install network
plugin" link, the Preferences `installed_networking` checkbox's offer, the wizard's offer, the
`ZUserLogin` hyperlink, and - worst, because it needs no click - `m_networking_need_update`, which
`on_init_network()` sets whenever the loaded plug-in's version does not match the Bambu build we
forked from. That mismatch is true of UltraNet *by construction*, so a normal startup was one prompt
away from downloading Bambu's plug-in over ours.

## Marker design

The library keeps Bambu's name, so the file on disk cannot say whose plug-in it is. A marker file
settles it:

- `<data_dir>/plugins/ultranet.txt` - written by the first-run copier beside the DLLs. If the
  sidecar `<exe dir>/ultranet` ships its own copy, that one is used; otherwise the copier generates
  it. Packaging installs a marker into `./ultranet` alongside the DLLs (root `CMakeLists.txt`), so
  shipped builds carry one either way.
- The test is **library AND marker**. A marker with no library beside it is a leftover (the user
  replaced the plug-in by hand) and must not lock the CDN path out; a library with no marker is a
  Bambu-original or CDN-updated plug-in and keeps the stock behaviour in full.

The decision lives in `src/slic3r/GUI/PluginGuard.{hpp,cpp}` - a wx-free, filesystem-free pair:

```
bool is_ultranet_plugin(bool plugin_present, bool ultranet_marker);
bool bambu_cdn_download_allowed(bool plugin_present, bool ultranet_marker);
LoginGuardAction plugin_guard_decision(bool plugin_present, bool ultranet_marker,
                                       bool installed_networking, bool agent_loaded);
```

`LoginGuardAction` is `ShowLogin` / `RestartRequired` / `OfferPluginDownload`. The one non-obvious
rule: a **loaded agent always wins**. The running agent can exchange the ticket, so a stale
`installed_networking=false` must not block a session that already works.

`GUI_App` caches the filesystem answer in `m_ultranet_plugin_installed`, refreshed by
`refresh_ultranet_plugin_state()` at startup (right after the first-run copy) and again inside
`install_plugin()` so a late copy is still seen.

## What is hidden, and when

Everything below keys off "UltraNet installed" only. An empty plug-ins folder or a Bambu-original
plug-in leaves the CDN path completely untouched.

| Entry point | UltraNet installed |
| --- | --- |
| `GUI_App::ShowDownNetPluginDlg()` (the central download dialog) | returns without showing |
| `GUI_App::install_plugin()` | refuses, returns -1, after a fresh disk re-read |
| `m_networking_need_update` handling (`GUI_App.cpp`, post-startup) | flag cleared, prompt skipped |
| home-page banner `begin_network_plugin_download` | ignored |
| Device-tab install link (`Plater::priv::install_network_plugin`) | ignored |
| "Network Plug-in is not detected" notification (`show_install_plugin_hint`) | suppressed |
| Preferences `installed_networking` checkbox's download offer | suppressed |
| Preferences "Use legacy network plugin" switch | hidden (`sizer_page->Hide`, after the Add, so no gap is left) |
| `ZUserLogin`'s "Click here to download it." hyperlink | replaced by a restart message |

"Stealth mode" and "Enable Bambu network plugin" stay visible - neither downloads anything. Legacy
goes because it only picks *which* Bambu CDN build to fetch, and with UltraNet nothing is ever
fetched.

Every suppression logs at info level with the same phrase, so one grep finds them all:

```
[UltraNet] UltraNet present, Bambu CDN download disabled (<which entry point>)
```

## Login guard

`GUI_App::ShowUserLoginGuarded()` is the new front door, called from `request_login()` (menu and
topbar) and `check_login()` (the choke point every Device-tab cloud action already went through).

- agent loaded -> `ShowUserLogin()`, unchanged.
- no agent, UltraNet installed -> a message dialog: *"The network plug-in is installed but not loaded
  yet. Please restart EdgeSlicer and sign in again."* Never the CDN download, which would overwrite
  ours. The usual cause is the first-run copy landing after the plug-in load point.
- no agent, no UltraNet -> *"Signing in to a Bambu account needs the network plugin. Install it
  now?"* (Yes/No); Yes opens `ShowDownNetPluginDlg()`. After the install the plug-in still needs the
  restart before an agent exists, so the retry is the user's next Login click, which then lands on
  the restart branch.

Logged as `[UltraNet] login guard armed: ...`.

## Proofs

**Unit tests** - `tests/slic3rutils/plugin_guard_tests.cpp`, five Catch2 cases (18 assertions, all
passing) over the pure helper:
marker semantics (both halves required, leftover marker and bare Bambu library both rejected), the
CDN gate being the exact complement, a loaded agent winning over every other input, the reported
no-plug-in bug offering the download, and UltraNet-installed-but-unloaded asking for a restart.

**Two scratch instances** of this build under `snorca_hubtest` (`inst_pguard`), each with its own
data dir (`log_severity_level` seeded to `info` - the default is `warning`) and stopped by PID:

- (a) data dir with **no** `plugins` folder - log shows the guard path armed
  (`[UltraNet] no UltraNet plugin (present=false, marker=false), Bambu CDN download path available`)
  and the app runs.
- (b) data dir seeded with the UltraNet DLLs **and** `ultranet.txt` from
  `snorca_hubtest/ultranet_out/ship/ultranet` - log shows
  `[UltraNet] UltraNet present, Bambu CDN download disabled` and no update prompt fires; the DLLs
  are byte-for-byte untouched afterwards.

## Owner click test

1. Fresh data dir, no plug-in. Account -> Login. Expect the "Signing in to a Bambu account needs the
   network plugin. Install it now?" prompt, **not** the sign-in webview. Say No; nothing happens.
   Say Yes; the Bambu download dialog opens as before.
2. Same, but on a build whose `<exe dir>/ultranet` ships the plug-in: the first run copies it in and
   writes `ultranet.txt`. Restart; Account -> Login now opens the real sign-in page and Google
   sign-in completes through the loopback.
3. With UltraNet installed, open Preferences -> Ultra -> Bambu Network. "Enable Bambu network plugin"
   and "Stealth mode" are there; "Use legacy network plugin" is gone. Toggling the enable checkbox
   on does **not** pop a download dialog.
4. With UltraNet installed, confirm `<data_dir>/plugins/bambu_networking.dll` keeps its timestamp
   across a startup - no "network plug-in needs updating" prompt, and the log line
   `[UltraNet] UltraNet present, Bambu CDN download disabled (network plug-in update prompt skipped)`
   is present.
5. Delete `ultranet.txt` but keep the DLL (simulating a Bambu-original plug-in): the download and
   update prompts come back exactly as before, proving the CDN path was gated, not removed.
