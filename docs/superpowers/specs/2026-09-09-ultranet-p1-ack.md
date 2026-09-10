# P1-series LAN send: "uploaded but the printer did not start" (-4030)

Status: fixed, awaiting hardware confirmation on a P1S.
Branch: `fix/ultranet-p1-ack`.

This describes protocol behaviour and the shape of the fix. The network plug-in
itself is a separate private component and none of its source appears here.

## Symptom

On a fresh install with a Bambu P1S on firmware 1.07.x in LAN mode (reported after
the firewall fix landed), sending a plate uploads the file and then fails:

> -4030 — "printer did not acknowledge the start command"
> ("The file was uploaded but the printer did not start the job")

It was not known whether the printer nevertheless started. X1C / H2D / H2C sends
from the same build were unaffected.

## Cause

Two independent defects, both specific to the P1/A1 branch of the range. Either one
alone produces a failed send; together they explain both the failure and the doubt
about whether the job actually started.

### 1. The file was uploaded to the wrong directory (the real root cause)

Bambu printers do not all keep uploaded jobs in the same place, and the slicer
already knows this. Each printer profile may declare an `ftp_folder`:

| Model | `dev_type` | `ftp_folder` |
|---|---|---|
| P1P, P1S | C11, C12 | `sdcard/` |
| A1, A1 mini | N1, N2S | `sdcard/` |
| X1, X1C, X1E | BL-P001, BL-P002, C13 | *(absent → FTP root)* |
| H2 series | O1D, … | *(absent → FTP root)* |

The host reads that key (`DeviceManager::get_ftp_folder`) and hands it to the
plug-in as `PrintParams::ftp_folder`. The plug-in **ignored the field entirely**:
it always uploaded to the FTP root and always built `url: "ftp:///<name>.3mf"`.

For X1 and H2, whose `ftp_folder` is empty, root *is* correct — which is why every
machine the author owns worked and the bug went unseen. For a P1 the file landed in
the FTP root while the printer was told to fetch it from a path it resolves under
`sdcard/`. A real captured P1 `project_file` uses `file:///sdcard/cache/<name>.3mf`,
confirming the P1 fetches from `sdcard/`.

`C13` is the **X1E**, not a P1; the P1 series is C11/C12 only.

### 2. The success test required a PUBACK the P1 may never send

The plug-in published `project_file` at QoS 1 and treated the **broker's PUBACK** as
the definition of success, with a 5 s budget. That conflates two different things:

* a PUBACK says the printer's *MQTT broker* accepted the message;
* the printer's own report topic says the *job* was accepted.

Whether a P1's embedded broker PUBACKs a QoS-1 publish is **not documented by any
public source** — no packet capture or firmware teardown covers it. What is known is
that Bambu Studio sends `project_file` itself at **QoS 0**, and that every open-source
client (pybambu, bambu-connect) publishes at QoS 0 with no delivery wait, relying
instead on the printer's report. There are also multiple reports of a P1S accepting
and printing a job while answering nothing at all on the report topic.

So the old test could fail a send that had plainly worked — which is exactly the
reported symptom, and why it was unclear whether the print had started.

## Protocol facts used

Sourced only from public documentation, open-source clients, and this fork's own
AGPL tree. Each is logged with its licence in the plug-in's `PROVENANCE.md`.

* `param` is `"Metadata/plate_<n>.gcode"`; the extension is required — it is the
  path inside the 3MF archive, not a chosen filename.
* `project_id` / `profile_id` / `task_id` / `subtask_id` are the **strings** `"0"`
  for a local print. `md5` is optional (pybambu omits it and prints succeed).
* The ack, when it comes, is
  `{"print":{"command":"project_file","result":"success"|"fail","reason":…,"sequence_id":…}}`
  on `device/<sn>/report`, echoing the request's `sequence_id`. `sequence_id` is
  echoed as a quoted string by some firmware and as a bare int by others.
* Result casing is inconsistent in the wild: this fork's own report handler compares
  against uppercase `"FAIL"` while sibling commands compare lowercase `"fail"`.
