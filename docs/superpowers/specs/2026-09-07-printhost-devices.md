# Print-host devices

Phase 1 (`feat/printhost-devices-p1`, cut from `origin/feat/ultra-preferences` at `09af327a37`):
the store, the migration and the dialog.
Phase 2 (`feat/printhost-devices-p2`, cut from `origin/feat/ultra-preferences` at `bfc949efca`):
no main printer, the sidebar icon, the send, the Device tab.

## The problem

A printer preset carries exactly one address. `print_host`, `printhost_apikey`,
`printhost_authorization_type`, `printhost_user`, `printhost_password` and `host_type` are ordinary
options on the printer preset (`src/libslic3r/PrintConfig.cpp:752-824, 4662`), and every send path -
`PrintHostSendDialog`, `PrintHostJob(DynamicPrintConfig*)`, the hub's `RemoteSend::list_hosts` -
reads that one config.

Somebody with three identical printers on three IPs therefore has to either retype the address
before every send, or keep three near-identical presets that differ only in `print_host` - which
then also splits everything that keys off the preset name (filament and process compatibility
lists). What they want is to slice once and send that plate to any of the three.

## What was built in phase 1

Store, migration, dialog and a read-only listing on the hub. **No send path changed.** The preset is
still the single address every send reads; the dialog's "Use this device" writes the chosen device's
address and credentials into it.

> **Superseded by phase 2.** That bridge is gone: nothing writes to the preset, the send picks a
> device, and the store remembers only which one was last used. Read the rest of this section as
> history; "What was built in phase 2" below is what the code does now.

| Piece | Where |
|---|---|
| The store (wx-free model) | `src/slic3r/Utils/PrintHostDevices.{hpp,cpp}` |
| The dialog | `src/slic3r/GUI/PrintHostDevicesDialog.{hpp,cpp}` |
| "Devices..." beside the existing editor | `src/slic3r/GUI/PhysicalPrinterDialog.{hpp,cpp}` |
| `/api/printers` entries | `src/slic3r/GUI/RemoteSend.cpp` (`list_hosts`) |
| Status probing for those entries | `src/slic3r/GUI/RemoteControl.cpp` (`list_host_targets`, `describe_hosts`) |
| Tests | `tests/slic3rutils/printhost_devices_tests.cpp` |

### Why a fork-native store and not PrusaSlicer's `PhysicalPrinter`

The research (`multi_device_send_research.md`, §3b) recommended this and the code bears it out:

* `PhysicalPrinter`/`PhysicalPrinterCollection` (`src/libslic3r/Preset.hpp:860-1030`) are compiled
  in, but every path that creates or edits one was cut by BambuStudio: the migration
  `load_printers_from_presets()` is inside `#if 0` (`Preset.cpp:3660`), the `Tab.cpp` button and its
  binding are commented out, `PresetComboBoxes.hpp` no longer declares `add/edit_physical_printer`,
  and `PresetBundle.cpp:3046` hardcodes the preset→device link empty. Reviving it means undoing four
  deliberate decisions and then building a new dialog anyway (the class called
  `PhysicalPrinterDialog` in this fork is a *single-address* host editor, not Prusa's manager).
* `PhysicalPrinter` conflates "a device" with "the presets selected under it" (`preset_names`),
  which is the opposite of the one-preset-many-addresses shape wanted here.
* The hub reads `<datadir>/hub/*.json` and has no path that reads presets at all. A JSON file next
  to `snapmaker_lan.json` is something `api_printers` can merge with no new plumbing.

So the store is modelled on the fork's own `SnapmakerLan::Device` (`GUI/SnapmakerLan.cpp:85-190`):
pretty-printed JSON, written through a `.tmp` and renamed, read tolerantly (anything unreadable is
an empty list), in `<datadir>/hub/`.

The model is deliberately wx-free so the Catch2 suite can exercise it directly; the GUI, the hub and
(later) the send fan-out are all just callers.

## The file

`<datadir>/hub/print_host_devices.json` - on Windows,
`%APPDATA%\EdgeSlicer\hub\print_host_devices.json`.

```jsonc
{
 "version": 1,
 "models": {
  // the key: the preset's printer_model, so all the variants of one machine
  // ("0.4 nozzle", "0.2 nozzle") and every user preset derived from them share
  // one device list. A preset with no printer_model at all falls back to
  // "preset:<preset name>".
  "Elegoo Centauri Carbon": {
   "current": "d3a17f0c9b21",     // the device whose address the preset holds (may be "")
   "migrated": ["192.168.1.41"],  // normalized addresses already imported from a preset
   "devices": [
    {
     "id": "d3a17f0c9b21",        // generated once, stable for the life of the entry
     "alias": "Left bay",         // what a person calls it
     "address": "192.168.1.41",   // host[:port], an IP or a URL - whatever print_host would hold
     "host_type": "elegoolink",   // a host_type enum key (PrintConfig.cpp:80-95)
     "auth_type": "key",          // printhost_authorization_type: "key" | "user"
     "apikey": "",                // printhost_apikey     (auth_type == key)
     "user": "",                  // printhost_user       (auth_type == user)
     "password": "",              // printhost_password   (auth_type == user)
     "printer_model": "Elegoo Centauri Carbon",
     "created": 1757203200,       // unix seconds
     "last_used": 0               // unix seconds, 0 = never
    }
   ]
  }
 }
}
```

Reading rules (all of them exercised by the tests):

* the file missing, unparseable, or not a JSON object → an empty store;
* a `devices` entry that is not an object, or has no `address` → dropped;
* a field of the wrong type → the field's default (`host_type` "octoprint", `auth_type` "key",
  `created`/`last_used` 0), never a refusal to load;
