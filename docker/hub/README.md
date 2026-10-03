# EdgeSlicer phone hub in a container (service mode)

The hub (phone page, camera relay, printer list, push) plus one slicer instance, running unattended
on a Linux box, in a container or under systemd. Background and measurements:
`tests/research_always_on_hub.md` (sections 9 and 10).

What the container runs:

```
Xvfb :99                                  a virtual display (nobody looks at it)
EdgeSlicer --hub --hub-service --hub-phone
  go2rtc                                  the camera relay, bundled in the AppImage
  EdgeSlicer (GUI instance, "hidden")     started, watched and respawned by the hub
```

## Build and run

The image is built from an AppImage made from this branch or later (it carries `go2rtc` and ffmpeg; the Dockerfile
refuses one that does not). CI builds one per push (`build_all.yml`, artifact `EdgeSlicer_Linux_ubuntu_2404_*`).

```
docker build -t edgeslicer-hub docker/hub --build-arg APPIMAGE=EdgeSlicer_Linux_AppImage_Ubuntu2404_<ver>.AppImage
docker run -d --name edgeslicer-hub --restart unless-stopped --network host -v edgeslicer-hub-data:/data edgeslicer-hub
docker logs edgeslicer-hub        # "hub up: port 13640, phone link path /r/<token>/"
```

`--network host` is what a real LAN wants (Bambu SSDP discovery, the phone reaching the hub on the host's own
address). Without it, publish the port and say which address phones should use:

```
docker run -d --name edgeslicer-hub -p 13640:13640 -e EDGESLICER_HUB_PUBLIC_HOST=192.168.1.50 -v edgeslicer-hub-data:/data edgeslicer-hub
# the published port differs: -p 13650:13640 -e EDGESLICER_HUB_PUBLIC_HOST=192.168.1.50:13650
```

The phone link is `http://<host>:<port>/r/<token>/`; the token is in the volume's `hub/hub.json` and in the log.
It survives restarts and upgrades as long as the `/data` volume does (the hub identity and push registrations live there too).

## Images and CI

`build_all.yml` (manual run, input `only`): `appimage` builds the x86_64 AppImage and this image, `arm64` the arm64 AppImage and
an arm64 image on GitHub's hosted arm runner (native, no emulation). Each image is started in CI by `docker/hub/smoke.sh`
(instance starts, slice, thumbnail, go2rtc and ffmpeg present, kill and respawn) and kept as a workflow artifact for three
days; nothing is pushed to a registry. Run the same check locally with `docker/hub/smoke.sh <image> tests/data/frog_legs.obj`.

## Service mode

`--hub-service` on the hub, or `EDGESLICER_SERVICE=1` in the environment (the image sets it), turns on:

| Behaviour | Detail |
| --- | --- |
| Seeded first start | A data dir with no `EdgeSlicer.conf` gets a minimal one: first-run answered (privacy "no", first guide done), one printer (Snapmaker U1 0.4) so there is nothing to ask. A config that exists is never touched. |
| No dialogs | No printer wizard, no privacy prompt, the "use the system TLS certificate store" question is answered yes (and remembered), a missing `en_US.UTF-8` falls back to English with a log line instead of the "Switching language failed" box. |
| Instance | The hub starts one instance and keeps it: when it dies the hub notices, logs it, and starts another (backoff 2 s, doubling to 2 min if it keeps dying young). `EDGESLICER_SERVICE_INSTANCES=0..4` changes how many (0 = the hub starts none). |
| Rows | Printer rows nobody has refreshed for 3 minutes after their instance is gone are dropped from `/summary`. `GET /r/<token>/api/printers` answers from the hub with no instance (and a dead `/i/<pid>/api/printers` answers from the hub too, naming the pid that is gone). |
| OpenGL | On Linux the instance shows its window on the virtual display (GTK cannot make GL current on a window that was never shown); to the hub it stays "hidden", so every hidden-instance rule (no modal dialogs, auto-answers) still applies. Windows keeps its true hidden mode. |

`GET /hub/info` (admin plane) shows `service: {on, respawns, failures, next_spawn_in_s}`.

## Camera relay