* Older firmware may send **no ack at all** and only move `gcode_state` to
  `PREPARE` / `RUNNING`.
* `nozzles_info`, and `nozzleId` inside `ams_mapping_info`, are two-nozzle (H2)
  concepts: the host emits `nozzles_info` only when `nozzle_diameter` has exactly
  two entries.
* Known P1 broker limitation (fw ≥ 01.04): effectively one local MQTT client — only
  the last to connect receives reports. Current firmware can also disable local MQTT
  *control* while the printer is logged into Bambu Cloud; LAN-only plus Developer
  Mode restores it. Both are worth checking if a P1 send still misbehaves.

## Fix

1. **Honour `ftp_folder`.** Normalise it to `""` or `dir/` and use it for both the
   FTPS upload target and the `url` in `project_file`, so the two always name the
   same path. X1/H2 profiles declare no folder, so their upload path and URL are
   byte-for-byte what they were.
2. **Accept either kind of evidence.** A start succeeds on the broker PUBACK *or* on
   the printer's own report — a `project_file` echo, or `gcode_state` moving to a
   printing state. An explicit `result: fail` (either casing) still fails, now with
   the printer's own `reason` shown to the user.
3. **Timeout 5 s → 30 s**, and only for the report wait. X1/H2 PUBACK promptly and
   never enter that wait, so their sends are exactly as fast as before.
4. **Omit H2-only fields for single-nozzle machines.** `nozzleId` is stripped from
   `ams_mapping_info` when the machine has no second nozzle. `nozzles_info` and the
   tri-state calibration ints were already correctly gated.
5. **Say what was actually observed.** The -4030 text now distinguishes "uploaded,
   command sent, but no acknowledgement and no job seen in 30 s — check the printer
   screen before sending again" from a dropped connection and from an explicit
   rejection, instead of the bare "did not acknowledge".

Because public sources do not settle whether the P1 PUBACKs, or whether its parser
rejects unknown keys, the fix deliberately depends on neither: the PUBACK is
optional evidence, and H2-era keys are omitted rather than assumed harmless.

## Verification (no printer involved)

Against a loopback fake printer on `127.0.0.1` that speaks MQTTS 8883 and FTPS 990,
plus a pure-function unit check of the ack matcher.

* **H2C/H2D unchanged — the release gate.** The dual-nozzle `project_file` payload is
  **byte-identical** before and after, as are the single-nozzle payloads that carry
  no `ftp_folder` and no `nozzleId`. Diffed field by field from the fake printer's
  own recording of what was published.
* **The bug reproduced and fixed.** Against a fake printer modelling the P1 case
  (never PUBACKs a QoS-1 publish, but does push `gcode_state: RUNNING` after a
  `project_file` — i.e. the job really did start), the previous build returns
  **-4030 on every job** and the fixed build returns **success on every job**.
* **Upload path.** With `ftp_folder = "sdcard/"` the previous build wrote
  `p1s sdcard.3mf` to the root while telling the printer `ftp:///p1s sdcard.3mf`;
  the fixed build writes `sdcard/p1s sdcard.3mf` and sends
  `ftp:///sdcard/p1s sdcard.3mf` — upload and URL agree.
* **Ack matcher, 24 cases, all passing.** Accepts (i) a `project_file` success echo
  with `sequence_id` as string or int, and a bare command mirror; (ii) a
  `gcode_state` `RUNNING`/`PREPARE` transition with no echo at all; rejects (iii) a
  failure echo in either casing, surfacing its `reason`. Correctly ignores idle /
  finished / failed states, other commands' acks, and an echo for a different
  `sequence_id`.
* **Existing session gate** (`normal`, `noack`, `drop`) still passes, including
  wrong-device publish refusal and reconnect after the FTPS transfer displaces the
  MQTT session.

## Owner test

Install the built plug-in into `%APPDATA%\EdgeSlicer\plugins`, send a plate to the
P1S, and expect the job to start with no -4030. If it still fails, the message now
says which stage was reached; check that the printer is in LAN-only mode and that no
other MQTT client (Bambu Studio, Home Assistant, a relay) holds its single local
connection.
