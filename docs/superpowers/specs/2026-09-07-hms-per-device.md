# Per-device HMS text

2026-09-07, branch `feat/hms-per-device` off `feat/ultra-preferences` (526ed0afdc).

On 2026-09-06 the user's H2C (serial `31B...`, model `O1C2`) stopped a print and reported HMS
`05004046`. The hub notification, and the Devices card behind it, said

> 3DPO reported error 05004046

and nothing else. The printer's own sentence for that code is

> The print has stopped because the 3MF file is invalid. Please verify that the correct printer
> model was selected during slicing, or update Studio and re-slice the file.

This note records why the fork could not produce it, what upstream does instead, and what was
ported.

## Where the text comes from

An HMS table is a JSON document with two dictionaries, `device_hms` (16-hex codes, the ones the
printer lists on its own screen) and `device_error` (8-hex print errors, the ones that stop a
print), each keyed by language and holding `{"ecode", "intro"}` rows. `05004046` is a print error,
so it lives in `device_error`.

Upstream ships one table **per device series and language**. The series is the first three
characters of the serial number - `31B` is the H2C, `094` the H2D, `20P` the P1 line - and the same
code says different things on different machines. `05004046` is present in `hms_en_20P.json` and
`hms_en_31B.json` and in none of the other five sets, which is the whole point: a single shared
table cannot answer it.

### Upstream's resolution, step by step

All references are `C:\Dev\BambuStudio` at `66e405477`.

| step | upstream |
|---|---|
| series from the printer | `HMSQuery::get_dev_id_type` - `obj->get_dev_id().substr(0, 3)` (`src/slic3r/GUI/HMS.cpp:312`) |
| language | `HMSQuery::hms_language_code` (`HMS.cpp:248`), app language with `uk/cs/ru/tr/pt/ko` folded to `en` |
| file name | `HMSQuery::get_hms_file` - `hms_<lang>_<series>.json`, `hms_action_<series>.json` (`HMS.cpp:281`) |
| load | `HMSQuery::load_from_local` reads `<datadir>/hms/<name>` and takes `j["data"]` (`HMS.cpp:181`) |
| resources fallback | `HMSQuery::init_hms_info` - when the loaded version is empty or `0`, `copy_from_data_dir_to_local()` copies `<resources>/hms` over `<datadir>/hms` and retries (`HMS.cpp:589`, the STUDIO-9512 fix); only for the seven series in `package_dev_id_types` (`HMS.cpp:13`) |
| cloud refresh | `HMSQuery::download_hms_related` - `query.php?lang=<lang>&v=<local ver>&d=<series>` and `hms/GetActionImage.php?v=&d=` (`HMS.cpp:54`), at most once a day per series, skipped when the remote `ver` is not newer |
| version file | there is no separate version file: the version travels **inside** the table, as `ver` in the shipped/downloaded envelope or `version` in what was saved (`HMS.cpp:205-210`), and is echoed back as the `v=` query parameter |
| lookup | `_query_hms_msg` / `_query_error_msg` walk `device_hms[lang]` / `device_error[lang]` for an `ecode` match (`HMS.cpp:322`, `HMS.cpp:426`) |
| cached per series | `m_hms_info_jsons` / `m_hms_action_jsons`, keyed by series (`HMS.hpp:29-30`), dropped by `clear_hms_info()` on a language change (`GUI_App.cpp:4354`) |

## Where the fork diverged

The fork is a snapshot of the code from *before* the tables were split per device, so the device
never enters the lookup at all. Line numbers are `C:\Dev\SnapmakerOrcaPhone` at `526ed0afdc`.

1. **No device in the file name.** `HMSQuery::get_hms_file(hms_type, lang)`
   (`src/slic3r/GUI/HMS.cpp:181`) returns `hms_<lang>.json` / `hms_action.json`. There is no
   parameter for the series and no caller that could pass one.
2. **One table, not a table per series.** `json m_hms_info_json` / `m_hms_action_json`
   (`src/slic3r/GUI/HMS.hpp:24-25`) are single documents.
