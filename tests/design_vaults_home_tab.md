# Design: Vaults on the Home tab

Status: design, revised 2026-10-13 after the owner's decisions. Phase 1 is PR #476 (`feat/vaults-home-tab`); phase 2 is the stacked PR on `feat/vaults-providers`.
Builds on: `tests/design_vendor_templates_webapp_tabs.md` (2026-10-10, "the vendor study"), `tests/design_home_hub_browser.md`, `tests/printhq_vault_integration.md`.

---

## 1. Summary

A **vault** is the user's own **storage** for model files, outside EdgeSlicer: a NAS, a network share, a WebDAV server, an Azure Blob container or an S3-compatible bucket. Home gets a **Vaults** section that lists the user's vaults. For each one it shows the sign-in state, lists the model files, and offers **Download & Open** and **Download**.

**Vendors are not vaults** (owner, 2026-10-13). A vendor connector such as CPL3D is a store's API over models the user bought. It keeps its own **Vendors** section, unchanged: its name, rail entry, links, import and "Add connector". The rail reads **Recent, Library, Vendors, Vaults, Print History**.

**A later merge of Vendors and Vaults is possible** (owner: maybe eventually, not now). The code is kept easy to merge:
- one card component on the page (`sourceCard` in `home.js`, used by vault cards and the folders card, in the same `.conn` style as vendor cards);
- one files-grid card (`card()`);
- one safe-download routine on the slicer side (`GUI/HomeDownload.{hpp,cpp}`), written so that Vendors' downloads can move onto it later.

The model follows the owner's direction:
- **Built-in vaults** are curated templates we ship. For now only **Synology NAS**, which is a WebDAV template with Synology's setup hints.
- **Custom vaults** use a form where the user picks the provider kind and fills in the address, sign-in and options.

Both are the same JSON spec (section 5).

### Owner decisions (2026-10-13)

| # | Decision |
|---|---|
| 1 | CPL3D is not a vault. Vendors stays as it was; Vaults is a separate section for storage. Add vault offers storage kinds only. A later merge is possible, not now. |
| 2 | Synology ships as a built-in (WebDAV template). No other built-ins for now. |
| 3 | Providers: **WebDAV, Azure Blob and S3-compatible**, all three. |
| 4 | Plain `http` is allowed on local networks. The user gets a warning that names the risk (the password crosses the local network unencrypted). Each vault can dismiss it for good, stored per vault in app config. |
| 5 | Vault downloads go to a folder set in Preferences and per vault in the vault's settings, with a sensible default. Download & Open opens straight away (no save dialog). The folder is shown in the Library. |
| 6 | PrintHQ: the owner contacts them after phase 2 and asks what connection they can offer. PrintHQ stays out of these PRs; the design keeps a slot for an app-vault kind shaped by their answer (section 9). |

### Defaults for the questions not answered (taken from the recommendations, listed in the PRs)
- Vaults are **read-only** until phase 4 (no upload, no delete, no rename).
- Synology goes through **WebDAV only**, not the File Station API.
- **No consumer clouds** (OneDrive, Google Drive, Dropbox).
- **Size cap: 500 MB per file**, the same as the browser and "Open in" download path (`untrusted::MODEL_DOWNLOAD_SIZE_LIMIT`).
- The vendor badge question is moot, because vendors are not vaults.

---

## 2. What exists today

| Piece | Where | Notes |
|---|---|---|
| Home page | `GUI/HomePanel.cpp`, `resources/web/home/` | wxWebView, our own page only; messages only from our own page (`HomePanel.cpp:328`) |
| Vendors | `Utils/VendorConnector.*`, `GUI/HomeVendors.*` | Unchanged by Vaults |
| Library | `HomePanel.cpp` library code, `LibraryIndex` | Folders, including mounted NAS shares. Offline folders show as "not reachable" |
| HTTP client | `Utils/Http.*` (curl) | TLS is verified for public hosts; private/LAN hosts are not verified (`Http::tls_verify_for`, the print-host policy). So a NAS's self-signed certificate works on the LAN |
| Safe downloads | `libslic3r/UntrustedInput.*` | `claim_unused_download_name`, `place_download_file`, `content_matches_extension`, `has_model_extension`, `MODEL_DOWNLOAD_SIZE_LIMIT` (500 MB); used by the Downloader and the Models browser |
| 3MF settings guard | `GUI/UntrustedSettingsGuard.cpp` | A 3MF from a vault loads through it like any other |

