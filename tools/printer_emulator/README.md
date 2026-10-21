# EdgeSlicer printer emulator

Fake printers on your own PC that EdgeSlicer can be pointed at by IP address. Each one answers
like a real printer, with whatever Box slots you set, and logs every request EdgeSlicer sends:
the Box mapping (`SAVE_VARIABLE value_t0 ...`), the upload, the print start. You can see exactly
what a send dialog did, and break the printer on purpose to see how EdgeSlicer reacts.

Phase **E1** has the generic Moonraker / Klipper printer and the **QIDI** printer (X-Max 4,
X-Plus 4, Q2, with or without a QIDI Box). Elegoo and the Snapmaker U1 come in E2; Creality,
Flashforge, OctoPrint and PrusaLink come in E3. Design: `tests/design_printer_emulator.md`.

It never contacts a real printer. Printers listen on `127.0.0.x` (your PC only). It is not
part of the EdgeSlicer installer.

## Start it

- **Windows:** double-click `tools\printer_emulator\EdgeSlicer Emulator.bat`. Your browser opens
  the control page at <http://127.0.0.1:8790/>. Close the black window to stop the emulator.
- **Mac:** double-click `tools/printer_emulator/emulator.command` (run `chmod +x emulator.command`
  once if the Mac refuses). A Mac has only `127.0.0.1` until you add an alias for each printer
  address, for example `sudo ifconfig lo0 alias 127.0.0.11 up`. The emulator prints the line
  when it needs one.
- Needs Python 3 (standard library only). The `.bat` tells you where to get Python if it is
  missing.

The first start creates two printers:

| Printer | Address | What it is |
|---|---|---|
| EMU QIDI X-Max 4 (1 Box) | `127.0.0.11:7125` | Box with PLA red, ASA black, an empty slot and PETG white; the printer still remembers `value_t0 = slot3`, `value_t1 = slot2` from an "earlier print" |
| EMU QIDI Q2 (no Box) | `127.0.0.12:7125` | No Box |

Use **Add printer** at the top of the page for more (X-Plus 4 with 2 Boxes read the
QIDIStudio way, or a generic Klipper printer). Each one gets its own `127.0.0.x` address.

Everything is kept in `%LOCALAPPDATA%\EdgeSlicerEmulator` (Mac:
`~/Library/Application Support/EdgeSlicerEmulator`): `printers.json`, the logs and the uploaded
files (capped at 200 MB).

## Add it in EdgeSlicer

Each printer card's grey box tells you what to enter. For the QIDI X-Max 4:

1. Select the printer preset **Qidi X-Max 4 0.4 nozzle**.
2. Add a printer device for it: open the connection settings button next to the printer preset,
   or add a printer in the send dialog's printer list. Pick
   host type **Octo/Klipper** and type the hostname **127.0.0.11:7125**. Give it a name such as
   **QIDI test**, then press Test. It should say the connection works.
3. Slice a plate and press **Print** / **Send**. The send dialog reads the Box: you see the
   emulator's slots as swatches, and the QIDI options (Box, bed leveling, timelapse, polar
   cooler).

## Set the slots

On the printer's card, under **QIDI Box**:

- Set the number of Boxes (0 to 4) and, for each slot, tick **Loaded** and set the type (PLA,
  ASA ...), sub-type, vendor and colour. Press **Apply Box and slots**. EdgeSlicer sees the
  change on its next read of the printer (reopen the send dialog).
- **Stale value_t after a reset** is what the printer "remembers" from its last print. **Reset
  state** puts those values back and forgets uploads and prints. This is how you check that
  EdgeSlicer really sends a fresh mapping.
- **Saved variables now** is what the printer holds right now. This is what EdgeSlicer reads back
  after it sends the mapping.
- **Print state** switches the printer to printing or paused. A busy printer must make EdgeSlicer
  refuse to start another print.

## Watch the log

The right-hand side is the live command log. Each line is one request, decoded:

- `save_variable var=value_t0 value=slot0`: the Box mapping, one line per `SAVE_VARIABLE`
- `readback`: EdgeSlicer reading the variables back to confirm them
- `upload name=... size=... sha256=...` and `start file=...`: the file and the print start
- `gcode cmd=G31`: bed leveling; `timelapse`: the timelapse setting
- `probe`: the send dialog reading the printer

Click a line to see the full payload and the printer's state after it. **copy as test
assertion** turns that line into a line for a gate script. **hide polls** hides EdgeSlicer's
background status checks. **Save bundle** downloads a zip of this session's log, the printers
and the uploaded file list, for a PR comment. Logs are kept for the last 20 sessions.

## Break it on purpose

The **Faults** on each card change how the printer behaves:

| Fault | What EdgeSlicer should do |
|---|---|
| Drop SAVE_VARIABLE | The printer says "ok" but keeps the old slots. EdgeSlicer's read-back must catch it: nothing uploaded, no print. |
| Slow probe (12 s) | The QIDI read times out (10 s). The dialog should offer upload only, with a warning. |
| Box unreadable / filament list 404 | EdgeSlicer cannot tell what is loaded and must not start on old slots. |
| Box unplugged | Prints from the extruder's own spool, no slot mapping. |
| Refuse G31 | Bed leveling refused. The print must still start. |
| Timeout, connection refused, HTTP error per path, upload fails, upload only on :8080, refuse start, timelapse 404 | General failure handling. |

## For scripts and gates

```
py -3 emu.py status
py -3 emu.py set qidi-xmax4 faults.drop_save_variable=true
py -3 emu.py set qidi-xmax4 slots.1.type=PLA slots.1.color="#FFFFFF"
py -3 emu.py reset qidi-xmax4
py -3 emu.py log qidi-xmax4 --kind save_variable
py -3 emu_assert.py expect qidi-xmax4 kind=save_variable var=value_t0 value=slot0
```

`emu_assert.py` is also a Python module (`Emulator(...).expect / expect_order / expect_none`).
The gates in `tests/emulator/` start their own private emulator on a scratch folder and a free
port, and never touch your printers.json:

- `tests/emulator/test_emulator_selftest.py`: the emulator on its own (about 20 s).
- `tests/emulator/test_send_qidi_gate.py`: EdgeSlicer's hub send against an emulated QIDI, rows
  Q1-Q9 (about 5 min; needs an installed build and a template data dir).

## Safety

- Printers bind to `127.0.0.x` only. A printer can opt in to one private LAN address
  (`"lan": true` in printers.json, `10.x`, `172.16-31.x`, `192.168.x` or `169.254.x`), for testing
  from a phone. The card then shows the bind in red. `0.0.0.0`, public addresses and host names
  are refused.
- The control page is always `127.0.0.1` only.
- Never connects out: no discovery answers, no cloud, nothing. It has no Bambu personality.
- Every fake printer is obviously fake: names start with `EMU`, host names with `emu-`.
- The QIDI `officiall_filas_list.cfg` it serves is synthetic. QIDI's own file is not copied.
- The QIDI Box object shape follows how EdgeSlicer and QIDIStudio read it. It has not been
  checked against a firmware capture yet.