**Passthrough first.** A camera is relayed as the printer sends it (go2rtc, a few percent of a core per stream). The phone's
Medium/Low steps are re-encodes, opt-in, started by go2rtc only while someone watches. The hub offers them only when a real
test encode succeeded at start-up (`ffmpeg -encoders` alone is never trusted) and says what it found in `GET /pair` and the
admin plane's `GET /hub/info` under `camera`: `{passthrough, quality, transcode, encoder, hardware, reason, max_transcodes,
active_transcodes, downgraded, still, probes, ports}`; `features` carries `camera_transcode` / `camera_still` accordingly, so
the app can hide steps that are not available.

| Host | Probe order | Steps offered | Concurrent transcodes (default) |
| --- | --- | --- | --- |
| x86 Linux | `h264_qsv`, `h264_vaapi` (both need `/dev/dri`), then software `libx264` / `libopenh264` if there are 4+ cores | Medium, Low | hardware 6; software cores/2, at most 4 |
| ARM Linux (Pi 4) | `h264_v4l2m2m` (needs `/dev/video11`), then software | Medium, Low | 2 |
| ARM Linux without a hardware encoder (Pi 5, other aarch64) | software `libopenh264` | **Low only** | **1** |
| x86 with fewer than 4 cores and no GPU | none | passthrough only | - |
| Windows / macOS | software, as before | Medium, Low | no cap |

Docker: pass the GPU in with `--device /dev/dri` (and `--group-add` the `render` group) for QSV/VAAPI, `--device /dev/video11`
on a Pi 4. The hardware paths follow go2rtc's and ffmpeg's documented arguments but have **not** been run on real hardware by
us; the log (`RemoteHub: encoder probe ...`) shows what each probe did, and a failing probe simply falls through.

* **Cap.** `EDGESLICER_MAX_TRANSCODES` (or `--hub-max-transcodes`, or `"go2rtc": {"max_transcodes": n}` in `hub/settings.json`;
  0 = no limit). It counts distinct streams: two viewers of the same printer's Low stream share one ffmpeg. When the cap is
  reached a further viewer is handed the passthrough stream (full quality), never an error. A slot is held while a viewer's
  stream is open and freed when the last viewer leaves, so switching from printer A to printer B frees A's slot.
* **Still-image fallback** for H.264 cameras (X1, H2, U1): `GET /r/<token>/still?id=<camera id>&fps=1&w=480` is MJPEG from
  keyframes only (ffmpeg `-skip_frame nokey`, 1-5 fps, 160-1280 px wide), read from the relay's loopback RTSP, so there is
  still one connection to the printer. `<img src=...>` plays it. At most `EDGESLICER_MAX_STILLS` at once (4, 2 on ARM); more get a
  503. The P1/A1 keep their own JPEG relay (`/bambu?id=&fps=`).
* **go2rtc** (v1.9.14, MIT, AlexxIT) is bundled in the AppImage and the Flatpak, amd64 and arm64, like `go2rtc.exe` on
  Windows. Pinned with sha256 in `scripts/fetch_go2rtc_linux.sh` (also in the Flatpak manifest).
* **ffmpeg** (LGPL-3.0, FFmpeg n8.1.3 static build with libopenh264, from BtbN/FFmpeg-Builds) is bundled next to go2rtc,
  amd64 and arm64, in the AppImage, the Flatpak and the image. It is the Linux twin of the LGPL `ffmpeg.exe` the Windows
  package ships (`--enable-version3`, no `--enable-gpl`, x264/x265 disabled). Pinned by sha256 in
  `scripts/fetch_ffmpeg_linux.sh` (and the Flatpak manifest); its licence text, source offer and provenance ship beside it
  (`FFMPEG-LICENSE.txt`, `FFMPEG-NOTICE-LINUX.txt`). BtbN removes old dated release tags after a couple of weeks, so the
  pinned URL needs bumping from time to time (the script says how; a failed download only warns, a hash mismatch fails the
  build). A system `ffmpeg` on `PATH` is still used when the bundled one is absent, with its encoders probed the same way.

### Ports, and sharing the box with Home Assistant or Frigate

Home Assistant's go2rtc, Frigate and a standalone go2rtc all default to 1984 / 8554 / 8555. The hub's go2rtc takes a random
loopback port for its API (nothing else is exposed), a random loopback port for RTSP, and the first free of 8555-8574 for
WebRTC, so it does not collide by default. To pin them: environment variable, flag, or `hub/settings.json` (environment wins):

| Listener | Environment | Flag | settings.json `go2rtc` key |
| --- | --- | --- | --- |
| API (loopback) | `EDGESLICER_GO2RTC_API_PORT` | `--hub-go2rtc-api-port` | `api_port` |
| RTSP restream | `EDGESLICER_GO2RTC_RTSP_PORT` | `--hub-go2rtc-rtsp-port` | `rtsp_port` |
| WebRTC media (UDP+TCP) | `EDGESLICER_GO2RTC_WEBRTC_PORT` | `--hub-go2rtc-webrtc-port` | `webrtc_port` |
| RTSP address | `EDGESLICER_GO2RTC_RTSP_LISTEN` (+ `_USER`, `_PASS`) | `--hub-go2rtc-rtsp-listen` | `rtsp_listen` |

A port you asked for that is taken is logged (`go2rtc WebRTC port 8555 is already in use ... using 8556 instead`) and the next
free one is used; **in service mode the hub does not guess**: it refuses to start with that message in the log and in
`hub/hub_error.txt`, so a container restart loop tells you what is wrong.

**Reading the relay from Home Assistant or Frigate instead of the printer.** Bambu printers allow only a couple of camera
clients; every extra reader (ha-bambulab, Bambuddy, a phone app) takes one. Let them read the hub's relay instead:

* Same machine, host networking (`--network host`): set `EDGESLICER_GO2RTC_RTSP_PORT=8564` (anything free) and point them at
  `rtsp://127.0.0.1:8564/<stream name>`. The stream names are the `rname` values in `GET /r/<token>/state` (for a U1, the name
  of its `/relay/h264` stream). The RTSP port stays on loopback.
* Another machine or a bridged container: set `EDGESLICER_GO2RTC_RTSP_LISTEN=0.0.0.0`, `EDGESLICER_GO2RTC_RTSP_USER` and
  `_PASS` (the hub refuses to open it without credentials), publish the port, and use `rtsp://user:pass@<host>:8554/<name>`
  (the port defaults to 8554 when the address is open). Treat the LAN as trusted: RTSP is not encrypted.

## Tailscale

`tailscale status` / `tailscale serve` are run when the `tailscale` CLI is on `PATH` (the hub page's remote-access card
then works on Linux too). In Docker the clean way is a Tailscale sidecar sharing the network namespace
(`network_mode: service:tailscale`) with `TS_SERVE_CONFIG` pointing 443 at `127.0.0.1:13640`.

## Not covered

See the end of `tests/research_always_on_hub.md` section 10: arm64 image, real-printer checks of UltraNet on Linux,
no watchdog for a hung (not dead) instance, the go2rtc relay is not restarted if it exits.
