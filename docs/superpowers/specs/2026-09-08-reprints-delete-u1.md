# Reprints: delete, U1 records, and swipe-to-delete

Branch `fix/reprints-delete-u1`, from `feat/ultra-preferences` at `da184c2e01`.

Three complaints about the phone hub's **Reprints** tab (the G-code archive), one fix each.

## 1. Delete did nothing, and said "no record exists"

### Root cause

`GcodeArchive::list()` and `GcodeArchive::find()` disagreed about what a record's id is.

* `list()` (`src/slic3r/GUI/GcodeArchive.cpp`) builds each row with `record_from_sidecar()`, which
  takes `r.id` from the **`"id"` field inside the sidecar JSON**. That id is what `GET /api/archive`
  hands the phone, and therefore what the phone sends back to delete.
* `find()` resolved a record by **file stem** - `dir()/<id>.json` - and then additionally insisted
  `r.id == id`, returning an empty record on any mismatch:

  ```cpp
  const fs::path sidecar = fs::path(dir()) / (id + ".json");
  if (!fs::is_regular_file(sidecar, ec)) return out;
  Record r = record_from_sidecar(sidecar);
  if (r.id != id) return Record();      // <- the row the tab is showing, refused
  ```

For every record the module writes itself the stem and the field are equal, which is why the gates
passed and why this never showed up in testing. They come apart as soon as the folder is not the one
that wrote it: a sidecar restored from a backup under a different name, an archive folder copied
from another PC, or a file a person renamed by hand. The row still lists (list() reads the field),
but `find()` - and so `remove()`, which calls it - answers "no such record".

`RemoteAccess::api_archive_delete()` then returned `404 {"error":"no such record"}`, which is the
"no record exists" the user saw.

The list never refreshing was a second, independent defect in the page: `rpDelete()` in
`resources/web/orca/stream_center.html` called `rpClose()` **before** `rpJson = ''`, and `rpClose()`
sets `rpState = null`. On the failure path the `.catch` guard `if (rpState === s)` was then false,
so the error was dropped on the floor; on the success path the refresh did run, but any failure left
the tab looking untouched with no message at all.

### The fix

* `find()` keeps the fast path (`<id>.json` whose field agrees) and otherwise falls back to the
  record in `list()` that actually claims that id. The id is still checked against `sanitize()`
  first, so nothing new reaches the file system.
* A new `sidecar_path_of()` finds the sidecar a record was really read from, and `remove()` deletes
  by the record's own resolved `path` / `thumbnail_path` rather than by re-deriving `<id>.<ext>`.
* `api_archive_delete()` distinguishes the two failures: `404` "no such record - it may already have
  been deleted" when the record is genuinely unknown, `500` with the real reason when the record is
  found but its files will not go (read-only folder, file open elsewhere).
* `rpDelete()` is split into `rpDelete()` (sheet) and a shared `rpDeleteId()`, which clears `rpJson`
  and calls `refreshReprints()` on **both** paths, then reports. A failed delete now leaves the row
  on screen with the PC's own message.

## 2. U1 jobs were never recorded

### Root cause

The desktop U1 send goes `Plater::send_gcode_to_printhost` -> `PrintHostSendDialog` ->
`WebPreprintDialog` (the bundled Flutter page) -> SSWCP. The only archive hook on that path was in
`SSWCP_MachineOption_Instance::sw_FinishPreprint()` (`src/slic3r/GUI/SSWCP.cpp`), which runs only
when the pre-print page decides it is finished and calls `sw_FinishPreprint` itself.

`sw_MachinePrintStart` - the call that actually starts the print - had no hook. So a print that
started but whose page was dismissed, or that ended on the printer's own device screen, was never
archived. Bambu and print-host sends were unaffected because they archive on the host queue's path
(`RemoteSend.cpp`), which is also why the phone's own LAN send already recorded correctly.

The user's session log (`2026-09-08-10-39-21.log.0`) is consistent with this and shows nothing
further: the log level is warning+, the archive logs at info, and the page's own `[WCP]` lines stop
at 10:52:03 in device-connection setup - so the log neither confirms nor refutes that
`sw_FinishPreprint` ran. The missing hook at print start is the defect that is provable from the
code, and is fixed regardless.

### The fix

`SSWCP::archive_print_once(mode)` holds the archive call and a one-shot guard keyed on the active
file. Both hooks call it:

* `sw_MachinePrintStart` - every started print, including one whose page is then dismissed;
* `sw_FinishPreprint` - the upload-that-never-prints case, which reaches no other hook.

Whichever runs first stores the record; the other is a no-op. `SSWCP::clear_archived_print()` closes
the send in `sw_FinishPreprint` so the next send can store the same path again. If archiving fails
the guard is released so the other hook may retry. Archiving still cannot fail a send: every
precondition simply returns, and `GcodeArchive::archive()` swallows its own errors.

The phone LAN path (`RemoteSend::archive_sent`) already archived and is unchanged.

## 3. Swipe-left to delete

Touch only, in `stream_center.html`. Each row is wrapped by `rpSwipeWrap()` in a `.rpsw` clipping
frame over a red `.del` panel; the row follows the finger and settles open past a third of the
panel's 96px. A vertical drag is handed back to the list (the swipe gives up as soon as `|dy| >
|dx|`), only one row is open at a time, an open row's tap closes it instead of opening the sheet,
and the action confirms before calling the same `rpDeleteId()` the sheet uses. `'ontouchstart' in
window` gates the whole thing, so desktop keeps the plain row and the sheet's Delete button.

## Gates

`test_gcode_archive2.py` section H gains: a missing id returns a clear message as well as 404; a
sidecar deliberately renamed out from under its id is still listed, still found, and now deletes
(the regression above); and after a delete the list the tab reads no longer carries the record. The
Snapmaker-U1-through-mock-printhost send that produces an archive record was already covered in
section D and is what proves (2) at the API level.
