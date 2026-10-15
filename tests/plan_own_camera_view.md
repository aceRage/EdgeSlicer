# Our own desktop live view for Bambu printers (replacing BambuSource + live555)

Status: investigation + prototype, branch `feat/own-camera-view-spike` (draft PR #448). Revision 2
(2026-10-12, owner): the setting is **"New Bambu LiveView", ON by default** (fresh installs and
upgrades; an explicit OFF is kept), applies at the next Play without a restart, and OFF falls back
to the existing Bambu-player path, with Bambu's camera files from PR #443 or, without them, the old
"download camera component" prompt.
Date: 2026-10-11. Context: PR #443 ships Bambu's camera files as a stopgap; the owner wants our
own path proven first, on Windows and macOS, with a **clean data dir**
(memory note `bambu-camera-files-decision`).

## TL;DR

- **The phone already gets every RTSPS Bambu camera through our bundled go2rtc** (X1/X1C, H2D, H2C,
  H2S, P2S: `rtspx://bblp:<code>@<ip>:322/streaming/live/1`, H.264 passed through, no transcode).
  The owner's four Bambu printers (X1C, H2S, H2D, H2C) are all on the camera wall that way.
- **P1/A1 (JPEG-over-TLS on port 6000) never goes through go2rtc today.** Our own C++ relay
  (`BambuCamRelay`, in the hub) speaks the port-6000 protocol and serves MJPEG; the phone opens one
  relay session (and so one printer connection) **per viewer**. go2rtc 1.9.14 has no Bambu source
  of its own. Nothing in the owner's logs or printers exercises this path, so it is **unverified on
  hardware**.
- **Recommended display: option (a)**, the hub's existing go2rtc player page (`player.html`, the
  same one the Stream tab and the phone use) in a `wxWebView` in the Device tab's camera slot,
  playing **MSE over the hub's loopback WebSocket** for RTSPS and **MJPEG frames on the same
  WebSocket** for P1/A1. No new native decoder, no new dependency on any platform, no firewall
  prompt, one shared go2rtc stream with the phone. It is already proven on Windows by the Stream
  tab. Options (b) native ffmpeg decode and (c) others cost far more and would need an ffmpeg on
  macOS, which we deliberately do not ship.
- The **prototype** (Preferences → Bambu Network → **"New Bambu LiveView"**, key `liveview_go2rtc`,
  **on by default**) does exactly that, and never touches the Bambu player for a printer it plays.
  OFF, or a printer it cannot play (no LAN stream/address/access code, e.g. cloud-only), takes the
  existing Bambu-player path unchanged. Draft PR and owner test plan below.

## 1. How the phone relay gets each Bambu camera today

### The pieces

| Piece | Where | What it does |
|---|---|---|
| Hub (`EdgeSlicer.exe --hub`) | `src/slic3r/GUI/RemoteHub.cpp` | One per data dir. Owns go2rtc, BambuCamRelay, the loopback control plane (`/hub/*`) and the phone listener (13640). |
| go2rtc 1.9.14 | `resources/tools/go2rtc/go2rtc.exe` (Windows, committed); Mac CI fetches + lipo's the same release into `Contents/Resources/tools/go2rtc`; local Mac/Linux builds fall back to Homebrew/PATH | Started by `HubServer::start_go2rtc()` on a random loopback API port with per-run credentials, `allow_paths: [/api/ws, /api/streams, /api/onvif]`, RTSP listener loopback-only (only when an ffmpeg exists, for Quality variants), WebRTC media on 8555 (or the next free port, `free_webrtc_port()`), SRTP off. |
| Stream registration | `HubServer::register_streams()` | For every camera in the Stream tab's list (`<datadir>/hub/streams.json`, posted by `stream_center.html`), `PUT /api/streams?name=<rname>&src=<rsrc>`; plus `_med`/`_low` ffmpeg variants when an ffmpeg is present. |
| Player | `resources/web/orca/player.html` + verbatim `video-rtc.js`/`video-stream.js` | Served by the hub at `/stream.html`. Its `api/ws` goes through the hub (`tunnel()`), which adds go2rtc's credentials; the browser never reaches go2rtc directly. The phone authenticates with the `rt` cookie; the PC's own pages pass the hub secret as `lt` (loopback only). |
| BambuCamRelay | `src/slic3r/GUI/BambuCamRelay.cpp` (runs in the hub) | `GET /bambu?ip=&code=[&fps=]` on 127.0.0.1: TLS to `<ip>:6000`, the 80-byte auth packet (`bblp` + access code), then scans JPEG SOI/EOI markers and re-serves them as `multipart/x-mixed-replace`. One printer connection per HTTP client. |

