# EdgeSlicer printer emulator

Fake printers on your own PC that EdgeSlicer can be pointed at by IP address. Each one answers
like a real printer, with whatever Box, CANVAS or toolhead slots you set, and logs every request
EdgeSlicer sends: the QIDI Box mapping (`SAVE_VARIABLE value_t0 ...`), the Elegoo start command
with its `slot_map`, the U1's `SET_PRINT_EXTRUDER_MAP`, the upload, the print start. You can see
exactly what a send dialog did, and break the printer on purpose to see how EdgeSlicer reacts.

Personalities:

- **E1:** the generic Moonraker / Klipper printer and the **QIDI** printer (X-Max 4, X-Plus 4, Q2,
  with or without a QIDI Box).
- **E2:** the **Elegoo Centauri Carbon 1** with a CANVAS (SDCP websocket, chunked upload, camera)
  and the **Snapmaker U1** (four toolheads, the mapped start, optional LAN/pairing MQTT).
- E3 (next): Creality, Flashforge, OctoPrint and PrusaLink.

Design: `tests/design_printer_emulator.md`. It never contacts a real printer. Printers listen on
`127.0.0.x` (your PC only). It is not part of the EdgeSlicer installer.

## Start it

- **Windows:** double-click `tools\printer_emulator\EdgeSlicer Emulator.bat`. Your browser opens
  the control page at <http://127.0.0.1:8790/>. Close the black window to stop the emulator.
- **Mac:** double-click `tools/printer_emulator/emulator.command` (run `chmod +x emulator.command`
  once if the Mac refuses). See "Mac: one address per printer" below first.
- Needs Python 3 (standard library only). The `.bat` tells you where to get Python if it is
  missing.

The first start creates four printers. An emulator you already ran in E1 gets the two new ones
added once (remove them on the page if you do not want them; they stay removed):

| Printer | Address | What it is |
|---|---|---|
| EMU QIDI X-Max 4 (1 Box) | `127.0.0.11:7125` | Box with PLA red, ASA black, an empty slot and PETG white; the printer still remembers `value_t0 = slot3`, `value_t1 = slot2` from an "earlier print" |
| EMU QIDI Q2 (no Box) | `127.0.0.12:7125` | No Box |
| EMU Elegoo CC1 (CANVAS) | `127.0.0.21` (ports 80, 3030, 3031) | CANVAS with ELEGOO PLA red (in the toolhead), an untagged white PLA (brand `— — — —`), ELEGOO ASA black and an empty tray; camera on `:3031/video` |
| EMU Snapmaker U1 | `127.0.0.31` (port 80) | Toolheads: red PLA, white PLA, yellow PETG, and an empty one |

Use **Add printer** at the top of the page for more. Each one gets its own `127.0.0.x` address:
an Elegoo printer's ports are fixed in EdgeSlicer (3030 for SDCP, 3031 for the camera), so two
CC1s need two addresses; the emulator picks the next free one.

Everything is kept in `%LOCALAPPDATA%\EdgeSlicerEmulator` (Mac:
`~/Library/Application Support/EdgeSlicerEmulator`): `printers.json`, the logs and the uploaded
files (capped at 200 MB).

## Mac: one address per printer

A Mac has only `127.0.0.1` on its loopback until you add an alias for each other address. Before
starting the emulator, run one line per printer address you use (they last until the Mac
restarts):

```
sudo ifconfig lo0 alias 127.0.0.11 up    # QIDI X-Max 4
sudo ifconfig lo0 alias 127.0.0.12 up    # QIDI Q2
sudo ifconfig lo0 alias 127.0.0.21 up    # Elegoo CC1
sudo ifconfig lo0 alias 127.0.0.31 up    # Snapmaker U1
```

Each printer card shows its own line under "Mac:", and a printer that cannot bind prints it in its
error. The emulator never runs `sudo` itself. To remove an alias: `sudo ifconfig lo0 -alias 127.0.0.21`.
Windows needs nothing: all of `127.0.0.0/8` is loopback there.

## Add it in EdgeSlicer

Each printer card's grey box tells you what to enter.

