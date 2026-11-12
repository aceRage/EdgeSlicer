# Phase 5 — Plugin signing and allow-list

Revision: Edge `feature/plugins-p5-trust`.
Plan: plugins plan section 7 (signing / allow-list). Catalog publishing is Phase 11.

All of this is compiled only when `EDGE_PYTHON_PLUGINS` is ON (default OFF). Pipeline and
printer-agent hooks stay unwired. Nothing here changes G-code.

## Policy

A package may be **enabled** or **run** (including Home > Plugins Run / script launch) only when
it is either:

1. **Signed by a trusted publisher**, or
2. **Pinned locally** (the user said Yes on the install trust prompt).

Anything else is refused with: *"This plugin is untrusted. Approve it when installing from a
local file, or install a package signed by a trusted publisher."*

| Action | Signed by a shipped or user-added publisher key | Same `plugin_key` already pinned to this SHA-256 | Otherwise |
|---|---|---|---|
| Local install | Prompt (default **No**). Yes pins the hash. | No extra prompt; pin kept | Prompt (default **No**). Yes pins the hash. |
| Local replace / different hash of the same `plugin_key` | Prompt again | Prompt again | Prompt again |
| Enable / load | Allowed | Allowed | Refused |
| Run / script launch | Allowed | Allowed | Refused |
| Home card | `Signed by …` (takes precedence) | `Approved locally` | `Untrusted` |

Existing installs from before phase 5 have no pin and no signature. They come up **Untrusted**
and must be re-approved (reinstall from a local file and click Yes, or install a package signed
by a trusted publisher).

## Signing

A package may carry a sibling `{file}.py.sig` / `{file}.whl.sig` JSON sidecar:

- SHA-256 of the `.py` or `.whl` bytes
- Ed25519 signature over a canonical payload: `edge-plugin-sig-v1` plus hash, name, version,
  capability types, and min orca version
- The signer's 32-byte public key (64 hex chars)

Verification uses the in-tree OpenSSL EVP Ed25519 API (`EVP_PKEY_ED25519`,
`EVP_DigestSign` / `EVP_DigestVerify` with a null digest). No new crypto library.

Editing the file, the metadata, or truncating the signature invalidates it. The public keys the
app trusts ship in `resources/python_plugins/trust_keys.json` (empty until catalog keys exist).
**No private key is stored in this repository.** RFC 8032 test vectors live only under
`tests/data/plugins/trust/` and are marked TEST-ONLY.

## Allow-list sidecar

`{data_dir}/orca_plugins/trust.json` records:

- user-added publisher keys
- pinned content hashes (optional `plugin_key`)

It is written with the same atomic replace as `.install_state.json` (unique temp, then POSIX
`rename` / `MoveFileExW` REPLACE). A missing file is empty-trust. A corrupt or unparseable file
is empty-trust (no user pins, no user keys), never a crash; shipped keys still apply.

This path is **never** `{data_dir}/plugins` (UltraNet). Writes that resolve there are refused.
The case-insensitive / NFC/NFD / dangling-symlink / `equivalent()` checks are the same
`PluginAuditManager::is_ultranet_plugins_path` used in phase 4.

## Home > Plugins

Cards show the trust string from `plugin_trust::evaluate`. Installing a local file always goes
through a Yes/No dialog (`wxYES_NO | wxNO_DEFAULT`, No focused). Yes copies the package (and a
sibling `.sig` when present) into `{data_dir}/orca_plugins` and pins the SHA-256.
