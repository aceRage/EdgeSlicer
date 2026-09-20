# Printer error-code descriptions (HMS overrides)

When a Bambu printer reports a problem it sends a numeric code, not a sentence. The sentence comes
from lookup tables published by Bambu, which EdgeSlicer ships a snapshot of under `resources/hms/`
and refreshes from `e.bambulab.com` when it is allowed to reach the network.

Sometimes there is no sentence to be had:

* Bambu publishes an **empty description** for the code. This is not rare and not a bug in the
  download - `0C00010000020015` ("Nozzle Camera is malfunctioning", as Bambu Studio shows it) has
  an empty entry in every shipped language *and* in the live cloud table.
* The code is newer than the snapshot and the machine is offline, or stealth mode is on, so the
  refresh never runs.

In both cases the user would otherwise see a bare `0C00010000020015`. The override layer is how a
description gets attached to a code once somebody has seen what it means.

## Where a description comes from

The first of these that has an entry wins:

| Order | Source | Path |
|---|---|---|
| 1 | This user's own captures | `<datadir>/hms/overrides.json` |
| 2 | Shipped with EdgeSlicer | `resources/hms/edgeslicer_overrides.json` |
| 3 | Bambu's tables | `<datadir>/hms/` then `resources/hms/hms_<lang>_<model>.json` |
| 4 | Fallback | `Printer error 0C00 0100 0002 0015 - see the printer screen or Bambu's error-code page` |

Within a file: the exact language, then `en`, then any language the entry was written in.

`<datadir>` is `%APPDATA%\EdgeSlicer` on Windows, `~/Library/Application Support/EdgeSlicer` on
macOS and `~/.config/EdgeSlicer` on Linux.

Every user-facing surface resolves through the same helper (`HMSQuery::describe_error`), so the
device tab, the hub's event stream and web page, the phone's push notifications and `--hms-lookup`
always agree.

## Adding a description

### From the command line

```
Snapmaker_Orca.exe --hms-add 0C00010000020015 "Nozzle Camera is malfunctioning. If this issue occurs multiple times during printing, please contact customer support."
```

Options:

* `--hms-add-lang de` - the language you wrote it in (default `en`).
* `--hms-add-model 31B` - only for that printer series, the first three characters of the serial
  (`31B` is the H2C, `094` the H2D, `20P` the P1 line). The default `*` means every printer.
* `--hms-add-force` - replace a description already recorded for the same code, language and model
  instead of refusing.

Check what any code now resolves to, with no printer attached:

```
Snapmaker_Orca.exe --hms-lookup 31BA0123456789:0C00010000020015:en
```

`HMS_TEXT` is what Bambu's table says (empty if nothing), `HMS_OVERRIDE` is your recorded
description, and `HMS_DESCRIPTION` is what the user is actually shown.

### From the hub

`POST /hub/hms/override` on the loopback admin listener, with the per-run `X-Hub-Secret`:

```json
{ "code": "0C00010000020015",
  "text": "Nozzle Camera is malfunctioning.",
  "lang": "en",
  "model": "31B",
  "note": "seen on the printer screen at the same time",
  "force": false }
```

It answers `{"ok":true,"code":…,"description":…,"file":…}` - `description` is what the surfaces
will show, so the caller can confirm the capture landed. `POST /hub/hms/reload` re-reads the
overlay files after an edit made outside the app; the lookup also notices a changed file by itself,
so a running slicer picks up a new description without a restart.

### By hand

`<datadir>/hms/overrides.json`:

```json
{
  "version": 1,
  "overrides": [
    {
      "code": "0C00010000020015",
      "model": "*",
      "lang": "en",
      "text": "Nozzle Camera is malfunctioning. If this issue occurs multiple times during printing, please contact customer support.",
      "source": "Bambu Studio UI",
      "date": "2026-09-19",
      "note": "Bambu publishes this text under 0C00010000020014 and an empty entry for ...0015"
    }
  ]
}
```

| Field | Required | Meaning |
|---|---|---|
| `code` | yes | 8 hex digits (a print error) or 16 (an HMS code). Spaces and case are ignored, so you can paste `0C00 0100 0002 0015` straight from the UI. |
| `text` | yes | What the user is shown. |
| `model` | no | Printer series, or `*` / omitted for every printer. |
| `lang` | no | Language code as the tables spell it (`en`, `de`, `zh-cn`). Default `en`. |
| `source` | no | Where the text came from, so a later reader can judge it. |
| `date` | no | When it was captured, ISO 8601. |
| `note` | no | Anything uncertain about the entry. Recorded, never shown. |

Use `note` honestly. The seeded entry for `0C00010000020015` records that Bambu publishes that
sentence under a *different* code (`...0014`) and that the mapping is inferred from what Bambu
Studio displayed, not from anything Bambu documents. Showing the sentence is much more useful than
showing the bare code, but the next person to read the file should know how sure we are.

## Contributing a description

Descriptions that are likely to help everybody belong in the shipped overlay,
`resources/hms/edgeslicer_overrides.json`. Copy the entry out of your own `overrides.json`, keep
the `source` and `note` fields, and open a PR. Please say in the PR how you captured it - the
printer's own screen, Bambu Studio, or Bambu support.

## Refreshing Bambu's own tables

Separate from overrides, the bundled snapshot of Bambu's tables can be brought up to date:

```
python scripts/hms/refresh_hms_snapshot.py --dry-run   # report what would change
python scripts/hms/refresh_hms_snapshot.py            # merge, all languages and models
```

It **merges**: an entry only in the snapshot is kept, an entry only on the server is added, and a
non-empty server description replaces an empty local one. It never deletes a code and never
replaces a real description with an empty one - Bambu's cloud `device_hms` table is currently
*smaller* than the shipped file, so a plain overwrite would lose thousands of codes that still
resolve today. Re-run it before a release and review the diff per file.
