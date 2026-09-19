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

## Related

- [Phone access and the companion app](Phone-access)  
- [Remote access (Tailscale)](Remote-access-Tailscale)  
- [Printer event notifications](Printer-notifications)  
- [Store G-Code Files](Store-G-Code-Files) — archive used by phone Reprints  
- [Stream tab](Stream-tab) — desktop camera wall; shares phone link / QR concepts  
- [Home](Home)  
