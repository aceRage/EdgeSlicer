# Phase 6 — Slicing pipeline plugins

Revision: Edge `feature/plugins-p6-pipeline`.
Plan: plugins plan section 8 (slicing pipeline). Catalog publishing is Phase 11. Infill
plugins (upstream #16941) and printer-agent hooks stay unwired.

All of this is compiled only when `EDGE_PYTHON_PLUGINS` is ON (default OFF). With it OFF, or
ON with no pipeline plugin selected, the G-code **config block omits** the five plugin keys
(`plugins`, `slicing_pipeline_plugin`, `print_plugin_config_overrides`,
`printer_plugin_config_overrides`, `filament_plugin_config_overrides`) when they are empty, so
the dump matches main. Empty is not nil, which is why `append_full_config` skips them
explicitly. Option definitions stay in `PrintConfig.cpp` so OFF still round-trips ON 3MFs and
presets. Startup never loads a pipeline plugin in golden or CI runs: a missing
`.install_state.json` sidecar means `enabled=false`.

## What was built

`SlicingPipelinePluginCapability` is hooked into Edge's print graph (`Print::process`) and
`PostProcessor.cpp` the way upstream OrcaSlicer does.

| Step | Where it runs | Sandbox |
|---|---|---|
| `posSlice` … `psSkirtBrim` | Slicing worker, live `Print` / `PrintObject` | **Unsandboxed in-process.** No per-run prompt. The Home card says so. |
| `psGCodePostProcess` | After G-code export | Audited. Write root is the exported G-code's folder only. UltraNet `{data_dir}/plugins` is denied. |

This is the documented exception from the plan: early steps get the live graph, so they cannot
be spawned out of process in v1.

## Trust

A pipeline plugin must pass the Phase 5 check (signed by a trusted publisher, or pinned locally)
and the tree hash is re-checked before load **and** before `execute_capabilities_from_refs`
runs it.

Chosen semantics (documented because the plan allowed either):

| Situation | Slice / export |
|---|---|
| Plugin missing, inactive, or disabled | **Skip with a warning** (`BOOST_LOG_TRIVIAL(warning)` plus **stderr** on CLI), matching upstream `execute_capabilities_from_refs`. Headless `--slice` does **not** fail: that is the same skip-with-warning as the GUI. The GUI also warns on preset save / 3MF open (upstream #16929). |
| Plugin present but **untrusted** | **Fail** with a clear `SlicingError` / `ExportError`. Edge does not silently skip an untrusted package. |

Pipeline plugins cannot pop permission dialogs off the UI thread. Phase 4's worker-thread
deny-first behaviour is unchanged: an audited event on the slicing worker is denied and the
grant, if any, is posted asynchronously.

## Presets and 3MF

`slicing_pipeline_plugin` (capability names) and `plugins` (`name;uuid;capability` manifest)
are print options. They survive a project 3MF round-trip. `ConfigBase::collect_plugin_manifest`
/ `update_plugin_manifest` / `unresolved_plugin_capabilities` rewrite the manifest when a
resolver is installed (GUI). Headless / CLI leave the stored `plugins` values as-is.

## Retry (upstream #17095)

Already on Edge: a post-process or plugin failure is an `ExportError`.
`SlicingProcessCompletedEvent::invalidate_plater()` is false, so Slice and Print can retry
without wiping the plate.

## Known limitations

- Early slicing steps run **in-process and unsandboxed**. A trusted plugin can still corrupt
  the slice graph.
- A trusted plugin at `psGCodePostProcess` can rewrite the G-code inside the export folder.
- Phase 5 **evaluate-then-run TOCTOU** residual: the tree hash is re-checked in
  `execute_capabilities_from_refs` immediately before dispatch, but the files can still change
  between that `evaluate()` and the Python import / `execute()`.
- U1 test prints are required before any pipeline plugin is claimed to work.
- Smoke tests owed at merge: Mac GUI, Windows, Linux pkg, Flatpak. The Mac re-gate result is owed from Russ.

## Ported upstream PRs

- Pipeline capability + print-graph / post-process hooks (Orca slicing-pipeline work).
- #16929 — warn on preset save or 3MF open when a referenced plugin is missing.
- #17095 — Slice and Print can retry after a post-process or plugin failure (Edge already had
  the `ExportError` / `invalidate_plater` path; this phase adds the plugin call site).

Left out: #16941 infill plugins, printer-agent hooks, catalog.
