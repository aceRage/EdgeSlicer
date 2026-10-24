# Gap analysis: work package 5C-3, OrcaSlicer #16156 (AMS Sync, Filament Mapping, Storage Tab)

Date 2026-10-24. Read-only analysis against origin/main `0b3af03608` (after #678). Upstream: merge `029bc7928f`, +3835/-258, 48 files.
Plan reference: `tests/orca_port_plan_remaining.md` section 5C-3 (owner picks 2026-10-15: A Storage tab, B filament mapping with prints for Klipper multi-material printers, C AMS dialog fixes for non-Bambu; D capability gating skipped).

## 1. Bottom line

**Drop 5C-3 as a package.** Almost all of it is either already shipped in Edge under other names or is plumbing for an architecture Edge does not have.

- #16156 is built on Orca's `IPrinterAgent` / `OrcaPrinterAgent` layer (about 1000 of its 3835 added lines are OrcaPrinterAgent, IPrinterAgent, NetworkAgent forwards and their tests). Edge has no printer-agent layer (the #612 body and research X1 record this decision: keep the print-host probe plus send dialog). Everything that talks to that layer is not applicable.
- **A (Storage tab) is done**: #548 (`85b24a12eb`, "Orca #16156 part A"), the shared `IFileGridModel`/`RemoteFileGridModel`, `PrinterStorage` and `PrinterStoragePanel`, on the Device page for Moonraker/Klipper and the Snapmaker U1 LAN.
- **B (mapping) is mostly done** for every printer where a mapping can really be sent: Elegoo CC1 + CANVAS (#625, #660, phases 1-3 of the CANVAS design), QIDI Box (#643, #661), Snapmaker U1 (tool slots), Bambu (BambuSendMapping, BambuReprint, hub `ams_mapping` via #673/#674). What is left is a **protocol question**, not a port: there is no standard way to send a mapping to a generic Klipper MMU (Happy Hare, AFC) at print start. Upstream's own Moonraker agent does not do it either (its `start_print` is a stub, per the #612 body).
- **C (AMS dialog fixes for non-Bambu) has no target in Edge.** The AMS widgets (`AMSItem`, `AMSMaterialsSetting`, `StatusPanel`) only ever see Bambu `MachineObject`s here; non-Bambu printers use the Device web page, the CANVAS card, the send-dialog swatch map and the hub rows.

What is genuinely left is small: two optional Bambu-side robustness bits (section 4) and optional Storage extensions to non-Moonraker printers (CC1, Creality). None needs 5C-3 as a work package.

## 2. #16156 broken into pieces

| # | Upstream piece (files) | Size (approx. lines) | Status in Edge | Where / why |
|---|---|---|---|---|
| 1 | Capability gating: `IPrinterAgent` `supports_command/feature`, `uses_filament_mapping`, get_capabilities parsing (`IPrinterAgent.hpp`, `OrcaPrinterAgent.*`, `NetworkAgentFactory.hpp`) | ~350 | **Not applicable** | No agent layer. Owner already skipped D. Edge's equivalent is the probe's slot facts plus `slot_send_policy` (`PrintHostDeviceStatus.cpp:1082`) and the QIDI/CANVAS policies. |
| 2 | Mapping serializer: `ams_mapping2` to `print.gcode_file.filament_mapping`, `FilamentMappingUtils.hpp`, refusals "unsupported / incomplete", hooks in `SelectMachine.cpp` and `SendMultiMachinePage.cpp` | ~230 GUI + agent side | **Not applicable as written, intent covered** | The wire format is Orca's own connector protocol (`OrcaPrinterAgent::build_filament_mapping`). Edge sends mappings per real protocol: CANVAS `slot_map` (`ElegooStartPrint`, `ElegooCanvasMapping`), QIDI `SAVE_VARIABLE value_tN` (`qidi_start_script`), Bambu `ams_mapping`/`ams_mapping2` (`BambuSendMapping`). The refusal rules (all used filaments mapped, material and busy checks) exist as `slot_send_policy`, `qidi_send_policy`, `plan_unattended_send` (#660, #661, #643). |
| 3 | `SyncAmsInfoDialog.cpp` comment only (array position equals logical filament index, never re-densify) | 6 | **Not applicable** | Documents the Orca agent's code. Edge's Bambu sync is unchanged. |
| 4 | `PresetBundle::sync_ams_list`: a printer-set tray (no preset id, but a material type) resolves to the matching Generic preset with the tray's colour; `ams_infos.back().valid = true`; `get_printer_model_display_name`; printer-name assert relaxed | ~90 | **Not covered**, low value | Edge keeps the older behaviour (typeless or unknown trays become Generic PLA or are dropped). Only matters for trays whose preset id is empty but whose type is set. On Bambu, third-party spools set on the printer screen already carry a generic id, so the symptom is unlikely there; the change was written for the Orca agent. Optional, see 4.3. |
| 5 | `DevAmsTray::UpdateEmptyState` / `is_empty`, `AMSItem` "Empty" rendering, `AMSPreview` strike line | ~60 | **Not covered**, Bambu-neutral | Distinguishes an explicitly empty tray from an unknown one on non-Bambu agents. Bambu already reports empties via `AMS_CAN_TYPE_EMPTY`. Needed only if a non-Bambu source ever fills `MachineObject`. Edge's non-Bambu tray displays (CANVAS card, swatch map `ThirdPartyFilamentMap`) have their own `empty` flag. |
| 6 | `AMSMaterialsSetting.cpp` (122 lines): model resolved from agent id or selected profile so the dropdown and Confirm work on non-Bambu | ~120 | **Not applicable** | The Materials Setting dialog opens only from the Bambu Status page. No non-Bambu `MachineObject` exists. |
| 7 | `StatusPanel.cpp`: a third-party tray pre-fills nozzle temps for every tray type, not only BBL | 8 | **Not covered**, trivial | Real but tiny: for a non-BBL tray on a Bambu AMS the dialog opens with empty min/max temps. See 4.2. |
| 8 | Hide K/N (flow dynamics) for non-Bambu agents (`AMSItem`, `AMSDryControl`, `AMSSetting`, `CaliHistoryDialog`, `uiAMSBestPositionPopup`, `supports_extrusion_cali`) | ~80 | **Not applicable** | Only matters when a non-Bambu printer is shown in the Bambu AMS widgets. |
| 9 | `DeviceManager.cpp/.hpp` (+133): non-Bambu machine objects fed by the agent | ~130 | **Not applicable** | Agent plumbing. |
| 10 | Storage tab UI: `StoragePanel`, `OrcaFilesPanel`, `Monitor.cpp` swap of `MediaFilePanel` | ~400 | **Covered (differently)** | `PrinterStoragePanel` (a wxDataViewListCtrl list, not ImageGrid) hosted by `PrinterWebView` beside the printer page, #548. Bambu keeps its own storage page (`MediaFilePanel`/`PrinterFileSystem`, UltraNet storage module; #443 keeps the camera module apart). We did not replace Bambu's page. |
| 11 | File-grid abstraction `IFileGridModel`, `RemoteFileGridModel`, `BambuFileGridModel`, `ImageGrid` rework | ~900 | **Partly covered** | `IFileGridModel` and `RemoteFileGridModel` ported in #548. `BambuFileGridModel` and the `ImageGrid` rework were deliberately not ported: the Bambu storage page works and touching it risks the UltraNet storage module. Not worth unifying. |
| 12 | File APIs `list_printer_files`, `get_printer_file_thumbnail`, `delete_printer_file`, `get_printer_file_metadata` on the agent, Moonraker `/server/files` backend | ~700 | **Covered** | `PrinterStorage.{hpp,cpp}` (Moonraker backend, tests against a mock Moonraker plus `fake_moonraker.py`), plus start-print from the tab with the usual checks. Backends for **non-Moonraker** printers (Elegoo CC1 SDCP file list, Creality, FlashForge) do not exist; `PrinterStorage.hpp` notes another protocol "would be another implementation". |
| 13 | Tests (`test_orca_printer_agent`, `test_ams_item`, `test_device_manager_integration`, `test_preset_bundle_ams_sync`, multifilament) | ~870 | **Not applicable**, except `test_preset_bundle_ams_sync` | Comes along with piece 4 if that is taken. |

## 3. The owner's picks, mapped

### A. Storage tab: done (#548)

Remaining, all optional extensions and not part of #16156:
- A1. Storage backend for the Elegoo Centauri Carbon (CC1) over SDCP (file list, delete, thumbnail commands). The CC1 is not Moonraker, so the tab does not show for it today. Size M. Risk low (read-only plus delete with confirmation). **Needs the owner's CC1** to confirm command and field names against the SDCP V3 spec.
- A2. Creality (CFS machines): the open Creality PRs (#616, #618, #619) come first; a Storage backend would follow what they establish. Do not start before they merge.
- A3. The same file list in the hub/phone is an app/hub decision, not a desktop port.

### B. Filament mapping sent with prints: mostly done

| Printer | State on main |
|---|---|
| Elegoo CC1 + CANVAS | Read/sync (phase 1), mapping at Upload and Print (phase 2, `ElegooCanvasMapping`, remembered mapping by type/name/colour), read-only card on the Device tab (`ElegooCanvasUI`, phase 3), Bambu-style swatch picker in the send dialog (#625), unattended sends (hub, phone, reprint) with the slot send policy: loaded tray of its own material, busy refusal, tray gone since the dialog read it (#660). Hub CANVAS rows exist (`RemoteControl.cpp` includes `ElegooCanvas.hpp`). |
| QIDI Box | Never uses the printer's stale mapping (#643); slot presence from firmware, inconsistent Box or empty slot refused (#661); swatch picker (#625); emulator gates in open PRs #654/#662. |
| Snapmaker U1 | Tool slots are the mapping (T1..T4); swatch picker (#625); owner prefers a runtime printer query (memory: u1-filament-mapping-decision). |
| Bambu | `BambuSendMapping`, `BambuReprint`, hub explicit `ams_mapping` and send preview (#673, #674), native device view AMS controls (hub PR 153). |
| Happy Hare / AFC (generic Klipper MMU) | **Display only**, open PR #612. No mapping is sent: upstream's own Moonraker agent start is a stub and #16156's serializer targets only Orca's connector protocol. A real implementation means generating `MMU_TTG_MAP` or lane assignments per start, a per-firmware design question, not a port. |
| Creality CFS | Open PRs #616, #619. |
| Anycubic ACE | Open #620, #621 (spike). |
| Elegoo CC2 (Centauri Carbon 2, MQTT) | **Not supported for send at all** (ECC2 profiles exist; only camera relay and remote-control code mention it). Upstream Orca #15464 (CC2 print dialog and mapping, open upstream) is a separate package, not part of #16156. This is the real gap if "variants" means CC2. |

Remaining B items (none is a #16156 port):
- B1. CC2 transport and mapping (MQTT adapter, `slot_map` in config, auto-refill). Size L, risk medium, **needs CC2 hardware** (the design doc scopes phases 1-2 to CC1 only). Park.
- B2. Happy Hare/AFC mapping at start. Size M after #612 merges, risk medium (printer-side macro behaviour), needs a real MMU printer; none among the owner's. Park; merge #612 first.
- B3. Hand-test closure of the CANVAS work with the owner's CC1 + CANVAS (design doc R3: what the printer does with an empty or partial `slot_map`; R1: tray `status` rule). Not coding; decides whether `AS_SLICED_IDENTITY_MAP` flips.

### C. AMS dialog fixes for non-Bambu printers: no target in Edge

Pieces 5, 6, 8, 9 only apply if a non-Bambu printer is rendered by the Bambu AMS widgets. Edge renders them elsewhere (CANVAS card, swatch map, hub rows). Nothing to port. Two Bambu-relevant leftovers are in section 4.

## 4. Recommendation: trimmed scope

**Close 5C-3 as a package.** Mark #16156 as covered / N/A in the plan (the DEF row is resolved by #548 plus the Edge equivalents above). Optional PR-sized items, in priority order:

1. **Merge #612** (Happy Hare/AFC slot reading) after confirming it sits on #625 as its body says. Already written; goes through the normal review. Hardware to verify: a Happy Hare or AFC printer (none needed to merge).
2. **Bambu: third-party tray temps in the AMS Materials Setting dialog** (piece 7; `StatusPanel::on_filament_edit` and `on_ext_spool_edit` set `temp_max/temp_min` for every tray). Size S (about 12 lines), risk low, needs a Bambu AMS printer and a non-BBL spool for the hand test. Optional.
3. **Bambu: printer-set tray resolves to a Generic preset by type and colour** (piece 4, with `test_preset_bundle_ams_sync.cpp`). Size S-M, risk medium (touches `sync_ams_list`, which every Bambu AMS sync goes through; needs the Bambu sync regression pass). Do it only if the owner has seen a printer-set tray vanish or turn into "Generic PLA" in a sync; otherwise drop.
4. **CC1 Storage backend (A1)**. Size M, needs the owner's CC1. Only if the owner wants file management on the CC1.
5. **Parked, hardware-dependent**: CC2 send and mapping (B1), generic MMU mapping at start (B2).

Not worth doing: the agent layer, capability gating, the filament-mapping serializer, K/N hiding, `AMSMaterialsSetting` model resolution, `BambuFileGridModel`/`ImageGrid` rework, non-Bambu `DeviceManager` plumbing, the Orca agent tests.

## 5. Evidence checked

- Upstream commit read from the local object (`git diff 029bc7928f^ 029bc7928f`); PR body via `gh pr view 16156 -R OrcaSlicer/OrcaSlicer`.
- Edge: header comments of `PrinterStorage.hpp` (states this fork has no printer-agent layer), `PrinterStoragePanel.hpp`, `ThirdPartyFilamentMap.hpp`, `ElegooCanvasMapping.hpp`, `PrintHostDeviceStatus.cpp` (`slot_send_policy`), `BambuReprint.hpp`, `RemoteSend.cpp` canvas guard, and `tests/design_elegoo_canvas_sync_device_page.md` (in the main checkout, untracked).
- PR states via gh: #625, #660, #661, #643, #673, #674, #593, #361, #443, #548 merged; #612, #613, #616-#622 open; #598 closed (its phase 3 card code is on main through the stacked branches, `ElegooCanvasUI.cpp`).
- Not run: GUI, builds, printers, AppData.
