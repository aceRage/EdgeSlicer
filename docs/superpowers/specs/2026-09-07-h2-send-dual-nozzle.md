# What an H2D/H2C LAN send carries, and what it was missing

Date: 2026-09-07
Host branch: `fix/h2-send-dual-nozzle` (from `origin/feat/ultra-preferences`, `302d19f34d`)
Plugin branch: `fix/h2-project-file` in `src/ultranet` (from `fix/mqtt-session-per-printer`, `d2fb00d`)
Upstream reference: `C:\Dev\BambuStudio` at `66e405477` (2026-08-31). All upstream line numbers are
from that commit. Upstream's *plugin* is closed source, so the upstream side quoted here is always
the host: what it puts into `PrintParams`, and what the machine profiles and firmware documents say.

Two symptoms were reported from a real H2C on 2026-09-07, after the 3MF schema fix made sends
succeed at all (see `2026-09-06-h2c-3mf-schema.md`):

1. severe Z-offset problems, because the job never runs the printer's nozzle offset calibration;
2. a job stored on the printer does not re-print as a dual-nozzle job - re-printing from the
   screen only offers filament from the left nozzle's AMS.

---

## 1. Field by field

### 1a. The `project_file` command

`upstream PrintParams` is what BambuStudio's host hands its plugin; `our plugin JSON` is the key
this fork's `bambu_networking.dll` actually publishes on `device/<sn>/request`
(`src/ultranet/UltraNetAgent.cpp`, `Agent::startPrint`). Where the JSON key is marked *(inferred)*
the wire name could not be read from any source available here - see §4.

| upstream `PrintParams` / dialog option | our host fills it | our plugin JSON key | status |
|---|---|---|---|
| `dev_id` | yes, `PrintJob.cpp:227` | *(routing only - the publish topic)* | ok |
| `filename` (the .3mf) | yes | `url` `"ftp:///<name>.3mf"` + `md5` | ok |
| `plate_index` | yes | `param` `"Metadata/plate_N.gcode"` | ok |
| `project_name` / `task_name` | yes | `subtask_name` | ok |
| `task_bed_type` | yes (always `MachineBedTypeString[0]`) | `bed_type` | ok |
| `task_bed_leveling` (bool) | yes, from the checkbox | `bed_leveling` | ok |
| `task_flow_cali` (bool) | yes, from the checkbox | `flow_cali` | ok |
| `task_vibration_cali` (bool) | always `false` | `vibration_cali` | ok (upstream also passes `false`) |
| `task_layer_inspect` (bool) | always `true` | `layer_inspect` | ok |
| `task_record_timelapse` (bool) | yes, from the checkbox | `timelapse` | ok |
| `task_use_ams` (bool) | yes | `use_ams` | ok |
| `ams_mapping` (v0, global tray index per filament) | yes | `ams_mapping` | ok |
| `ams_mapping2` (v1, `[{ams_id,slot_id}]`) | yes | `ams_mapping2` | ok |
| `ams_mapping_info` (rich, per filament) | yes, **but without `nozzleId`** | `ams_mapping_info` | **was a gap - fixed** |
| `nozzles_info` (`[{id,diameter,flowSize,type}]`) | yes, **but `flowSize` hard-coded** | `nozzles_info` | **was a gap - fixed** |
| `auto_bed_leveling` (int 0/1/2) | no - hard-coded `0` | `auto_bed_leveling` *(inferred)* | wired, still always 0 |
| `auto_flow_cali` (int 0/1/2) | no - hard-coded `0` | `auto_flow_cali` *(inferred)* | wired, still always 0 |
| **`auto_offset_cali` (int 0/1/2)** | **no - hard-coded `0`** | **`auto_offset_cali` + `nozzle_offset_cali` *(inferred)*** | **the bug - fixed** |
| `nozzle_mapping` (nozzle-rack target) | field does not exist in this fork's `PrintParams` | not sent | H2D Pro nozzle rack only - out of scope, see §5 |
| `extruder_cali_manual_mode` (int, the PA switch) | field does not exist | not sent | see §5 |
| `task_ext_change_assist` (bool) | field does not exist | not sent | see §5 |
| `task_timelapse_use_internal` (bool) | field does not exist | not sent | internal-timelapse storage only |
| `try_emmc_print`, `svc_context`, `slicer_uid`, `queue_plate_id` | fields do not exist | not sent | cloud/queue features this fork has no path for |
| - | - | `project_id`/`profile_id`/`task_id`/`subtask_id` `"0"` | LAN-only constants, unchanged |

