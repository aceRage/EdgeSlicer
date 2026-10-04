# Always-on hub: feasibility study (2026-10-02)

Question: can the EdgeSlicer phone hub (and optionally the slicer) live on an always-on device (Pi, N100 mini PC, NAS, cloud VM) so the companion app works with the desktop off?
Research only: no code changed, nothing built or run. Everything is "confirmed by reading code" unless marked **unverified**. Line counts are against `main` at `89f2871c9e`.

## 1. Bottom line

Feasible, and closer than it looks, but not as a small "hub daemon". Today's `--hub` process is already headless and wx-free in its server core, but it is only a **proxy and cache**: everything printer-facing (Bambu `DeviceManager`, `NetworkAgent`/UltraNet, `SnapmakerLan`, `RemoteEvents` watchers, `RemoteSend`, `RemoteControl`, G-code archive, slicing) runs inside a full wxWidgets `GUI_App` instance that the hub talks to over loopback (`RemoteAccess.cpp`). Hidden service mode (window never shown, OpenGL still alive) already exists and is how the phone slices on Windows.

**Recommendation:** ship a Docker image of "hub + one hidden GUI instance under Xvfb" first (option a/c), on x86_64 (N100 / NAS / Unraid), then arm64 for Pi 5. Treat a GUI-free daemon (option b) as a later, optional 2-3 month project. Buy-advice: Intel N100/N150 mini PC with 16 GB and an SSD; Pi 5 only if slicing on the box is not needed.

## 2. How coupled is the hub today

| Piece | Where | wx / GUI coupling |
|---|---|---|
| Hub server (LAN :13640, admin loopback port, token routes, uploads, events, push, go2rtc and Bambu MJPEG relays) | `RemoteHub.cpp` 5.6k lines, `RemoteNotify`, `WebPush`, `AppPush`, `HostedProvider`, `BambuCamRelay` | Server is Boost.Asio + libcurl + OpenSSL, wx-free. Only a `wxTaskBarIcon` tray (`HubApp`, `wxEntry`) and 2 `wxGetApp()` calls (HMS). |
| Printer status, control, send, archive, slicing, presets | `RemoteAccess` 2.7k, `RemoteControl` 1.5k, `RemoteSend` 1.9k, `RemoteEvents` 0.9k, `RemoteSnapmaker`, `GcodeArchive` | Heavy: ~250 `wxGetApp`/Plater/DeviceManager refs; requests run on the GUI thread; includes `Plater`, `PartPlate`, `GLCanvas3D`, `Tab`. |
| Bambu model | `DeviceManager.cpp` 7.7k lines | 51 `wxGetApp()` calls (app_config, preset_bundle, agent, push, HMS), includes `MsgDialog`/`Plater`/`ReleaseNote`; only 9 wx event uses. Extractable, not trivial. |
| U1 (Moonraker, mDNS) | `SnapmakerLan.cpp` 1.4k, `Utils/MoonRaker.cpp`, `MQTT.cpp`, `Http.cpp` | Mostly portable; `MoonRaker.cpp`/`PrintHost.cpp` use `wxString`. |
| Slicing | `PartPlate`/`Plater`/`BackgroundSlicingProcess` for the phone; `CLI::run` for `--slice` | CLI is windowless, not headless (links `libslic3r_gui`, thumbnails via GLFW/OSMesa; `--no-thumbnails` exists). The planned `PlateSet`/`slicerd` refactor (`docs/superpowers/specs/2026-09-01-headless-slicer-roadmap.md`, phases B/C) was **never built** (no `src/slicerd`, no `PlateSet`). |

POSIX gaps inside the hub itself (small): `run_capture()` returns false off Windows, so the Tailscale card/`tailscale serve` control does nothing on Linux; `go2rtc.exe` path is hard-coded and only `resources/tools/go2rtc/go2rtc.exe` is shipped (no Linux go2rtc/ffmpeg bundled); firewall helpers are Windows-only (no-ops are fine). Spawning is already POSIX-safe (double fork in `spawn_process`), and `RemoteHub.cpp` compiles on POSIX.

## 3. Architecture options

| | Description | Pros | Cons | Effort |
|---|---|---|---|---|
| **(a) Headless GUI app** | `--hub` + one `--hidden` instance under Xvfb + Mesa llvmpipe; VNC/web only for first setup | Reuses everything incl. phone slicing, archive, reprint, timelapse, U1 and Bambu paths; no refactor; matches the architecture already shipped on Windows | Needs X display + GL (GTK realises even a hidden canvas); ~1.5-2.5 GB RAM; heavy image (GTK, WebKit, Mesa); Linux GUI path is less exercised than Windows; modal dialogs auto-answered by the existing policy but first-run wizard/cloud login need a human | **2-3 weeks** to a usable image (after the spike) |
| **(b) GUI-free daemon** | Extract hub + `DeviceManager`/`NetworkAgent` + Remote* subset; slicing via CLI or future `slicerd` | Small image (~200 MB), 100-300 MB RAM, Pi Zero/NAS-friendly, no display stack | Large refactor: `DeviceManager` off `GUI_App` (interface for app_config/preset/agent/HMS/push), Remote* rewritten off Plater, slicing path = roadmap phases B+C ("L"); regression risk to the desktop | Status/control/events/push/camera only: **6-8 weeks**. With slicing parity: **3-4 months** |
| **(c) Containerise** | Packaging layer for (a) or (b): linux/amd64 + arm64 images | Reproducible, easy updates, NAS-native; arm64 CI runner already used by the Flatpak job and green on 2.4.2.0 | Host networking needed for Bambu SSDP; ARM compile is 60-90 min | **+1 week** on top of a/b |
| **(d) Desktop slices, box only watches** | Always-on box does status, push, camera, archive; slicing stays on desktop (or wakes it) | Smallest footprint; Pi-class hardware is enough | Same GUI-coupling problem for status (needs a or a new thin watcher: Bambu MQTT + Moonraker status ~3-5 weeks); phone slicing dead when desktop off, which is half the pitch | **3-5 weeks** for a fresh thin watcher; ~0 extra if built as (a) with slicing left idle |

