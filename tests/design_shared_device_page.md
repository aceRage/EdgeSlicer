# One shared device page for every vendor (design note, 2026-10-26)

**Owner direction (2026-10-26, with D6):** every vendor's device page converges on one shared,
vendor-neutral template and layout, even if the typical Bambu page is the base.

**Status:** the Elegoo Centauri Carbon 2 is the first printer on it (feat/elegoo-cc2-device-page).
Nothing else moves yet. This note says what the template is, and what each later vendor needs.

## What the template is

- **Data model:** the `/api/printers` row (RemoteAccess::api_printers). It is already vendor-neutral
  and is what the hub's `/summary`, the phone app's native device view and the Printers tab read.
  - State: `status` / `print_status` (printing, paused, complete, cancelled, error, standby, offline),
    `online`, `printing`, `stage`, `print_error {code, message}`, `hms`.
  - Job: `task`, `percent`, `left_time_s`, `layer`, `total_layers`, `thumb_id` / `cover_url`.
  - Readings and controls: `controls` (DeviceControls::to_json: `heaters[]` with `settable`, `speed`,
    `light {on}`, `fans[]`, `filament`), plus the plain `bed_temp`, `nozzles[]`, `chamber_temp`.
  - Filament: `ams[]` (Bambu AMS units, Elegoo CANVAS units: `type`, `name`, `trays[]` with
    `exists`, `type`, `color`, `remain`, `active`), `ext_spools[]`, `toolheads[]` (U1).
  - Buttons: `can_pause`, `can_resume`, `can_stop`; commands go to `/api/printers/{id}/control`.
  - Vendor details: one object per protocol (`sdcp` for the CC1, `cc2` for the CC2) with `protocol`,
    `live`, `age_s`, `stale`, `firmware` and protocol facts. The page shows it under "Printer".
- **Renderer:** `PrinterCards.device(row, opts)` in `resources/web/orca/printer_cards.js`, next to
  the Printers tab's card renderer, so the two cannot drift. Sections in the Bambu Device tab's
  order: printer header, print job (with Pause / Resume / Stop), temperatures, fans and light,
  filament, printer details. A section with no data is left out.
- **Page:** `resources/web/orca/device.html?id=<row id>`, in the Device tab's web view
  (PrinterWebView). It uses the Printers tab's messages over the `wx` channel (`printers_get`,
  `printers_control`, `printers_job`, `printers_thumb`), answered by `PrinterRowsBridge` - the same
  code the Printers tab now uses. Only our own page may send them, and only for its own row id.
- **Which printer gets it:** `PrinterWebView::shared_device_page_url` (today: host type Elegoo Link
  and model Centauri Carbon 2 / Centauri 2). The Printers tab's "Open Device page" goes there for any
  row with `cc2` or `"device_page": true` (PrintersMonitor::uses_device_page).

## What it does not do yet (all later, and none needed for the CC2)

1. **Setting things:** heaters, speed, light and fans are shown read-only. The phone app already
   sets them through `/control` (set_temp, set_speed, set_light, set_fan, DeviceControls checks).
   Port its controls to the page and route them through PrinterRowsBridge (a new `printers_set`
   message). The CC2 has none (elegoo-link enables no CC2 set method), so it would show none.
2. **Camera:** the Bambu page has the live view. The row's `camera_hint` (CC1) and the Stream tab's
   tiles are the source; a `<video>` panel reusing stream_center's player is the step.
3. **Filament actions:** load / unload (`controls.filament`, FilamentCommands) and the AMS settings.
4. **Files and history:** the Storage tab (PrinterStoragePanel) stays a separate wx tab for now.
5. **HMS details:** the Bambu page's HMS list and error dialog actions (`print_error.actions`).

## What each vendor needs to move onto it

| Vendor | Row today | Missing for the page | Notes |
|---|---|---|---|
| **Elegoo CC2** | full (ElegooCc2Status::fill_row) | nothing | on it (D6) |
| **Elegoo CC1** | full (Sdcp::fill_row), camera hint | the camera panel (step 2); then retire the printer's own web page + ElegooCanvasPanel card | settable light / fans need step 1 |
| **QIDI** | probed through Moonraker (`describe_hosts`): state, temps, `controls` for `host` only | the Box as `ams` (PrintHostDevices already reads it for the send dialog); `controls` for `ph:` devices; `can_*` for `ph:` rows (read-only today) | keep the Fluidd page one click away |
| **Klipper / Moonraker** | as QIDI | Happy Hare / AFC lanes as `ams` (read-only); `ph:` controls | the Fluidd / Mainsail page stays the full UI; the shared page is the summary |
| **Flashforge** | own Orca-Flashforge device tab, not in `/api/printers` rows yet | a Flashforge row (state, temps, IFS slots as `ams`) from its HTTP API; then the tab can switch | the biggest step: today it is a separate wx tab |
| **Snapmaker U1** | full row (SnapmakerLan), `toolheads[]` | nothing in data; it has its own Snapmaker page with print setup | move when step 1 lands |
| **Bambu** | full row (describe_bambu) | steps 1-5 | the native StatusPanel is the base the layout follows; switch last |

## Refactor order (proposal)

1. Step 1 (setting things) in the shared page, so it can replace a page that has controls.
2. CC1: switch to the shared page with the camera panel (step 2); drop ElegooCanvasPanel.
3. QIDI and Klipper `ph:` rows: controls and the Box / lanes as `ams`; offer the shared page with a
   "Printer's web page" link.
4. Flashforge: add its row, then the page.
5. Bambu and the U1 last, once steps 1-5 are there.

Owner decisions this needs before step 3: whether the Klipper / QIDI Device tab opens the shared
page or the printer's own web UI first, and whether the Storage tab moves into the page.