### Per camera type

| Printer | Printer stream | Stream tab / phone path today | Works today? |
|---|---|---|---|
| X1 / X1C / X1E | RTSPS, port 322, needs "LAN Only Liveview" on | Stream tab kind `rtsps` → `rkind: go2rtc`, `rsrc = rtspx://bblp:<encodeURIComponent(code)>@<ip>:322/streaming/live/1`, `rname = 's' + hashStr(rsrc)`. Phone plays it via `player.html` (WebRTC when reachable, else MSE). | **Yes** - in use (see evidence). |
| H2D / H2C / H2S | RTSPS, port 322 | Same as X1. | **Yes** - in use. |
| P2S | RTSPS, port 322 | Same as X1. | Expected yes (same path); not in the owner's fleet. |
| P1P / P1S / A1 / A1 mini | JPEG frames over TLS, port 6000 | Stream tab kind `p1` → `rkind: p1`. **Not go2rtc**: the PC's Stream tab draws `<img src=http://127.0.0.1:<relay>/bambu?...>`; the phone draws `<img src=/r/<token>/bambu?id=...>` which the hub tunnels to BambuCamRelay. Each tile/viewer = its own TLS session to the printer. | **Unverified** - no P1/A1 in the owner's fleet, no relay session in the logs. |
| Snapmaker U1 (for comparison) | camera-streamer: WebRTC page `/webcam/webrtc`, raw H.264 `/webcam/stream.h264` | PC Stream tab embeds the printer's own `/webcam/webrtc` page (WebRTC straight from the printer). Phone: the hub pulls the raw H.264 itself (`/relay/h264`, SPS-first) and registers `u1_<id>` in go2rtc, so the U1's one-consumer limit is respected. | Yes. |

The U1 shows the pattern we want for Bambu too: **the hub opens the one printer connection and
go2rtc fans it out**.

### Evidence from the owner's data dir (read-only; no serials or addresses quoted)

- `hub/streams.json`: 7 cameras. **4 are `kind: rtsps` / `rkind: go2rtc` with `rtspx://…:322`
  sources and access codes**, 3 are U1 `/webcam/` URLs. **No `p1` cameras.**
