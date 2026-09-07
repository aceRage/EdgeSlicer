# Print-host devices, phase 1: the store, the migration and the dialog

Branch `feat/printhost-devices-p1`, cut from `origin/feat/ultra-preferences` at `09af327a37`.

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
address and credentials into it. Phase 3 replaces that bridge with a real fan-out.

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
Remove / Test / Use this device / Close. Status reads `unknown` for every row in phase 1 - the
column is there so phase 2 can fill it from the same probe the hub uses, rather than the layout
moving under the user later.

* **Add / Edit** open a small row editor with exactly the fields the single-address editor has:
  name, address, host type (from `print_config_def.get("host_type")`, so the list never drifts),
  authorization type, API key, user, password.
* **Test** builds a copy of the printer preset's config, applies the row's address and credentials to
  the copy, and runs the same `PrintHost::test()` the single-address editor runs. The preset is not
  touched.
* **Use this device** is the phase-1 bridge: it writes the row's address and credentials into the
  preset's own `print_host`/`printhost_*`/`host_type` fields (`apply_to_config`), records the device
  as this model's `current`, and stamps `last_used`. Every existing send path - desktop Send to
  printer, the hub, the phone - therefore keeps working unchanged, reading the one preset it always
  read.

Reached from the **"Devices…" button on the print-host line of `PhysicalPrinterDialog`**, the
single-address editor the sidebar's connection icon opens today (`Plater.cpp:2372`). That editor is
unchanged and still does everything it did; the new button sits beside Browse / Test / Log Out on the
same line. When the dialog changed the preset's fields, the editor reloads its option group so the
address shown is the one now selected.

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

`can_upload`/`can_print` are `false` on purpose: phase 1 is read-only, and the phone should keep
sending through the `host` entry until the fan-out exists.

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

## Phases 2-5

* **Phase 2 - status in the dialog.** Fill the Status column from the same probe the hub uses (the
  Moonraker probe cache), and give the non-Moonraker host types a cheap liveness check of their own
  (`PrintHost::test()` on a worker, or Elegoo's UDP 3000 discovery) rather than "unknown".
* **Phase 3 - the fan-out, and the end of the bridge.** `PrintHostSendDialog` gets a device
  multi-select when the model has more than one device; confirming enqueues one `PrintHostJob` per
  selected device into the existing `PrintHostJobQueue` (already a single background thread pulling
  one job at a time, already one row per job with a `COL_HOST` column), each job holding a shallow
  copy of the preset config with that device's address and credentials applied. One plate, N uploads,
  no re-slice. `can_upload`/`can_print` on the hub entries turn on, and `api_send` learns to take a
  `ph:<id>` target. The "Use this device" bridge stays as the single-target shortcut.
* **Phase 4 - the archive.** `GcodeArchive::Meta` already carries `printer_id`/`printer_kind`/
  `printer_name`/`printer_model` per send; record `ph:<device id>` so a reprint replays to the device
  it actually went to, and let the Reprints list retarget it to another device of the same model.
* **Phase 5 - discovery and Preferences.** Elegoo's UDP 3000 / the existing Bonjour pass to offer
  addresses instead of typing them, and a Preferences page listing every model that has devices, for
  setting a farm up before a preset is selected.

## Not done, deliberately

* No send path changed, no queue change, no archive change.
* Bambu and Flashforge keep their own device lists; this store is for the `print_host` host types
  only. They meet only in the merged `/api/printers` list.
* Nothing validates that a device's `host_type` matches what the address actually answers - the
  research flags this; the dialog's Test is the manual answer for now, and phase 2's probe is the
  automatic one.