* writing to a broken file repairs it.

### The key

`model_key(printer_model, preset_name)`:

* `printer_model` non-empty → the `printer_model` string itself. It is what the codebase already
  groups printer presets by (`PresetCollection::diameters_for_same_printer_model`,
  `Preset.cpp:2794-2810`), and it is the same for every nozzle variant of a machine and for every
  user preset derived from them - exactly the grouping wanted: one physical printer, one device
  list, whatever variant is selected.
* `printer_model` empty (a hand-made preset, "Custom Printer") → `"preset:<preset name>"`. Grouping
  every such preset under one key would put unrelated machines in one list; the preset name is the
  finest identity still available.

The vendor is deliberately **not** part of the key. `Preset::vendor` is only set on system presets,
so a vendor prefix gives `Snapmaker U1 (0.4 nozzle)` the key `Snapmaker:Snapmaker U1` and the user's
own copy of it the key `:Snapmaker U1` - two lists for one machine, the very split this feature
exists to remove. (Measured on `dd_lan`, whose selected preset is a user copy.) `printer_model`
strings are vendor-qualified in practice anyway: "Elegoo Centauri Carbon", "Snapmaker U1", "Bambu
Lab X1 Carbon".

### Secrets

An API key or password is stored **in clear**, in the user's data dir, exactly as the printer preset
stores `printhost_apikey` today. This codebase has no secret store at all - no `wxSecretStore`, no
`CredWrite`, no keychain call anywhere in `src/` - so introducing one for this file only would leave
the identical secret in the preset beside it. If a secret store is ever added, both should move
together.

## The migration

Runs whenever the devices dialog is opened and on every `/api/printers` - in
`RemoteControl::list_host_targets` and again in `RemoteSend::list_hosts`; the first of those runs
before the probe targets are collected, so even the very first poll both lists a device and probes
it. An existing single-address user therefore sees their printer as device 1 without doing anything.

Per printer preset with a non-empty `print_host` (every visible one, plus the preset being edited,
whose address may not be saved yet):

1. normalize the address (trim, drop a trailing `/`, lower-case);
2. if that address is already in this model's `migrated` list → **stop**. This is what keeps a device
   the user deleted afterwards deleted;
3. otherwise record it in `migrated`, and if no device of that model already has that address,
   create one: `alias` = the preset name, `address`, `host_type`, `auth_type` and the credentials
   copied from the preset. If the model had no `current`, the new device becomes it;
4. the preset itself is **never** written to.

Idempotent by construction: a second run creates nothing. Bambu printers are skipped
(`use_bbl_network()`) - they have their own device list in the Device tab.

## The dialog

`PrintHostDevicesDialog` (`_L("Devices")`), a `DPIDialog` in the style of `PrintHostQueueDialog`: a
`wxDataViewListCtrl` with **Name / Hostname, IP or URL / Host Type / Status**, and Add… / Edit… /
Remove / Test / Use this device / Close. Status reads `unknown` for every row - the column is there
so a later phase can fill it from the same probe the hub uses, rather than the layout moving under
the user later.

* **Add / Edit** open a small row editor with exactly the fields the single-address editor has:
  name, address, host type (from `print_config_def.get("host_type")`, so the list never drifts),
  authorization type, API key, user, password.