Key insight: (a) already delivers "status, push, camera, then slicing" in one step, because the hidden instance slices today. Phase ordering by feature is not needed; ordering by **hardware class** is.

## 4. Per-feature portability (Linux, ARM)

| Feature | x86_64 Linux | aarch64 | Notes |
|---|---|---|---|
| Host build | AppImage in hosted CI (ubuntu-24.04) | Flatpak aarch64 job green on 2.4.2.0 (run 36858562565) | No aarch64 AppImage/deb; Flatpak is the only arm64 artifact and carries no UltraNet (private, deploy-key build is only in the AppImage/mac path). Docker would need its own build. |
| UltraNet (Bambu LAN) | Built in CI with the deploy key, `loadtest` passes (`get_version()` 02.01.01.53, ultranet@634feff8e7). Never exercised against a real printer on Linux (**unverified**) | `scripts/build_ultranet_posix.sh` is arch-agnostic (links the host deps prefix) so an arm64 runner should just work; **not built or tested today** | Bambu's own plug-in ships no ARM build, so UltraNet is the only route. Cloud sign-in is a stub on POSIX (`SessionStore`: no Keychain/libsecret as of the 2026-09-24 plan), so LAN-only + Developer Mode is the supported mode; that is also what current Bambu firmware needs for local control. |
| Bambu camera | `BambuCamRelay` (P1/A1 JPEG :6000) is in-hub and portable; X1/H2 RTSPS goes through go2rtc | same | Linux `BambuSource` camera tunnel does not exist, but the hub path does not use it. |
| go2rtc / ffmpeg | linux_amd64 builds exist upstream | linux_arm64 builds exist upstream | Must be bundled/installed and the exe path made portable. Pi 5 has no H.264 hardware encoder, so camera "Quality" transcodes are software (1-2 streams OK; **unverified** on the Pi). |
| U1 / Moonraker / mDNS | Fine | Fine | Plain HTTP/WebSocket/MQTT. |
| FlashForge | Legacy TCP-8899 / HTTP-8898 host paths are portable | same | FlashNetwork is an x64 Windows DLL (CMake installs `*.dll` only; needs FLASHNETWORK9.DAT), so Creator 5 camera/cloud via FlashNetwork stays Windows-only. Acceptable: not a blocker for Bambu + U1. |
| CLI slicing | `Snapmaker_Orca --slice ... --no-thumbnails` works without DISPLAY (Phase A, landed 2026-09-01); thumbnails need GLFW/OSMesa; G-code gets no embedded thumbnails (U1 previews) | same | Binary still links GTK/wx, so libs must be installed even when headless. |
| Slicing speed | N100 | Pi 5 | No published OrcaSlicer/PrusaSlicer timings for either (searched). Proxy: Geekbench 6 multi-core N100 3,272 vs Pi 5 1,523 (CNX Software), about 2x. Desktop CPUs are roughly 5-10x a Pi 5 (my estimate). So a plate that takes ~10 s on the desktop is ~30-50 s on an N100 and ~60-120 s on a Pi 5; heavy multi-colour/tree-support jobs can be tens of minutes on a Pi. |
| Memory | Hub + go2rtc ~150-250 MB; hidden GUI instance idle ~0.5-0.8 GB (**estimate, measure in the spike**); slicing 1-4 GB | same | Community Orca Docker images advise ~2 GB RAM and a 4 GB swapfile below 4 GB. Cap `MAX_INSTANCES` (6 today) to 1-2. |
| OpenGL | Mesa llvmpipe (GL 4.5, CPU) | Pi hardware GL is 3.1 and Orca >= 2.3.2 wants 3.2 for the G-code viewer, so llvmpipe is required | Fine for thumbnails/preview frames; costs CPU while rendering only. Pi OS 16K-page kernel breaks some allocators (jemalloc); `kernel=kernel8.img` restores 4K pages if a dependency trips. |

## 5. Hardware

| | Pi 5 8 GB | Intel N100/N150 mini PC (16 GB) | NAS Docker (Synology Plus / Unraid) |
|---|---|---|---|
| Cost | Board $95 official (Dec 2025), reports of $125 in Feb 2026 and ~$199 street (Jul 2026) due to DRAM prices; + PSU, active cooler, NVMe HAT and SSD: roughly $200-300 all-in | $110-$450, typical 16 GB/512 GB $150-220 with SSD and PSU | Already owned, else $300+; ARM or 2 GB-RAM models are unsuitable |
| Power | ~3 W idle, ~9-10 W load | ~6-10 W idle, 17-30 W load | NAS idle 15-30 W with disks (already running) |
| Slicing | ~1x baseline; thermal throttling without cooler | ~2x Pi 5 | Depends on CPU; J4125-class is Pi-5-like |
| Image | arm64 (needs new CI + UltraNet arm64 build) | amd64 (UltraNet and AppImage already built) | amd64 |
| Ease for non-technical users | Best story possible (pre-made SD/NVMe image, Raspberry Pi Imager customisation) but SD is wrong for archive/timelapse | Plug-in appliance only with a bootable image; otherwise Docker/Linux knowledge | Container Manager GUI is easy for NAS owners; host networking and RAM limits bite |
| Storage | **Boot from NVMe**, not SD (SD wears out under 24/7 writes; timelapse and archive are heavy writes) | Internal SSD | Volume on array |

Pick: N100/N150 for "full hub incl. slicing" (amd64 works first, 2x faster, similar all-in price now). Pi 5 8 GB for watch-only or patient slicers, after phase 2. A cloud VM is a poor fit: printers sit on the home LAN (Bambu SSDP/MQTT/FTPS, U1), so a home always-on node plus Tailscale subnet routing is needed anyway; a VM only makes sense as a printer-agnostic slicing worker (out of scope).

## 6. Setup, sharing and updates

