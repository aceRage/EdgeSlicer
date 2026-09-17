# Stream tab

## What it is

The **Stream** tab is a live **camera wall** (up to **9×9**) for watching printers and IP cameras from the desktop. It bundles the open-source [go2rtc](https://github.com/AlexxIT/go2rtc) helper and can share the same phone link / QR concepts used by [Edge Hub](Edge-Hub).

Supported sources include Snapmaker U1, Moonraker, Bambu Lab (LAN liveview), Flashforge cameras, and RTSP / ONVIF IP cameras (Reolink, Amcrest, Hikvision, Tapo, …; Wyze via RTSP firmware or wyze-bridge), with LAN discovery where applicable.

## Where to find it

- Main UI → **Stream** tab
- Screenshot: `docs/images/menu-stream.jpg` (example multi-camera wall)

## How to use it

1. Open the **Stream** tab.
2. Add or discover cameras / printer streams for your devices.
3. Arrange the wall (up to 9×9). **Drag names to reorder**.
4. Use the wall while printing to watch several machines at once.
5. For phone viewing of streams, use Edge Hub phone access (and Tailscale when away). See [Edge Hub](Edge-Hub) and [Phone access](Phone-access).

## Limits / notes

| Situation | What happens |
|---|---|
| Bambu live view | Needs Bambu’s own camera component (cannot be redistributed); Device tab may offer to download it once. Network plug-in is separate ([Preferences → Extras](Preferences-Extras) / [Bambu Lab](Bambu-Lab)). |
| Wyze | Needs RTSP firmware or wyze-bridge — not a stock Wyze cloud stream. |
| Layout persistence | On Windows, Stream layout can live with the embedded browser profile under the data directory migration path. |

Other notes:

- go2rtc is bundled unmodified under `resources/tools/go2rtc`.
- Phone MJPEG / MP4 playback behaviour differs by path (LAN vs remote / iOS); see phone and remote pages for details.

## Related

- [Edge Hub](Edge-Hub)  
- [Phone access](Phone-access)  
- [Bambu Lab in EdgeSlicer](Bambu-Lab)  
- [Home](Home)  