---

## 3. Vault kinds

| Kind | What it is | Phase |
|---|---|---|
| **Folder / NAS share** | A folder the OS can read: local disk, external drive, SMB share (`\\nas\3d`, `/Volumes/3d`). Lives in the **Library**; Vaults shows one card for it | 1 |
| **WebDAV** (`webdav`) | A folder tree over WebDAV: Synology/QNAP WebDAV Server, Nextcloud, ownCloud, Apache `mod_dav`, rclone serve | 2 |
| **Synology NAS** (built-in) | A WebDAV vault with Synology's defaults and setup hints | 2 |
| **Azure Blob** (`azure_blob`) | One container, optional prefix, with a read+list SAS (or a public container) | 2 |
| **S3-compatible** (`s3`) | One bucket, optional prefix: AWS S3, Cloudflare R2, Backblaze B2, MinIO, Wasabi, Synology C2 | 2 |
| **App vault** (`app`, reserved) | A desktop app with its own library (PrintHQ Vault). Shaped by what PrintHQ offers | after phase 2 |

Not vaults:
- Vendors (store APIs).
- Public model sites (the Models browser).
- Tools (web generators): a later rail entry next to Models.

### 3.1 The provider interface (`src/slic3r/Utils/VaultProvider.{hpp,cpp}`, no wx, unit tested)

```
namespace Slic3r::Vaults {
enum class Kind { WebDav, AzureBlob, S3 };
struct Spec { id, name, kind, url, prefix, bucket, region, addressing, auth, recursive, max_depth, builtin, downloads_dir };
struct File { id (path in the vault, '/'-separated), name, folder, size, mtime, etag };
struct Request  { method, url, headers, body, sink };            // one HTTP request, never redirected by the client
struct Response { status, headers, body, error };
using HttpFn = std::function<Response(const Request&)>;

class Provider {                                                    // one per kind, make_provider(spec)
    virtual Listing list(secrets, http, cancel, now, options) = 0;  // walks the tree / pages, filters model files
    virtual Request download_request(secrets, file, now) = 0;       // credentials or signature included
};
Response fetch(spec, http, request, max_redirects);                 // follows redirects; credentials stay on the vault's origin
}
```

- **WebDAV:** `PROPFIND Depth: 1`, folder by folder, breadth first up to `max_depth`, with limits on files and requests.
  - The multistatus XML is read with namespace prefixes ignored (`D:`, `d:`, `lp1:`).
  - `href`s are percent-decoded, and any outside the vault's root path are ignored.
  - Auth is none, basic or bearer.
- **Azure Blob:** `GET <container>?restype=container&comp=list&prefix=&marker=` with the SAS token appended, reading `NextMarker` for the next page. Without "include subfolders", the request adds `delimiter=/`.
- **S3:** `GET ?list-type=2&prefix=&continuation-token=`, signed with SigV4 (HMAC-SHA256 from OpenSSL). Path-style or virtual-hosted addressing; the region is configurable (`auto` for R2). Requests carry the empty-body hash for `x-amz-content-sha256`. A public bucket can be listed without signing.
- **File filter:** the app's model types only (`untrusted::has_model_extension`: 3mf, stl, step, stp, obj, zip).
- **Caching:** `<data_dir>/vaults/<id>/files.json` (files, fetch time, last error). Fetching is by hand (the Fetch files button), so nothing runs in the background.
- **Thumbnails:** none in phase 2; storage listings have none. Phase 3 adds the 3MF's embedded thumbnail after the first download, and STL/OBJ renders.

---

## 4. UI

**Vaults section:**
- **Header:** status ("N files"), **Add vault**.
- **Vault cards:**
  - Each card shows:
    - name;
    - kind label (WebDAV / Synology NAS / Azure Blob / S3-compatible);
    - address (host and path, never a token);
    - "N files · fetched <time>", or "Not fetched yet";
    - credential state ("saved in your system's credential store" / "Password not set");
    - "Unencrypted (http) on your local network" when the vault uses http;
    - "SAS link expires <date>", with a warning in the last 7 days;
    - the downloads folder;
    - the last error.
  - Buttons: **Fetch files**, and Set/Change for each credential.
  - Menu: Test connection, Edit, Open downloads folder, Forget credentials, Remove vault.