- **Pairing without a desktop.** The hub page and admin plane are loopback-only, and the phone link token lives in `settings.json`. A headless box needs: the link/QR printed in the container log and served from a one-time setup page (or via SSH port-forward to the admin port). The phone link survives restarts. **hubid is per data dir**, so a new box means a new hub identity: re-pair the app and re-register push (the hosted forwarder keys on that identity); keep the data volume across upgrades.
- **Adding printers.** U1 has `/api/snapmaker/add`; Bambu LAN printers are added in the Device tab (IP + serial + access code). A new route (e.g. `/api/bambu/add`) is needed for headless use. SSDP discovery needs `network_mode: host`.
- **Desktop and hub sharing printers.** Separate data dirs mean access codes are entered twice; acceptable for v1. Longer term: desktop uses the hub as printer gateway or imports its printer list. Open risk (**unverified**): concurrent LAN MQTT sessions from desktop and hub to one Bambu printer (UltraNet keeps one LAN session per window; printers may cap local clients) and the G-code archive splitting between two machines (archive today fills only from sends on that PC).
- **Updates.** Docker image on GHCR with a version tag and watchtower/compose pull (pairs with the existing `version` and `features` fields in `/pair`); Pi image for non-technical users later; an apt repo is the worst fit (GTK/WebKit dependency tree). Same release pipeline as the AppImage (UltraNet from main, `ship_ultranet.sh` rule applies).

## 7. Security