### 1b. The dialog option itself

| | upstream (`SelectMachine.cpp`) | this fork, before | this fork, after |
|---|---|---|---|
| widget | `PrintOption` tri-state switch, `ops_auto` = Auto / On / Off, param `"nozzle_offset_cali"` (`:687`) | did not exist | `create_item_checkbox(_L("Nozzle Offset Calibration"), ..., "nozzle_offset_cali")` |
| shown when | `obj->GetConfig()->SupportCalibrationNozzleOffset()`, parsed from the device's `print` json key `support_nozzle_offset_calibration` (`DeviceCore/DevConfig.cpp:63`); `:2604` | - | `obj->is_multi_extruders()` **or** the selected printer preset having two `nozzle_diameter` values (this fork does not parse that capability flag - see §5) |
| default | `"auto"` when the config has no stored value (`load_option_vals`, `:3006`); forced to `"auto"` for a multi-extruder machine using >= 2 nozzles, `"off"` for one nozzle without a switcher (`:3014-3025`) | - | checked, i.e. auto |
| persistence | `AppConfig` `[<printer_type>] nozzle_offset_cali = auto\|on\|off`, written by `save_option_vals` on every change (`:3111`) | - | `AppConfig` `[print] nozzle_offset_cali = 0\|1`, written by the checkbox handler, read back in `set_default` - the same mechanism the fork's other three options use |
| forced off | by-object print sequence (`:4136`) | - | not ported (see §5) |
| value sent | `m_checkbox_list["nozzle_offset_cali"]->getValueInt()` -> `set_print_config(..., auto_offset_cali, ...)`, `0 = off, 1 = on, 2 = auto` (`PrintOption::getValueInt`, `:7318`) | `set_print_config(..., 0, 0, 0)` - all three ints hard-coded | ticked -> `2` (auto), unticked or hidden -> `0` |

### 1c. `ams_mapping_info`, entry by entry

Upstream `SelectMachineDialog::get_ams_mapping_result` (`:1425-1509`) vs this fork's (`:1119`):

| key | upstream | fork before | fork after |
|---|---|---|---|
| `ams` | tray id | same | same |
| `filamentId` | preset filament id | same | same |
| `filamentType` | e.g. `"PLA"` | same | same |
| `sourceColor` / `targetColor` | yes | same | same |
| **`nozzleId`** | `s_convert_filament_map_nozzle_id_to_task_nozzle_id(filament_maps[i])`, i.e. project `filament_map` (1 = left, 2 = right) mapped to the task numbering (**1 = left, 0 = right**) (`:1483`) | **absent** | present, but only when the printer preset really has two nozzles |

`CloudTaskNozzleId { RIGHT = 0, LEFT = 1 }` and `FilamentMapNozzleId { LEFT = 1, RIGHT = 2 }` are
identical in both trees; the fork was missing the second enum and the converter, which are now
ported verbatim.

### 1d. `nozzles_info`, entry by entry

| key | upstream (`:1541-1589`) | fork before | fork after |
|---|---|---|---|
| `id` | `CloudTaskNozzleId` per config index: index 0 (left) -> 1, index 1 (right) -> 0 | same | same |
| `diameter` | `nozzle_diameter[i]` | same | same |
| `type` | `nullptr` - always. Upstream never fills it either, so the fork comment calling it a gap was wrong | `nullptr` | `nullptr` |
| `flowSize` | `get_nozzle_volume_type_cloud_string(project_config nozzle_volume_type[i])`: `standard_flow` / `high_flow` / `tpu_high_flow` / `e3d_high_flow` / `hybrid_flow` | **hard-coded `"standard_flow"`, marked `// TODO: Orca hack`** | the real per-nozzle value; the constant only survives as the fallback when the option is missing |
| built when | `nozzle_diameter.size() == 2` | same | same |

`nozzle_volume_type` is in this fork's project-config option list (`PresetBundle.cpp:196`), so the
value was there all along.