- **Folders and network shares** card (the Library's folders): counts, unreachable shares, Show in Library, Manage folders.
- **Files grid:** cards with name, folder, size, date and a type badge, plus vault and type filters and search. A click is Download & Open. The menu has Download & Open and Download.
- **Empty state:** "Vaults are your own model storage: a NAS, a network share or cloud storage. Add one to browse its models here and open them straight into a project." + Add vault.
- **Add vault chooser (storage only):** Synology NAS (built-in), WebDAV server, Azure Blob container, S3-compatible bucket, Folder or network share (the Library's Add folder).
- **Vault editor:** fields by kind.
  - All kinds: name, address.
  - Azure Blob and S3: prefix.
  - S3 only: bucket, region, addressing.
  - Sign-in (by kind), include subfolders and depth.
  - Downloads folder, with Browse and Use default.
  - A built-in shows its setup hints. Credentials are set from the card, in native dialogs, never typed into the page.

**Download & Open** saves into the vault's downloads folder (no dialog) and opens the file:
- a 3MF as a project, after the usual unsaved-changes question;
- a mesh onto the plate;
- `.gcode.3mf` in the preview.

**Download** saves and offers Show in folder.

---

## 5. One schema for built-ins and Custom

A vault is a JSON spec, `{"format": "edgeslicer-vault", "version": 1, ...}`. It is kept in app_config `home_vaults` (a JSON array), without secrets:

```json
{ "format": "edgeslicer-vault", "version": 1,
  "id": "nas-3f9a", "name": "DiskStation", "kind": "webdav",
  "url": "https://diskstation.local:5006/3d/", "auth": "basic",
  "recursive": true, "max_depth": 6, "builtin": "synology", "downloads_dir": "" }
```
Azure adds `"auth": "sas" | "none"` and `"prefix"`. S3 adds `"bucket"`, `"region"`, `"addressing": "path" | "virtual"`, `"auth": "sigv4" | "none"` and `"prefix"`.

**Why not the vendor-connector format?** The earlier coordinator question asked whether the vendor template could carry vaults. It could only with a `kind` switch. Nearly every vendor field (list path, paging, field map, files, download endpoint, quota) means nothing to a storage kind, and vendors and vaults are now separate sections. So vaults get their own small format. They keep the same rules:
- a format/version envelope, where a newer version is refused by older builds;
- no secrets in the file;
- credentials in named slots in the credential store;
- unknown kinds refused;
- a strict parser that rebuilds the saved JSON from checked fields.

If the sections merge later, a vendor connector becomes one more provider kind (`api`) behind the same `Provider` interface, with its existing spec as that kind's options.

**Built-ins** ship as `resources/vaults/<id>.json`: the spec plus `title`, `description` and `hints` (setup text). Choosing one opens the editor pre-filled; saving records `"builtin": "<id>"`. Synology: kind `webdav`, auth `basic`, an address hint `https://<your NAS>:5006/<shared folder>/`. Its hints cover:
- turning on **WebDAV Server** in Package Center;
- HTTPS on port 5006 (5005 is plain http);
- making a DSM user that can only read the models folder;
- not using QuickConnect relay addresses.

**Custom** is the same editor with the kind picked in the chooser.

---

## 6. Providers compared

| Provider | Auth | Windows | macOS | Effort | Risk | Phase |
|---|---|---|---|---|---|---|
| Folder / SMB/NAS share (Library) | the OS's; EdgeSlicer stores nothing | UNC and mapped drives | `/Volumes/...` once mounted | exists | low | 1 |
| WebDAV (incl. Synology built-in) | none, basic, bearer | yes | yes | M | medium: LAN http (warned), self-signed certs (LAN TLS is not verified; public TLS is), servers that refuse deep listings (we walk per folder) | 2 |
| Azure Blob | read+list SAS (container), or public | yes | yes | S-M | low-medium: a SAS is a bearer link (section 7) | 2 |
| S3-compatible | access key id + secret (SigV4), or public | yes | yes | M | medium: users paste over-privileged keys (the editor says "use a read-only key") | 2 |
| Synology File Station API | DSM password + 2FA | yes | yes | M-L | high (stores a NAS password with full DSM rights) | not planned |
| App vault (PrintHQ) | per partner | per partner | per partner | ? | ? | after the owner's contact |
| OneDrive / Google Drive / Dropbox | OAuth2 + app registration | yes | yes | L each | medium | not planned |

From LayerMate and Meshory (both local-first library apps, read 2026-10-13): de-duplication by content hash, rendered thumbnails, tags/collections, print history per model, a print-later queue. These go to phase 3, across Library and Vaults.

---

## 7. Secrets and security

**Storage:**
- Specs in app_config `home_vaults`, without secrets.
- Secrets in wxSecretStore (Credential Manager on Windows, Keychain on macOS), service `EdgeSlicer vault/<id>/<slot>`. Slots:
  - WebDAV basic: `user`, `password`
  - WebDAV bearer: `token`
  - Azure: `sas`
  - S3: `access_key_id`, `secret_access_key`
  
  The S3 key id is not strictly a secret, but it stays out of plain config.
- Where there is no credential store, secrets are kept in memory for the session, and the UI says so.
- Removing a vault forgets its secrets and deletes its cache.
- Errors, logs, the page and the cache never hold a secret, a SAS token or a signed URL.
- Secrets are asked for in **native** dialogs; the page never sees one.

**Azure SAS links:**
- The user pastes the container's SAS URL (`https://acct.blob.core.windows.net/models?sv=...&sp=rl&se=...&sig=...`) in a native dialog. EdgeSlicer **splits** it: the address goes into the spec (or must match it) and the token goes into the credential store.
- Checks before saving:
  - **Refused:**
    - no `sig` (not a SAS);
    - an **account SAS** (`ss`/`srt` present: it reaches more than one container);
    - a **blob SAS** (`sr=b`: one file, not a container);
    - missing `r` or `l` permission (EdgeSlicer needs read and list);
    - already expired.
  - **Warned:**
    - write, delete, add, create or other change permissions ("this link can change or delete files; EdgeSlicer only needs Read and List");
    - expiry within 7 days.
  - The card shows the expiry date.
- The token is appended only to requests for the vault's own origin. Redirects to another origin go without it.

**Signed S3 requests:** SigV4 per request, with the key never leaving the process. A redirect to another origin drops the signature.

**Network rules:**
- `https` to any host.
- Plain `http` **only to a local-network host**: private IPv4 ranges, IPv6 ULA/link-local, `localhost`, `.local`, `.lan`, `.home.arpa`, `.internal`, or single-label names. Plain http to a public host is refused when the vault is saved.
- **The http warning:**
  - Shown before the first connection of each session to an http vault (fetch, test, download).
  - Text: "<name> uses plain http. On your local network, anyone who can see the traffic can read your password for this vault and the files, because http sends them unencrypted. Use https if your NAS offers it."
  - Buttons Connect / Cancel, and a "Don't warn me again for this vault" checkbox. The dismissal is stored per vault id in app_config `home_vault_http_ok` and cleared when the vault is removed or its address changes.
- Credentials never follow a redirect to another origin. Redirects to a refused address end the request.

**Downloads (safe-download checks, `GUI/HomeDownload`):**
1. Only model types (`has_model_extension`).
2. Name sanitized and claimed exclusively in the downloads folder (`claim_unused_download_name` and its marker).
3. Streamed to a `.part` file with a running **500 MB** cap.
4. Then `content_matches_extension` (magic bytes / STL size).
5. Then `place_download_file` (never replaces an existing file).
6. Markers and partial files are removed on any failure.

A 3MF then loads through the untrusted-settings guard.

**No web view:** vaults are a native list, so there is no script bridge to secure.

---

## 8. Downloads folder

- Preferences > Downloads > **Vault downloads**: a folder with Browse, saved in app_config `vault_downloads_dir`. When unset, the default is `<Downloads>/EdgeSlicer Vaults`.
- Per vault: `downloads_dir` in the vault's settings ("" = the Preferences folder).
- The first time a file is saved into a folder, that folder is **added to the Library** (category "Vaults"), once. Folders added this way are remembered in app_config `vault_downloads_library`, so if the user removes one from the Library it is not added again.

---

## 9. App vaults (reserved for PrintHQ)

**PrintHQ: the owner contacts them after phase 2** and asks what connection setup they can offer. The product is at printhq.io (printhq.com is unrelated). PrintHQ Vault is a local-first desktop app for Windows and macOS. It has no public API, CLI or URL scheme today, and its slicer list does not include EdgeSlicer.

The `app` kind is reserved in the format. Its shape depends on what they offer:
- **They launch us** (already works): `EdgeSlicer.exe <file>`, `open -b io.github.acerage.EdgeSlicer <file>`, `edgeslicer://open?file=`. Ask to be added to Vault's slicer list.
- **A local API** (for example a loopback HTTP endpoint with a pairing token): becomes a provider like the others (list / fetch), with the token in the credential store and loopback-only rules.
- **A cloud API** (Cloud Sync / share links): becomes a provider like Azure/S3, with OAuth or a key.
- **Detect and launch only:** code-defined detection (uninstall key + signature check on Windows, bundle id on macOS). A template never names an executable.

---

## 10. Windows and macOS

| Topic | Windows | macOS |
|---|---|---|
| Secrets | Credential Manager via wxSecretStore | Keychain via wxSecretStore (unsigned local builds may prompt) |
| SMB/NAS | UNC paths work without mounting | Mount first (Finder > Connect to Server); then `/Volumes/...` |
| TLS | curl; LAN hosts not verified (self-signed NAS certs work), public hosts verified | the same curl build |
| Default downloads folder | `%USERPROFILE%\Downloads\EdgeSlicer Vaults` | `~/Downloads/EdgeSlicer Vaults` |

---

## 11. Phases

| Phase | Content | Size |
|---|---|---|
| **1 (#476)** | The Vaults section beside an unchanged Vendors: rail entry, folders/NAS card, empty state, Add vault chooser (Folder or network share). Tests: section rules; `tests/phone/home_vaults_gate.js` | S |
| **2 (stacked PR)** | `Vaults::Provider` with WebDAV, Azure Blob (SAS split + checks) and S3 (SigV4); Synology built-in; vault cards, files grid and editor; http warning with per-vault dismissal; downloads folder (Preferences + per vault, shown in Library); safe-download checks; unit tests with mock servers and AWS's published SigV4 test values | L |
| 3 | Thumbnails (3MF embedded after download, STL/OBJ renders); de-dup by hash and ETag; tags and collections across Library and Vaults; print history per file; certificate pinning for self-signed public hosts; scheduled refresh | M-L |
| 4 | Upload ("Save project to vault"), only if wanted; vault spec import/export; Vendors' downloads moved onto `HomeDownload` | M |
| Later, maybe | **Possible later merge of Vendors and Vaults** (owner: not now): vendor connectors as an `api` provider kind, one section, one card component | M |
| After the owner's contact | PrintHQ app vault (section 9) | ? |
| Tools | Separate project | - |

---

## 12. Still open (not blocking)
1. Whether a vault export/import (sharing a vault's settings without secrets, as Vendors does) is wanted.
2. Whether "Download & Open" for a `.zip` should unpack it (today a zip opens through the same path as a dropped zip).
3. Whether fetching should run on a schedule (phase 3) or stay manual.

## Sources
- Code: `origin/main`, file references above.
- PrintHQ Vault: https://printhq.io/vault (read 2026-10-13)
- LayerMate: https://www.layermate.app/ ; Meshory: https://meshory.com via https://alternativeto.net/software/meshory/about/ (read 2026-10-13)
- AWS Signature Version 4 examples: https://docs.aws.amazon.com/AmazonS3/latest/API/sig-v4-header-based-auth.html
- Azure List Blobs and SAS: https://learn.microsoft.com/rest/api/storageservices/list-blobs , https://learn.microsoft.com/rest/api/storageservices/create-service-sas
- WebDAV PROPFIND: RFC 4918 section 9.1