- Today: one bearer token in the URL path (`/r/<token>/`), plain HTTP on the LAN, HTTPS only via Tailscale Serve (which sets `Tailscale-User-Login`, trusted only from a loopback peer). In a container, run Tailscale as a sidecar sharing the network namespace (`TS_SERVE_CONFIG` proxies 443 to 127.0.0.1:13640, per Tailscale's Docker docs) so that loopback trust still holds; otherwise `trusted_proxy_headers` rejects it, which is the safe default.
- Bambu access codes are stored in the app config in plaintext (also on Windows today). On a headless box: 0600 files, non-root container user, read-only rootfs where possible, no `docker.sock`, disk encryption, LAN-only firewall on 13640, never forward it to the internet.
- **Do not store Bambu cloud credentials on the box**: UltraNet has no secure store on Linux, cloud login disables local control on recent firmware anyway, and the hub does not need it. Use LAN-only + Developer Mode.
- The hub's Ed25519 private key (push identity) sits in `settings.json`; treat the data volume as a secret. Rotating the phone link (`new_link`) already exists. Per-device tokens are a worthwhile follow-up with the relay.
- Compose with the Tailscale ACLs (tag the node, restrict to the owner's phones).

## 8. Recommended path

1. **Phase 0, spike (3-5 days, no product change):** run the existing Linux x86_64 AppImage in WSL/VM under Xvfb with `--hub` and `--hidden`; confirm hub registration, hidden OpenGL warm-up on llvmpipe, idle CPU and RSS, a U1 mock gate (`mock_printhost`/mock U1) and a slice. In parallel add an arm64 UltraNet build + `loadtest` to CI (arm runner already used by Flatpak).
2. **Phase 1 (2-3 weeks): amd64 Docker image.** Xvfb + Mesa + extracted AppImage, Linux go2rtc + ffmpeg bundled and path made portable, POSIX `run_capture` (or a Tailscale sidecar config), tray off, setup page/log QR, `/api/bambu/add`, instance cap 1-2, volume layout, healthcheck, compose example for N100/Unraid/Synology. Gate: existing `tests/phone` mocks against the container; hardware test on one Bambu LAN printer and the U1.
3. **Phase 2 (2-3 weeks): arm64 image + Pi 5 recipe** (NVMe boot, active cooling, 4K-page kernel note), Pi Imager image, desktop-plus-hub MQTT coexistence test on hardware, update docs, relay.edgeslicer.com integration.
4. **Phase 3 (optional, 6-10 weeks):** GUI-free status daemon (option b-lite: extract `DeviceManager` behind an interface + hub), then slicing via `slicerd`/PlateSet if demand justifies it. Desktop-uses-hub-as-gateway later.

Total to a useful Pi/N100 hub: roughly **5-7 engineer-weeks** (phases 0-2). Full GUI-free daemon with slicing: add 3-4 months.

**Main risks:** (1) GUI stack under Xvfb on Linux is unproven for the hidden-instance path (memory, idle CPU, GTK/WebKit deps); (2) UltraNet on Linux/arm64 has no real-printer verification and no cloud session store; (3) Bambu firmware/LAN-session limits and the AGPL/ToS climate around third-party Bambu clients (see `tests/plan_bambu_macos_linux.md`); (4) Pi DRAM-driven price swings and slow slicing set wrong expectations; (5) a second data dir doubles credential handling and archive state; (6) support burden of a Linux appliance for non-technical users.

## 9. Spike results (2026-10-03)

Phase 0 run on this PC (Windows 11, 20 logical cores, 32 GB) in WSL2 Ubuntu 24.04, then in Docker Desktop (linux/amd64). Build under test: the official `v2.4.2.0-edge` Linux AppImage (sha256 `47a682a7...a45`), data dir inside WSL / a Docker volume only, no printer ever contacted (loopback mocks). Nothing in the app was changed.

**Verdict: the hub-plus-slicer-under-Xvfb recipe works today, with one non-negotiable workaround (a *visible* instance on Xvfb; the *hidden* mode is broken on Linux) and a short list of Windows-only gaps.** Result by question:

| Question | Result |
|---|---|
| Starts headless? | Yes: `Xvfb` + Mesa llvmpipe (GL 4.5) + `AppRun --hub --datadir D --hub-phone`, then one instance spawned through the hub's admin plane. Hub listening 0.7 s after launch; instance registered in 3-9.5 s (9.5 s on a brand-new data dir, 5 s after a container restart). |
| Idle RAM / CPU (WSL) | hub 161 MB RSS, 12 threads, 0.0 % CPU; instance 541 MB RSS idle (83 threads), 0.2 % CPU; Xvfb 66-70 MB. All three about **0.75 GB, 0.2 % CPU**. |
| After slicing | RSS stays high: 1.63 GB peak and retained after a 3-object plate, CPU back to 0.2 % within seconds. Budget **2 GB** as the study estimated. |
| Docker | Image 2.03 GB on disk; idle container 0.14 % CPU, 962 MiB in `docker stats` (RSS sum 750 MB); `healthy`; survives `docker restart`. |
| Hub reachable from Windows? | WSL: `localhost:<port>` from Windows works (WSL relay) and so does the WSL VM IP. The Windows LAN IP does **not** reach WSL in NAT mode (needs `networkingMode=mirrored` or a `netsh interface portproxy`; not changed here). Docker published port: `localhost:13650` and `10.0.0.131:13650` (this PC's own LAN address) both answered. A second device on the LAN was not available to test. |
| Works without printers | `GET /r/<token>/pair, /summary, /state, /events, /api, /api/instances, /api/archive, /manifest.webmanifest, /push/key`; admin plane `GET /hub/info, /instances, /state, /events, /notify, /push, /apppush`, `POST /hub/new, /hub/remote`; per instance `GET /api/info, /api/presets, /api/plates, /api/printers, /api/snapmaker/devices, /api/plates/0/thumbnail.png, /preview.png`, `POST /api/slice, /api/presets/select`, upload via `POST /i/<pid>/open`. |
| Against the mock U1 | `mock_u1_controls.py` added with `POST /api/snapmaker/add?ip=127.0.0.1:18189`: `/api/printers` shows it (4 toolheads, controls), `/summary` carries it, `set_light` / `set_speed` / `set_temp` reach the mock as `SET_LED`, `M220`, `SET_HEATER_TEMPERATURE`, and a mock `printing` flip produced a hub `started` event ("Mock U1 controls started printing") 2 s later. Add, printers list and `set_light` were repeated through the Docker container from Windows. |
| Phone-style slice + send | Upload `.obj/.3mf` through the hub, slice, thumbnail and layer preview PNG render on llvmpipe, then `POST /api/plates/0/send mode=upload` to the `mock_printhost.py` kept in the owner's `snorca_hubtest` scratch (not in the repo; stored a 6.7 MB `.gcode` there). The slice, thumbnail and mock control path were also done in the container. |
| Timelapse listing | **Not testable**: the hub has no U1 timelapse route (grep: nothing in `Remote*.cpp`); only the Bambu storage/timelapse research exists. |
| Bambu / UltraNet on Linux | Not exercised: the fake Bambu printer lives in the private UltraNet tools. The AppImage does carry `bin/ultranet/libbambu_networking.so` + `libBambuSource.so`. |

### Slicing numbers (20-core desktop; use CPU-seconds, not wall time, to scale to a small box)

| Model (on the U1 0.4 profile) | wall | CPU time | RSS peak |
|---|---|---|---|
| `ipadstand.obj` | 1.3 s | n/a | n/a |
| `frog_legs.obj` | 1.1 s | 5.5 s | 693 MB |
| `extruder_idler.obj` | 1.6 s | 12.4 s | 768 MB |
| `mcbrim_test.3mf` (3 objects) | 7.6 s | 45.3 s | 1,626 MB |

On a 4-core N100 the 45 s CPU job is roughly 20-40 s of wall time (**estimate**, not measured); a Pi 5 about twice that.

### Blockers and gaps found (most important first)

1. **Hidden mode does not work on Linux/GTK; slicing in it crashes the instance.** Startup logs `post_init: hidden instance: OpenGL warm-up FAILED` (`GLCanvas3D::ensure_gl_ready()`: GTK cannot make a GL context current on an unrealised canvas, as the comment there already warns), the ImGui font atlas is never built, and the first slice dies with SIGSEGV in `ImFont::CalcTextSizeA <- NotificationManager::PopNotification::count_spaces <- SlicingProgressNotification::set_status_text <- Plater::priv::on_slicing_update` (null font). **Workaround, and what the recipe uses:** run the instance *visible* (`POST /hub/new` without `hidden=1`, i.e. `SNORCA_HIDDEN=0`) on Xvfb, where nobody sees the window. Real fixes: make the hub spawn visible instances on Linux, or realise the canvas in a shown 1x1/off-screen window, plus a null-font guard in the notification measuring.
2. **A fresh data dir needs a human**, and the visible path has none of the hidden-mode auto-answers: the "no printer configured" wizard, the privacy prompt, and the **"use system SSL certificate: /etc/ssl/certs/ca-certificates.crt" modal** (blocks start-up forever). Without `en_US.UTF-8` generated, start-up stops on a "Switching language failed" message box (`AppRun` sets `LC_ALL=C`). All of it is solved by shipping a seeded `EdgeSlicer.conf` (see `docker/hub/seed.conf.json`; JSON, newline, `# MD5 checksum <upper-case md5 of the JSON text>`) and `locale-gen`. Note the config file is `EdgeSlicer.conf` now, not `Snapmaker_Orca.conf` (a seeded old name is ignored). A real setup flow (printer choice, Bambu add) is still needed.
3. **Nothing supervises the instance.** If the instance dies (e.g. blocker 1) the hub keeps serving `/summary` with the last printer rows and an ever-growing `age_s`, `/api/printers` is 404, and nobody respawns it. The entrypoint draft polls `/hub/instances` and re-spawns.
4. **Windows-only parts of the hub, confirmed:**
   - *Tailscale*: `run_capture()` is `_WIN32`-only, so `tailscale status/serve` are never run. `/hub/info` reports `remote.access.state: not_installed` with a *Windows* download link, and `POST /hub/remote` just returns the same state. Use a Tailscale sidecar/host install with `tailscale serve` configured outside the app (loopback-peer trust still holds when sharing a network namespace).
   - *go2rtc / ffmpeg*: only `resources/tools/go2rtc/go2rtc.exe` exists; `/hub/info` shows `go2rtc_exe: .../go2rtc.exe`, `go2rtc_port: 0`, so X1/H2 RTSPS and the "Quality" transcodes have no relay. Needs `linux_amd64` / `linux_arm64` go2rtc + ffmpeg bundled and the hard-coded `.exe` path made portable (`go2rtc_exe_path()`; `ffmpeg_path()` also uses `where`, Windows-only). The P1/A1 JPEG relay (`BambuCamRelay`) is in-process and did start.
   - *Firewall helper text*: `/hub/info` shows a `netsh advfirewall ...` hint on Linux (cosmetic).
5. **LAN URL is whatever the hub's own interface is.** `lan_url`/`/pair` carry the WSL VM address (172.17.x) or the container address in Docker bridge mode, which the phone cannot reach. Use `--network host` (also needed for Bambu SSDP) or add an override.
6. **Port 13640 is taken by the Windows hub on this PC.** Inside WSL the hub binds the first free of 13640..13659 *in its own network namespace*, so I had to pre-occupy 13640-13644 in WSL (hub went to 13645) for the localhost-forwarding test. Not an issue on a dedicated box. (While finding this I sent two read-only GETs with a wrong token to the real Windows hub: 404, no state touched.)
7. **Container restart on the same volume dies without cleanup**: a stale `/tmp/.X99-lock` makes Xvfb refuse `:99` and the hub then exits for lack of a display; `hub/hub.json` and `hub/instances/*.json` carry PIDs from the previous run and are removed defensively. Both are in the entrypoint draft. `libopengl0` and `libglu1-mesa` are also required on a minimal base image (the AppImage stops with "missing host OpenGL runtime library libOpenGL.so.0").
8. Minor: the first load of a fresh data dir logs hundreds of `The config .../system/Snapmaker/filament/... contains incorrect keys: filament_cooling_before_tower, ...` lines at `error` level (system filaments carrying newer keys; noise, not a failure).

### Repeat it (WSL, Ubuntu 24.04; `docker/hub/` holds the same recipe as a Dockerfile)

```bash
# once, as root
apt-get install -y xvfb mesa-utils libgl1-mesa-dri libglx-mesa0 libegl1 libopengl0 libglu1-mesa \
    libgtk-3-0t64 libwebkit2gtk-4.1-0 libsecret-1-0 locales xauth curl
locale-gen en_US.UTF-8

# build
gh release download v2.4.2.0-edge -R aceRage/EdgeSlicer -p "*AppImage"
chmod +x EdgeSlicer_Linux_Ubuntu2404_V2.4.2.0.AppImage && ./EdgeSlicer_Linux_Ubuntu2404_V2.4.2.0.AppImage --appimage-extract

# data dir: separate, seeded (printer preset + first-run answers); never the Windows %APPDATA%\EdgeSlicer
D=$HOME/es_hub_data; mkdir -p $D
body=$(cat docker/hub/seed.conf.json)
printf '%s\n# MD5 checksum %s\n' "$body" "$(printf '%s' "$body" | md5sum | cut -d' ' -f1 | tr a-f A-F)" > $D/EdgeSlicer.conf

# headless run
Xvfb :99 -screen 0 1280x800x24 -nolisten tcp &
export DISPLAY=:99 LIBGL_ALWAYS_SOFTWARE=1
squashfs-root/AppRun --hub --datadir $D --hub-phone &                 # hub: port in $D/hub/hub.json
sleep 3; AP=$(grep -o '"admin_port": *[0-9]*' $D/hub/hub.json | grep -o '[0-9]*$')
SEC=$(grep -o '"secret": *"[^"]*' $D/hub/hub.json | sed 's/.*"//'); TOK=$(grep -o '"token": *"[^"]*' $D/hub/hub.json | sed 's/.*"//')
curl -X POST -H "X-Hub-Secret: $SEC" -d x=1 http://127.0.0.1:$AP/hub/new   # VISIBLE instance (not ?hidden=1)

# poke it (port = "port" in hub.json, 13640 unless taken)
curl http://127.0.0.1:13640/r/$TOK/summary
PID=$(curl -s http://127.0.0.1:13640/r/$TOK/api/instances | grep -o '"pid":[0-9]*' | head -1 | cut -d: -f2)
python3 tests/phone/mock_u1_controls.py 18189 &                         # mock U1, loopback only
curl -X POST -d x=1 "http://127.0.0.1:13640/r/$TOK/i/$PID/api/snapmaker/add?ip=127.0.0.1:18189"
curl -X POST -H "X-File-Name: frog_legs.obj" --data-binary @tests/data/frog_legs.obj http://127.0.0.1:13640/r/$TOK/i/$PID/open
curl -X POST -d x=1 "http://127.0.0.1:13640/r/$TOK/i/$PID/api/slice?plate=0"
```

Docker (draft, built and run locally, never pushed): copy the AppImage into `docker/hub/`, `docker build --platform linux/amd64 -t edgeslicer-hub-spike docker/hub`, `docker run -d --name edgeslicer-hub-spike -p 13650:13640 -v edgeslicer-hub-spike-data:/data edgeslicer-hub-spike`; the phone link token is in the volume's `hub/hub.json`. Use `--network host` on a real LAN.

### UltraNet on aarch64: what a CI job would need (private repo, nothing dispatched)

Read from `aceRage/ultranet` `.github/workflows/posix.yml` / `CMakeLists.txt` / `tools/ci_build_openssl.sh` and the host's `scripts/build_ultranet_posix.sh`, `build_orca.yml`:

- The build looks arch-agnostic: no SIMD or assembly in the plug-in's CMake, the pinned OpenSSL 3.5.7 is configured `no-asm`, and the host script links the host deps prefix's static OpenSSL and builds `bambu_networking`, `BambuSource` and `loadtest`, then runs `loadtest` (prints `get_version()`).
- The private repo's own workflow is `ubuntu-24.04` x86_64 only (Linux loopback gate with `fakeprinter.py`, which needs `sudo` for FTPS port 990; storage gate; ~8 min reconnect gate; macOS only on demand at 10x billing). An arm64 copy would be a second matrix row on `ubuntu-24.04-arm` using the same arch-neutral `ci_build_openssl.sh`. Arm runners on a private repo are billed minutes, so **do not add it there**.
- Cheaper: build it in the *public* `aceRage/EdgeSlicer` workflow, which already fetches UltraNet with `ULTRANET_DEPLOY_KEY` on public (free) minutes. The Flatpak job already has an `ubuntu-24.04-arm` row but ships no UltraNet. A new aarch64 Linux job (or a step in it) needs: the aarch64 deps prefix (`deps/build/destdir/usr/local`, built by `build_deps`, 60-90 min cold, cacheable), `ninja`, the `ULTRANET_DEPLOY_KEY` secret, `bash scripts/build_ultranet_posix.sh <ultranet-src> <out>` with `EDGESLICER_VERSION` set, and `ULTRANET_BIN_DIR=<out>` for packaging into whatever arm64 artifact the hub image uses (there is no aarch64 AppImage today, only the Flatpak). The arm64 `loadtest` runs natively on the arm runner. A `fakeprinter.py` gate on arm would need the private repo's tools checked out the same way.
- Not covered by any gate today: UltraNet against a real printer on Linux or arm64, and Bambu cloud sign-in (no secure store on POSIX, per the study).

### Things only real hardware can answer (the owner offered to buy test hardware)

- **Raspberry Pi 5 8 GB + active cooler + NVMe (boot from NVMe)**: llvmpipe frame cost for thumbnails/preview, real slice times (compare against the CPU-second figures above), thermals under a 20-minute slice, the 16K-page Pi OS kernel vs the app's allocators, go2rtc + ffmpeg software transcode of one H.264 camera, idle RSS of the arm64 image. Needs the aarch64 UltraNet and an arm64 AppImage/image build first.
- **Intel N100/N150 mini PC, 16 GB, SSD** (the study's pick): real wall time of the `mcbrim_test.3mf` job, 24/7 idle RSS/CPU drift over days, power draw, whether Xvfb + the visible instance stays responsive during a slice.
- **A real Bambu LAN printer (H2D or X1C, Developer Mode) from the Linux box**: UltraNet on Linux has never talked to a printer; concurrent LAN sessions from the desktop and the box to one printer; SSDP with host networking; X1/H2 RTSPS camera through a Linux go2rtc.
- **A real U1 on the LAN** from the box (the mocks follow its documented API, but mDNS discovery and the filament macros were only checked against mocks).
- **A second device on the LAN** (a phone) hitting `http://<host-lan-ip>:13640/r/<token>/` through WSL mirrored mode or a host-networked container: this PC only proved its own LAN address.

### Cleanup done

WSL Ubuntu scratch (`~/es_spike` incl. the 187 MB AppImage and 545 MB extracted tree, `~/es_hub_data`, `~/es_logs`), the Windows scratch `%TEMP%\spike`, and the Docker container, image and volume named `edgeslicer-hub-spike*`. Apt packages stay installed in the Ubuntu distro (xvfb, mesa-utils, libgtk/webkit, gdb, strace, x11-utils, imagemagick, x11-apps). The `PJARCZAK-BAMBU` distro and the Windows `%APPDATA%\EdgeSlicer` were not touched.

## 10. Linux headless blockers: fixes and verification (2026-10-03)

Branch `feat/linux-headless-hub` (on top of the spike). Service mode is `--hub-service` on the hub or `EDGESLICER_SERVICE=1`; nothing changes for a desktop start. Recipe, flags and the camera policy: `docker/hub/README.md`.

| Spike blocker | Fix |
|---|---|
| 1. Hidden mode crashes on Linux/GTK | `ImGui::CalcTextSize` (deps_src/imgui) no longer dereferences a missing font or an empty atlas (rough size instead; notifications re-measure on their first real frame), so a hidden or not-yet-realised canvas cannot crash on any platform. On Linux a service-mode instance shows its window on the virtual display (GTK needs a realised canvas for GL) while staying "hidden" for the hub, so every hidden-instance rule (auto-answered dialogs) still applies; `RemoteAccess::raise_attention` does not un-hide it in service mode. The warm-up is retried from the event loop until the window is realised (it is, after one retry). Windows keeps its true hidden mode. A hidden instance also no longer starts a new project over a model loaded before its start-up finished (found by the CI smoke test: "plate is empty"). |
| 2. First start needs a human | `AppConfig::seed_service_defaults()` writes a minimal `EdgeSlicer.conf` when none exists (first-run answered, Snapmaker U1 0.4 as the one printer); in service mode no wizard, the TLS-store question is answered yes and remembered, a missing locale is logged and the app continues in English, the Bambu setup notice is not shown. |
| 3. No supervisor | A service-mode hub starts its instance, notices a death (`HubSupervisor.hpp`, unit-tested), respawns with backoff (2 s doubling to 2 min, reset by a minute of health), drops printer rows nobody refreshes for 3 minutes, answers `GET /api/printers` itself and answers a dead `/i/<pid>/api/printers` from its own rows. `/hub/info` shows `service {respawns, failures, next_spawn_in_s}`. |
| 4. Camera relay | `go2rtc` path cross-platform; `start_go2rtc` no longer Windows-only (tied to the hub with `PR_SET_PDEATHSIG`, reaped in the loop). Official go2rtc v1.9.14 (MIT) amd64/arm64 bundled in the AppImage and the Flatpak (`scripts/fetch_go2rtc_linux.sh`, sha256 pinned and checked; Flatpak sources with sha256). **ffmpeg: an LGPL build exists, so that is what ships** (below). |
| 5. Tailscale on POSIX | `run_capture` implemented with `posix_spawnp`, a pipe and a deadline; the hub's status/serve controls work when the `tailscale` CLI exists (checked against a stand-in CLI), download link is the Linux one. |
| 6. LAN URL in containers | `EDGESLICER_HUB_PUBLIC_HOST` / `--hub-public-host`: `host`, `host:port`, `[v6]:port` or a URL; used by `/hub/info`, `/pair`, `/summary`, the tray and push links. The entrypoint still clears stale `.X99-lock` and `hub.json`. |

### ffmpeg on Linux: LGPL, same family as the Windows package

The Windows package ships an LGPL-3.0 BtbN/FFmpeg-Builds static build (`--enable-version3`, no `--enable-gpl`, x264/x265 disabled, H.264 encoder libopenh264; `docs/superpowers/specs/2026-09-12-bundled-ffmpeg.md`). BtbN publishes the same builds for `linux64-lgpl` and `linuxarm64-lgpl`; the FFmpeg 8.1 release-branch static builds contain libopenh264 (and the QSV, VAAPI and V4L2 M2M wrappers) and need only glibc. So the owner's rule (LGPL where it exists) gives: bundle it, no GPL build anywhere.

* Pinned in `scripts/fetch_ffmpeg_linux.sh` and the Flatpak manifest: release `autobuild-2026-10-01-13-06`, `ffmpeg-n8.1.3-14-g330caae0c1-linux64-lgpl-8.1.tar.xz` (sha256 `3c2c4d6066432b2eab54be830d3ce1f5b68a3f34ecaa6d9f2d0230a49e778560`) and `...linuxarm64-lgpl-8.1.tar.xz` (`d407cc9405caf75d67b3a10c9793f1db38d55705d09ac47d279d36d2823cb73d`). Only the `ffmpeg` binary (142 MB / 117 MB) and `LICENSE.txt` are taken.
* Beside it: `FFMPEG-LICENSE.txt` (the LGPL-3.0 text) and `FFMPEG-NOTICE-LINUX.txt` (what it is, the exact release, archive and sha256, FFmpeg source commit `330caae0c1`, the BtbN recipe, a written offer of the source for three years).
* **Pin rot, and the mirror.** BtbN deletes its dated tags after a couple of weeks, so these URLs would stop working. Both Linux archives, the FFmpeg source at commit `330caae0c1`, the BtbN recipe at the commit the tag pointed to and the licence texts are mirrored, byte-identical, in the public repo `aceRage/edgeslicer-deps` (release `ffmpeg-n8.1.3-btbn-2026-10-01`); the fetch script and the Flatpak manifest use the mirror first and BtbN second, the sha256 check unchanged, and the written offer in `FFMPEG-NOTICE-LINUX.txt` points at the mirror. The Windows package's `ffmpeg.exe` (sha256 `ff2adedd...`, its pin autobuild-2026-09-12-13-12 was already gone upstream) is mirrored there too, in release `ffmpeg-win64-lgpl-btbn-2026-09-12`, found on the build VM and checked against the pinned hash; the original zip no longer exists anywhere we could find.
* Sizes: AppImage amd64 241 MB, arm64 about 229 MB; Flatpak x86_64 200 MB, aarch64 188 MB; hub image 1.52 GB on disk (727-756 MB as a `docker save` .tar.gz).

### Camera policy: passthrough first (added after the monitoring comparison)

`tests/research_hub_monitoring_comparison.md` section 4.1 and the owner's follow-up: a phone watches one, or a few, cameras at a time; the target on a Pi-class host is one camera working properly.

* **Passthrough is the default.** Medium/Low are opt-in, lazy (go2rtc starts ffmpeg only while someone watches), and offered only if a real test encode passed at start-up: x86 QSV, VAAPI (need `/dev/dri`), ARM `h264_v4l2m2m` (Pi 4, `/dev/video11`), then software libx264/libopenh264, software only on x86 with 4+ cores. A Pi 5 or other aarch64 host without a hardware encoder gets **one software transcode at Low** (not strictly passthrough-only); x86 with fewer than 4 cores and no GPU is passthrough-only. Windows is unchanged apart from the test encode (software, no cap).
* **Cap** on concurrent transcodes (`EDGESLICER_MAX_TRANSCODES`, `--hub-max-transcodes`, settings.json): default 1 on Pi-class ARM, 2 with a Pi 4 hardware encoder, cores/2 (at most 4) on x86 software, 6 with x86 hardware, none on Windows. Counted per distinct stream (viewers of one printer's Low stream share one ffmpeg); past the cap a viewer is served the source stream, never an error; the slot is held while the viewer's WebSocket is open, so leaving printer A frees it for printer B.
* **Advertised** in `/pair` (`features`: `camera_passthrough`, `camera_transcode`, `camera_still`; `camera` object), `/state` and `/hub/info`: encoder, hardware, the steps, the cap, active and downgraded counts, every probe result, the ports.
* **Still-image fallback** `GET /r/<token>/still?id=&fps=&w=` for H.264 cameras: keyframes only (`-skip_frame nokey`), 160-1280 px, MJPEG, from the relay's loopback RTSP (one connection to the printer); `fps` is a ceiling (no picture is repeated); capped (4, 2 on ARM).
* **Ports** for go2rtc's API, RTSP and WebRTC are configurable (environment, `--hub-go2rtc-*-port`, settings.json); a taken port moves to the next free one on a desktop hub with a log line and refuses to start in service mode (message in the log and in `hub/hub_error.txt`); the RTSP restream can be opened to Home Assistant or Frigate on another machine only with credentials. Recipe in `docker/hub/README.md`.
* The hardware encoder paths (QSV, VAAPI, V4L2 M2M) follow ffmpeg's documented arguments and are covered by the probe-ordering unit tests with a mocked probe, but **have not run on real hardware**.

#### Measured cost (one stream, software libopenh264; synthetic `testsrc2` content, so real cameras will cost more)

| Where | still (keyframes to MJPEG, 480 px) | full decode | Low 854 px 10 fps 600 kb/s | Medium 1280 px 15 fps 1.5 Mb/s |
|---|---|---|---|---|
| Dev PC, Windows 11, i7-12700K, 720p15 file | 0.3 % of one core | 2.4 % | 7.3 % | 9.5 % |
| Dev PC, Windows 11, 1080p15 file | 0.2 % | 3.8 % | 8.8 % | 13.7 % |
| WSL2 on the same CPU, 720p15 mock RTSP camera through the hub | 1.3 % | 2.8 % | 7.5 % (10.9 % measured inside the live hub path) | 8.1 % |

Through the hub on the WSL box: go2rtc itself 0.6 % of a core for a passthrough viewer, 0.8 % while it feeds a transcode. Checked live with two mock cameras and a cap of 1: viewer 1 on `mockcam1_low` gets the transcoder (one ffmpeg child), viewer 2 on `mockcam2_low` is served `mockcam2` at full quality (2 MB in 8 s) with the log line `transcode cap reached (1); mockcam2_low is served as mockcam2`, `active_transcodes` 1 / `downgraded` 1; when the viewers leave there are 0 ffmpeg children and 0 active slots, and the next viewer of the other printer gets a transcoder. A still viewer's ffmpeg is gone when the client closes.

**arm64 (native, GitHub's hosted `ubuntu-24.04-arm` runner, 4 vCPU Neoverse):** the CI smoke test passed there (hub up, instance started by the hub, slice, thumbnail, go2rtc, LGPL ffmpeg with libopenh264, kill and respawn; idle 2 % CPU, 1.0 GiB). No transcode cost was measured on arm64: QEMU emulation was not available on this Docker Desktop at the time and its numbers would not be representative of a Pi anyway, so none are reported.

**What a real Pi 5 (8 GB, NVMe, active cooler) test should check:** `docker run` the arm64 image and read `hub/info` `camera` (expect `encoder: libopenh264`, `quality: [low]`, `max_transcodes: 1`, no probe for v4l2m2m); one 1080p X1/H2 camera and one U1: CPU of the Low transcode (the go2rtc `ffmpeg` child) as a percentage of one of the four cores at 854x480/10 fps, over several minutes and with the SoC temperature; the same for passthrough (go2rtc) and for the still endpoint; a second viewer on another printer while the first holds the slot (expect passthrough, not an error) and the slot freeing within seconds of leaving; a hub with the slicer instance idle plus one transcode: total RSS and whether the phone page stays responsive; on a Pi 4 the `/dev/video11` probe and whether `h264_v4l2m2m` really encodes through go2rtc.

### Verification

* Windows (shared script, Ninja, JOBS=8): `slic3rutils_tests` all pass (the new cases cover service mode, supervisor, public host, tool lookup, encoder detection and, for the camera policy, `hub_media_tests.cpp`: candidate order per host, mocked probe, caps, the gate, query rewriting, ports and RTSP exposure); `libslic3r_tests` 1570 of 1573 pass, 3 failed as expected (as before), including the 4 seed cases.
* CI (`build_all.yml` with the new `only` input, public repo): AppImage + hub image amd64 and arm64, Flatpak x86_64 and aarch64 (both built; go2rtc inside is the pinned sha256 on both arches). Every Linux run builds `docker/hub` from its own AppImage and runs `docker/hub/smoke.sh` on it.
* Linux and Docker verification as before (WSL "Ubuntu" under Xvfb with no seeded config; Docker Desktop with `-p` and `EDGESLICER_HUB_PUBLIC_HOST`); mocks only, no real printer or camera was contacted.

### Still missing

* The hardware encoders (QSV, VAAPI, V4L2 M2M) on real hardware; transcode cost on a real Pi; UltraNet against a real printer on Linux (it builds and passes its loadtest on aarch64).
* A watchdog for a hung (alive but unresponsive) instance; go2rtc is reaped and logged but not restarted if it exits.
* A setup flow for the real printers (the seeded U1 is a placeholder; Bambu LAN add has no hub route yet) and a one-time pairing page.
* The phone page does not use the still endpoint yet (it is advertised in `features`); Gtk-CRITICAL `gtk_widget_set_size_request` lines at start-up (harmless) and `filament_cooling_before_tower` config noise in the log.

## Sources

- Repo: `RemoteHub.hpp/.cpp`, `RemoteAccess.hpp/.cpp`, `DeviceManager.cpp`, `docs/superpowers/specs/2026-09-01-headless-slicer-roadmap.md`, `2026-09-02-hidden-service-mode.md`, `tests/plan_bambu_macos_linux.md`, `scripts/flatpak/README.md`, `build_all.yml`/`build_orca.yml`, CI run 36858562565.
- [CNX Software: Pi 5 vs N100 mini PC benchmarks and power](https://www.cnx-software.com/2024/04/29/raspberry-pi-5-intel-n100-mini-pc-comparison-features-benchmarks-price/)
- [CPU-Monkey: Pi 5 vs N100](https://www.cpu-monkey.com/en/compare_cpu-raspberry_pi_5_b_broadcom_bcm2712-vs-intel_processor_n100)
- [Raspberry Pi: Pi 5 price rises, Dec 2025](https://www.raspberrypi.com/news/1gb-raspberry-pi-5-now-available-at-45-and-memory-driven-price-rises/)
- [Tom's Hardware: second Pi 5 price rise](https://www.tomshardware.com/raspberry-pi/raspberry-pi-5-price-increases-drastically-as-ai-shortage-bites-16gb-version-now-usd205-second-price-increase-in-three-months-over-70-percent-more-expensive-than-original-msrp)
- [matszwe02/OrcaSlicer_arm (RAM/swap and OpenGL 3.2 note)](https://github.com/Matszwe02/OrcaSlicer_arm)
- [Pi 5 NVMe vs SD for 24/7 servers](https://raspberry.tips/en/raspberrypi-tutorials/nvme-ssd-raspberry-pi-5-buying-guide)
- [Pi 5 16K page size / jemalloc issues](https://github.com/quickwit-oss/quickwit/issues/4785)
- [Tailscale Docker parameters (TS_AUTHKEY, TS_SERVE_CONFIG)](https://tailscale.com/docs/features/containers/docker/docker-params)