---

## 2. Symptom 1: the Z offset

**The machine was always ready to do this.** The H2C and H2D `machine_start_gcode` this fork ships
is byte-identical to upstream's (verified by diffing every key of
`resources/profiles/BBL/machine/Bambu Lab H2C 0.4 nozzle template machine_start_gcode.json` and the
`Bambu Lab H2C*.json` / `H2D*.json` machine presets against `C:\Dev\BambuStudio`: no key differs).
Its first lines are

```gcode
;M1002 set_flag extrude_cali_flag=1
;M1002 set_flag g29_before_print_flag=1
;M1002 set_flag auto_cali_toolhead_offset_flag=1
```

and further down

```gcode
;===== xy ofst cali start =====
M1002 judge_flag auto_cali_toolhead_offset_flag
...
```

The leading `;` is the whole mechanism. Neither this fork nor upstream ever writes or rewrites a
`set_flag` line - `grep -rn "set_flag" src/` in both trees matches nothing but an unrelated lambda
in the G-code viewer. The printer's own job pre-processor un-comments the `set_flag` line for each
option the *task* asked for, and `judge_flag` then branches on it. `flow_cali` drives
`extrude_cali_flag`, `bed_leveling` drives `g29_before_print_flag`, and the nozzle offset
calibration drives `auto_cali_toolhead_offset_flag`.

Our `project_file` said nothing about the nozzle offset, so `auto_cali_toolhead_offset_flag` was
never set, the `judge_flag` block was skipped on every send, and each nozzle kept whatever Z trim
it last had. On a machine whose two toolheads are calibrated against each other that is exactly a
"severe Z-offset" on the second nozzle.

### Other causes considered, and what the evidence says