3. **Data dir only.** `load_from_local` (`HMS.cpp:104`) reads `<datadir>/hms/<name>` and nothing
   else. There is no resources fallback, and `resources/hms/` did not exist in the fork at all - so
   even shipping upstream's files would have changed nothing.
4. **No `d=` on the download.** `download_hms_related` (`HMS.cpp:44`) asks
   `query.php?lang=<lang>&v=<ver>`, so what comes back is the legacy shared table.
5. **One download at startup.** `check_hms_info()` (`HMS.cpp:349`) fires once from
   `GUI_App::on_user_login`-time (`GUI_App.cpp:1262`) and once on a language change
   (`GUI_App.cpp:4127`). With no printer connected there is no series to ask for, which is exactly
   why upstream deleted it.

The three places the fork turns a code into text, all through the same two entry points:

* **(a) the device page** - `StatusPanel::update_error_message` calls
  `query_print_error_msg(print_error, error_msg)` and `query_print_error_url_action(print_error,
  obj->dev_id, used_button)` (`src/slic3r/GUI/StatusPanel.cpp:2277,2279`). Note the second one
  already had the serial: the action/image table was the only per-device thing in the fork, and
  even there the *text* lookup next to it had none. The HMS list on the same page goes through
  `HMSPanel::append_hms_panel` -> `query_hms_msg(long_error_code)`
  (`src/slic3r/GUI/HMSPanel.cpp:43,190`).
* **(b) the hub event text** - `RemoteEvents::snapshot_bambu` fills `PrinterState::error_text`
  either from `print_error_message(m->print_error)` (`src/slic3r/GUI/RemoteEvents.cpp:283,319`) or,
  when there is no print error, from `query_hms_msg(p.error_code)`
  (`RemoteEvents.cpp:326`). `/api/printers` does the same in `describe_bambu`
  (`src/slic3r/GUI/RemoteControl.cpp:66,481,492`), and `RemoteSend` after a send
  (`src/slic3r/GUI/RemoteSend.cpp:1017`).
* **(c) notifications** - there is no separate lookup. `RemoteEvents::step` builds the sentence
  from `PrinterState::error_text` alone: `cur.error_text.empty() ? name + " reported error " +
  cur.error_code + "." : name + ": " + cur.error_text` (`RemoteEvents.cpp:129-134`). The user's
  "3DPO reported error 05004046" is that first branch, verbatim. Web Push and APNs carry the same
  `Event::body`, so fixing `error_text` fixes every one of them at once.

So the fork had no per-device table, no way to name one, no file to read, and a notification text
whose fallback branch is precisely what the user saw.

## The port

### Files

`resources/hms/` now carries upstream's 111 tables, byte-identical (`git show --stat` and a
`filecmp` pass against `C:\Dev\BambuStudio\resources\hms`): 15 languages x 7 series
(`093 094 20P 22E 239 26A 31B`) plus the seven `hms_action_<series>.json`. They are CRLF on disk,
as upstream has them, and `.gitattributes`'s `* text=auto` with `core.autocrlf=true` checks them
back out that way.

`resources/hms/local_image/` was **not** copied. Those 28 MB of PNGs are consumed by upstream's
`HMSQuery::query_image_from_local`, which this fork does not have - it passes the `image` field of
the action table straight to the error dialog as a URL. Porting the image path is a separate job.

### Code

`src/slic3r/GUI/HMS.{hpp,cpp}` is rewritten along upstream's lines: tables keyed by series
(`m_hms_info_jsons`, `m_hms_action_jsons`), `get_hms_file(hms_type, lang, dev_id_type)`,
`get_dev_id_type`, `init_hms_info`, `clear_hms_info`, `_query_hms_msg` / `_query_error_msg` bodies
taken from upstream unchanged. `download_hms_related` is upstream's, including the `d=` parameter,
the `remote_ver <= local_version` check and saving the server's whole envelope.

