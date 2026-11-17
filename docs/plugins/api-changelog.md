# Plugin API changelog

Advertised as `orca.api_version` / `k_orca_api_version`. Plugins declare
`requires-orca-api` in PEP 723 metadata; a requirement newer than this build is refused.

## 1.1.0

- Hosted G-code post-process (`psGCodePostProcess`) pins `ctx.gcode_path` to a working
  copy under the plugin's `plugin_data` folder. The helper never grants the original
  export directory. A write outside `plugin_data` raises `PermissionError` naming
  `ctx.gcode_path`. In-process fallback still points `ctx.gcode_path` at the export file.

## 1.0.0

- Initial advertised API: script capabilities and the Phase 3/6 host bindings.
