# Unload filaments at end of print (Snapmaker U1)

Branch `feat/u1-unload-at-end`, from `feat/ultra-preferences` at `bfc949efca`.

## The short version

The U1's firmware already unloads filament at the end of a job. Nobody had to write the unload
sequence, and nothing belongs in the G-code: the printer only needs to be told, before the print
starts, which toolheads to unload. This branch adds the printer-preset switch that decides that and
puts the answer on the wire the slicer already sends before it starts a print.

## 1. The mechanism, and why it is not G-code

### What the firmware does

`PRINT_END` — the whole of the U1's `machine_end_gcode` — ends with, verbatim from the live
printer's `GET /printer/objects/query?configfile` dump (`tests/u1_configfile.json`,
`gcode_macro print_end`):

```
M400
INNER_PRINT_END
{% if "xy" in printer.toolhead.homed_axes %}
SM_PRINT_END_AUTO_UNLOAD_FILAMENT
PARK_EXTRUDER
...
```

and `gcode_macro sm_print_end_auto_unload_filament` is:

```
{% for i in range(printer.configfile.settings.printer.max_physical_extruder_num) %}
{% if printer.print_task_config['extruders_used'][i] and
printer.print_task_config['filament_soft'][i] == False and
printer.print_task_config['end_unload_filament'][i] == True %}
AUTO_FEEDING EXTRUDER={i} UNLOAD=1 STAGE=prepare
AUTO_FEEDING EXTRUDER={i} UNLOAD=1 STAGE=doing
{% endif %}
{% endfor %}
```

So the entire feature is: get `print_task_config.end_unload_filament[i]` set to true for the
toolheads this job uses. The firmware already:

* skips a toolhead the job did not use (`extruders_used[i]`);
* skips a toolhead holding a flexible filament (`filament_soft[i] == False`) — the TPU/TPE
  exclusion the community end-G-code hacks have to write out by hand is built in;
* runs the real unload (`AUTO_FEEDING ... UNLOAD=1`, prepare then doing), parks and cools down
  afterwards as `PRINT_END` always did.

`print_task_config` is a klippy extra, not a config section: it appears in
`GET /printer/objects/list` (`u1_objects_list.json`) but not in `configfile.settings`.

### What sets it

Snapmaker's own firmware source, `Snapmaker/u1-klipper`, `klippy/extras/print_task_config.py`,
registers these G-code commands. Two of them accept `END_UNLOAD_FILAMENT`:

```
SET_PRINT_PREFERENCES      ... END_UNLOAD_FILAMENT=[1,1,0,0]
SET_PRINT_TASK_PARAMETERS  ... END_UNLOAD_FILAMENT=[1,1,0,0]
```

The value is parsed with `ast.literal_eval` and must be a list, so it is a Python list literal with
no spaces in it (a Klipper parameter value ends at a space). The firmware zeroes the whole array
first and then copies `min(len(list), PHYSICAL_EXTRUDER_NUM)` entries, so a four-entry list is both
complete and safe. `SET_PRINT_PREFERENCES` also writes `reprint_info`, so the printer's own
"print again" repeats the choice; `SET_PRINT_TASK_PARAMETERS` does not.

`SDCARD_PRINT_FILE_WITH_PARAMETERS` (`klippy/extras/virtual_sdcard.py`) is the combined form: it
calls `print_task_config.cmd_SET_PRINT_TASK_PARAMETERS(gcmd)` and then starts the file.

There is also a Moonraker webhook, `print_task_config/set_print_preferences`, but it handles only
`auto_replenish_filament`, `filament_entangle_detect`, `filament_entangle_sen`,
`replenish_ignore_color` and `end_led_turn_off` — **not** `end_unload_filament`. The G-code
commands are the only way in.

### Why the flag cannot travel inside the G-code file

Both commands refuse to run during a print. From `print_task_config.py`:

```python
print_stats = self.printer.lookup_object('print_stats', None)
if print_stats is not None and print_stats.state in ['printing', 'paused']:
    if ... or end_unload_filament is not None:
        raise gcmd.error(message="[print_task_config] not allow to set preferences during printing!", id=531, ...)
```

and `cmd_SET_PRINT_TASK_PARAMETERS` raises unconditionally in the same states. A printed file's own
lines run after `virtual_sdcard.do_resume()` has called `print_stats.note_start()`
(`self.state = "printing"`), so a `SET_PRINT_PREFERENCES` line placed in `machine_start_gcode` — or
anywhere else in the file — would raise error 531 and take the job down with it. The same reason
explains why the firmware's own `machine_start_gcode` only *reads* `print_task_config`
(`SM_PRINT_AUTO_FEED`, `SM_PRINT_START_LINE`, …) and never writes it.