* **Test** builds a copy of the printer preset's config, applies the row's address and credentials to
  the copy, and runs the same `PrintHost::test()` the single-address editor runs. The preset is not
  touched.
* **Use this device** was the phase-1 bridge, and **no longer exists** (phase 2). It wrote the row's
  address and credentials into the preset's own host fields so every send path kept reading the one
  preset it always read - which made one device the model's main printer.

Reached from the **"Devices…" button on the print-host line of `PhysicalPrinterDialog`**, the
single-address editor the sidebar's connection icon opened in phase 1 (`Plater.cpp:2372`). That
editor is unchanged and still does everything it did; the new button sits beside Browse / Test /
Log Out on the same line.

> **Phase 2:** the sidebar icon opens this list instead, the editor is reached from **"Edit
> connection..."** inside it, the buttons are the fork's `Button`/`SetStyle` ones, and nothing here
> writes the preset any more, so there is no option group to reload. The **Status** column still
> reads `unknown` for every row: phase 2 put the probe in the *send* dialog, where the answer is
> needed to offer a mapping, and left this column alone. Filling it means running the same probe per
> row on a worker; it is not done.

## The hub / the phone

`GET /api/printers` gains one entry per device of the currently selected printer's model, beside the
existing `host` / `connect` / Bambu / Snapmaker entries:

```jsonc
{
  "id": "ph:d3a17f0c9b21",   // "ph:" + the device id
  "kind": "printhost",
  "name": "Left bay",        // the alias, or the address when there is none
  "alias": "Left bay",
  "device_id": "d3a17f0c9b21",
  "model_key": "Elegoo Centauri Carbon",
  "model": "Elegoo Centauri Carbon",
  "url": "192.168.1.41",
  "host_type": "elegoolink",
  "is_current": true,        // this is the device the preset points at
  "online": true,
  "status": "unknown",
  "can_upload": false,       // read-only in phase 1
  "can_print": false,
  "can_pause": false, "can_resume": false, "can_stop": false,
  "print_error": null,
  "upload_name": "MyPlate.gcode"
}
```

