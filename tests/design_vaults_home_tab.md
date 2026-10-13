# Design: Vaults on the Home tab

Status: design, plus phase 1 built on `feat/vaults-home-tab` (see section 11). Date 2026-10-13. Code references are to `origin/main` 83267707ad.
Builds on: `tests/design_vendor_templates_webapp_tabs.md` (2026-10-10, "the vendor study"), `tests/design_home_hub_browser.md`, `tests/printhq_vault_integration.md`, `tests/printhq_integration_requirements.md`, `tests/plan_models_browser_macos.md`.

---

## 1. Summary and recommendation

A **vault** is a place where the user's own models are kept, outside EdgeSlicer: a store's model library, a cloud bucket, a NAS, or another app's library. The Home tab gets one **Vaults** section that lists them all. Each one shows its sign-in state, lets the user browse its files and **Open / Download & Open / Download** them, and remembers what was opened recently.

The shape follows the owner's model: **a few built-in vault connectors** (curated templates with known endpoints, auth, icon and capabilities: CPL3D, Synology, PrintHQ Vault), plus **one "Custom" vault** with a form for everything else. Built-ins and Custom share **one schema**: the existing vendor-connector template (`edgeslicer-vendor-connector`, version 1, `src/slic3r/Utils/VendorConnector.hpp:35`), extended to version 2 by a `kind` field and a few fields per kind (section 5). A built-in is a template we ship; a Custom vault is one the user makes with the same form.

