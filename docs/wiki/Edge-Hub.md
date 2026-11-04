# Edge Hub overview

## What it is

**Edge Hub** is the same EdgeSlicer binary run as a **tray-only background process**. It serves the phone page and camera relays. Any slicer window can start it, and it keeps running after you close those windows.

Through the hub you get LAN phone access (QR / token link), optional remote access over Tailscale, printer event notifications, hidden slicer windows for phone-driven slice/send, and the control surfaces that only answer on loopback.

## Where to find it

- System tray → Edge Hub (balloon / menu)
- Hub page (loopback only) — phone QR, notifications card, window / hidden-instance controls
- Preferences → Extras → **Phone access** (related options; see [Preferences → Extras](Preferences-Extras))

## How to use it

1. Start EdgeSlicer as usual — a slicer window starts the hub if it is not already running.
2. Open the hub from the tray when you need the phone QR, notification destinations, or window controls.
3. Enable **phone access on the LAN**, scan the **QR code**, and use the phone page (`http://<pc>:13640/r/<token>/`) for Streams, Prepare, Devices, and Reprints. Details: [Phone access](Phone-access).
4. Optionally set up **Tailscale remote access** for HTTPS away from home. Details: [Remote access (Tailscale)](Remote-access-Tailscale).
5. Configure **notifications** on the hub page’s Notifications card. Details: [Printer event notifications](Printer-notifications).
6. Let the hub open further slicer windows, including **hidden** ones (no desktop window) so the phone can slice and send without UI on the PC. A hidden instance that would have raised a dialog flags *needs attention* on the hub page and in the tray menu; you can toggle hidden ↔ visible either way.

## Limits / notes

| Situation | What happens |
|---|---|
| Hub page / tray control plane | Answers only on **loopback** — no tunnel reaches the control plane. |
| Phone link | Token-gated; survives restarts. **New link** rotates the token and invalidates saved links and home-screen icons. |
| Firewall (Windows installer) | Adds a rule for **TCP 13640** for the hub. |
| Closing slicer windows | Hub can keep running in the tray. |

## Which copy owns the hub (launch flags)

All copies of EdgeSlicer that share a data folder share one hub. By default a copy that starts with no hub running starts one, and a copy that finds a hub from another build or another install asks it to quit and starts its own (once per process; a copy the hub started never does).

A copy that must leave the hub alone, typically a test build, can run as a client only:

| Switch | Effect |
|---|---|
| `--take-hub` or `EDGESLICER_TAKE_HUB=1` | Normal behaviour, even when the switches below ask otherwise. Strongest. |
| `--no-hub` or `EDGESLICER_NO_HUB=1` | Never start, restart or take over the hub or go2rtc, and never ask a running hub to quit. |
| A file named `edgeslicer_no_hub` next to `EdgeSlicer.exe` | Same as `--no-hub` for every launch of that copy (test copies installed by edgebuild get it). |

Order: `--take-hub` / `EDGESLICER_TAKE_HUB`, then `--no-hub` / `EDGESLICER_NO_HUB`, then the marker file, then normal. The log says which one applied. A no-hub copy still uses a running hub as a client (Stream tab, Mobile Hub page, own-camera view, the Printers tab). It cannot change phone access or the shared camera list, and says so; with no hub running those panels report that no hub is running.

## Related

- [Phone access and the companion app](Phone-access)  
- [Remote access (Tailscale)](Remote-access-Tailscale)  
- [Printer event notifications](Printer-notifications)  
- [Store G-Code Files](Store-G-Code-Files) — archive used by phone Reprints  
- [Stream tab](Stream-tab) — desktop camera wall; shares phone link / QR concepts  
- [Home](Home)  