Status: a device whose `host_type` speaks Moonraker (`octoprint`, the fork's "Octo/Klipper" label) is
added to `RemoteControl::list_host_targets` under the same `ph:<id>` id, so
`RemoteControl::describe_hosts` probes it from the request thread through the existing probe cache
and 30 s offline backoff (`RemoteControl.cpp:127-160`) and fills in `status`, `online`,
`print_status`, the temperatures and the control predicates. Anything else (Elegoo Link speaks SDCP
over its own websocket, Duet, PrusaLink…) is never probed and keeps `status: "unknown"` - guessing
would cost the caller a timeout per address on every poll.

`can_upload`/`can_print` were `false` on purpose in phase 1: it was read-only, and the phone kept
sending through the `host` entry. **Phase 2 answers them** and accepts `ph:<id>` as a send target.

## Tests

`tests/slic3rutils/printhost_devices_tests.cpp` (Catch2, tag `[PrintHostDevices]`). The store is
wx-free and `slic3rutils_tests` already links `libslic3r_gui`, so no CLI hook was needed. Every case
points the store at its own temp file via `set_store_path()`; nothing reads or writes the user's
data dir.

1. **round trip** - add, read back every field, the file exists and is JSON; a duplicate address in
   the same model is refused;
2. **id stability** - the id survives a rename, a readdress and a reload; `created` is not rewritten
   by an edit; `current`/`last_used`/`remove` behave;
3. **grouping by model** - two models, the same address in both is fine, a device of one is not
   visible under the other, `all_devices()` groups them, and the two nozzle variants of one machine
   produce the same key while a preset with no `printer_model` falls back to its own name;
4. **tolerant read** - not JSON; JSON of the wrong shape; the right shape with a non-object entry, an
   entry with no address and fields of the wrong type; no file at all;
5. **migration** - a preset's address becomes device 1 and this model's `current`; a preset with no
   `print_host` contributes nothing; a second run creates nothing; another variant of the same
   machine at the same address (with a trailing slash) creates nothing; a device deleted after the
   import stays deleted; a genuinely new address does arrive;
6. **the preset bridge** - `apply_to_config` writes the fields the send path reads, `from_config`
   reads them back, the host-type and auth-type mapping and `normalize_address`.

## What was built in phase 2

Phase 1 was read-only and kept the preset as the single address every send read; its "Use this
device" button wrote the chosen device into the preset. The owner's review of it settled the shape:
**there is no main printer.** The preset keeps whatever address the user typed into it, nothing
writes to it behind their back, and a send picks a device at send time.

| Piece | Where |
|---|---|
| `last_used_id` / `set_last_used` (the bridge and `current` are gone) | `src/slic3r/Utils/PrintHostDevices.{hpp,cpp}` |
| `can_send_for(preset)` - the one send-enable condition | `src/slic3r/Utils/PrintHostDevices.{hpp,cpp}` |
| Asking a device what is loaded | `src/slic3r/Utils/PrintHostDeviceStatus.{hpp,cpp}` (new) |
| The restyled dialog, "Edit connection..." | `src/slic3r/GUI/PrintHostDevicesDialog.{hpp,cpp}` |
| The sidebar's connection icon | `src/slic3r/GUI/Plater.cpp:2370` |
| Device dropdown + filament mapping in the send dialog | `src/slic3r/GUI/PrintHostDialogs.{hpp,cpp}` |
| The job built from the device, not the preset | `src/slic3r/GUI/Plater.cpp` (`send_gcode_legacy`) |
| `device_id` / `device_name` / `filament_mapping` on the job, and the sidecar | `src/slic3r/Utils/PrintHost.{hpp,cpp}` |
| `ph:<id>` as a send target | `src/slic3r/GUI/RemoteSend.cpp` (`prepare_host`, the dispatch, `list_hosts`) |
| The Device tab's device picker | `src/slic3r/GUI/PrinterWebView.{hpp,cpp}`, `Plater.cpp` (`update_all_preset_comboboxes`) |
| Tests | `tests/slic3rutils/printhost_devices_tests.cpp`, `snorca_hubtest/test_printhost_devices_p2.py` |

### 1. No main printer

`current` / `set_current` / `touch` became `last_used_id` / `set_last_used`. The difference is not
the name: `current` meant "the device the preset points at" and something had to keep the preset in
step with it; `last_used` is a *memory of where the last plate went*, read only to preselect a row.

* "Use this device" is gone, and with it `config_changed()` and the option-group reload it forced on
  `PhysicalPrinterDialog`. Editing a device no longer writes the preset either.
* `apply_to_config` is unchanged but now only ever fills in a **copy**: `config_for(device,
  preset_config)` is the one-liner the send and the dialog's Test use.
* The migration no longer marks the device it imports. An import is not a send.
* A phase-1 store's model-level `"current"` is still read as the last-used device - it is the best
  guess at "the one you last sent to" - and `set_last_used` overwrites both keys so an older build
  reading the file cannot resurrect a stale choice.
* `/api/printers` keeps `is_current` (phase 1's name, which the phone's list reads) and adds
  `last_used` beside it. Both mean "where the last plate went".

### 2. The sidebar's connection icon

The wifi icon on the printer row (`PlaterPresetComboBox`'s owner-drawn connection button, shown for
every non-Bambu, non-U1 preset) opened `PhysicalPrinterDialog` - the single-address host editor. It
now opens `PrintHostDevicesDialog` for the current printer model. The old editor is one explicit
click away, behind **"Edit connection..."** inside that dialog, which is also where
`sm_disconnect_current_machine()` moved to; the address it saves is imported as a device on the way
back, so the list is right immediately.

The buttons are the fork's own `Button` + `Button::SetStyle(ButtonStyle, ButtonType)` - the call
`FileArchiveDialog.cpp:378-410` and `MsgDialog.cpp:166-170` make - so they follow the dark-mode
palette instead of being the app's only stock `wxButton`s. Add / Edit / Test are `Regular`, Remove
is `Alert`, Close is `Confirm`; the row editor's OK / Cancel replaced
`CreateStdDialogButtonSizer`. The list takes focus, Tab walks the buttons left to right in the
order they are drawn, and Esc closes.

### 3. What enables Print

`MainFrame::can_send_gcode` asked one question - does the edited preset's `print_host` hold a
string (`MainFrame.cpp:1730`) - so a model with three addresses in its device list and an empty
`print_host` had its Print button greyed out. That is precisely the case this feature exists for.

`PrintHostDevices::can_send_for(preset)` is now the one place that decides: **the preset's own
`print_host` has an address, OR the model has at least one device that does.** `has_devices(key)` is
the half of it the hub wants alone. Callers:

* `MainFrame::can_send_gcode` (`MainFrame.cpp:1729`) - the desktop Print/Send button.
* `RemoteSend::list_hosts` - the `ph:` entries' `can_upload` / `can_print`, which phase 1 pinned to
  `false`. They are now answered the way the `host` entry's are: build the host from that device's
  config copy and ask it.
* `Sidebar::update_all_preset_comboboxes` - a model with devices sets the button to Send even when
  the preset's `print_host` is empty (it used to fall through to Export G-code and load
  `missing_connection.html`).

