# Anycubic network send

## What it is

EdgeSlicer can send a sliced plate to an Anycubic Kobra over the LAN using the **print-host** connection that every Anycubic profile already ships (`host_type: octoprint`). There is no native “Anycubic LAN” host (SSDP / signed handshake / MQTT on port 9883). That path exists only in Anycubic Slicer Next.

Two firmware setups work today:

| Firmware | Host Type | Address | Notes |
|---|---|---|---|
| **Stock** | Octo/Klipper | printer IP (port **80**) | Anycubic’s OctoPrint-compatible upload API. API key optional. |
| **Rinkhals** | Octo/Klipper, or **Moonraker** when that host type is available | `http://IP:7125` | Moonraker. Since [Rinkhals PR #68](https://github.com/rinkhals-community/Rinkhals/pull/68) the bare IP is the Rinkhals web portal, not the slicer API. |

Kobra 1 and Kobra 2 Neo have no printer-side network stack; use USB to OctoPrint or Klipper on a Pi. Photon is out of scope.

## Where to find it

Printer settings → **Physical printer** / **Devices** (the connection dialog): Host Type, Hostname, API Key.

After slicing: **Print** / **Send** → Upload or Upload and Print.

## How to use it

### Stock firmware (Kobra 3 / S1 and similar)

1. On the printer, note the LAN IP (Settings → Network). LAN Mode in Anycubic’s own slicer is **not** required for this path, and turning LAN Mode **on** drops the printer from Anycubic Cloud.
2. In EdgeSlicer, Host Type = **Octo/Klipper**.
3. Hostname = the printer IP. Do not add `:7125`. Port 80 is the compat API.
4. Leave API Key empty unless you set one on the printer.
5. **Test**. Then **Upload and Print**.

### Rinkhals

1. Install Rinkhals. Moonraker listens on **7125**.
2. Host Type = **Octo/Klipper** (Moonraker’s OctoPrint-compat), or **Moonraker** if that row is in the Host Type menu ([PR #613](https://github.com/aceRage/EdgeSlicer/pull/613)).
3. Hostname = `http://<printer-ip>:7125`. A bare IP hits the password-protected portal on port 80.
4. API Key only if you configured Moonraker authorization.
5. **Test**, then send.

If Test on a bare Anycubic IP sees the **Rinkhals portal** (HTML / non-JSON), EdgeSlicer retries port 7125 and asks you to save `http://IP:7125`. A bare 401/403 (Pi OctoPrint with no API key) is **not** retried. Saving a device does **not** silently append `:7125` — a Klipper-behind-nginx box on port 80 would break if it did.

Non-Anycubic Moonraker boxes are never rewritten. Rinkhals on a **Kobra 2 Pro / Plus / Max** has no automatic hint or :80→:7125 retry — enter `:7125` on the hostname yourself.

### Upload vs Upload and Print

On **stock** firmware (Kobra 3 / S1 / X family), **Upload** (print=false) can hang the printer on “Checking files, please wait” ([OrcaSlicer #14349](https://github.com/OrcaSlicer/OrcaSlicer/issues/14349)). **Upload and Print** works. EdgeSlicer therefore makes Upload and Print the default for those models and warns if you pick Upload only. Pi OctoPrint models (Kobra 1, i3 Mega, Chiron, Vyper) and Rinkhals/Moonraker hosts keep the ordinary Upload default. That hang is a firmware bug; the slicer cannot fix it.

Upload and Print starts immediately and skips the printer’s on-screen filament / leveling menus. If you need those menus, use Upload and Print and stop the job on the printer, then start the saved file from the touchscreen (the workaround from that issue).

## Limits / notes

- **No Anycubic Cloud.** Not implemented; Anycubic blocks third-party cloud MQTT.
- **No native LAN-mode MQTT.** Anycubic Slicer Next only.
- **ACE slots** on Rinkhals come from Rinkhals `mmu_ace` / Moonraker, not from an Anycubic-specific mapping in this page.
- Profiles keep `host_type: octoprint`. They are not switched to Moonraker automatically.

## Related

- [Devices list](Devices) — several printers of one model, by address
- [Home](Home)