Every caller now passes the serial. `HMSPanel` gained upstream's `dev_id` member and parameters
(`HMSNotifyItem(dev_id, parent, item)`, `append_hms_panel(dev_id, item)`), matching
`BambuStudio/src/slic3r/GUI/HMSPanel.cpp:19,188`.

### Where this deviates from upstream, and why

1. **No 124 MB copy into the data dir.** Upstream's `copy_from_data_dir_to_local()` copies the
   whole `resources/hms` tree into `<datadir>/hms` the first time a series has no local table.
   That tree is 124 MB of JSON here; duplicating it into everyone's `%APPDATA%` to answer one
   error code is not worth it. Instead `load_from_local` looks in `<datadir>/hms` first and reads
   `<resources>/hms` in place if the data dir has nothing (`HMS.cpp`, `hms_file_candidates`). A
   cloud refresh still writes to `<datadir>/hms`, so a downloaded table shadows the shipped one
   exactly as upstream's copy does.
2. **The legacy table is still read.** Upstream only ever looks for `hms_<lang>_<series>.json`. An
   X1C (`00M...`, `BL-P001`) or a P1 has no packaged table here, and this fork's users have a
   `<datadir>/hms/hms_<lang>.json` that its old downloader wrote. `hms_file_candidates` therefore
   ends with the name that has no series in it, and `load_from_local` accepts both shapes: the
   server's `{"result","ver","data"}` envelope, and the older files where the payload sits at the
   top level. Nothing that had text before loses it.
3. **The cloud refresh is detached.** Upstream downloads inline, holding `m_hms_mutex`, from
   whatever thread asked. Here the asking thread is often the GUI one - the event watcher resolves
   the text on its one-second heartbeat - and two `perform_sync` calls of up to 20 s each would
   freeze the window and trip the hub's GUI watchdog. `refresh_from_cloud` spawns a detached
   thread, which swaps the result in under the lock; `m_cloud_hms_refreshing` keeps one thread per
   series. The cost is that a code that is in neither the shipped table nor the data dir stays
   unknown until the next query.
4. **`query_print_error_msg` keeps its `bool` return.** Upstream returns a `wxString` and added
   `is_internal_error` to tell "found but deliberately blank" from "not found". The fork's callers
   (`StatusPanel`'s `is_errocode_exist`, and the three `q && q->query_print_error_msg(...)` in the
   hub code) want the bool, so it stays, defined as "the text is not empty" - which folds the
   blank-intro case in with not-found, the same way upstream's `is_internal_error` suppresses it.
5. **`check_hms_info()` is gone**, as upstream deleted it. The startup call
   (`GUI_App.cpp:1260`) had no series to ask about; the language-change call
   (`GUI_App.cpp:4126`) becomes `clear_hms_info()`, upstream's `GUI_App.cpp:4354`.
6. **`hms_language_code()` tolerates no app.** One `wxApp::GetInstance()` check, so the unit test
   can call into the class without a `GUI_App`. Upstream's `uk/cs/ru` fold is left as the fork had
   it (upstream has since added `tr/pt/ko`); those languages have their own tables here, so
   folding them to English would be a regression.
7. **`DevJsonValParser::get_longlong_val` is inlined** as `hms_ver_string` - the fork has no
   `DeviceCore/`, and the body is four lines.

### The debug routes

`RemoteEvents::state_of_json` now fills `error_text` from the HMS tables when a replayed snapshot
gives an `error_code` and no text, exactly as `snapshot_bambu` fills it: the snapshot's `id` is the
serial and its first three characters pick the table. That makes `POST /api/debug/events` (with
`SNORCA_DEBUG_ROUTES=1`) a check on the lookup and not only on the transition rule.