`ph:<id>` is also a real send target now: `preselect()` lets it through, `prepare()` routes it to
`prepare_host` (in phase 1 it fell into the Bambu arm and 404'd), and `prepare_host` builds the host
from the device rather than the preset - which is what makes a send work at all when the preset has
no `print_host`. It records the device as the model's last used one at the same moment the desktop
dialog does: on the choice, not on the outcome.

### 4. The send dialog

`PrintHostSendDialog` starts with **Printer:** - a read-only combo of the model's devices, the last
one sent to preselected - above the file name. `Plater::send_gcode_legacy` builds `ph_config` (the
preset's config with the preselected device applied) *before* the dialog, because the dialog needs
the host's post-upload actions, groups and storage; after OK it rebuilds `PrintHostJob` against the
chosen device, carrying `upload_data` across.

The job carries `device_id`, `device_name` and `filament_mapping`, so `PrintHostJobQueue::
perform_job`'s archive hook writes the sidecar as `"ph:<device id>"` with the device's own name and
the mapping instead of the flat `"host"` - the phase-5 "send this again, to that one" hook, cheap
now.

#### What a print host will actually tell you about its filaments

This is the part worth being blunt about, because the answer is mostly "nothing", and it was
measured against this fork's own clients rather than assumed.

* **Elegoo Link** (the Centauri Carbon) - **no filament, material, box or slot data exists in the
  protocol as this fork speaks it.** `ElegooLink` (`src/slic3r/Utils/ElegooLink.cpp`) opens SDCP over
  its own websocket at `ws://<host>:3030/websocket` and sends exactly two commands: `Cmd 0` (status),
  of whose answer it reads one field - `Status.CurrentStatus`, and only to notice the value `8`,
  "checking the file" - and `Cmd 128` (start print). `Cmd 1` (`Attributes`), which is where an SDCP
  device would describe its materials, is declared in the enum at `ElegooLink.cpp:37-44` and **never
  sent**; `PrintInfo` is never read. Detection is a `GET <host>/` whose body is regex-searched for
  "ELEGOO" (`ElegooLink.cpp:250`); the upload is `POST <host>/uploadFile/upload`, chunked, with no
  per-extruder parameter anywhere. The only extra print parameters that exist at all are `bedType`,
  `timeLapse` and `heatedBedLeveling`, and the dialog that would supply them
  (`ElegooPrintHostSendDialog`) is never instantiated. So the dialog says "Elegoo Link reports no
  filament or slot data (its SDCP client asks only for the print status), so there is nothing to
  map. The plate is sent exactly as it was sliced.", and sends without a mapping.
* **Moonraker / Octo-Klipper** - one `GET <base>/printer/objects/query?print_stats&
  print_task_config&extruder&extruder1&extruder2&extruder3`, 3 s timeout, the same objects
  `RemoteControl::describe_hosts` already asks for plus `print_task_config`. That object's parallel
  arrays (`filament_type`, `filament_sub_type`, `filament_vendor`, `filament_color_rgba`,
  `filament_exist`) are the loaded filaments per toolhead - the same object `SnapmakerLan::
  toolheads_of` reads (`SnapmakerLan.cpp:456`) and the same one SSWCP's machine filament info comes
  from. A stock Klipper has no `print_task_config`; then the `extruder`/`extruder1`... objects still
  say how many tools the machine has, which is a slot list with nothing in it - so the dialog shows
  the status and says it did not learn what is loaded, rather than offering a table of blanks.
* **Everything else** (Duet, PrusaLink, Repetier, SimplyPrint) - not probed. This fork has no client
  for their status APIs, and guessing would cost a timeout per address on every open.

The mapping table therefore appears only when the printer **named the materials**, not merely the
tools: one row per filament the plate actually used, its colour swatch, its type, and a slot choice
preset by the closest loaded colour (redmean, the same distance `SnapmakerLan::auto_match` uses, so
the two auto-matches agree). It yields `"0:1,1:2"` - the wire form the Snapmaker LAN path already
speaks - which travels on the job and into the sidecar's `mapping`.

The probe blocks with a busy cursor rather than threading, exactly as the devices dialog's Test
does: a modal send dialog can afford one short request, and a worker thread would have to outlive a
dialog the user can close.

### 5. The Device tab

The Device tab for a print-host printer is `PrinterWebView` showing the printer's own web UI, and
the address it showed was always the preset's - so with three printers in the list you could look at
exactly one of them. `PrinterWebView::set_devices(model_key, devices, select_id)` gives it a picker
above the page; an empty list hides it again (a Bambu or Snapmaker printer, or a model with no
devices), which is what the tab did before. Choosing a row loads that device's URL with that
device's API key and records it as the model's last used one, so the Device tab and the next Print
agree about which printer you are looking at.

`print_host_webui` is an override for the preset's own address, so it is honoured only for the
device that carries that address; every other device is reached at its own.

## Phase 2's proofs

* Clean Release build of the worktree (`Snapmaker_Orca`, `Snapmaker_Orca_app_gui`, both test
  binaries), VS2022 x64, `BUILD_TESTS=ON`.
* `slic3rutils_tests "[PrintHostDevices]"`: 9 cases, 151 assertions, green. New coverage:
  `last_used_id`/`set_last_used` and that removing a device takes the memory with it; the migration
  *not* marking what it imports; reading a phase-1 store's `"current"`; `config_for` leaving the
  preset alone; `can_send_for` on a preset with no address and one device, across nozzle variants,
  and not across models; and `parse_moonraker_status` - the network-free half of the probe - against
  a `print_task_config` with four filaments, a stock Klipper with two bare tools, a non-Moonraker
  answer, and Elegoo Link (never asked, and the note says why).
* `libslic3r_tests`: 654 passed, 2 failed as expected - unchanged.
* LAN, LAN-control, phone-UI, control and archive gates against a scratch install of this build
  (`snorca_hubtest/gate_dev2.sh`, a copy of `gate_all.sh` pointed at this worktree, installing to
  `inst_dev2`): all PASS. (The LAN gate failed once on a `mock_printhost.py` left in `printing` by an
  earlier session; it passes against a freshly started mock.)
* `snorca_hubtest/test_printhost_devices_p2.py` (new, `gate_dev2.sh devices`): a hidden instance on a
  copy of `dd_lan` shows `/api/printers` answering `can_upload` true for the imported device; a
  second device added to the store by hand - one the preset has never named - is listed and
  uploadable; `POST /api/plates/0/send?printer=ph:<id>` is accepted, the mock receives the file, the
  archive sidecar records `"ph:<id>"` with the device's own name, the store remembers it, and an
  unknown device id is a 404 rather than a fall-through into the Bambu arm.
* `mock_printhost.py` needed no extension: it already answers `print_task_config` with four loaded
  filaments, so it is also what the desktop mapping table can be seen against.

**Nobody clicked any of the dialogs.** Everything above is the store, the hub API and the send path;
the GUI was compiled, not exercised. The clicks that need a person, with one Elegoo Centauri Carbon:

1. Sidebar, the wifi icon on the printer row - it should open **Devices**, not the old host editor.
2. **Add...** - name it, put in the Centauri's IP, host type **Elegoo Link**, OK. **Test** should
   say it found it. Status stays `unknown`: expected, Elegoo has no status API this fork speaks.
3. **Edit connection...** - the old single-address editor should open, and only from there.
4. Close. Slice a plate. **Print** should be enabled even if the preset's own address field is empty.
5. Print -> the send dialog's **Printer:** dropdown lists the Centauri. Under it, the note about
   Elegoo Link reporting no filament data, and no mapping table. **Upload and Print**.
6. The Device tab should now show a **Printer:** picker above the page, on the Centauri.
7. For the mapping table, point a second device at `mock_printhost.py` (`127.0.0.1:18089`, host type
   Octo/Klipper) and pick it in the send dialog: four slots, a row per plate filament.

## Phase 2 follow-up: the Elegoo Upload routing (`fix/elegoo-upload-routing`)

The owner, with an Elegoo Centauri Carbon selected and a Snapmaker U1 on the same LAN:

> "for the Elegoo printer, it allows the 'print' option, but the 'upload' option leads the user to
> the Snapmaker U1-specific pre-treat menu."

### The cause

`use_new_connect` is an app-config key, and it is **global and sticky**. SSWCP
(`SSWCP.cpp:6954`), `SMPhysicalPrinterDialog::OnOK` (`SMPhysicalPrinterDialog.cpp:701`) and
`RemoteSnapmaker`'s `announce_connected` all set it to `"true"` the moment *any* Snapmaker machine
connects; only a disconnect clears it. Nothing about it is per-preset - it is the state of one
connection, and it survives every printer-preset change.

Three places read it on its own, as if it meant "this printer uses the connect flow":

| Where | What it decided | What went wrong with an Elegoo selected |
|---|---|---|
| `Plater::send_gcode_legacy` (`Plater.cpp:22442`) | `use_new_connect \|\| is_snapmaker_u1` → the whole Snapmaker arm: a `PrintHostSendDialog` built with `StartPrint` and **no `set_devices`**, then `WebPreprintDialog` | Print handed the plate to the Snapmaker pre-print page instead of building a `PrintHostJob` |
| `MainFrame::can_send_gcode` (`MainFrame.cpp:1726`) | `use_new_connect` → Print always enabled | Enabled for a preset with no address and no devices, whose Print then opened that page |
| `Sidebar::update_all_preset_comboboxes` (`Plater.cpp:3739`) | `!use_new_connection && !is_snapmaker_u1` gated the *entire* print-host row | No connection icon, no device list, no `set_devices` - and the Device tab on `missing_connection.html` |

That third row is why the owner's screenshot has **no "Printer:" dropdown**. The dropdown is not
gated on device count - `build_device_ui` draws it for one device on purpose ("a farm of one is a
farm") - it was simply never given a list, because the Elegoo preset never reached the print-host
branch at all.

And why "Upload and Print" looked right while "Upload" did not: **both** went to
`WebPreprintDialog`, and `set_send_page(post_action == None)` picks which page it shows. `Upload and
Print` (`StartPrint`) got the *pre-print* page, whose own Print button does start a print - so it
appeared to work. `Upload` (`None`) got the *pre-treat* page - the U1 toolhead-mapping page - with
an Elegoo file and nothing to map. Neither ever built a `PrintHostJob`; one of the two just happened
to end in a print.

### The fix

One wx-free decision function in the store, so the question has a single answer and a test:

```cpp
enum class SendFlow { PrintHost, SnapmakerConnect };
SendFlow send_flow_for(const std::string& printer_model, bool connect_flow_active);
```

* a **non-Snapmaker** `printer_model` → always `PrintHost`, whatever the flag says. This is the fix:
  an Elegoo Link, Octo/Klipper, PrusaLink, Duet, Repetier or SimplyPrint printer takes the
  print-host path, so Upload is a `PrintHostJob` with `post_action = None` and Upload and Print is
  `StartPrint`, both against the device chosen in the dropdown;
* a **Snapmaker U1** → always `SnapmakerConnect`, exactly as before the fix (its toolhead mapping
  page is the only way to choose the tools for a plate);
* **any other Snapmaker** → follows its own connection: `SnapmakerConnect` when connected, otherwise
  the `print_host` address it holds like any other host;
* an **empty** `printer_model` (a hand-made preset) → `PrintHost`, never swept into the connect flow.

All three call sites above route through it, so the button, the sidebar row and the send it triggers
cannot disagree. The flag is never read alone again.

### The device dropdown with one device

Kept as it was, and now actually reached: **shown whenever the model has a device list**, including
a list of one. The migration imports the preset's own address as device 1, so any preset that can
send at all has at least one row - the user always sees which address the plate is about to go to.
The only case with no dropdown is a preset with no address anywhere, where Send is disabled.

### Proofs

* Clean Release build of the worktree (`Snapmaker_Orca`, `Snapmaker_Orca_app_gui`,
  `slic3rutils_tests`), VS2022 x64, `BUILD_TESTS=ON`.
* `slic3rutils_tests "[PrintHostDevices]"`: the new case **"which send a preset takes"** puts every
  (model, flag) combination through `send_flow_for` - the Centauri with the flag on and off, five
  other print hosts, the U1 both ways, another Snapmaker both ways, an empty model name - plus the
  two model predicates.
* `snorca_hubtest/test_printhost_devices_p2.py` gained the end-to-end half: a `mode=upload` send to
  a `ph:` device lands on `mock_printhost.py` with **`print=false`**, starts **no** print, and makes
  **no** pre-print call (the mock records every `/printer/gcode/script`, so a Snapmaker
  `SET_PRINT_EXTRUDER_MAP` / `SET_PRINT_USED_EXTRUDERS` pass would show); a new section D sends
  `mode=print` to the same device and requires that one **does** start, so the two modes are proved
  different rather than uniformly broken.
* The LAN and archive gates against a scratch install of this build.

**Nobody clicked the dialogs.** The desktop branch is compiled and its decision is unit-tested; the
end-to-end evidence is the hub's dispatch, which shares the contract but not the buggy branch. The
clicks that need a person, with the Elegoo Centauri Carbon selected **while a Snapmaker U1 is
connected** (that is the broken state - with nothing connected it always worked):

1. Slice a plate, press **Print**. The send dialog opens with a **Printer:** dropdown naming the
   Centauri - not the Snapmaker pre-treat page.
2. **Upload**. The file appears on the printer and **no further dialog opens**; the Snapmaker
   pre-treat page must not appear.
3. Slice again, **Upload and Print**. The print starts.
4. The sidebar's printer row shows the connection (wifi) icon, and the Device tab shows the
   Centauri's own web UI rather than `missing_connection.html`.
5. Select the U1 again: its Print must still open the Snapmaker pre-print page, unchanged.

## Phases 3-5

Phase 2 turned the read-only list into the thing a send actually targets, which changes what is left.

* **Phase 3 - the fan-out.** The send dialog's device dropdown becomes a **multi-select**: pick two
  or three of the model's devices and confirming enqueues one `PrintHostJob` per device into the
  existing `PrintHostJobQueue` (already a single background thread pulling one job at a time,
  already one row per job with a `COL_HOST` column), each holding its own config copy and its own
  filament mapping. One plate, N uploads, no re-slice. The plumbing is done - a job already carries
  its device and its mapping, and the archive already records which device each went to - so this is
  the dropdown's selection model, N `PrintHostJob`s in the loop that currently builds one, and a
  queue row that names the device rather than the host type.
* **Phase 4 - phone parity.** The hub can already send to `ph:<id>`, but the phone's Send sheet still
  offers the `host` entry as the print-host target and has no mapping step for it. Parity means: the
  Devices page lists the model's devices as first-class cards (they are already in `/api/printers`
  with `can_upload`/`can_print`), the Send sheet's target picker offers them, and it carries the same
  `mapping=` parameter the Snapmaker LAN path already takes - `/api/plates/{i}/send` accepts it
  today, `prepare_host` just ignores it. The slot list a phone would draw needs
  `PrintHostDeviceStatus::probe` exposed on the device entries, which is one field on the
  `describe_hosts` probe rather than a new poll.
* **Phase 5 - Reprints retarget.** The sidecar already says `"ph:<device id>"` with the device's name
  and the mapping used, so a Reprint knows exactly where its bytes went. Retarget means letting the
  Reprints list send an archived record to *another* device of the same model - the record's
  `printer.model` is the key into the store - with the mapping re-offered against that device's
  slots. `/api/archive/{id}/send` takes a `printer=` already; it needs the same `ph:` case
  `prepare_host` grew, plus a device picker in the Reprints UI.
* **Still open from phase 1's plan, now smaller.** Discovery (Elegoo's UDP 3000, the existing Bonjour
  pass) to offer addresses instead of typing them, and a Preferences page listing every model that
  has devices so a farm can be set up before a preset is selected. Neither blocks anything above.

## Not done, deliberately

* **Phase 1 only:** no send path changed, no queue change, no archive change. Phase 2 changed all
  three; what is still deliberately out is below.
* Bambu and Flashforge keep their own device lists; this store is for the `print_host` host types
  only. They meet only in the merged `/api/printers` list.
* Nothing validates that a device's `host_type` matches what the address actually answers - the
  research flags this; the dialog's Test is the manual answer, and the send dialog's probe is the
  automatic one for the Moonraker-shaped hosts. For Elegoo and the rest there is nothing to check
  against.
* No fan-out yet: one send goes to one device. The queue and the archive are ready for N; the
  dropdown is single-select on purpose until phase 3.
* No secret store, still. A device's API key or password sits in clear in
  `<datadir>/hub/print_host_devices.json`, exactly as `printhost_apikey` sits in the preset beside
  it.
* The mapping is offered and recorded, but nothing rewrites the G-code with it: it travels to the
  host as the plate was sliced plus a mapping the host may or may not act on. For a Snapmaker-shaped
  Moonraker that is what `SET_PRINT_EXTRUDER_MAP` is for; wiring the mapping into a start script for
  print-host devices is phase 3's job, alongside the fan-out.