**So the mechanism is the print-start request, not the G-code file.** This is a deliberate
departure from the "prefer the G-code form" instruction: the G-code form does not exist on this
firmware.

### What upstream does

Nothing. `git grep` over `snapmaker-upstream/{release_2_3_6, release_2_3_7, main,
platform-project-2.5.0, …}` finds no `end_unload_filament` / `END_UNLOAD_FILAMENT` anywhere — not in
the C++, not in the shipped Flutter Device-page bundle. That bundle's own list of `print_task_config`
fields (`main.dart.js`) stops at `extruders_replenished` and knows neither `end_unload_filament` nor
`end_led_turn_off`, so Snapmaker's slicer 2.3.7 predates this firmware feature. There is therefore
no upstream wire format to match and no upstream send-dialog checkbox to copy; what the bundle
*does* build (`SET_PRINT_EXTRUDER_MAP` / `SET_PRINT_USED_EXTRUDERS` / `SET_PRINT_PREFERENCES`, then
`printer.print.start`) is exactly the sequence this fork's LAN path already sends, so adding one
parameter to that `SET_PRINT_PREFERENCES` line stays inside the shape Snapmaker's own app uses.

### `filament_soft` is the printer's business, not ours

`filament_soft[i]` is set by `SET_PRINT_FILAMENT_CONFIG ... SOFT=<0|1>`, and when `SOFT` is omitted
the firmware derives it itself from the loaded filament's vendor/type/sub-type
(`self.filament_param_obj.get_is_soft(...)`). It describes what is *physically loaded in a
toolhead*, which the printer knows better than the slicer does. The slicer neither sets it nor
needs to: the TPU/TPE exclusion happens inside `SM_PRINT_END_AUTO_UNLOAD_FILAMENT` regardless of
what we ask for. We deliberately do not send `SOFT` from here.

## 2. What this branch adds

**The option.** `unload_filaments_at_end`, a printer-preset bool (`PrintConfig.cpp`,
`PrintConfig.hpp` in `GCodeConfig`, `Preset.cpp`'s `printer_options`), default off, shown on the
Machine tab's *Basic information* page in a new *End of print* group. `TabPrinter::toggle_options()`
hides the whole line unless `is_snapmaker_toolchanger()` — a Snapmaker `printer_model` with more
than one nozzle and `single_extruder_multi_material` off — so no other printer is offered a switch
it will ignore. Being a printer-preset option, it is written into every G-code file's config block,
which is how the choice travels with the file.

**The wire format.** `SnapmakerLan::mapping_script(mapping, unload_at_end)` appends
` END_UNLOAD_FILAMENT=[..]` to the `SET_PRINT_PREFERENCES` line it already sends, with a 1 for every
toolhead the mapping uses and an explicit 0 for the rest (four entries, `TOOLHEAD_COUNT`). The whole
thing is still one `POST /printer/gcode/script`, sent before `POST /printer/print/start`, i.e. while
the printer is idle and will accept it.

**The send paths.** `RemoteSend::prepare()` reads the flag off the printer preset on the GUI thread
(gated by `is_snapmaker_toolchanger`) into `Prepared::unload_at_end`; `run_snapmaker()` passes it to
`start_print_mapped()` and reports it in the send result (so a `dry_run=1` shows the exact script).
`GcodeArchive::Meta` remembers it in the sidecar as `unload_at_end`, and
`RemoteSend::prepare_from_record()` reads it back, so a reprint from the archive behaves the way the
print it replays did — exactly how `mapping` is already handled. A record written before this
existed has no such key and reprints unchanged.

**Not covered.** The PC's own Device tab starts Snapmaker prints from the bundled compiled Flutter
web app (`resources/web/flutter_web/main.dart.js`) over SSWCP/MQTT, not from C++. That bundle builds
its own `SET_PRINT_PREFERENCES` line and cannot be extended from this repo, so a print started from
the Device tab does not carry the flag. Prints sent from the phone/agent API's LAN path (and
reprints of those from the archive) do.

## 3. Proofs

* `libslic3r_tests` — see the branch's commit message / the agent report for the count; the base has
  two known failures.
* CLI slices on an isolated `--datadir`: a U1 plate with the option off is byte-identical to the
  same slice from the head apart from the timestamp and the new `; unload_filaments_at_end = 0`
  config line; with the option on, the only difference is that line reading `1`. This is the
  expected shape of the proof and not a weakness: **no G-code changes**, because the firmware
  refuses the command inside a print (§1). A Bambu P1S slice differs by the same single config line
  and nothing else.