- `hub/printers.json` (the hub's printer cache): Bambu X1 Carbon, H2S, H2D, H2C; no P1/A1. So
  every Bambu printer the owner has is RTSPS, and every one is on the phone wall through go2rtc.
- `log/hub.log.0` (the hub keeps only its latest run): go2rtc started (`credential-only`, WebRTC
  media on 8556 because 8555 was taken), "quality variants off (no ffmpeg found)" (that run was a
  test copy without ffmpeg), BambuCamRelay listening, `starting go2rtc took 7212 ms`, and the
  Windows Firewall note that go2rtc.exe has no rule (WebRTC to the phone falls back to MSE). No
  stream errors.
- Main logs (28 files): 46 `MediaPlayCtrl` lines, all "switch machine"; **no live-view session**,
  no `BambuCamRelay: session`, no `Load: clsid` (the Windows Bambu filter load). So the logs
  prove the RTSPS cameras are configured and the hub runs, but they hold no playback trace either
  way. (Follow-up: keep 3 hub log rotations; one run is too little for support.)

### Offline measurements of go2rtc 1.9.14 (no printer, loopback only)

`tests/phone/go2rtc_share_probe.py` runs the bundled go2rtc against a local MJPEG server that
stands in for BambuCamRelay and counts upstream connections (all PASS on this PC):

1. go2rtc accepts an **HTTP multipart MJPEG source** (BambuCamRelay's output) - so P1/A1 can go
   *through* go2rtc and be shared.
2. **4 viewers (2 HTTP MJPEG + 2 player-WebSocket `mjpeg`) share 1 upstream connection.**
3. The player's WebSocket `mjpeg` mode (what `player.html?mode=mjpeg` uses) delivers JPEG frames.
4. **A second `PUT` of an existing stream name while viewers watch makes go2rtc build a new stream
   object, and the next viewer opens a SECOND upstream connection** (the first keeps running).
   `PATCH` keeps the existing stream. `GET /api/streams?src=<name>` answers 404/200 for
   unregistered/registered.
5. go2rtc answers its API immediately, but its stream handlers (`/api/stream.mjpeg`, the WS
   `mse`/`mjpeg` types) only register after the WebRTC module has enumerated interfaces: **7-12 s
   after start** on this PC (12 s under build load). Matches the hub's own "starting go2rtc took
   7212 ms".

Finding 4 matters for "the phone and the desktop must share one stream": the hub's
`register_streams()` re-`PUT`s **every** Stream-tab camera on **every** `/hub/state` post (any
Stream-tab edit, from any window). If the phone is watching a printer at that moment, the next
viewer of that camera opens a second RTSPS session to the printer. That is existing behaviour;
the prototype's own registration does not do it (it reuses a registered stream), and fixing
`register_streams()` (skip a name whose source is unchanged since this go2rtc started) is
follow-up #1 below. **Fixed by #450** (`plan_stream_sync`: only what changed is touched), which
this branch now merges and uses for the Device tab's streams too.

## 2. Desktop display options

Today: Windows plays the `bambu:` URL through Windows Media Player/DirectShow with Bambu's
`BambuSource.dll` registered as the source filter (`wxMediaCtrl2.cpp`); macOS loads
`libBambuSource.dylib`'s `BambuPlayer` class (`wxMediaCtrl2.mm`); Linux uses a GStreamer element
(`gstbambusrc`) that calls into `libBambuSource.so`. All three need Bambu's closed module and
live555.

| | (a) go2rtc player page in `wxWebView` | (b) bundled ffmpeg → native panel | (c1) WebRTC page instead of MSE | (c2) native player on go2rtc's RTSP/HLS |
|---|---|---|---|---|
| How | Load `http://127.0.0.1:<hub>/stream.html?src=<name>&mode=mse|mjpeg&lt=<secret>` in a web view in the camera slot | ffmpeg (subprocess `-f rawvideo` pipe, or linked libavcodec) decodes; frames painted into a wxPanel / GL texture | Same page, `mode=webrtc,mse` | Linux: GStreamer `playbin` on `rtsp://127.0.0.1:<go2rtc rtsp>/<name>`; macOS: AVPlayer on go2rtc HLS |
| Latency (expected) | MSE: ~0.5-1 s (go2rtc's low-latency fMP4; the phone and Stream tab use it today). MJPEG: frame-by-frame, < 0.3 s | ~0.2-0.4 s | ~0.2-0.3 s | RTSP ~0.5 s; HLS 2-6 s |
| CPU | go2rtc remuxes only (~1-2 %); decode is hardware-accelerated in WebView2 / WebKit; one web-view renderer (~50-100 MB) | High with a pipe (1080p RGB at 30 fps is ~190 MB/s through a pipe, plus a software decode unless we wire up hwaccel); moderate if linked in | Same as (a) plus ICE | Low |
| Licensing | Nothing new (go2rtc MIT, already shipped) | LGPL ffmpeg is fine on Windows (already shipped as `ffmpeg.exe`), **but macOS has no bundled ffmpeg by owner decision** (Homebrew only) and BtbN has no macOS builds - we would have to build and ship LGPL dylibs for Mac and Linux | Same as (a) | Platform frameworks |
| Windows | WebView2 (Chromium): MSE H.264 yes. Proven: the Stream tab plays go2rtc cameras exactly like this | Works (ffmpeg.exe present) | Needs go2rtc's media port; the hub log shows **no firewall rule for go2rtc.exe** on the owner's PC | No native RTSP in WMP/Media Foundation without extra code |
| macOS | WKWebView: MSE is supported on macOS (not on iOS), H.264 and HEVC in hardware. The page is plain http on 127.0.0.1: allowed by the app's ATS keys (`NSAllowsArbitraryLoads(InWebContent)`, `NSAllowsLocalNetworking`). **Loopback is exempt from Local Network privacy**; go2rtc's own connection to the printer is attributed to EdgeSlicer.app (child process), which already needs and has Local Network access for MQTT. Muted autoplay: the player mutes; to verify on the owner's Mac | No bundled ffmpeg → Homebrew dependency or a new shipped dylib set | WKWebView supports RTCPeerConnection, but host candidates to the LAN IP + macOS firewall + Local Network prompts: more to go wrong than MSE | AVPlayer cannot do RTSP; HLS too laggy |
| Linux | WebKitGTK: MSE on by default in current versions; H.264 needs GStreamer `libav`/`bad` plugins on the system (AppImage risk) | As Windows, plus packaging | As (a) | GStreamer playbin already used by wxMediaCtrl - a real option for Linux only |
| Code change | Small: one web view + a hub route + a state branch in `MediaPlayCtrl` (prototype: ~400 lines incl. tests) | Large: decoder lifecycle, threading, colour conversion, resizing, DPI, three platforms, packaging | (a) + firewall UX | Per-platform players |

**Recommendation: (a) with MSE (RTSPS) and MJPEG (P1/A1).** It reuses the exact player the phone
and the Stream tab already run, the hub already owns go2rtc, and the desktop talks to the hub over
loopback TCP only, so no firewall rule, no WebRTC/UDP and no Local Network prompt is involved for
the desktop. The cost is ~0.5 s more latency than Bambu's native player, which is fine for
monitoring a print. WebRTC (c1) can be added later as `mode=webrtc,mse` once the firewall rule is
in place (the page already falls back after 2.5 s). Keep (c2) GStreamer in mind for Linux only if
WebKitGTK's MSE turns out to be unreliable in the AppImage.

## 3. How it plugs into the UI

### Where

- `StatusBasePanel` (Device tab) builds `m_media_ctrl` (Bambu player), `m_custom_camera_view`
  (Orca's "custom camera source" web view) and `m_media_play_ctrl` (the play/stop bar,
  `MediaPlayCtrl`). The prototype adds `m_own_camera_view`, a third widget in the same sizer slot,
  **created on the first Play that uses it** (so never for someone with the setting off).
- `MediaPlayCtrl::Play()` - after the usual checks (connected, not busy, has a camera) - calls
  `try_play_own()`. It reads `liveview_go2rtc` **at every Play** (no restart needed) and returns
  false - letting the existing code run unchanged - when the setting is off, the printer has no
  LAN stream (`LVL_Rtsps`/`LVL_Rtsp`/`LVL_Local`), no LAN address or no access code, or no web view
  can be created. When it plays, everything after that branch (the `bambu:///…` URL,
  `wxMediaCtrl2::Load`, the TUTK/Agora cloud URL) is never reached, so neither the DirectShow
  filter nor `BambuPlayer` is asked for anything.
- `try_play_own()`: picks the kind from the printer's own report (`ipcam.liveview.local` /
  `rtsp_url` → `LVL_Rtsps` / `LVL_Rtsp` / `LVL_Local`), needs the LAN IP + access code, then on a
  worker thread: `RemoteHub::ensure_running("", false)` (phone access untouched) →
  `RemoteHub::register_own_camera(kind, ip, code)` → hub `POST /hub/camera` (control plane, hub
  secret, JSON body so the code never sits in a URL) → `HubServer::register_own_camera()`.
- The hub computes the stream **exactly as `stream_center.html` does** for RTSPS
  (`Testing::own_camera_stream`, unit-tested against values produced by the page's own JS), so a
  printer that is also on the Stream tab/phone wall with the same address and code is the **same
  go2rtc stream**. Since the merge with #450 the hub never PUTs it itself: it adds the camera to
  `m_own_streams`, which `desired_streams()` merges into what the Stream-tab sync wants
  (`Testing::merge_desired_streams`), and lets the sync worker (`plan_stream_sync`) do it - an
  existing stream is "unchanged" (nothing sent, viewers untouched), a missing one is PUT once.
  Because the Device tab's cameras are part of `desired`, a Stream-tab save that drops the same
  `s<hash>` camera does not delete or replace the stream the Device tab is playing (only that
  camera's Stream-tab `_med`/`_low` variants go). Live-view streams stay registered for the hub's
  run; an idle go2rtc stream has no printer connection.
  P1/A1: `src = http://127.0.0.1:<BambuCamRelay port>/bambu?ip=&code=`, name
  `b<hash(ip|code)>`, played as `mjpeg`.
- Back on the UI thread: load `http://127.0.0.1:<hub port>/stream.html?src=<name>&mode=<mse|mjpeg>&lt=<secret>`
  into the own view and swap it into the slot. Status "Playing (EdgeSlicer live view)".

### Lifecycle

| Event | Own view behaviour |
|---|---|
| Play button | `play_own()`; "Starting live view..." until the hub answers (cold hub: up to ~10 s, finding 5) |
| Stop button, tab hidden, window minimised (existing `on_show_hide` → `Stop()`), print-send modal (`ultra_pause`), switching to the custom camera | `stop_own()`: page → `about:blank` (closes the WebSocket; go2rtc drops the consumer and closes the printer connection when it was the last one), slot back to the Bambu control (hidden), state idle |
| Printer switched (`SetMachineObject`) | `Stop(" ")` → `stop_own`; a hub answer still in flight is dropped by a sequence number |
| Player page hidden by the OS | `player.html` already pauses on `visibilitychange` |
| Errors | Hub/go2rtc missing, bad code/address: status line message. Camera unreachable / wrong code: go2rtc only finds out when the first viewer connects; the player overlay shows go2rtc's error text. (Follow-up: bridge it to the status line and the IP/access-code dialog.) |

### Existing flows

- **"LAN Connection Failed (Failed to start liveview)"** (`Stop()` → `show_modal_ip_address_enter_dialog`)
  and **"download camera component"** (`wxMediaCtrl2::Load` → `offer_bambu_camera_component`) are
  both on the Bambu-player path: **not reached** for a printer the own view plays, and exactly as
  before when the setting is OFF (or the printer falls back). With OFF: if Bambu's camera files are
  installed (PR #443: `cameratools` on Windows and macOS) they play; without them, Windows offers
  the camera component as before. For a product version, wire the IP dialog to the own path's
  "unreachable" error too.
- **"Go Live" virtual camera** (`ToggleStream`, `cameratools/bambu_source.exe` + ffmpeg) is still
  Bambu-only; hide it while the own view is on (follow-up), or drop it.
- **Storage browser and timelapse** (`MediaFilePanel` → `PrinterFileSystem` → `plugins/BambuSource`,
  which is UltraNet's storage module) are untouched and stay on UltraNet.
- **Cloud-only printers** (no LAN address/code; TUTK/Agora remote liveview): no own path exists -
  those protocols are Bambu-cloud proprietary. The own view says so on the status line.

### Multiple printers, windows, and printer-side client limits

- One go2rtc per data dir (the hub), so every window and the phone share it. Each printer is one
  stream; switching printers stops one consumer and starts another.
- **Sharing:** with the same name, N viewers (desktop windows, phone tiles, Stream tab tiles) =
  1 printer connection (finding 2). Conditions: the Stream-tab entry uses the same address and code
  as the Device tab. (Re-saving the Stream tab mid-session no longer splits it: #450.)
- **Printer limits** (unverified, to measure): RTSPS servers on X1/H2 are reported to accept only a
  few sessions; the P1/A1 port-6000 server is reported to allow very few (possibly one) and to run
  at ~1 fps. Bambu Studio / Orca / Bambu Handy on another device are consumers we do not control.
  The test plan counts connections with `Get-NetTCPConnection` / `lsof` instead of guessing.
- **P1/A1 on the phone still opens one relay session per tile** (it uses `/r/<token>/bambu`, not
  go2rtc). Moving the phone's `p1` tiles onto the same go2rtc `b<hash>` stream (MJPEG over the
  player WebSocket, finding 3) is follow-up #2; until then a phone tile + the desktop = 2 sessions
  on a P1/A1.

### Timelapse / storage

Stay on UltraNet (`plugins/BambuSource` storage module). Nothing in the prototype touches them;
the test plan checks they still work in the same clean-data-dir run.

## 4. Risks and unknowns

1. **P1/A1 port 6000 (highest risk).** BambuCamRelay's protocol code (80-byte auth, SOI/EOI scan)
   has never been exercised against a real P1/A1 in the owner's logs. Unknowns: concurrent-session
   limit (likely 1), whether firmware still accepts the `bblp`/code auth on current versions, frame
   rate, whether frames are split in ways the SOI/EOI scan mishandles (it ignores the 16-byte frame
   headers). go2rtc's side of it is proven offline (findings 1-3). Needs one real P1 or A1.
2. **Concurrent RTSPS clients** on X1/H2: unknown limit; the prototype shares where it can, but
   the existing `register_streams()` re-PUT can double up (finding 4).
3. **Codec of H2D/H2C/H2S streams.** The phone plays them, so they are something WebView2/WebKit
   MSE decode on the owner's devices; if any model sends HEVC, WebView2 needs hardware HEVC + the
   HEVC extension, and an older Mac may not decode it. Check the player overlay ("MSE") and video.
4. **macOS** (not built here, no Mac CI): WKWebView MSE + muted autoplay inside `wxWebView`,
   `wxWebView` loading `http://127.0.0.1`, and go2rtc on a local build (bundled via
   `GO2RTC_BIN_DIR`, else Homebrew `go2rtc`). **Found by the owner's Mac test (revision 3) and
   fixed in revision 4:** `wxMediaCtrl2.mm`'s constructor called `create_player()`, which (since
   #443) `dlopen`s Bambu's `cameratools/libBambuSource.dylib` and `liblive555.dylib` as soon as the
   Device tab's media control was built - before any Play. The constructor no longer does; the
   player (and with it Bambu's module) is made by the first `Load()`/`Play()`, i.e. the first Play
   that takes the Bambu-player path, and `Stop()` with no player only reports "stopped". Windows
   was already lazy (the DirectShow filter is only instantiated by `Load()` of a `bambu:` URL;
   start-up only copies files and writes the per-user registration) and so is Linux (`gstbambusrc`
   loads the library when the element starts).
5. **Windows: a stale DirectShow registration.** A clean data dir does **not** remove the HKCU
   registration of Bambu's filter left by OrcaSlicer / Bambu Studio
   (`HKCU\Software\Classes\CLSID\{233E64FB-…}\InprocServer32` → `%APPDATA%\OrcaSlicer\plugins\BambuSource.dll`
   on the owner's PC). With the preference on, no DirectShow graph is built, so it is never loaded -
   but only a **process module check** proves that, not the log alone. Best: also run once on a
   Windows account or VM without OrcaSlicer/Bambu Studio (the build VM).
6. **Cold start**: the first Play after the hub starts can see go2rtc's stream handlers not yet
   registered (finding 5); the player retries within 15 s. Possibly worth making the hub wait for
   go2rtc readiness before answering `/hub/camera`.
7. **Secrets at rest**: go2rtc writes registered streams (with access codes) back into
   `<datadir>/hub/go2rtc.yaml` - the same as the Stream tab's cameras today, and the same codes are
   in `streams.json`. Nothing new, but worth knowing.
8. **Linux**: WebKitGTK MSE + H.264 GStreamer plugins in the AppImage; not tested.
9. **Merge with PR #443**: both touch `MediaPlayCtrl.cpp`; the spike's hunks are additive (a branch
   in `Play()`/`Stop()` + new functions) and should rebase cleanly.

### What the clean-data-dir test must prove

1. Live view plays on each RTSPS model (H2D, H2C, X1C, H2S; P1/A1 if one can be borrowed) with
   **no Bambu camera files anywhere EdgeSlicer looks**: the data dir starts empty (only UltraNet,
   copied by the app's first run), the test copy has no `cameratools`.
2. **No Bambu camera module and no live555 in any EdgeSlicer process** (UI and hub), even with the
   stale HKCU filter registration present - checked by module list, not only by the log.
3. The log shows the own path (`[own camera]`, hub `own live view stream … registered|reused`) and
   none of the Bambu-player lines (`MediaPlayCtrl: bambu:///`, `MediaPlayCtrl: start load`,
   `Load: clsid`, `[camera component]`, `LAN Connection Failed`).
4. **One printer connection** for desktop + phone/Stream tab watching the same printer
   (hub log `reused`, and exactly one ESTABLISHED connection to port 322/6000 from go2rtc / the hub);
   zero a few seconds after every viewer stops.
5. Stop on tab switch / minimise / printer switch really stops (connection count returns to 0).
6. Storage browser and timelapse still work (UltraNet), in the same run.
7. With the setting **OFF** (no restart needed), the old behaviour is back (Play takes the Bambu
   path: Bambu's files from #443 if present, else the "download camera component" prompt) - the
   setting is the only switch. A fresh data dir and an upgraded one both start with it ON.

## Prototype (this branch)

- Setting: Preferences → Bambu Network → **"New Bambu LiveView"** (`liveview_go2rtc`). ON by
  default: `AppConfig::set_defaults()` sets it when the key is missing, which covers fresh installs
  and configs from older builds; a stored `false` is kept. Read at every Play, so it applies without
  a restart. Tests: `[AppConfig]` "New Bambu LiveView ..." in `tests/libslic3r/test_appconfig_trailing_newline.cpp`.
- Files: `MediaPlayCtrl.{h,cpp}` (`SetOwnView`, `try_play_own`, `own_ready`, `stop_own`; branches
  in `Play()`/`Stop()`), `StatusPanel.{hpp,cpp}` (the web view, created on first use),
  `AppConfig.cpp` (the default),
  `RemoteHub.{hpp,cpp}` (`Testing::own_camera_stream`, `js_hash_str`, `js_encode_uri_component`,
  `HubServer::register_own_camera`, `POST /hub/camera`, client `register_own_camera`),
  `Preferences.cpp`, unit tests `[own-camera]` in `tests/slic3rutils/remote_hub_tests.cpp`, and the
  offline probe `tests/phone/go2rtc_share_probe.py`.
- With the setting off nothing new is created and `Play()`/`Stop()` behave as before
  (`try_play_own()` returns false, `m_own_busy` stays false). `wxMediaCtrl2.mm` is untouched (#443
  changes it); a merge-tree of this branch with `origin/feat/bambu-camera-module` is clean.
- The `register_streams()` re-PUT fix (follow-up 1) is #450, merged into main and into this branch;
  the Device tab's registration goes through it (see section 3). Tests:
  `[own-camera][go2rtc-sync]` in `tests/slic3rutils/remote_hub_tests.cpp`.

## Follow-ups (not in the spike)

1. ~~`register_streams()`: remember name → source per go2rtc run and skip unchanged ones (finding 4).~~ Done in #450.
2. Phone P1/A1 tiles through go2rtc `b<hash>` + `mjpeg` so they share with the desktop.
3. Bridge player errors (go2rtc's WS `error`) to the status line and to the IP/access-code dialog.
4. Hide "Go Live" (virtual camera) while the own view is on.
5. Hub: answer `/hub/camera` only once go2rtc's handlers are up (finding 5); keep 3 `hub.log` rotations.
6. Optional `webrtc,mse` once the go2rtc firewall rule is part of the installer.
7. If the own view is accepted: make it the default for LAN printers, keep Bambu's files optional
   (#443), and retire the `cameratools` seeding for live view.

## Owner's test plan (clean data dir)

Never point `--datadir` at `%APPDATA%\EdgeSlicer` or `~/Library/Application Support/EdgeSlicer`
for this. Use a new empty folder each time you want "clean".

### Windows

Test copy: `C:\Dev\EdgeSlicerTest\camspike2` (revision 2 of this branch; has `ultranet\` copied
from `C:\Dev\ultranet_ship_stage`, no `cameratools`, no ffmpeg). The older `camspike` copy is
revision 1 (setting off by default) - don't use it.

1. Keep your normal EdgeSlicer running (so its hub keeps 13640, the port Tailscale Serve and the
   phone use); the test copy's data dir gets its own hub on the next free port. If Windows Firewall
   asks about the test copy's `go2rtc.exe` (its WebRTC port), Cancel is fine: the desktop view uses
   loopback only.
2. `mkdir C:\Dev\EdgeSlicerTest\camspike_data` (must be empty), then
   `C:\Dev\EdgeSlicerTest\camspike2\EdgeSlicer.exe --datadir C:\Dev\EdgeSlicerTest\camspike_data`.
   Finish the setup wizard (Bambu Lab printers on).
3. Check nothing Bambu is there: `dir C:\Dev\EdgeSlicerTest\camspike_data\cameratools` → not found;
   `dir C:\Dev\EdgeSlicerTest\camspike_data\plugins` → only UltraNet (`bambu_networking.dll`,
   `BambuSource.dll` ~2.2 MB, `ultranet.txt`).
4. Preferences → Bambu Network: **"New Bambu LiveView" is already ticked** (fresh data dir = ON).
   Nothing to change.
5. Device tab: add/connect the H2D in LAN mode (IP + access code). Press Play. Expect "Starting
   live view..." then video and "Playing (EdgeSlicer live view)"; the player overlay says MSE.
   Note the delay (stopwatch on your phone in front of the camera vs the screen) and CPU in Task
   Manager (EdgeSlicer, msedgewebview2, go2rtc).
6. Repeat 5 for H2C, X1C and H2S (switch printer while playing once: it should stop cleanly).
   P1/A1 if available: expect MJPEG in the overlay.
7. **Module check** while a camera plays (PowerShell):
   ```powershell
   Get-Process EdgeSlicer, go2rtc -ErrorAction SilentlyContinue | ForEach-Object {
     $p = $_; $p.Modules | Where-Object { $_.ModuleName -match 'live555|BambuSource' } |
       Select-Object @{n='pid';e={$p.Id}}, ModuleName, FileName }
   ```
   Pass: no `live555.dll` at all, and any `BambuSource.dll` is
   `C:\Dev\EdgeSlicerTest\camspike_data\plugins\BambuSource.dll` (UltraNet). Fail: anything from
   `cameratools`, `%APPDATA%\OrcaSlicer`, Bambu Studio, or any `live555`.
8. **Log check**: in `C:\Dev\EdgeSlicerTest\camspike_data\log\`:
   ```powershell
   Select-String -Path *.log* -Pattern '\[own camera\]','own live view stream' | Select-Object -Last 20
   Select-String -Path *.log* -Pattern 'MediaPlayCtrl: bambu:','MediaPlayCtrl: start load','Load: clsid','\[camera component\]','LAN Connection Failed'
   ```
   Pass: the first finds lines, the second finds **nothing**.
9. **Sharing**: Stream tab → + Add → type "Bambu X1 / H2", same IP and access code as the Device
   tab → tile plays. Then Device tab → Play. `hub.log` shows `own live view stream s… (rtsps, mse)
   already registered, reused`. Count printer connections:
   ```powershell
   Get-NetTCPConnection -RemotePort 322 -State Established | Select-Object LocalPort, RemoteAddress, OwningProcess
   ```
   Pass: one row, owned by go2rtc.exe. (P1/A1: `-RemotePort 6000`, owned by the hub EdgeSlicer.)
   Stop both viewers (Stream tab to another tab, Device tab Stop): within ~10 s, no rows.
10. Storage: Device tab → storage/timelapse list still opens and downloads a file (UltraNet).
11. Untick "New Bambu LiveView" (no restart), Stop, Play: Play takes the Bambu path again -
    proving the setting is the only switch. Tick it again and Play: back to the own view. On your PC that path may even *play*, through OrcaSlicer's registered DirectShow filter
    (risk 5); the step-7 module check then shows `live555.dll` and OrcaSlicer's `BambuSource.dll`,
    which is exactly the contrast with step 7 that we want to see.
12. Optional, strongest: repeat 2-8 on the build VM (no OrcaSlicer/Bambu Studio, so no HKCU filter).
13. Upgrade check: start camspike2 once on a **copy** of a data dir from an older build (copy an old
    test data dir, never `%APPDATA%\EdgeSlicer` itself): the setting comes up ticked. Untick it,
    restart: it stays unticked.
14. With #443 (optional, once both are built together): with the setting OFF and Bambu's files in
    `cameratools`, Play uses them; with the setting ON the module check still shows no `live555`.

### macOS (local build, no Mac CI)

```sh
git fetch origin && git checkout -B camspike origin/feat/own-camera-view-spike
# go2rtc: either have Homebrew's (brew install go2rtc) on PATH, or put the same v1.9.14 mac
# binary CI uses in a folder and pass GO2RTC_BIN_DIR. UltraNet as for the release builds.
GO2RTC_BIN_DIR=~/edge-go2rtc ULTRANET_BIN_DIR=<ultranet mac out> ./build_release_macos.sh -s -x -a arm64
mkdir ~/edge-camspike-data      # must be empty
build/arm64/Snapmaker_Orca/EdgeSlicer.app/Contents/MacOS/EdgeSlicer --datadir ~/edge-camspike-data
```

1. Steps 3-6 as on Windows (`~/edge-camspike-data/cameratools` must not exist; `plugins` only
   UltraNet). If macOS asks for Local Network access, allow it (it is the app's existing prompt).
2. **Module check** while playing:
   ```sh
   for p in $(pgrep -f 'EdgeSlicer.app/Contents/MacOS/EdgeSlicer'); do echo "pid $p"; vmmap "$p" 2>/dev/null | grep -iE 'live555|BambuSource' | awk '{print $NF}' | sort -u; done
   ```
   Pass: no `liblive555` and nothing from `cameratools` - with the Device tab open, a printer
   selected, and Play pressed (own view). A `libBambuSource.dylib` may appear **only** from
   `~/edge-camspike-data/plugins/` (UltraNet's, loaded by the storage browser); confirm it is not
   Bambu's camera module:
   `nm -gU ~/edge-camspike-data/plugins/libBambuSource.dylib | grep -c BambuPlayer` → `0`.
   Then untick "New Bambu LiveView" and Play: now `cameratools/libBambuSource.dylib` and
   `liblive555.dylib` appear (the Bambu player's first Play loads them).
3. Log check: every Play logs one warning line `MediaPlayCtrl: Play -> own view (go2rtc, mse)` or
   `MediaPlayCtrl: Play -> Bambu player: <reason>` (`New Bambu LiveView is off`, `no LAN stream ...`,
   `no LAN address`, `no access code`, `no web view could be created`);
   `grep -h '\[own camera\]\|own live view stream' ~/edge-camspike-data/log/*` finds lines;
   `grep -h 'MediaPlayCtrl: bambu:\|MediaPlayCtrl: start load\|\[camera component\]\|LAN Connection Failed' ~/edge-camspike-data/log/*` finds nothing.
4. Sharing as Windows step 9, counting with `lsof -nP -iTCP:322 -sTCP:ESTABLISHED`.
5. Storage/timelapse, setting-off and upgrade as Windows steps 10, 11 and 13 (the setting is ON by default).
6. Note whether the picture starts by itself (WKWebView autoplay) and the overlay says MSE.

Report back per model: plays (Y/N), delay, overlay mode, module check, log check, connection count.