**QIDI X-Max 4:** printer preset **Qidi X-Max 4 0.4 nozzle**; add a printer device for it (the
connection settings button next to the printer preset, or the send dialog's printer list), host type
**Octo/Klipper**, hostname **127.0.0.11:7125**, a name such as **QIDI test**, then Test.

**Elegoo CC1:** printer preset **Elegoo Centauri Carbon 0.4 nozzle**; add a device with host type
**Elegoo Link** and hostname **127.0.0.21** (no port: EdgeSlicer uses 80 for the upload and 3030
for SDCP), a name such as **CC1 test**. The send dialog reads the CANVAS (Cmd 324) and shows the
trays; **Upload and Print** sends the chunked upload and then Cmd 128 with its `slot_map`. The
Stream tab and the Printers tab show the emulated camera (a test pattern with the printer's name and
a clock).

**Snapmaker U1:** add it by IP from the phone app or the Printers tab (**Add printer**, IP
**127.0.0.31**; the hub's `POST /api/snapmaker/add?ip=127.0.0.31`). A send from the phone (or
`/api/plates/{i}/send`) uploads the file with `print=false`, then sends the toolhead mapping
macros and starts it.

## Set the slots

**QIDI Box:** set the number of Boxes (0 to 4) and, for each slot, tick **Loaded** and set the type
(PLA, ASA ...), sub-type, vendor and colour. Press **Apply Box and slots**. **Stale value_t after a
reset** is what the printer "remembers" from its last print; **Reset state** puts those values back
and forgets uploads and prints. **Saved variables now** is what the read-back returns.

**CANVAS (CC1):** per tray tick **Loaded**, set the type, name, brand and colour, and pick the
**Active** tray (the one in the toolhead, status 2). **Untagged** makes the brand the four spaced
em dashes a CC1 reports for a spool without an RFID tag (EdgeSlicer shows it as "Generic"). Untick
**CANVAS attached** for "CANVAS detached": Cmd 0 then says `AmsConnectStatus 0` and Cmd 324
answers Ack 1. **Last start** shows the last Cmd 128: the file, the `slot_map` form (`absent`,
`empty` = `[]`, or `entries`) and the Ack.

**Toolheads (U1):** per toolhead tick **Loaded** and set type, sub-type, vendor and colour; the
card shows what the last `SET_PRINT_EXTRUDER_MAP` / `SET_PRINT_USED_EXTRUDERS` /
`SET_PRINT_PREFERENCES` set.

**Print state** switches any printer to printing or paused. A busy printer must make EdgeSlicer
refuse to start another print.

## Watch the log

The right-hand side is the live command log. Each line is one request or command, decoded:

- QIDI: `save_variable var=value_t0 value=slot0` (one line per `SAVE_VARIABLE`), `readback`,
  `gcode cmd=G31`, `timelapse`, `probe`
- CC1: `upload_chunk` (each chunk; hidden with the polls), `upload name=... size=... md5=...`
  once the file is complete and MD5-checked, `start` (Cmd 128) with `slot_map_form`, the full
  `slot_map`, the Ack and **warnings** (a `slot_map` entry naming an empty tray, a missing tray, or a
  tray whose material differs from the file's own filament), `canvas` (Cmd 324), `camera_url`
  (Cmd 386), `camera` / `camera_close` (a viewer of `:3031/video`), `ws_open` / `ws_close` /
  `ws_refused`
- U1: `upload`, `extruder_map config=0 map=2`, `used_extruders extruders=[2,0]`,
  `preferences end_unload_filament=[1,0,1,0]`, `start`, `probe`; with MQTT on, `mqtt_pairing`,
  `mqtt_rpc`, `mqtt_connect`

Click a line to see the full payload and the printer's state after it. **copy as test
assertion** turns that line into a line for a gate script. **hide polls** hides EdgeSlicer's
background status checks. **Save bundle** downloads a zip of this session's log, the printers
and the uploaded file list, for a PR comment. Logs are kept for the last 20 sessions.

## Break it on purpose

The **Faults** on each card change how the printer behaves:

| Printer | Fault | What EdgeSlicer should do |
|---|---|---|
| QIDI | Drop SAVE_VARIABLE | The printer says "ok" but keeps the old slots. The read-back must catch it: nothing uploaded, no print. |
| QIDI | Slow probe (12 s) | The QIDI read times out (10 s). The dialog should offer upload only, with a warning. |
| QIDI | Box unreadable / filament list 404 | EdgeSlicer cannot tell what is loaded and must not start on old slots. |
| QIDI | Box unplugged / Refuse G31 | No slot mapping / the print must still start. |
| CC1 | Upload MD5 mismatch | The last chunk answers an MD5 error; the send fails naming the MD5 check; no Cmd 128. |
| CC1 | Refuse Cmd 128 with Ack N | 1 busy, 2 not found, 3 MD5, 4 file I/O, 7 wrong model: the send reports the printer's reason. |
| CC1 | Viewer limit reached (busy) | Every websocket and camera slot taken (a CC1 allows 4): the upload lands, the start cannot connect; the camera turns viewers away. |
| CC1 | Websocket refuse / mute, file check (status 8 for N s) | Handshake refused / a printer that hangs / ElegooLink waits before Cmd 128. |
| U1 | Refuse SET_PRINT_EXTRUDER_MAP | The mapping fails: the file stays on the printer, no start. |
| U1 | Login required, print start ends in error | The phone cannot reach it / the hub reports the printer's refusal. |
| all | Timeout, connection refused, HTTP error per path, upload fails | General failure handling. |

The U1 also refuses `SET_PRINT_PREFERENCES` (the end-of-print unload flag) while it is printing or
paused, as its firmware does.

## For scripts and gates

```
py -3 emu.py status
py -3 emu.py set qidi-xmax4 faults.drop_save_variable=true
py -3 emu.py set cc1 canvas.attached=false
py -3 emu.py set cc1 slots.2.type=PLA
py -3 emu.py set u1 faults.refuse_extruder_map=true
py -3 emu.py reset qidi-xmax4
py -3 emu.py log cc1 --kind start
py -3 emu_assert.py expect cc1 kind=start slot_map_form=empty ack=0
py -3 emu_assert.py expect u1 kind=extruder_map config=0 map=2
```

`emu_assert.py` is also a Python module (`Emulator(...).expect / expect_order / expect_none`).
The gates in `tests/emulator/` start their own private emulator on a scratch folder and a free
port, and never touch your printers.json:

- `test_emulator_selftest.py` (QIDI) and `test_emulator_e2_selftest.py` (CC1, U1, U1 MQTT): the
  emulator on its own (about 20 s each).
- `test_send_qidi_gate.py` (Q1-Q9), `test_send_cc1_gate.py` (C1-C7) and `test_send_u1_gate.py`
  (U1-U6): EdgeSlicer's hub send against an emulated printer (about 5 min each; need an installed
  build and a template data dir). Rows that hit a known product gap print XFAIL; when the product
  closes the gap they print XPASS and fail, so the marker gets removed.
- `test_conformance_ledger.py`: the conformance ledger (`conformance/*.json`) lists every field
  EdgeSlicer reads from or sends to each personality as `provided`, `not_in_protocol` or
  `deferred`. It fails when EdgeSlicer's source starts reading or sending a field the ledger does not
  list, or when a field marked `provided` is missing from a live emulator's answers. No build needed.
- `tests/slic3rutils/run_emulator_e2e.py --exe <build>/tests/slic3rutils/slic3rutils_tests.exe`:
  the hidden Catch2 tag `[.EmulatorE2E]` calls `PrintHostDevices::probe`, `query_qidi_variables`,
  `canvas_query` and `SnapmakerLan::run_script` directly against the emulator.

## The U1's MQTT (optional)

`"u1": {"mqtt": {"enabled": true}}` on a U1 opens its pairing broker (plain MQTT on 1884, topic
`<code>/config/request`, code `12345678`) and a LAN broker (JSON-RPC on `<sn>/request`). With
`auto_approve` a `request_lan_auth` is approved at once. The real LAN broker is MQTT over TLS with
the printer's own certificate; the emulator's is plain, so EdgeSlicer's own LAN session cannot use
it (only tests that build the session without TLS can). There is no cloud broker.

## Safety

- Printers bind to `127.0.0.x` only. A printer can opt in to one private LAN address
  (`"lan": true` in printers.json, `10.x`, `172.16-31.x`, `192.168.x` or `169.254.x`), for testing
  from a phone. The card then shows the bind in red. `0.0.0.0`, public addresses and host names
  are refused.
- The control page is always `127.0.0.1` only.
- Never connects out: no discovery answers, no cloud, nothing. It has no Bambu personality.
- Every fake printer is obviously fake: names start with `EMU`, host names with `emu-`, serials
  and mainboard ids with `EMU-`.
- The QIDI `officiall_filas_list.cfg` it serves is synthetic. QIDI's own file is not copied.
- Not yet checked against a firmware capture (marked `unverified` in the log): the QIDI Box object
  shape, the CC1 upload's error body for an MD5 mismatch, what a real CC1 does with a `slot_map`
  naming an empty tray, and the U1's exact wording when it refuses `SET_PRINT_PREFERENCES`.