**Recommendations:**
1. **Vaults replaces the Vendors section.** Rename and widen it; don't add a second section next to it. Vendor connectors become the first vault kind, "Model library API". Internal names (`vendor_*` commands, app_config `home_vendor_connectors`, `<data_dir>/vendors/` caches, credential-store names) stay, so nothing migrates. The Library keeps its "Vendor" tag, which records who sold a model.
2. **Library stays the home of folders**, including NAS shares mounted as folders (`\\nas\models`, `/Volumes/models`). The Vaults list shows them as one card that points to the Library. This is the "local folder vault" and the SMB/NAS-path vault, and it already works today, including offline detection.
3. **Phase 1 (built in this PR, S):** the Vaults section shell. It lists the configured connectors with their existing actions and the folders/NAS card, has an empty state and an **Add vault** chooser that reuses the existing flows (the connector editor, import from file/link/paste, Library's Add folder). No new network service, no new secret format, nothing shipped that talks to a third party.
4. **Phase 2 (M):** schema version 2 (`kind`, built-in metadata), the built-in template mechanism, and two new provider kinds: **WebDAV** (covers Synology, QNAP, Nextcloud and most NAS and self-hosted servers) and **Azure Blob with a read+list SAS**. Ship CPL3D as the first built-in, and Synology as a built-in that uses the WebDAV kind, once the owner says yes (section 12). Vault downloads also go through the safe-download checks.
5. **S3-compatible** (SigV4 signing) is phase 3. **Synology's File Station API**, **consumer clouds** (OneDrive, Google Drive, Dropbox: OAuth plus app registration) and **upload** come later, and only if they are asked for.
6. **PrintHQ Vault is an "app vault".** It has no public API, CLI or URL scheme (printhq.io/vault, read 2026-10-13). The useful direction is that Vault launches EdgeSlicer with the file, which already works (`tests/printhq_vault_integration.md`). On our side: detect it, show an "Open PrintHQ Vault" card, and nothing more. It is code-defined, never template-defined, because a template must never be able to make EdgeSlicer start a program. It is blocked on PrintHQ telling us their install identifiers (section 9).
7. **The vendor study's open choices:** section 10 gives a recommendation for each.

**The old open point about the PrintHQ URL is resolved.** The product is at **printhq.io**, not printhq.com, which is an unrelated paper-goods store. PrintHQ Vault (v1.0.3, Windows 10/11 x64 and macOS universal, releases on GitHub `Kyvex/printhq-vault-releases`) is local-first. It indexes folders, has an Inbox, a 3D viewer, "Discover" browsing of MakerWorld, Printables, Thingiverse and Thangs, optional PrintHQ Cloud Sync (20 GB), and share links with password, expiry and download limit. It detects nine slicers, and its list includes OrcaSlicer and Bambu Studio but not EdgeSlicer. It is part of the $5/month "Maker" plan. `printhq_integration_requirements.md` section 0 should be updated before anything is sent.

---

## 2. What exists today

| Piece | Where | Notes |
|---|---|---|
| Home page (wxWebView, our page only) | `HomePanel.cpp:127-145` (built lazily), `:305-326` (no navigation away), `:328-349` (messages only from our own page) | Sections Recent, Library, Print History, Vendors, plus a Models rail entry (`resources/web/home/index.html:11-33`). Section names in `HomeTabLogic.cpp:11` |
| Vendor connectors: engine | `Utils/VendorConnector.{hpp,cpp}`. No wx, tested in `tests/slic3rutils/vendor_connector_tests.cpp` | Spec: base_url, auth none/bearer/header/query/basic, fixed and secret headers, list path, paging page/offset/cursor/next, `since` delta, field map, files/variants, download endpoint or direct field, `downloads_limited`, quota headers. `fetch()` sends credentials only to the API's own origin, never on redirects. https only (http for localhost). Import is stricter: always a new connector, refuses any credential, refuses `version > 1` (`VendorConnector.cpp:1260-1271`) |
| Vendor connectors: host | `GUI/HomeVendors.{hpp,cpp}` | Specs in app_config `home_vendor_connectors`. Secrets in wxSecretStore as `"EdgeSlicer vendor connector/<id>/<slot>"` (`HomeVendors.cpp:269-356`), else kept in memory for the session. Caches in `<data_dir>/vendors/<id>/`. Import from file, link (`edgeslicer://connector?url=`) or paste. Export, CSV |
| Download & Open (#446, merged 2026-10-12) | `HomeVendors.cpp:1081-1190`, `HomePanel::open_downloaded` (`HomePanel.cpp:756`) | A save dialog (default: the first Library folder), streamed to `.part` and then renamed, then opened as project / added to plate / G-code preview. **No magic-byte or size check** like the browser's safe-download path has (gap, phase 2) |
| Library | `HomePanel.cpp:836-1129`, `LibraryIndex` | Folders with category, vendor and licence tags. Offline folders show as "not reachable", keeping the last scan. 3MF plate thumbnails. Off-thread scans |
| Models browser | `ModelBrowserPanel.{hpp,cpp}`, `ModelBrowserPanelMac.mm` (#464), rules in `libslic3r/ModelBrowserRules.{hpp,cpp}` | Isolated profile, no script bridge, blocked hosts (localhost, IP, LAN, `.ts.net`), "Open in" links to the downloader, download classification. Fixed site list |
| 3MF settings guard | `GUI/UntrustedSettingsGuard.cpp`, called on project load (`Plater.cpp:12311, 13008`) | Strips or asks about untrusted `post_process` from a 3MF. Applies to vault downloads too, because they load through the same path |

---

## 3. What a vault is

| Vault kind | What it is | Lives in | Example |
|---|---|---|---|
| **Model library API** (`api`) | A JSON REST API that lists the models the user has access to | `HomeVendors` (exists) | CPL3D, a store's "my purchases" |
| **HTTPS index** (`api` with a fixed shape, or `index`) | A static JSON list of files on any HTTPS host | template only (phase 2) | A GitHub Pages/S3 static site with `index.json` |
| **WebDAV** (`webdav`) | A folder tree over WebDAV | new provider (phase 2) | Synology/QNAP WebDAV server, Nextcloud, ownCloud, Apache `mod_dav` |
| **Azure Blob** (`azure_blob`) | One container (and an optional prefix) | new provider (phase 2) | A container with a read+list SAS |
| **S3-compatible** (`s3`) | One bucket (and an optional prefix) | new provider (phase 3) | AWS S3, Cloudflare R2, Backblaze B2, MinIO, Wasabi |
| **Synology File Station** (`synology`) | DSM's own file API | later, only if WebDAV is not enough | DSM without the WebDAV package |
| **Folder / NAS share** (`folder`) | A folder the OS can read: local disk, external drive, SMB share | **Library** (exists) | `D:\Models`, `\\diskstation\3d`, `/Volumes/3d` |
| **App vault** (`app`) | A desktop app with its own library that opens files in slicers | code-defined (phase 2b) | PrintHQ Vault |

Not vaults: **public model sites** (Printables, MakerWorld) belong in Models (an isolated browser, other people's models). A **web-app UI** of a vault (for example the PrintHQ web app) would be a Models-browser site, not a vault. **Tools** (web model generators) is a later, separate rail entry.

### 3.1 The provider abstraction (C++, no wx, testable like VendorConnector)

```
namespace Slic3r::Vaults {
enum class Kind { Api, WebDav, AzureBlob, S3, Synology };      // Folder and App are not providers (Library / AppVaults)

struct Capabilities {
    bool folders;          // a tree to walk (webdav, blob/s3 with "/" delimiter) vs a flat list (api)
    bool delta;            // an incremental "since" fetch (api with since; blob/s3 by ETag/Last-Modified compare)
    bool thumbnails;       // the listing gives a thumbnail URL (api); others: embedded 3MF thumbnail after download
    bool variants;         // files/variants per item (api)
    bool upload;           // later
    bool quota;            // api quota headers
};

struct Entry {             // = Vendors::Item plus storage fields
    std::string id;        // stable within the vault: api item id, or the object key / WebDAV href
    std::string name, folder, page_url, designer, license, description, updated;
    int64_t     size = 0;  std::string etag, sha256;   // for de-duplication and cache validation
    std::vector<std::string> tags;
    std::vector<Vendors::SubItem> subs;
    std::string thumbnail; // URL, host-only, never shown to the page
};

class Provider {
public:
    virtual ~Provider() = default;
    virtual Capabilities caps() const = 0;
    // One page of a listing. `cursor` is opaque (page number, marker, continuation token, next href).
    virtual ListPage list(const ListRequest& r, const Secrets& s, const HttpFn& http, const std::atomic<bool>& cancel) = 0;
    // The request(s) that give the file's bytes: direct, or an endpoint answering with a short-lived link.
    virtual bool fetch_request(const Entry& e, const SubItem* sub, const Secrets& s, Request& out, bool& is_direct) = 0;
    virtual TestResult test(const Secrets& s, const HttpFn& http) = 0;
    // upload(): not in v1.
};
std::unique_ptr<Provider> make_provider(const Vendors::Spec& spec);   // by spec.kind
}
```

- `Vendors::sync()` becomes the `Api` provider. `fetch()` (credentials only to the vault's own origin, checked redirects), `Cache`, `Quota`, `secret_slots()` and the CSV export are shared by all kinds.
- Listing a tree (WebDAV, Blob/S3 with a delimiter) is breadth-first with limits: `max_depth` (default 6), `max_items` (50,000, as now), `max_requests` per sync, a file-type filter (default 3mf, stl, step, stp, obj, amf, zip, gcode.3mf), and cancel.
- **Caching:** `<data_dir>/vendors/<id>/items.json` as today, plus `thumbs/<hash>`. Delta: `api` uses `since`. `webdav` compares `getetag`/`getlastmodified` per folder (a folder whose ETag did not change is skipped where the server gives collection ETags). `azure_blob`/`s3` compare the object ETag. A Refresh interval (`refresh.minutes`, default off, minimum 15) runs only while Home is open, never for metered (`downloads_limited`/quota) APIs.
- **Thumbnails:** the API's thumbnail URL (exists). For storage kinds, after a file was downloaded once: the 3MF's embedded thumbnail (Library code already reads it). Rendering STL/OBJ thumbnails offline is a later item (LayerMate and Meshory both do it).

---

## 4. Relationship to Vendors, Library and Models

- **Vendors → Vaults (merge and rename).** The owner's direction lists "the existing vendor connectors" as vaults, so a separate Vendors section would duplicate the same cards. Users see "Vaults". Each vendor connector card gets a kind label, "Model library API". Saved settings that name the old section open Vaults. The app_config key, cache folder, `vendor_*` messages and credential names do not change. The Library folder field "Vendor" keeps its name: it is a tag for who sold the model, not a place.
- **Library** indexes folders, including NAS shares. A NAS can therefore be reached in two ways: as a share path in Library (works today; the OS handles credentials), or as a WebDAV vault (phase 2; works away from home over HTTPS, and EdgeSlicer handles credentials). The Add vault chooser offers both, explains the difference in one line, and leads to Library for the folder case.
- **Models** stays the browser for public sites. Vault items never open in it. A vault's "Open page" goes to the system browser, as Vendors does today.
- **Rail order:** Recent, Library, **Vaults**, Print History, then the Models action. A later **Tools** entry goes after Models, as another action that swaps in a native panel (the same mechanism Models uses).

---

## 5. One schema: can the vendor template carry built-ins and Custom?

**Today (version 1) it can carry:** the `api` kind completely (the CPL3D gist is a complete example); a static **HTTPS index** (a JSON file with an array of files is "an API with one page", so `list.path`, `items` and `fields` cover it with no engine change); auth none/bearer/header/query/basic; fixed and secret headers; delta; files/variants; download endpoint or direct link; quota.

**It cannot carry yet:**

| Needed | For | Proposal (version 2) |
|---|---|---|
| A discriminator | every new kind | `"kind": "api" \| "webdav" \| "azure_blob" \| "s3" \| "synology"`. Missing = `api`. `spec_from_json` must **refuse an unknown kind**, so an older app never reads a WebDAV vault as an API |
| Where in the store | webdav, blob, s3 | `"root": "/3d/"` (WebDAV path or Blob/S3 prefix), `"recursive": true`, `"max_depth": 6` |
| File-type filter | storage kinds | `"types": ["3mf","stl","step","stp","obj","zip"]`, checked against the app's own allow-list (a template can narrow it, never widen it) |
| SAS auth | azure_blob | `"auth": {"type": "sas"}`. The token (the query string from `sv=` to `sig=`) is a secret slot, appended to requests for the vault's origin only (as `query` auth is) |
| Key auth | s3 | `"auth": {"type": "aws_sigv4", "region": "auto"}`. Slots `access_key_id` (stored as a secret too, though not strictly one) and `secret_access_key`. `"addressing": "path" \| "virtual"` |
| Session auth | synology | `"auth": {"type": "synology_session"}`: user + password slots, plus an OTP prompt each sign-in, never stored |
| OAuth | consumer clouds, later built-ins | `"auth": {"type": "oauth2_pkce", "authorize": ..., "token": ..., "scopes": [...]}`, with tokens in the credential store (the vendor study's plan, unchanged) |
| LAN | NAS over WebDAV at home | `"network": "lan"`: allows a private-address host and, after a warning, plain `http`, or `https` with a pinned self-signed certificate (section 8). Only for webdav/synology/api. Never a template default: the user switches it on in the form |
| Refresh | storage kinds | `"refresh": {"minutes": 0}` (0 = by hand) |
| Built-in metadata | built-ins | `"builtin": {"id": "cpl3d", "version": 3, "publisher": "...", "homepage": "...", "tos_url": "...", "icon": "cpl3d.svg", "min_app_version": "2.5.0.0"}`, plus `"setup": [{"field": "base_url", "label": "Your DiskStation address", "hint": "https://nas.example.com:5006"}]` so a built-in asks only for the fields that vary |

- **Version 2** is written only when a v2 field is used, so a plain API connector stays version 1 and still imports into older builds. Import already refuses `version > 1` in older builds, which is the behaviour we want.
- **Built-ins** ship as `resources/vaults/<id>.json` plus `resources/vaults/<id>.svg`. Adding one creates a normal connector whose spec is the template with the user's setup fields filled in, and records `builtin.id` and `builtin.version`. When a new app version ships a newer template, the existing `plan_update()` logic applies it. It keeps the id and secrets, and asks again if the origin changes. A user who edits a built-in's advanced fields turns it into a Custom vault (the `builtin` link is dropped, so it stops receiving updates).
- **Custom** is the same editor as today with a **Kind** picker at the top. Each kind shows its own fields (API: everything the editor has now; WebDAV: address, root, LAN switch, auth basic/bearer, types; Azure Blob: container URL or a pasted SAS URL; S3: endpoint, region, bucket, prefix, addressing). "Edit as JSON" stays.
- **App vaults are not in the schema.** Detection paths and launch commands are code (section 9). A template can never name an executable.

---

## 6. Providers compared

| Provider | Auth | Windows | macOS | Effort | Risk | Phase |
|---|---|---|---|---|---|---|
| Model library API | none/bearer/header/query/basic (OAuth later) | yes | yes | done | low (exists, tested) | 1 (exists) |
| Folder / SMB/NAS share (via Library) | the OS's (Credential Manager / Keychain on mount); EdgeSlicer stores nothing | UNC paths and mapped drives | `/Volumes/...` once mounted in Finder | XS (an entry point) | low; offline shares already handled off-thread | **1** |
| HTTPS index (JSON) | none/basic/bearer | yes | yes | XS (a documented template) | low | 2 |
| WebDAV | basic over HTTPS, bearer; digest maybe | yes | yes | M (PROPFIND XML, tree walk, LAN mode) | medium: self-signed certs and http on a LAN; `Depth: infinity` is refused by many servers (walk per folder) | **2** |
| Azure Blob | read+list SAS (container); public container | yes | yes | S-M (List Blobs XML, marker paging, SAS split/expiry) | low-medium: SAS is a bearer link (section 8) | **2** |
| S3-compatible | access key + secret (SigV4), presigned URL | yes | yes | M (SigV4 signing with OpenSSL, tested against AWS's published test vectors; ListObjectsV2) | medium: users paste over-privileged root keys (guide them to a read-only key; warn) | 3 |
| Synology File Station API | DSM user + password, OTP for 2FA; session `sid` | yes | yes | M-L (login flow, 2FA, DSM versions, cert) | medium-high: stores a NAS password; QuickConnect relay not supported | later; ship Synology as a WebDAV built-in instead |
| PrintHQ Vault (app) | none (local app) | detect via the uninstall key, launch | detect via bundle id, `open -b` | S once identifiers are known | low if code-defined; must not launch arbitrary programs | 2b (blocked on PrintHQ) |
| PrintHQ Cloud / share links | PrintHQ account; share links with password and expiry | - | - | unknown: no public API | unknown | ask PrintHQ |
| OneDrive / Google Drive / Dropbox | OAuth2 PKCE, app registration and verification per provider | yes | yes | L each | medium (Google blocks embedded sign-in: system browser + loopback) | later, if asked |

**Picks:** phase 1 = API connectors (exist) + folders/NAS shares (exist, now reachable from Vaults). Phase 2 = **WebDAV** and **Azure Blob (SAS)**, plus the HTTPS-index template. WebDAV covers the most NAS boxes and self-hosted servers. Azure Blob is the owner's named cloud, and its SAS model maps onto the existing `query` auth. S3 waits for phase 3 because SigV4 is real signing code.

### Ideas taken from LayerMate and Meshory (read from layermate.app and meshory.com via a listing, 2026-10-13)
Both are local-first, paid once ($19.99 / $35), and index files where they are (folders, drives, NAS shares) without moving them. Both have content-hash de-duplication (Meshory also groups by geometry), rendered thumbnails, and search. LayerMate has a Print Queue and LAN sharing. Meshory has collections, tags, favourites, notes, archive preview and optional AI tags. Neither offers cloud vaults. For EdgeSlicer (phase 3+, across Library and Vaults):
- **De-duplication:** SHA-256 of downloaded files plus the vault's ETag/size, so "you already have this" shows before a metered download.
- **Collections and tags per item**, not only per folder (the Library tags folders today).
- **Thumbnails for STL/OBJ** rendered off-thread with the existing offscreen thumbnail code.
- **Print history per model:** when a vault file is opened and then printed, the G-code archive record gets the vault item key, so the item shows "printed 3x" (GcodeArchive already links `project_path`).
- A **"Print later" queue:** a collection with a fixed name.

---

## 7. UI

**Vaults section** (phase 1 in bold, later in plain text):
- **Header: status ("N models"), Export CSV, Add vault.**
- **Vault cards, one per vault:** **name, kind label ("Model library API" / "Folders and network shares"), vendor badge, state lines: fetched N models at time / not fetched, credentials "saved in your system's credential store" / "Credentials: API key not set", quota, last error; buttons: Fetch Changes, Set/Change credential; menu: Full Refresh, Test connection, Edit, Export, Copy JSON, Forget credentials, Remove.** The **folders card: N folders, M files, offline ones named; Manage folders, Show in Library.** Later: kind icon, "SAS expires in 5 days" warning, "Browse" (filters the grid to this vault), and "Signed in as" where the API says.
- **Grid of items across vaults with connector/category/licence filters, detail view with files, Download / Download & Open per file (exists).** Later: a folder breadcrumb for tree kinds, an Add-to-plate action, "already downloaded" badges (de-dup), and a "Recently opened from vaults" strip (last 20, stored in each vault's cache with the local path).
- **Empty state:** **"Vaults are where you keep your models outside EdgeSlicer: a model store's library, a NAS or cloud storage. Add one to browse it here and open models straight into a project." + Add vault.**
- **Add vault chooser:** **"Model library API" (the editor), "Import a shared vault" (file / link / paste), "Folder or network share" (Library's Add folder)**. Later: a "Built-in" row of curated cards (icon, name, publisher, one line, "needs: API key") above them, and a Kind picker in Custom.
- Sign-in state per kind: API/WebDAV/S3/Blob have "credential set / not set / rejected (401/403 on the last fetch)"; Synology has "signed in until ..."; folders have "online/offline"; the app has "installed / not found".

**Open semantics:** Open = Download & Open (3MF opens as a project after the unsaved-changes question, a mesh goes onto the plate, G-code goes to the preview: `HomeTab::download_open_kind`). Download asks where to save; the default is the first Library folder. Recommended for phase 2: an option to save into a managed `Vault downloads` folder that is added to Library automatically, so Download & Open needs no dialog (owner decision 6).

---

## 8. Data, secrets and security

**Storage:**
- Specs: app_config `home_vendor_connectors` (no secrets; `spec_to_json` never writes one).
- Secrets: wxSecretStore, which is Credential Manager on Windows and Keychain on macOS, service `EdgeSlicer vendor connector/<id>/<slot>`. Where there is no store: memory only, for the session, said in the UI. **Never in app_config, the cache, logs, CSV or exports.** No new secrets format: new kinds add slot names only.
- Caches: `<data_dir>/vendors/<id>/`.
- Remove vault = forget its secrets and delete its cache (exists).

**SAS tokens and other shared links:**
- A pasted Azure SAS URL `https://acct.blob.core.windows.net/models?sv=...&sp=rl&se=...&sig=...` is **split on paste**: the address goes into the spec, and the query (the token) goes straight into the credential store. The page never gets it back.
- The token is checked before it is saved:
  - `sp` must allow read and list. Write, delete, add or create permissions bring a warning: EdgeSlicer only needs read and list, so make a read-only one.
  - `se` (expiry) shows "expires on ...", with a warning when fewer than 7 days are left. An expired token gives a clear error, not a 403.
  - An account-level SAS (`ss`/`srt` present) is refused, because it is wider than one container.
  - A user-delegation SAS is fine.
- Requests: the token is appended only for the vault's own origin (`fetch()` already enforces this for `query` auth). Redirects get no token. Error texts never contain a request URL (already true in `VendorConnector`).
- Presigned S3 URLs and one-off share links (PrintHQ share links, a SAS for a single blob) are not vaults. They are **one file**: they go through `edgeslicer://open?file=` or a later "Open link" box, and are treated as a credential: not logged and not kept in Recent.
- Connector export/import already refuses credentials (`holds_credential`). It must also catch `sig=` and `X-Amz-Signature=` in any URL field (phase 2 test).

**Network rules:**
- HTTPS only, credentials bound to the origin, checked redirects (exists).
- New **LAN mode** (phase 2, per vault, the user switches it on):
  - allows private addresses and `.local` names;
  - for plain `http`, warns that "the password crosses your network unencrypted";
  - for a self-signed certificate, uses **trust on first use**: show the SHA-256 fingerprint, pin it, and fail loudly on change. This needs a pinned-key option in the HTTP layer (`CURLOPT_PINNEDPUBLICKEY`).
  - A LAN vault never sends its credentials to a public host, and a public vault never to a LAN host. Both follow from the origin binding.
- No vault uses a web view, so there is no script bridge to secure. Any vault web UI would go into the Models browser and its rules (isolated profile, no bridge). That browser blocks LAN hosts on purpose, so NAS web UIs stay out of it.

**Downloads (phase 2 fix, also for the existing vendor downloads):**
- Same checks as the browser's safe-download path: the extension allow-list, a 500 MB cap (vendors allow 1 GB today; keep 1 GB for vaults if the owner prefers), the magic-byte check (`content_matches_extension`), a sanitized name, `.part` then rename.
- A 3MF loads through the untrusted-settings guard (exists).
- `downloads_limited` asks first (exists).

**The page:** it only ever gets display fields and opaque keys (exists). Thumbnails are data URIs of PNG/JPEG/GIF/WebP only (exists). All text uses `textContent` (exists).

---

## 9. App vaults (PrintHQ Vault)

- **They launch us:** this already works. `EdgeSlicer.exe "<file>"` and `open -b io.github.acerage.EdgeSlicer <file>` hand the file to the single running instance (`tests/printhq_vault_integration.md`). `edgeslicer://open?file=<https URL>` works for web links. **Ask PrintHQ to add EdgeSlicer to Vault's slicer list.** Their public list has OrcaSlicer and Bambu Studio but not us. That ask is the partner doc, updated with the printhq.io facts.
- **We detect and launch them** (phase 2b, S), code-defined only:
  - **Windows:** read `HKLM` and `HKCU` `...\CurrentVersion\Uninstall\*`. Match on `DisplayName` = "PrintHQ Vault" and a publisher we hard-code once PrintHQ confirms it. Take `InstallLocation` and the exe name from code. Check the file exists and has a valid Authenticode signature from that publisher (`WinVerifyTrust`). Launch it with **no arguments**.
  - **macOS:** `NSWorkspace URLForApplicationWithBundleIdentifier:` with their bundle id (hard-coded once confirmed), then `openApplicationAtURL:`. Gatekeeper covers the signature.
  - The card says "Installed: Open PrintHQ Vault" or "Not installed", with a link to printhq.io/vault in the system browser. We never download or install it.
- **We hand them files:** not possible without a documented Vault CLI or URL scheme. Ask PrintHQ. Until then, the card says "Add the same folders to Library to see them here too".
- **PrintHQ Cloud Sync / share links:** no public API. If PrintHQ offers one, it becomes a built-in `api` template like CPL3D. A share link is one file (section 8).
- A **generic app vault** (any program the user picks) is **not** recommended. It would make Home a program launcher, which is a bigger attack surface than it is worth.

---

## 10. Windows and macOS

| Topic | Windows | macOS |
|---|---|---|
| Secrets | Credential Manager via wxSecretStore | Keychain via wxSecretStore. Unsigned local builds may prompt "allow access" on each run; notarized releases are fine |
| SMB/NAS | UNC paths work without mounting; credentials from Credential Manager / `net use` | Must be mounted first (Finder > Connect to Server). A later option could mount with `NetFSMountURLAsync`, but the Library sees `/Volumes/...` paths fine once mounted. Sleep and unmount show as offline (exists) |
| HTTP/TLS | the app's curl + OpenSSL (or Schannel per deps) | the same curl build. Pinning works the same |
| App vault detection | uninstall registry keys + WinVerifyTrust | bundle id + NSWorkspace |
| Incoming file from app vault | argv + single instance (exists) | Apple Events `openFiles` (exists) |
| Web views | none needed for vaults | none needed |

### The vendor study's open choices, decided here
1. PrintHQ identity: **printhq.io**, resolved above.
2. MVP scope: **user-defined Models sites without a bridge**, as the study recommended. It is independent of Vaults.
3. macOS browser host: done (#464). Nothing to decide.
4. Repository: **curated only at first.** `aceRage/edgeslicer-vendors`, served from GitHub Pages, `index.json` signed with minisign, key held by the owner. Built-ins ship inside the app until the repository exists, so phase 2 doesn't wait for it.
5. Revocation check: **on by default**, with a Preferences switch.
6. Bridge for community web-app tabs: **verified publishers only.**
7. OAuth in phase 2: **no.** Use keys and SAS until a built-in needs OAuth.
8. Expose the printer model to web apps: **no.**

---

## 11. Phases

| Phase | Content | Size |
|---|---|---|
| **1 (this PR)** | Vendors becomes Vaults (rail, title, strings; the saved section `vendors` opens Vaults); vault cards with a kind label and credential state; a folders/NAS card from the Library state; empty state; the Add vault chooser that reuses the connector editor, import from file/link/paste and Library's Add folder. Tests: `home_tab_tests` section rules; a headless web gate `tests/phone/home_vaults_gate.js` | S (1-2 days) |
| 2 | Schema v2 (`kind`, `root`, `types`, `network`, `refresh`, `builtin`, `setup`); refuse unknown kinds; the `Provider` interface with `Api` moved behind it; **WebDAV** and **Azure Blob (SAS)** providers with unit tests against stand-in servers; LAN mode with TOFU pinning; built-in mechanism (`resources/vaults/`) with **CPL3D** and **Synology (WebDAV)** templates; the Kind picker in the editor; vault downloads through safe-download checks; a "Recently opened from vaults" strip | M-L (2-3 weeks) |
| 2b | PrintHQ Vault app card (detect + launch), once PrintHQ confirms the identifiers | S (2-3 days) |
| 3 | S3-compatible (SigV4); STL/OBJ thumbnails; de-dup by hash; item tags and collections across Library and Vaults; print history per item | M-L |
| 4 | Template repository with signing and revocation (vendor study phase 2); OAuth2 PKCE; consumer clouds if wanted; upload ("Save project to vault"); Synology File Station if WebDAV is not enough | L |
| Tools | Separate project: curated web generators in the Models browser host, with downloads offered to the plate | - |

---

## 12. Owner decisions needed

Phase 1 needs none beyond accepting the rename. The rest gate phase 2 onwards.
1. **The rename:** "Vendors" becomes "Vaults" in the UI (phase 1 does this). Keep the word "Vendor" anywhere visible, for example the card's vendor badge and the Library's Vendor tag? (Recommended: keep both.)
2. **Built-ins policy:** the Vendors empty state says "EdgeSlicer includes no vendors". May we ship built-in templates? First: CPL3D (does CPL3D agree to be listed?), and Synology as a WebDAV template (no partner needed; Synology is a trademark, so the name is text only and the icon generic unless allowed).
3. **PrintHQ:** who contacts them, and do we ask for (a) EdgeSlicer in Vault's slicer list, (b) their Windows uninstall key, publisher and exe name and their macOS bundle id, (c) any CLI/URL scheme to hand Vault a file, (d) an API for Cloud Sync? Also update `printhq_integration_requirements.md` (it is written for a web tab, but Vault is a desktop app).
4. **Phase 2 provider picks:** WebDAV + Azure Blob SAS (recommended), or S3 first?
5. **LAN NAS:** allow plain `http` on a LAN with a warning, or HTTPS only (self-signed with pinning)? (Recommended: both, http behind a warning.)
6. **Where vault downloads go:** keep the save dialog (today), or a managed "Vault downloads" folder added to Library so Download & Open is one click?
7. **Read-only:** keep vaults read-only until phase 4 (recommended), or is "save project to vault" wanted sooner?
8. **Synology:** a WebDAV built-in only (recommended), or the File Station API too (stores a DSM password, needs 2FA handling)?
9. **Consumer clouds** (OneDrive, Google Drive, Dropbox): wanted at all? Each needs an app registration and, for Google, a verification review.
10. **Download cap:** vendors allow 1 GB per file, the browser's safe path 500 MB. Which cap for vaults?

## Sources
- Code: `origin/main` 83267707ad, file:line above.
- PrintHQ Vault: https://printhq.io/vault (read 2026-10-13), releases at https://github.com/Kyvex/printhq-vault-releases/releases
- LayerMate: https://www.layermate.app/ (read 2026-10-13)
- Meshory: https://meshory.com via https://alternativeto.net/software/meshory/about/ (read 2026-10-13)
- CPL3D connector template: https://gist.github.com/aceRage/6777406070c5f2d2c725b24756b7606b
