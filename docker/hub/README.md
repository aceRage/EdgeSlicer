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

The image is built from an AppImage made from this branch or later (it carries `go2rtc`; the Dockerfile
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

* **go2rtc** (v1.9.14, MIT, AlexxIT) is bundled in the AppImage and the Flatpak, amd64 and arm64, like `go2rtc.exe` on
  Windows. Pinned with sha256 in `scripts/fetch_go2rtc_linux.sh` (also in the Flatpak manifest). It carries
  X1/H2 RTSPS cameras and the P1/A1 relay needs none.
* **ffmpeg** is *not* bundled on Linux (the common builds are GPL). The camera "Quality" steps (Medium/Low re-encode)
  use the system's `ffmpeg` when it is on `PATH` and has an H.264 encoder (libx264 or libopenh264); otherwise those
  steps are off and the cameras stream as they come. In the image: `--build-arg WITH_FFMPEG=1` installs Ubuntu's
  `ffmpeg` (about 100 MB, GPL build, from the distribution). The Flatpak has none.

## Tailscale

`tailscale status` / `tailscale serve` are run when the `tailscale` CLI is on `PATH` (the hub page's remote-access card
then works on Linux too). In Docker the clean way is a Tailscale sidecar sharing the network namespace
(`network_mode: service:tailscale`) with `TS_SERVE_CONFIG` pointing 443 at `127.0.0.1:13640`.

## Not covered

See the end of `tests/research_always_on_hub.md` section 10: arm64 image, real-printer checks of UltraNet on Linux,
no watchdog for a hung (not dead) instance, the go2rtc relay is not restarted if it exits.
