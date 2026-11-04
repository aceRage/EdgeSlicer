# Printer emulator: design (2026-10-20)

Status: **design only**. Nothing is built and no real printer was contacted. Source: the owner's
idea after the QIDI Box stale-mapping incident (#611 shipped the hole, #643 fixed it). See the memory note
`printer-emulator-idea.md`.

## 0. Goal in one paragraph

The owner adds a printer in EdgeSlicer by IP (for example `127.0.0.11:7125`), and it answers like a real
QIDI / Elegoo / Snapmaker U1 / Creality / Flashforge / OctoPrint printer, with whatever Box, CANVAS or
toolhead slots the owner sets on a small local page. Every request EdgeSlicer sends is recorded:
`SAVE_VARIABLE value_t0`, `enable_box`, `slot_map`, uploads and print starts. The owner can see exactly what a
send dialog did. The same emulator drives automated hub-gate E2E tests, so a regression like #611 (a print
started on the Box's stale `value_t<n>`) fails a test instead of reaching a user's printer.

---

## 1. What exists already, and what can be reused

| Asset | Where | Speaks | Reuse |
|---|---|---|---|
| `mock_elegoo_sdcp.py` (471 lines) | `tests/phone/` | SDCP V3 over a hand-rolled websocket. Cmd 0, 1, 324 (CANVAS), 129/130/131, 403, 386 (camera URL), and an MJPEG camera on `--video-port`. A `/mock/*` control surface for print state, CANVAS attach, drop, refuse, limit, mute and video refusal. Records `commands`. | **High.** It becomes the Elegoo personality almost as-is. Missing: Cmd **128** start print, HTTP `uploadFile/upload` chunked upload, and settable trays. |
| `fake_moonraker.py` (325) | `tests/slic3rutils/` | U1 Moonraker HTTP: server/info, objects/query, upload, metadata, list, thumbnails, delete, print/start, gcode/script. Modes `slow` / `down` / `up` and a request counter. | **High.** It is the Moonraker core: routing, multipart parsing, file store and failure modes. |
| `fake_moonraker_ws.py` (210) | `tests/slic3rutils/` | Moonraker JSON-RPC `/websocket` (server.info, printer.gcode.script ...), drop/up | **Medium.** It adds WS to the Moonraker core (MoonrakerWs, Device-tab live status). |
| `mock_u1_controls.py` (210) | `tests/phone/` | U1 objects/query with heaters, gcode/help, and the controls macros (`SET_HEATER_TEMPERATURE`, `M220`, `SET_LED`, `M106`, `INNER_FILAMENT_UNLOAD` ...) | **Medium.** Its macro interpreter and `gcode/help` list move into the U1 personality. |
| `mock_u1_timelapse.py` (207) | `tests/phone/` | timelapse file list and Range downloads | Low (keep as a gate mock). |
| `mock_u1_mqtt.py` (366) | `tests/slic3rutils/` | A tiny MQTT 3.1.1 broker (CONNECT/SUB/PUB QoS 0-1/PING, `+`/`#`), the U1 pairing and LAN brokers, **and a fake Snapmaker cloud broker** | **Medium.** Reuse the broker and the LAN/pairing parts. **Do not** carry the cloud broker into the owner tool (rule: never pretend to be a cloud service). |
| `FakeBroker` (C++) | `tests/slic3rutils/mqtt_teardown_tests.cpp:46-183` | A minimal in-process MQTT 3.1.1 broker on loopback port 0, with modes AckAll / ResetOnPublish / never-ack | Pattern only. It is a C++ fixture for teardown races and is not reusable from Python. It shows that the in-process broker approach works for Catch2 E2E. |
| `mock_printhost.py` (236) | `%LOCALAPPDATA%\Temp\snorca_hubtest\` | OctoPrint subset plus U1 objects/query answers copied from a real U1 (fw 1.5.2), recording uploads/scripts/starts | Medium. It is OctoPrint-shaped. Known trap: `/mock/state` has no print state, and reset leaves the state `complete`. |
| `mock_moonraker.py` (113) | same | print_stats plus pause/resume/cancel | Low (superseded). |
| `mock_prusalink.py` | same | PrusaLink `/api/version`, `/api/v1/status`, `/api/v1/job` with digest/API-key auth | **Medium.** It becomes the PrusaLink personality. |
| `fake_bambu_status.py` | `tests/phone/` | Wraps **UltraNet's private** `tools/fakeprinter.py` (MQTT/TLS 8883 plus FTPS 990, loopback only) | Leave it where it is. See §2.7. |
| Gate runners | `tests/slic3rutils/run_elegoo_sdcp_e2e.py`, `run_u1_local_first_e2e.py`, `run_snapmaker_probe_e2e.py` | Start a mock on a free loopback port, run a hidden Catch2 tag with the port in an env var, and always stop the mock | **High.** This is the template for `run_emulator_e2e.py`. |
| Hub gates | `tests/phone/test_hub_elegoo_gate.py`, `test_device_controls_gate.py`, `test_timelapse_gate.py`; `snorca_hubtest/gate_*.sh` (44 scripts: `gate_hubelegoo.sh`, `gate_u1send_unload.sh`, `gate_all.sh` ...) | Start an installed EdgeSlicer with `--datadir`, seed `hub/print_host_devices.json`, and drive `/api/...` | **High.** It is the template for the QIDI send gate. |
| Test hooks in the product | `ElegooSdcpMonitor.cpp:700` `SNORCA_SDCP_PORT` (plus `_KEEP_S`, `_CHECKIN_S`) | Moves the **monitor's** SDCP port only | Partial. The send path's port 3030 stays hard-coded (see §2.2). |

All the Python mocks use the **standard library only** (hand-written websocket and MQTT framing,
`ThreadingHTTPServer`), and each one binds 127.0.0.1. That is the right base for the emulator.

**Housekeeping finding:** the `snorca_hubtest` mocks and gates live under `%LOCALAPPDATA%\Temp`, where a temp
cleaner can wipe them. The emulator and any gate that uses it go in the repo.

---

## 2. Protocols and personalities (priority order)

Each personality = one listener set + a state model + a command interpreter + log hooks.
"Minimum to answer" is what the emulator has to do for EdgeSlicer's current code paths to run end to end.

### 2.1 P1: Moonraker / Klipper, QIDI personality (X-Max 4, X-Plus 4/5, Q2 with Box)

What EdgeSlicer sends (desktop `PrintHostJobQueue`, and the phone `RemoteSend`, which duplicates the logic):

| Step | EdgeSlicer code | Request | Emulator must |
|---|---|---|---|
| Probe (send dialog) | `PrintHostDeviceStatus.cpp:405-457` | `GET /printer/objects/query?print_stats&print_task_config&multi_color_controller&output_pin%20polar_cooler&extruder..extruder3[&save_variables=variables]` (the save_variables part only if `qidi_box_capable_model`, `:799`) | Return `result.status` with `print_stats.state`, optional `multi_color_controller` (`box_count`, `boxes[].filament_info[4]`, `box_inserted_<n>`; `:199-236`), optional `save_variables.variables` (`enable_box`, `box_count`, `filament_slot<n>`, `color_slot<n>`, `value_t<n>`; `:258-271`), and `output_pin polar_cooler` when present. Leave out the objects the printer lacks. |
| Fallback filament read | `:440-452` | `GET /printer/objects/query?save_variables=variables` and `GET /server/files/config/officiall_filas_list.cfg` | Serve the variables and an INI with `[fila<id>] type= / filament=` and `[colordict] <n> = #RRGGBB` (parser `:459-493`). The content is **synthetic**; we don't copy QIDI's file. |
| Idle check | `PrintHost.cpp:545-564` | `GET /printer/objects/query?print_stats` | `state` = standby/printing/paused. Busy blocks the send. |
| Box mapping | `PrintHost.cpp:572-580`; script `PrintHostDeviceStatus.cpp:652-680, 905-920` | `POST /printer/gcode/script?script=SAVE_VARIABLE VARIABLE=enable_box VALUE=1\nSAVE_VARIABLE VARIABLE=value_t0 VALUE="'slot3'"\n...[enable_polar_cooler]` with body `{}` (bodyless POST trap, `SnapmakerLan.cpp:1228-1236`) | **Interpret** `SAVE_VARIABLE`: Klipper shlex, then `literal_eval` of VALUE, stored into `save_variables`, so `value_t0` becomes the string `slot3`. Answer `{"result":"ok"}` only after it has "run". |
| Read-back | `PrintHost.cpp:583-597`; `qidi_mapping_confirmed` `PrintHostDeviceStatus.cpp:564-609` | `GET ...?save_variables=variables` | Return the **stored** variables. This is the step that catches #611-class bugs. |
| Bed leveling | `PrintHost.cpp:604-611`; `:922-929` | script `G31` (probe) or `G32` (saved mesh) | Record it. The failure mode `refuse_g31` returns a Klipper error (the print must still go ahead). |
| Timelapse | `PrintHost.cpp:615-636` | `POST /machine/timelapse/settings?enabled=true|false` body `{}` | Record it. The failure mode makes it 404 (the print goes ahead). |
| Upload (+ start) | `MoonRaker.cpp:579-617` (multipart `print`, `path`, `file`; retries on **:8080** at `:606-617`) | `POST /server/files/upload` | Store name, size and sha256. When `print=true`, set state `printing` and log `start`. Optionally listen on 8080 too (`retry_8080` scenario). |
| Version/test | `MoonRaker.cpp:254, 395` | `GET /api/version`, `/printer/info`, `/server/info` | Moonraker-shaped answers with a hostname of `EMU-...`. |

Snapmaker U1 shares this core (see §2.3). **Creality K-series printers on Moonraker** (rooted K1/K2, Fluidd) use
the generic Moonraker personality. The stock Creality CFS path is a different protocol (§2.5).

### 2.2 P2: Elegoo SDCP (Centauri Carbon 1, CANVAS)

| Step | Code | Request | Emulator must |
|---|---|---|---|
| Test connection | `ElegooLink.cpp:236-262` | `GET http://<host>/` | Return a body that contains "ELEGOO". |
| Monitor / CANVAS probe | `ElegooSdcp.hpp:42` (`CMD_CANVAS=324`), `ElegooCanvasQuery.cpp:34-35` (**port 3030, path `/websocket`, fixed**) | WS Cmd 0 (status), 1 (attributes), 324 (CANVAS trays), 386 (camera URL) | The existing mock does all of this. Make the trays settable (canvas_id, tray_id, type, colour, vendor, loaded). |
| Upload | `ElegooLink.cpp:370-436` | `POST /uploadFile/upload`, chunked multipart: `Check=1, S-File-MD5, Offset, Uuid, TotalSize, File` | **New:** reassemble the chunks, check the MD5, reply `{"code":"000000"}`-style, and log name, size and md5. |
| Start | `ElegooLink.cpp:555-570, 673`; `ElegooStartPrint.hpp:116-150` | WS `ws://<host>:3030/websocket` Cmd **128** `{Filename:"/local/x", StartLayer, Calibration_switch, PrintPlatformType, Tlp_Switch, slot_map:[{t,canvas_id,tray_id}]}` | **New:** Ack 0, set status to printing, and log the full `slot_map`. The slot_map is absent when no CANVAS is attached and `[]` for "as sliced" (`ElegooStartPrint.hpp:19-35, 103-111`). |
| Camera | Cmd 386 returns `VideoUrl`; CC1 serves MJPEG on **:3031** | `GET /video` | The existing mock camera is reused, bound on 3031 of the same IP. |

**Fixed-port constraint:** the send path calls `get_host_from_url_no_port(m_host)` and then `"3030"`
(`ElegooLink.cpp:560-563`). Only the monitor honours `SNORCA_SDCP_PORT`. Two Elegoo emulators therefore need **two IP
addresses** (§3.2, D3).

### 2.3 P3: Snapmaker U1 (LAN)

The send is **HTTP only** (`SnapmakerLan.hpp:20-22`: "Nothing here needs the MQTT connection"). The Device tab, the
local-first route and the hub can also use MQTT (`U1LanRoute.cpp`, `mock_u1_mqtt.py`).

| Step | Code | Emulator must |
|---|---|---|
| Identify / probe | `SnapmakerLan.cpp:756-790` (`/server/info`, `/access/info`, `/machine/system_info`, objects/query incl. `print_task_config`), default HTTP port **80** (`:131, 387`) | `print_task_config.filament_type / filament_sub_type / filament_vendor / filament_color_rgba / filament_exist` per toolhead (`PrintHostDeviceStatus.cpp:156-185`), and a `gcode/help` list (`SnapmakerLan.cpp:782`) |
| Upload | `SnapmakerLan.cpp:1168-1198` (`print=false`, `root=gcodes`) | Store and log the file; serve `/server/files/metadata` (`:1200`) |
| Mapped start | `SnapmakerLan.cpp:1340-1395` | Interpret `SET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER= MAP_EXTRUDER=`, `SET_PRINT_USED_EXTRUDERS`, `SET_PRINT_PREFERENCES` (incl. the end-unload flag, which is **refused while printing**, `PrintHost.cpp:639-689`), and `POST /printer/print/start?filename=` (404 if the file was never uploaded) |
| LAN MQTT (optional, E2) | `U1LanRoute.cpp`, `mock_u1_mqtt.py` | The LAN plus pairing brokers only. **No cloud broker.** Plain MQTT on loopback, as the existing E2E does. |

### 2.4 P4: OctoPrint / PrusaLink (and Anycubic stock firmware's OctoPrint compatibility)

| Host | Code | Minimum |
|---|---|---|
| OctoPrint | `OctoPrint.cpp:244` `GET /api/version` (root `api`, text `OctoPrint ...`), `:385-408` `POST /api/files/local` multipart `print=true|false`, `X-Api-Key` (`:531`) | version JSON, upload store, start flag, optional API-key check |
| PrusaLink | `OctoPrint.cpp:652-732` (`/api/version`, `/api/v1/storage`), `:972-1071` `PUT /api/v1/files/<storage>/<name>` with `Print-After-Upload: ?1` | Reuse `mock_prusalink.py` with digest/key auth |
| Anycubic stock (#620, open) | OctoPrint-compatible on :80, Rinkhals Moonraker on :7125; the retry rule is "only when :80 returned the Anycubic portal" | An `anycubic_stock` variant of the OctoPrint personality: portal page on `/`, version text "Anycubic". |

### 2.5 P5: Creality K-series stock (WS 9999, CFS)

| Step | Code | Minimum |
|---|---|---|
| Upload | `CrealityPrint.cpp:131-140` `POST http://<host>/upload/<name>` multipart `path`, `file` | Store and log |
| Start (main) | `CrealityPrint.cpp:189-200` WS **:9999** `{"method":"set","params":{"opGcodeFile":"printprt:/usr/data/printer_data/gcodes/<f>"}}` | Accept it, log it, set status to printing |
| CFS (PR #616, **open**) | branch `cursor/creality-cfs-send-d523`, `CrealityPrint.cpp:540-551` `{"method":"get","params":{"boxsInfo":1}}`; start = `colorMatch` + `multiColorPrint` on one socket, fire-and-forget (`:609-633`); `enableSelfTest` | Answer `boxsInfo` from the configured CFS slots. Log `colorMatch` and `multiColorPrint` in full. **This is the hand-test #616 currently lacks.** |

Port 9999 is fixed (as 3030 is for Elegoo).

### 2.6 P6: Flashforge HTTP (Creator 5, AD5X/5M IFS)

| Step | Code | Minimum |
|---|---|---|
| Detail / IFS | `Flashforge.cpp:207` (**`http://<host>:8898/` fixed**), `:661-678` `POST /detail {serialNumber, checkCode}`, IFS parse `:305-330` (`hasMatlStation`, `matlStationInfo`) | Check the serial and check code from the config (a wrong code returns `code != 0`, which is a failure mode). Report the IFS slots. |
| Upload + start | `Flashforge.cpp:700-729` `POST /uploadGcode`, headers `serialNumber`, `checkCode`, the print flags, and a base64 `materialMappings` | Decode and log the material map. |
| Legacy TCP 8899 / UDP 48899 discovery | `:44, :374` | Out of scope (E3+, only on request). |

### 2.7 P7: Anycubic native LAN, and Bambu LAN

| Item | Verdict |
|---|---|
| Anycubic native LAN (#621 spike, "do not merge") | `GET :18910/info`, signed `POST /ctrl` (md5 sign, AES-128-CBC token), then MQTT over **TLS :9883**. The emulator picks its own token, so this is feasible without anyone's keys. It is only worth building if #621 is taken forward. **Deferred** (D9). |
| **Bambu LAN MQTT** | **Out of scope for the owner emulator.** Real Bambu LAN uses TLS with printer certificates issued by Bambu. Current firmware with Authorization Control expects commands signed through Bambu Connect. An emulator would have to present itself as a Bambu printer to the Bambu plug-in path, which is impersonation in all but name. It would never sign anything, but even an unsigned variant would only test UltraNet's code path, which UltraNet's own private `fakeprinter.py` already covers on loopback for the reprint gate (`tests/phone/fake_bambu_status.py`). The owner also owns X1C, H2D and H2C hardware, so the need is low. Keep it there. |

---

## 3. Owner UX

### 3.1 Shape

```
EdgeSlicer Emulator.bat  (Windows double-click)   /   emulator.command  (Mac)
   -> py -3 tools/printer_emulator/emu.py --config %LOCALAPPDATA%\EdgeSlicerEmulator\printers.json
   -> opens http://127.0.0.1:8790/   (control page, loopback only, always)
```

- **printers.json** (also editable on the page) holds one entry per emulated printer:

```json
{ "printers": [
  { "id": "qidi-xmax4", "personality": "qidi", "bind": "127.0.0.11", "port": 7125,
    "name": "EMU QIDI X-Max 4", "state": "standby",
    "qidi": { "box_report": "mcc+vars", "boxes": 1, "polar_cooler": true,
              "stale_value_t": { "0": "slot3", "1": "slot2" } },
    "slots": [ { "type": "PLA", "sub": "PLA Rapido", "vendor": "QIDI", "color": "#E03030", "loaded": true },
               { "type": "ASA", "color": "#202020", "loaded": true },
               { "loaded": false }, { "type": "PETG", "color": "#FFFFFF", "loaded": true } ],
    "faults": { "refuse_g31": false, "drop_save_variable": false, "probe_delay_s": 0 } },
  { "id": "cc1", "personality": "elegoo_cc1", "bind": "127.0.0.12", "canvas": true, "slots": [ ... ] }
] }
```

- **Control page** (one HTML file served by the emulator, no build step) has one card per printer:
  - start, stop and restart controls;
  - the "Add in EdgeSlicer" hint (host type to pick, address to type, preset to choose);
  - a slot editor (type, sub-type, colour picker, vendor, loaded);
  - a state switch (idle/printing/paused/error);
  - Box/CANVAS/CFS/IFS presence;
  - fault toggles;
  - a **live command log** (server-sent events) with filters and "copy as test assertion".
- **Fault catalogue (all personalities where they make sense):**
  - timeout (accept, never answer);
  - slow probe (delay N s, which can exceed the QIDI 10 s probe, `probe_timeout_for`);
  - connection refused (stop the listener);
  - HTTP 404 / 500 per path;
  - `refuse_g31`;
  - `drop_save_variable` (answers ok, does not store it, so the read-back must block the send);
  - `refuse_script`;
  - `busy` (printing/paused);
  - Box unplugged (`box_count 0`);
  - Box present but unreadable (`multi_color_controller` with no filament);
  - `officiall_filas_list.cfg` 404;
  - CANVAS detached;
  - websocket handshake refused or limit reached (the existing mock's `/mock/refuse`, `/mock/limit`);
  - Flashforge wrong check code;
  - upload MD5 mismatch (Elegoo);
  - 8080-only upload (Moonraker retry).
- **Presets:** ship `presets/*.json` for QIDI X-Max 4 with 1 Box, QIDI Q2 without a Box, Elegoo CC1 with CANVAS, U1
  4-head, Creality K2 Plus with CFS, Flashforge AD5X with IFS, and OctoPrint/PrusaLink generic. "Add printer" on the page is
  "copy a preset".
- **Script control:** `emu.py start|stop|status|set <id> key=value|log <id> [--since]` talks to the
  control port, and so do the tests. No tray app in E1 (D5): the page plus the .bat is enough, and a tray app
  adds a non-stdlib dependency (pystray/Pillow).

### 3.2 Addresses, ports and many printers at once

| Personality | Port rule in EdgeSlicer | Several at once on one PC |
|---|---|---|
| Moonraker (QIDI, generic, Anycubic Rinkhals) | honours `host:port` (`PrintHost.cpp:520-538`) | any free ports on 127.0.0.1 |
| U1 | port from the address, default 80 (`SnapmakerLan.cpp:413-425`) | ports or aliases |
| OctoPrint / PrusaLink | honours `host:port` | ports |
| Elegoo | **3030 fixed** (send and CANVAS); camera 3031 | **one per IP** |
| Creality stock | **9999 fixed** for WS; upload on the host's port | one per IP |
| Flashforge | **8898 fixed** | one per IP |

On **Windows** the whole of 127.0.0.0/8 is loopback with no setup, so each printer gets its own
`127.0.0.<n>` (presets use .11, .12 and up). On the **Mac** only 127.0.0.1 exists until
`sudo ifconfig lo0 alias 127.0.0.<n>`. The emulator prints that command and does not run it (D3).
A small product change, "honour an explicit `:port` for Elegoo/Creality/Flashforge when one is typed",
would remove the alias need. It is a separate PR and the owner decides (D3).

`is_local_address` / `is_lan_host` (`SnapmakerLan.cpp:211-230`) accept loopback, so these addresses
can be added today.

### 3.3 Safety rails (built in, not optional)

1. **Binds only to loopback by default.** A LAN bind (to test from the phone app or the MacBook) must name an
   RFC1918 / link-local address the PC actually owns. It is refused for 0.0.0.0, public addresses and host names.
2. **Never makes an outbound connection.** It is a server only: no discovery broadcasts, no SSDP/mDNS/UDP 48899
   answers (D13), no cloud brokers, no NTP, nothing.
3. **Never impersonates a cloud service** and never terminates TLS with a vendor's identity. There is no Bambu personality.
4. **Obviously fake identity:** serial `EMU-<personality>-<n>`, hostname `emu-...`, device name prefixed "EMU",
   and fake MainboardID / check codes. A real printer in a list can never be mistaken for an emulator and vice versa.
5. The **control port is loopback only**, even when printers bind to the LAN. There is no auth on the printer ports
   (as with the real ones), so a LAN bind is opt-in per printer and shown in red on the page.
6. Uploaded files are kept in a size-capped temp store (default 200 MB, oldest first) and are never executed.

---

## 4. Command log

| Field | Example |
|---|---|
| `ts`, `seq` | `2026-10-20T21:14:03.120Z`, 57 |
| `printer`, `personality`, `transport` | `qidi-xmax4`, `qidi`, `http` / `ws` / `mqtt` |
| `method`, `path`, `status`, `ms` | `POST`, `/printer/gcode/script`, 200, 12 |
| `kind` | `probe` · `script` · `save_variable` · `gcode` · `upload` · `start` · `ws_cmd` · `fault` |
| decoded payload | `{"var":"value_t0","value":"slot3","raw":"VALUE=\"'slot3'\""}`; `{"name":"plate_1.gcode","size":1834221,"sha256":"..."}`; `{"cmd":128,"slot_map":[{"t":0,"canvas_id":0,"tray_id":2}]}` |
| `state_after` | snapshot of `save_variables` / tray map / print state after the request |

- **Live:** an SSE stream on the control page, plus `GET /emu/log?printer=&since=<seq>`.
- **Saved:** JSONL per session at `%LOCALAPPDATA%\EdgeSlicerEmulator\logs\<date>_<printer>.jsonl` (Mac:
  `~/Library/Application Support/EdgeSlicerEmulator/logs`). The last 20 sessions are kept. A "Save bundle" button zips the log, the config
  and the uploaded file list for a PR comment.
- **Assertions:** `tools/printer_emulator/emu_assert.py` (importable and CLI):

```python
emu = Emulator("http://127.0.0.1:8790")
emu.reset("qidi-xmax4", stale_value_t={"0": "slot3"})
# ... drive a send ...
emu.expect("qidi-xmax4", kind="save_variable", var="enable_box", value=1)
emu.expect("qidi-xmax4", kind="save_variable", var="value_t0", value="slot1")
emu.expect_order("qidi-xmax4", ["save_variable", "probe", "upload", "start"])   # mapping before upload
emu.expect_none("qidi-xmax4", kind="upload")       # e.g. with drop_save_variable
emu.state("qidi-xmax4")["save_variables"]["value_t0"] == "slot1"
```

Every assertion failure prints the relevant log slice, so a red gate explains itself.

---

## 5. Use in tests and CI

| Layer | What runs | Catches |
|---|---|---|
| **Hub gate** (main use) | `tests/emulator/test_send_qidi_gate.py`, hub-gate style: start the emulator, start an installed EdgeSlicer with `--datadir` whose `hub/print_host_devices.json` lists `ph:` devices at the emulator addresses, slice a fixture project via CLI or `/api/plates`, then `POST /api/plates/{i}/send mode=print confirm=1 mapping=...` (`RemoteAccess.cpp:2562`) or `/api/archive/{id}/send`, follow `/api/jobs/{id}`, and assert on the emulator log. | the #611 class (stale `value_t`, missing `enable_box`, upload before mapping), busy-printer refusal, read-back refusal |
| **Catch2 E2E** | `run_emulator_e2e.py` (copy of `run_elegoo_sdcp_e2e.py`): the emulator on free loopback ports, a hidden tag `[.EmulatorE2E]` calling `PrintHostDevices::probe`, `query_qidi_variables`, `SnapmakerLan::run_script`, `ElegooCanvasQuery`, and the Creality `query_boxes_info` directly | the protocol parsers and the network half, without a GUI |
| **Owner hand test** | the desktop send dialogs (`PrintHostSendDialog`, `ElegooPrintHostSendDialog`, the swatch map from #625) against the same presets, using a hand-test card in `pr_status.json` | the UI: what the dialog shows, defaults, the wording of refusals |

Matrix for the first QIDI gate (each row is one send):

| # | Emulator setup | Send | Expect |
|---|---|---|---|
| Q1 | Box, 4 slots loaded, `value_t0=slot3` stale | 2 filaments mapped to 0:0, 1:1 | `enable_box=1`, `value_t0=slot0`, `value_t1=slot1`, then upload, then start |
| Q2 | same, `drop_save_variable` | same | **no upload, no start**; job error says "could not be confirmed" |
| Q3 | printing | any | no script, no upload; "busy" |
| Q4 | Box but `multi_color_controller` empty, vars + cfg only | 1 filament | slots read from vars + cfg; mapping sent |
| Q5 | Box unreadable (cfg 404) | 1 filament | policy refusal (qidi_send_policy), nothing sent |
| Q6 | `refuse_g31`, leveling on | 1 filament | G31 logged as refused, print **still** starts |
| Q7 | no Box (`box_count 0`) | 1 filament | `enable_box=0`, no `value_t` writes |
| Q8 | probe delay 12 s | any | probe times out and the dialog/send refuses cleanly |

Similar small matrices follow for CC1 (slot_map present / `[]` / absent, CANVAS detached, MD5 mismatch) and U1
(EXTRUDER_MAP per filament, unload refused while printing).

**Where it runs:** `edgebuild gate-run` on the local slot and the VM (both Windows) first. A GitHub Actions
job is optional later (D7): it needs an installed build, and the hub gates today run against installs, not CI
artefacts. Each gate asserts that every printer address it uses is loopback, so a misconfigured gate cannot hit a
real printer.

---

## 6. Language, runtime and packaging

| Option | For | Against |
|---|---|---|
| **Python 3.10+, stdlib only** (recommended) | Matches every existing mock, so their code moves in with little change. Runs on Windows and the Mac unchanged. No build. Agents and the owner can edit it. | It needs Python. The owner's PC and the Mac have it (the gates run Python); a fresh PC would not. |
| Small C++ tool (Boost.Beast, as in the product) | It could share EdgeSlicer's own parsers, and one exe. | It needs a build per change (build budget), the existing mocks would be rewritten, and sharing parsers is a trap: the emulator would agree with our bugs. |

Packaging:
- **Windows:** `tools/printer_emulator/EdgeSlicer Emulator.bat` runs `py -3 emu.py` and opens the page. It finds Python
  and prints the install hint if it is missing.
- **Mac:** `emulator.command` (chmod +x) runs `python3 emu.py`.
- **Optional later (D2):** a PyInstaller one-file `EdgeSlicerEmulator.exe` built on the VM. It would not be signed or shipped with EdgeSlicer.
- **Firewall:** a loopback bind triggers no Windows Firewall prompt. A LAN bind does (python.exe). The page says so.
- The emulator lives at **`tools/printer_emulator/`** in the repo. It is not in the installer. Gates live in `tests/emulator/`.

Layout:

```
tools/printer_emulator/
  emu.py                 # CLI + control server (127.0.0.1:8790) + supervisor
  core/http.py ws.py mqtt.py multipart.py log.py faults.py   # lifted from the existing mocks
  personalities/moonraker.py qidi.py u1.py elegoo_sdcp.py creality.py flashforge.py octoprint.py prusalink.py
  page/index.html        # single file, no framework
  presets/*.json
  emu_assert.py
```

---

## 7. Phases

| Phase | Scope | Size |
|---|---|---|
| **E1** Moonraker/QIDI + log + page | Core (HTTP, multipart, log, faults, supervisor, safety rails). Moonraker generic and the QIDI personality with a real `SAVE_VARIABLE` interpreter and read-back. Control page with slot editor, state, faults and live log. `emu_assert.py`. QIDI presets. Gate `test_send_qidi_gate.py` with Q1-Q8. Owner hand-test card for the desktop QIDI dialog. | **M-L**: about 1,500-2,000 lines of Python plus about 300 for the gate; 2-3 agent days. No product code changes. |
| **E2** Elegoo + U1 | Fold `mock_elegoo_sdcp.py` in, adding chunked upload, Cmd 128 slot_map logging, settable trays and camera on 3031 per IP. U1 personality (from `fake_moonraker.py` + `mock_u1_controls.py`): `print_task_config` slots, the mapped-start macros, unload refusal. Optional U1 LAN MQTT (from `mock_u1_mqtt.py`, without the cloud). CC1/U1 gate matrices. Mac alias helper text. | **M**: about 1,000-1,300 lines; 2 days |
| **E3** others | Creality stock WS 9999 + CFS `boxsInfo`/`colorMatch`/`multiColorPrint` (do it **first** in E3 and use it to hand-test open PR #616). Flashforge 8898 `detail`/`uploadGcode` with IFS. OctoPrint, PrusaLink, Anycubic stock compatibility. | **M**: about 1,200 lines; 2-3 days (Creality CFS alone about 1 day) |
| E3+ (only on request) | Anycubic native LAN (#621), Flashforge legacy 8899, discovery answers, PyInstaller exe, GitHub Actions job, "honour explicit :port" product PR | S each |

Order inside E1: core and log, then QIDI state and interpreter, then the gate Q1/Q2 (the #611 guard), then the page, then the faults.

---

## 8. Risks

| Risk | Impact | Mitigation |
|---|---|---|
| **The emulator encodes our understanding, not the firmware.** Tests pass against our own assumptions (for example `literal_eval` semantics, Box JSON shape). | False confidence | Every behaviour cites its source (QIDIStudio code reading, owner/user capture, upstream PR). An `unverified: true` flag on the page per field. Prefer shapes from real captures (the CC1 mock already follows firmware V1.4.49). Replace them with user logs (Diagnostics zip) when they arrive. |
| It shares parsers with the product | It would agree with our bugs | Never import EdgeSlicer code. The interpreter is written independently from the protocol description. |
| Fixed ports (3030/9999/8898) | Only one per IP | Loopback aliases (Windows free, Mac `lo0 alias`). Optional port-honour PR (D3). |
| Port clashes with the owner's own services (hub 13640, Tailscale Serve, the live slicer, edgebuild) | Flaky starts | Presets avoid them. The emulator checks the port before binding and reports which process holds it. |
| A LAN bind is reachable by anyone on the LAN | Junk uploads | Opt-in per printer, private addresses only, upload cap, no execution |
| A gate pointed at a real printer by mistake | A real print starts | Gates assert loopback addresses. The emulator identity (`EMU-` serial) is checked before any send step. |
| Desktop vs phone QIDI logic is duplicated (`PrintHost.cpp:541-603` vs `RemoteSend.cpp:1660-1710`) | The gate covers only the phone/hub path | The owner hand-tests the desktop against the same emulator. Follow-up: factor one shared `qidi_prepare_start()` (separate PR). |
| Mocks drift into two families (old gate mocks vs the emulator) | Upkeep | New gates use the emulator. Old mocks are migrated only when their gate is touched. |
| Python is missing on a fresh machine | The owner can't double-click | The .bat prints the install hint; the PyInstaller exe is the later answer (D2). |
| Copyright / licensing of vendor data | — | Synthetic filament lists and names. Protocol facts only (clean room, as in #621). No vendor binaries or files copied. |
| The CFS/Anycubic protocols change in open PRs | Rework | E3 is built against the PR branch at that moment and noted in the preset. |

---

## 9. Owner decisions

| # | Decision | Recommendation |
|---|---|---|
| **D1** | Language | **Python 3, stdlib only.** It reuses every existing mock. |
| **D2** | Packaging | **.bat + .command now**, and a PyInstaller exe only if the owner wants one off this PC. Never ship it in the EdgeSlicer installer. |
| **D3** | Several printers with fixed-port protocols (Elegoo 3030, Creality 9999, Flashforge 8898) | **Loopback aliases (127.0.0.x) now.** Separately, a small product PR that honours an explicit `:port` typed for those hosts. That is harmless for users and removes the Mac alias step. |
| **D4** | LAN binding (phone app / MacBook testing) | **Off by default, opt-in per printer**, private addresses only, shown in red. |
| **D5** | Tray app | **No** in E1. The control page plus the .bat is enough. Revisit if the owner keeps it running all day. |
| **D6** | Log location and retention | `%LOCALAPPDATA%\EdgeSlicerEmulator\logs`, JSONL, **last 20 sessions**, plus "Save bundle". |
| **D7** | CI | **edgebuild gate-run (local + VM) first**; GitHub Actions only after the gates are stable. |
| **D8** | Bambu LAN | **Out of scope.** It is not feasible without impersonating Bambu's TLS identity and command signing. UltraNet's private fakeprinter keeps covering the reprint gate. |
| **D9** | Anycubic native LAN (#621) | **Defer** until #621 is taken forward. E3 covers stock OctoPrint compatibility and Rinkhals Moonraker. |
| **D10** | Creality CFS | **Build it against the #616 branch before #616 merges**, so the emulator is #616's hand test. |
| **D11** | Existing mocks | **Don't rewrite.** Lift their code into the emulator. New gates use the emulator; old gates migrate when touched. Move the `snorca_hubtest` mocks into the repo when convenient (they sit in `%TEMP%`). |
| **D12** | Desktop QIDI path not covered by an automated gate | **Accept for E1** (owner hand-test card). Queue a separate refactor to share one QIDI start routine between `PrintHost.cpp` and `RemoteSend.cpp`. |
| **D13** | Answer LAN discovery (SSDP, UDP 48899, mDNS) | **No.** Add by IP only, which also keeps the emulator from showing up in other people's slicers on the LAN. |
| **D14** | Go / scope of the first dispatch | **E1 only** (QIDI + log + page + Q1-Q8 gate). Model choice per the agent-model note (Sonnet/Opus). Medium risk: no product code is touched. |
