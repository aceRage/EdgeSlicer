# Self-hosted update server for EdgeSlicer

EdgeSlicer does not check any update server by default (the upstream Snapmaker
default was removed; see `AppConfig::get_version_upgrade_url`). The client-side
machinery is still fully functional and can be pointed at a server we control
with **one ini key — no rebuild required**.

## Enabling it

Set `orca_upgrade_url` in the app config and restart:

- Windows: `%APPDATA%\EdgeSlicer\EdgeSlicer.ini`
- macOS: `~/Library/Application Support/EdgeSlicer/EdgeSlicer.ini`

```ini
orca_upgrade_url = https://downloads.example.com/edgeslicer/version.json
```

Once set, the app fetches the URL at startup and from **Help → Check for
Updates**, and shows the "New version of EdgeSlicer" dialog (release notes,
Download, Skip this Version) when the response reports a newer version.
Removing the key (or emptying it) disables the check again.

## The response

A **static file is enough** — any endpoint that returns HTTP 200 with this
JSON works (no signing, no auth). See `version.example.json` next to this
file for a filled template. Field reference, from the parser in
`GUI_App::check_new_version_sf()`:

| Field | Meaning / requirement |
|---|---|
| `code` | Must be `200`, top level, or the response is ignored. |
| `data.version` | Semver-ish (`1.2`, `1.2.3`, optional `-tag`). Must be **greater than the app's own build version** or the client reports "no new version". |
| `data.release_type` | Must be exactly `"stable"` or the release is ignored (used for filtering upstream's beta channel). |
| `data.platform_type` | `"win"` or `"mac"` per file. **Linux is refused client-side.** |
| `data.is_force_upgrade` | `false` normally. `true` shows the mandatory-upgrade dialog that closes the app and only offers to open the download URL — use only for "must not run this build" situations. |
| `data.is_full_upgrade` | Upstream's "full vs incremental" flag; kept for schema compatibility, not acted on. |
| `data.full.file_describe` | Release-notes text, shown verbatim in the dialog. Plain text with newlines. |
| `data.full.default.file_url` | Download link handed to the system browser when **Download** is clicked (Windows / fallback). |
| `data.full.arm.file_url` / `data.full.intel.file_url` | macOS Apple Silicon / Intel links; the client picks by build, falling back to `default`. |
| `file_size`, `file_md5`, `file_sha256`, `reserved_1`, `reserved_2` | Parsed but unused by the dialog; keep them empty unless a future downloader consumes them. |

## Gotchas

- **Version comparison is against the build's version constant** (`Snapmaker_VERSION`), not against previously seen server versions. Bump the build's version when tagging releases, or a "new" release with the same/lower version never notifies.
- **Skip this Version** persists the server version in the ini (`skip_version`); any server version `<=` that value is silently ignored until the key is cleared. Test with a fresh ini or delete the key.
- The URL must be reachable **without TLS errors**; the client has no retry or fallback host.
- If you serve per-platform files, the client only reads the block matching its `platform_type` — one `version.json` per platform URL is the simplest layout (`.../win/version.json`, `.../mac/version.json`).

## Testing

1. Host `version.example.json` (edited) at the chosen URL.
2. Set `orca_upgrade_url`, delete `skip_version` from the ini, restart.
3. The dialog should appear at startup; **Download** should open the URL in the default browser.
4. Help → Check for Updates re-triggers it on demand (passes `by_user`, which also surfaces server errors instead of staying silent).