`POST /api/debug/hms?dev=<serial>&code=<hex>[&lang=<code>]` (same gate) answers with everything
that decides the lookup: `dev_id_type`, `file`, `lang`, `app_lang`, `data_dir`, `resources_dir`,
`has_query`, and both a `local_*` answer (a private `HMSQuery` reading only the local tables) and a
`live_*` one (the app's own `HMSQuery`, cloud refresh and all). The two agree unless the app has no
`HMSQuery` at all or is set to another language, which is exactly what it is for.

## Proof

### Unit level: `--hms-lookup`

`--hms-lookup <serial>:<code>[:<lang>]` resolves one code against the local tables and exits - no
window, no printer, no network (`src/Snapmaker_Orca.cpp`, next to the existing
`--migrate-datadir-test` hook; the option is defined in `CLIMiscConfigDef`,
`src/libslic3r/PrintConfig.cpp`). It calls `query_print_error_msg_local` / `query_hms_msg_local`,
which are the live lookup with the language passed in and the cloud refresh not reachable, and it
prints `HMS_DEV_ID_TYPE`, `HMS_FILE`, `HMS_CODE`, `HMS_LANG`, `HMS_FOUND` and `HMS_TEXT`.

    31BA0123456789:05004046:en  -> HMS_FOUND=1  the H2C's sentence, out of resources/hms with an
                                   empty data dir (the fallback the fork could not use before)
    094A0123456789:05004046:en  -> HMS_FOUND=0  the H2D table has no such code; this is the whole
                                   reason the tables are split per device
    20PA0123456789:05004046:en  -> HMS_FOUND=1  the P1 table has it too, and says the same
    31BA0123456789:05004046:de  -> HMS_FOUND=1  "Der Druckvorgang wurde unterbrochen ..."
    00M09A0123456789:18048012:en-> HMS_FOUND=1  through the legacy <datadir>/hms/hms_en.json this
                                   fork's own downloader wrote, so an X1C keeps its text

`HMS_FILE` names the table the series and language *prefer*; when only the legacy file exists it is
that one that answered (the 00M line above prints `hms_en_00M.json`, which is not on disk).

**Why not a Catch2 case.** `tests/slic3rutils` is the only test target that links `libslic3r_gui`,
and it does not link. Referencing anything in `HMS.obj` drags `GUI_App.obj` in with it, and that
needs symbols only the application target provides - `common::get_flutter_version`,
`set_privacy_policy`, `Slic3r::sentryReportLog`, `CBaseException::set_log_folder`. The target has
never produced a binary in this fork (the main tree's build has no `slic3rutils_tests.exe` either),
so a case there would have meant fixing an unrelated link problem first. The CLI hook is the same
lookup with none of that.

### Through the hub

With `SNORCA_DEBUG_ROUTES=1` and a scratch data dir, `POST /api/debug/events` with two snapshots -
a clean one to seed, then the same printer with `"id": "31B..."` and `"error_code": "05004046"` -
returns an event whose body is the printer's sentence rather than the bare code. The script is
`snorca_hubtest/test_hms_events.py`; no printer is involved at any point.

`libslic3r_tests` is untouched by this branch and still passes: 609 cases, 607 passed and the
same 2 "failed as expected" as before, exit 0.

## Two things this does not fix

Both were found while proving the above, and neither is caused by the port.

1. **No `HMSQuery` at all in stealth mode.** `GUI_App::post_init` only builds it when stealth mode
   is off (`GUI_App.cpp:1182`), and `AppConfig::get_stealth_mode()` returns true unconditionally
   until the setup wizard has been finished (`AppConfig.cpp:101-106`). A profile in that state gets
   the bare code for every printer, per-device tables or not - the first run of the gate showed
   exactly the old sentence for this reason, because the test data dirs carry no `firstguide`
   section. `test_hms_events.py` now marks the wizard finished in the copy it works on.
2. **The API answers before the tables exist.** `RemoteAccess` serves `/api/*` well before
   `post_init` has run, so an error that arrives in the first seconds of a session still resolves
   to nothing. The gate waits for `/api/debug/hms` to report `has_query`. Making the watcher wait
   for the same thing, or building the `HMSQuery` earlier, would be a separate change.
