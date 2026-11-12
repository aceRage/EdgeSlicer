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

- SHA-256 of the plugin tree (sorted relative paths plus content), not only the entry `.py` / `.whl`.
  The walk skips `.sig` files, `__pycache__`, `__whl_extracted__`, and `.install_state.json`. Symlinks
  that escape the plugin root fail the hash. **uv-installed dependencies are unpinned** — they live
  outside the plugin root and are not part of this digest.
- Ed25519 signature over a canonical payload: `edge-plugin-sig-v1` plus the tree hash, name, version,
  capability types, min orca version, `plugin_key`, and declared deps (and their digest). Requested
  permissions are not bound: they are not on the descriptor at sign/evaluate time (they come from
  `request_permissions()` at first load).
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
- pinned content hashes keyed by `plugin_key` (an empty `plugin_key` is refused)

It is written with the same atomic replace as `.install_state.json` (unique temp, fsync the file,
POSIX `rename` then fsync the parent directory / `MoveFileExW` REPLACE). A missing file is
empty-trust. A corrupt or unparseable file, a missing `version`, or any version other than `1` is
empty-trust (no user pins, no user keys), never a crash; shipped keys still apply. Mutators refuse
to overwrite a corrupt sidecar until `reset_corrupt_trust_store` rewrites an empty valid file.

This path is **never** `{data_dir}/plugins` (UltraNet). Writes that resolve there are refused.
The case-insensitive / NFC/NFD / dangling-symlink / `equivalent()` checks are the same
`PluginAuditManager::is_ultranet_plugins_path` used in phase 4.

## Home > Plugins

Cards show the trust string from `plugin_trust::evaluate`. Installing a local file always goes
through a Yes/No dialog (`wxYES_NO | wxNO_DEFAULT`, No focused). Yes copies the package (and a
sibling `.sig` when present) into `{data_dir}/orca_plugins` and pins the tree SHA-256. The
`Signed by …` name comes from the shipped or user-added key file, never from the unsigned `.sig`
sidecar.

Enable / load re-hashes the plugin root immediately before the trust check. After that check
passes, load discards `__whl_extracted__`, `__pycache__`, and loose `.pyc`/`.pyo`, re-extracts
sibling wheels to a fresh directory, and unlinks importlib cache files (including those under
`sys.pycache_prefix`) so the bytes imported are the hashed `.py` / `.whl` bytes. Plugin interpreter
init sets `PyConfig.write_bytecode = 0` and `sys.dont_write_bytecode`; existing cache files are
still deleted because turning off writes does not stop CPython from reading a planted `.pyc`.

## Known limitations

- Cross-process last-writer-wins on `trust.json` (no file lock across processes).
- No in-place approve of an already-installed Untrusted package; reinstall from a local file and
  click Yes, or install a package signed by a trusted publisher.
- No trust check on permission mutation. Changing grants in `.install_state.json` does not
  re-prompt and does not change the tree hash (that sidecar is excluded).
- uv-installed Python dependencies are unpinned.
- Small evaluate-then-import window: the tree can change after `evaluate()` and before import.
- Full-file reads on every evaluate (the tree hash reads every included file).
- NFC/NFD path variants fail closed (a pin recorded under one Unicode normalization does not
  match the other).
- Home's already-pinned check hashes the source file/tree before copy; the installed plugin-root
  tree can differ (excluded host files aside), which re-prompts even when the same package is
  already pinned.
