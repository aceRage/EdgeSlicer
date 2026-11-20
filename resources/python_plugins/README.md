# Plugin publisher keys

Ed25519 public keys the slicer trusts for Python-plugin signatures (phase 5).

This file is shipped inside the app (`resources/python_plugins/trust_keys.json`).
Catalog keys land here in phase 11. The allow-list the user builds (local hash
pins, extra publisher keys) lives in `{data_dir}/orca_plugins/trust.json`, never
in `{data_dir}/plugins` (UltraNet).

**No private key is stored in this repository.**