* `test_u1_unload.py` + `gate_u1unload.sh` (in `snorca_hubtest`): the phone LAN send against
  `mock_printhost.py`, on a data dir whose printer preset is a Snapmaker U1, once with the option on
  and once off. With it on, the dry run reports `unload_at_end` and a `mapping_script` carrying
  `END_UNLOAD_FILAMENT=[0,1,1,0]` on the existing `SET_PRINT_PREFERENCES` line for a `0:1,1:2`
  mapping, the real send puts that on the wire, and the mock — which now applies
  `SET_PRINT_USED_EXTRUDERS` / `END_UNLOAD_FILAMENT` the way `print_task_config.py` does — then
  reports `print_task_config.end_unload_filament = [false,true,true,false]`. With it off, no
  `END_UNLOAD_FILAMENT` is sent and the printer holds no flags.

## 4. The hardware test (for the owner)

Nothing below was verified against a real printer; the U1s were off limits for this work.

1. Load two rigid filaments (PLA/PETG) in two toolheads and, deliberately, a TPU/TPE in a third.
   Slice a small two-colour plate that uses only the two rigid toolheads.
2. Turn *Unload filaments at end of print* on in the U1 printer preset, and send the plate **from
   the phone/agent API's LAN path** (the Devices card / `POST /api/plates/{i}/send`), not from the
   PC's Device tab — the Device tab's Flutter page does not carry the flag (§2).
3. Before the print starts, confirm the printer took the flag (read-only, no side effects):
   `GET http://<u1-ip>/printer/objects/query?print_task_config`
   and look at `end_unload_filament` — it should be `true` for exactly the two toolheads the job
   uses, `false` for the others; `extruders_used` should agree; `filament_soft` should be `true` for
   the TPU toolhead.
4. Watch the end of the job: only the two used, non-flexible toolheads should go through the
   `AUTO_FEEDING ... UNLOAD=1` prepare/doing cycle; the TPU toolhead must be left alone even though
   it holds filament. Time the tail so the real per-print cost of leaving the option on is known.
5. Afterwards, confirm heaters and fans are off and the feeder LED for each unloaded toolhead is
   out (the wiki's own criterion). Filament goes back up the feed tube; taking it off the spool
   holder is still a person's job — the U1 has no hub buffer.
6. Repeat with the option off and confirm the printer holds `end_unload_filament = [false × 4]` and
   the job ends exactly as it does today.

### Read-only queries the owner can run for us

Both are plain unauthenticated GETs on the U1 (`SnapmakerLan.cpp`'s own note: the U1 answers
`/printer/objects/query` with no credential) and change nothing:

* `GET http://<u1-ip>/printer/objects/query?print_task_config` — during or just before a job: the
  flags the printer is actually holding (`end_unload_filament`, `extruders_used`, `filament_soft`).
* `GET http://<u1-ip>/printer/objects/query?configfile` — confirms this firmware build really has
  `gcode_macro sm_print_end_auto_unload_filament` and `gcode_macro auto_feeding` (the dump in
  `tests/u1_configfile.json` says the owner's does).

## 5. Later: an "Unload now" button on the phone

`SnapmakerLan.cpp`'s file-local `gcode_script()` (`POST /printer/gcode/script?script=`) is already
the transport for everything in §1; it only needs a header declaration to become a generic
`run_script()`. A hub-side *Unload now*, and a per-printer "unload when a job finishes" that works
for prints this instance did not send, would:

* hang off `RemoteEvents`' existing `finished` transition (`RemoteEvents.cpp:154`) or a new action
  in `RemoteControl.cpp`'s `pause|resume|stop` table;
* send, at idle, for each toolhead `i` that reports filament loaded
  (`SnapmakerLan::Toolhead::loaded`, which already comes from `print_task_config.filament_exist`):
  `AUTO_FEEDING EXTRUDER=<i> UNLOAD=1 STAGE=prepare` then `... STAGE=doing`;
* guard it with the same soft-filament rule the firmware uses — the live
  `print_task_config.filament_soft[i]` is already in the same query the toolhead list is parsed
  from, so the phone can skip a flexible toolhead without guessing from a type string.

That path needs no `end_unload_filament` at all: it is the same `AUTO_FEEDING` the firmware's own
`SM_PRINT_END_AUTO_UNLOAD_FILAMENT` runs, issued directly while the printer is idle.