| candidate | verdict |
|---|---|
| **no nozzle offset calibration requested** | **the leading cause.** The gcode scaffolding is present and inert; only the task flag was missing. Fixed on both sides of this change - but see Test A: this is proven from the profiles and the source, not from hardware. |
| `nozzle_height` | not it. In both trees this is gantry clearance for arrange/skirt decisions (`Arrange.cpp`, `Print.cpp`), never a Z offset. Identical in both. |
| a per-extruder Z or `extruder_offset` value differing from upstream | not it. `extruder_offset` is XY, and the H2 machine presets are byte-identical to upstream, so no per-nozzle offset value differs. (The fork's own global `z_offset` and per-filament `filament_z_offset` are separate candidates - two rows down.) |
| `filament_map` / `physical_extruder_map` ordering | not a Z cause, but a real fork gap. `physical_extruder_map` is defined in the fork's `PrintConfig` and is never read by its `GCode.cpp`; upstream uses it at `GCode.cpp:1108-1110, 4461-4462`. The fork hard-codes the placeholder variables `current_nozzle_id`, `next_nozzle_id`, `initial_nozzle_id`, `curr_physical_extruder_id`, `most_used_physical_extruder_id` to `0` (`src/libslic3r/GCode.cpp:2860-2870`, the "single-mapped shim" block). In the H2C templates those only feed `nozzle_diameter_at_nozzle_id[...]`, `nozzle_volume_types[...]`, flush temperatures and `M620.10` purge parameters - flow and temperature, not Z. On an H2C with two 0.4 nozzles the diameter lookups even give the right answer by accident. It is a latent correctness problem for mixed-diameter or mixed-flow nozzles, and it is the deferred grouping-engine port, not this fix. |
| `z_offset` | upstream BambuStudio has **no** `z_offset` setting at all; this fork inherits Orca's global one (`PrintConfig.cpp:5837`, default 0, applied to every Z in `GCode.cpp`). Global, so it cannot by itself make one nozzle differ from the other - but if it is non-zero it shifts an H2C print that upstream would not have shifted. Worth checking on the user's machine. |
| `filament_z_offset` | fork-only too (`PrintConfig.cpp:2638`, default 0, not set by any BBL profile). It is folded into the job's global `z_offset`, and **only for the job's first printing filament** (`GCode.cpp:2523-2533`). On a two-filament dual-nozzle job with different per-filament values, the first filament's offset is applied to the whole print including the other nozzle. Off by default, so it is only a cause if the user set it - but it is exactly the shape of "per-nozzle Z offset" and worth ruling out first on the user's own presets. |
| first-layer print settings per extruder | not investigated beyond the machine-preset diff, which found no difference. Listed as unverified in §4. |

---

## 3. Symptom 2: the stored job is not dual-nozzle

What can tell the firmware that a stored job uses both nozzles:

| carrier | state |
|---|---|
| `project_file.nozzles_info` | present since the dual-nozzle send work; its `flowSize` was wrong (always `standard_flow`) and is now correct |
| `project_file.ams_mapping_info[].nozzleId` | **absent** - this is the per-filament nozzle assignment, and it is the only field in the whole payload that says *which* nozzle a given filament belongs to. Added by this change. |
| `project_file.ams_mapping2` (`[{ams_id,slot_id}]`) | present; says which slot, never which nozzle |
| `project_file.ams_mapping` (v0 global tray index) | present; same - slot only |
| `project_file.use_ams` | present |
| 3MF `Metadata/slice_info.config`: `filament_maps`, per-filament `group_id`/`nozzle_diameter`/`nozzle_volume_type`, `<nozzle>` tags, `enable_filament_dynamic_map` | present since the 2026-09-06 schema port (`bbs_3mf.cpp` `_add_slice_info_config_file_to_archive`) |
| 3MF `Metadata/filament_sequence.json`: `sequence`, `nozzle_sequence`, `optimal_assignment` | present since the same port |
| G-code `filament_map` placeholder | driven from the stored grouping result (`GCode.cpp:2820`), so per-filament routing is real; the *nozzle id* placeholders around it are still shimmed to 0 (see the table in §2) |

So the payload gap is exactly one field, `nozzleId`, and the 3MF side already carries the grouping.
That makes `nozzleId` the leading explanation and the cheapest thing to test. It is **not proven**:
no capture of a real BambuStudio dual-nozzle `project_file` exists here, and the printer-side
behaviour when re-printing a stored job is not documented anywhere readable. See §4 and §6.

---

## 4. What is inferred, and what stays unverified

**Inferred - the wire key names for the tri-state calibration ints.** Upstream's plugin is closed
source. The public protocol notes this plugin was written from (OpenBambuAPI, cited in
`src/ultranet/PROVENANCE.md`) predate the H2 series and list only the boolean options. Searching
upstream for a literal `"nozzle_offset_cali"`/`auto_offset_cali` JSON key finds only the host-side
field name and the AppConfig/checkbox param. This fork therefore sends **both** spellings with the
same value:

```json
"auto_offset_cali": 2, "nozzle_offset_cali": 2
```

Unknown keys are ignored by the firmware - already proven in this fork by `ams_mapping2`,
`ams_mapping_info` and `nozzles_info` being accepted by the real H2C - so the redundancy is free,
and whichever one turns out to be dead can be deleted once a capture settles it.

**Inferred - the meaning of `2`.** `PrintOption::getValueInt` (`SelectMachine.cpp:7318`) maps
`off -> 0, on -> 1, auto -> 2`, and `CalibUtils.cpp:2326` comments `nozzle_offset_cali = auto` next
to the literal `2`. That the printer reads the same encoding is inferred from the host, not
observed.

**Unverified.**

- Whether the H2C accepts and acts on either key. Only the user's hardware can answer that (§6).
- Whether `nozzleId` is what makes a stored job re-printable as dual-nozzle. Adding it is upstream
  parity, which is the strongest argument available, but the printer-side effect is untested.
- Whether the firmware wants `ams_mapping_info` on a single-nozzle machine to carry `nozzleId` too.
  Upstream adds it whenever `filament_maps` has the index, i.e. also on one-nozzle printers; this
  fork deliberately does not, to keep those payloads byte-identical. If a single-nozzle regression
  ever appears, that is the first difference to revisit.
- Per-extruder first-layer / `z_offset` print settings. The machine presets are identical to
  upstream; the print presets were not audited.
- Whether the shimmed nozzle-id placeholders in `GCode.cpp` cause any printer-visible problem on a
  two-0.4-nozzle H2C. Reasoned about, not measured.

## 4b. The phone send path

`src/slic3r/GUI/RemoteSend.cpp` is a second, independent assembler of the same `PrintParams` - it
is how the phone app sends a plate - and it had the same three holes, copied from the same source:
`nozzles_info()` hard-coded `flowSize` to `"standard_flow"`, its `ams_mapping()` wrote no
`nozzleId`, and nothing set `auto_offset_cali`. All three are fixed there too, gated identically
(two `nozzle_diameter` values in the printer preset), and the calibration follows the same
`[print] nozzle_offset_cali` AppConfig key the desktop checkbox writes - so a phone send honours
whatever the send dialog was last told. There is no phone-side control for it yet.

`get_nozzle_volume_type_cloud_string` is now declared in `SelectMachine.hpp` and shared by both,
rather than duplicated; it takes the raw enum value as an `int` so the header needs no
`PrintConfig.hpp`.

## 5. Deliberately not done

- **No new `PrintParams` field.** The plugin compiles against
  `C:\Dev\SnapmakerOrca\src\slic3r\Utils\bambu_networking.hpp` - a *different* repository and branch
  from the host being changed here (`src/ultranet/CMakeLists.txt` include path `${CMAKE_CURRENT_SOURCE_DIR}/..`).
  The two copies are byte-identical today, and the struct crosses the plugin boundary **by value**,
  so adding a member on one side only would corrupt every call. `auto_offset_cali` already exists in
  both copies, which is why this fix needs no ABI change at all. `nozzle_mapping`,
  `extruder_cali_manual_mode`, `task_ext_change_assist` and `try_emmc_print` would each need one, and
  are left alone.
- **`nozzle_mapping`** is upstream's nozzle-*rack* target (`obj_->GetNozzleRack()->IsSupported()`),
  an H2D Pro feature. Not an H2C concern.
- **The tri-state widget.** Upstream's `PrintOption` switch does not exist in this fork's send
  dialog, which uses plain checkboxes for its other three options. Porting the widget is a much
  larger change than the fix warrants, so the checkbox means upstream's *default* state, Auto (2),
  and unticking means Off (0). A user who wants "always calibrate" (1) cannot ask for it; Auto
  already runs the calibration whenever the printer thinks it is needed, which is the reported
  problem.
- **`support_nozzle_offset_calibration`.** Upstream gates the option on that device capability flag.
  This fork does not parse the device capability report at all for these options - see the
  `bbl_caps_fallback` comment in `update_select_layout` - so the option is gated on the machine
  having two nozzles instead. Two independent ways of knowing that are OR-ed, because each alone
  has a hole: `is_multi_extruders()` is false until the printer has pushed an `extruder` block,
  and the selected printer preset is right before that but describes the preset rather than the
  machine on the other end. Showing the option for a printer that turns out not to support it
  costs one ignored JSON key; hiding it from an H2C costs the calibration.
- **The by-object-sequence force-off** (upstream `:4136`) is not ported; it is a refinement of a
  feature that did not exist here a moment ago.

## 6. Hardware tests

These are the user's to run. Nothing here contacts a printer.

### Test A - the nozzle offset calibration actually runs

0. First, a 30-second pre-flight that costs nothing: in Printer settings check that **Z offset** is
   `0`, and in each of the two filament presets you are about to use check that **Filament Z
   offset** is `0`. Both are fork-only settings upstream does not have, both default to 0, and the
   second one is applied to the whole print from the *first* filament only. If either is non-zero,
   that alone can produce the symptom and should be cleared before testing anything else.
1. Open a two-filament model that uses **both** nozzles on the H2C (filaments assigned to the left
   and right nozzle in the filament-group panel).
2. Slice, then press the LAN send button. In the send dialog check that a **Nozzle Offset
   Calibration** checkbox is now present next to Bed Leveling / Flow Dynamics Calibration, and that
   it is **ticked** by default. Leave it ticked.
3. Send.
4. On the printer, watch the start-of-job sequence. Expected: after the bed-levelling / purge
   phase, the toolhead runs the **XY / toolhead offset calibration** pass (the `;===== xy ofst cali
   start =====` block) before the first layer. It is a distinct move pattern near the front-left of
   the bed, and the screen shows a calibration step rather than going straight to printing.
5. Then judge the print: the second nozzle's first layer should sit at the same Z as the first
   nozzle's.
6. Control: send the same plate again with the checkbox **unticked**. The calibration block should
   be skipped and the old Z behaviour should return. If unticking changes nothing either way, the
   key name is wrong - see §4, and tell us which of the two spellings to keep hunting.

### Test B - the stored job offers both AMS units

1. With the fixed build, send the same dual-nozzle two-filament plate to the H2C.
2. Let it finish (or cancel it after the first layer - the job is stored either way).
3. On the printer screen, open the stored/history job and choose **re-print**.
4. Expected: the filament-selection screen offers slots from **both** AMS units, and shows two
   filaments to map, one per nozzle.
5. If it still only offers the left nozzle's AMS, `nozzleId` is not the carrier. The next thing to
   capture is what a genuine Bambu Studio dual-nozzle send puts on
   `device/<sn>/request` - that single capture would settle both §4 questions at once.

### What to report back

- whether the calibration block ran (Test A step 4), and whether Z improved;
- whether unticking the box suppressed it;
- whether the stored job offered both AMS units;
- if you can, the printer's own log or an MQTT capture of the `project_file` message, which is what
  turns every *(inferred)* in §1 into a fact.

## 6b. Best-evidence ranking

**"Severe Z offset on the H2C"**, most likely first:

1. **The job never asked for the nozzle offset calibration.** The gcode scaffolding
   (`auto_cali_toolhead_offset_flag`) is present and byte-identical to upstream, the host never
   touches it, and the payload had no field that could turn it on. Direct, and fixed here.
2. **A non-zero `filament_z_offset` or `z_offset`.** Fork-only settings, default 0, but if set they
   shift Z globally and `filament_z_offset` is read from the first filament only. Cheap to rule out
   (Test A step 0).
3. **The shimmed nozzle-id placeholders** (`initial_nozzle_id` etc. hard-coded to 0). They feed
   diameter, flow and temperature lookups, not Z, and on a two-0.4-nozzle H2C they happen to give
   the right answer - so this is a real latent bug but an unlikely cause of *this* symptom.
4. Something machine-side that no slicer field reaches. Only ruled in if the calibration runs and Z
   is still wrong.

**"The stored job is not dual-nozzle"**, most likely first:

1. **`ams_mapping_info[].nozzleId` was missing.** It is the only field in the entire payload that
   says which nozzle a filament belongs to, upstream sends it, and we did not. Fixed here.
2. **`nozzles_info.flowSize` was always `standard_flow`.** If the printer matches the stored job's
   nozzles against the ones it has, a high-flow right nozzle described as standard-flow is a
   mismatch. Fixed here.
3. **A field only a real capture would reveal.** The v0/v1 AMS mappings and the 3MF grouping are
   already at upstream parity, so if 1 and 2 do not fix it, the remaining difference is something
   invisible from source - which is exactly what one capture of a genuine Bambu Studio dual-nozzle
   send would expose.
4. **A printer-side rule about stored jobs** unrelated to the payload (for example only jobs sent
   from Bambu's own cloud path being re-printable with full AMS choice). Cannot be tested from here.

## 7. Proof carried by this change

- **Plugin.** `src/ultranet/tools/payloadtest.cpp` (new) drives the real `bambu_networking.dll`
  across the plugin ABI against the loopback fake printer with two canned jobs, and the fake printer
  records exactly what was published. Between the branch point and the fix:
  - the P1S single-nozzle payload is **byte-identical** (compared as raw strings, not just as
    parsed JSON);
  - the H2D/H2C dual-nozzle payload gains exactly `"auto_offset_cali":2,"nozzle_offset_cali":2` and
    nothing else.
  The existing `sessiontest` gate (`normal`, `noack`, `drop`) still passes against the new DLL.
- **Host.** Release build of `Snapmaker_Orca` + `Snapmaker_Orca_app_gui` + `libslic3r_tests` from a
  clean worktree: no errors. `libslic3r_tests` is untouched by this change - it only edits
  `src/slic3r/GUI/SelectMachine.{cpp,hpp}` and `src/slic3r/GUI/RemoteSend.cpp`, none of which
  `libslic3r` or its tests link - and it runs green: 621 test cases, 619 passed, 2 failed as
  expected; 54559 assertions, 54557 passed, 2 failed as expected.
