# CLI options agents need for testing

Written 2026-11-28 with PR feat/cli-bed-type. Only gaps checked in `src/Snapmaker_Orca.cpp` and `src/libslic3r/Config.cpp` are listed.

| Need | State | Notes |
|---|---|---|
| Bed (plate) type | Done: `--bed-type <key>` | Keys: cool, engineering, high_temp, textured_pei, textured_cool, supertack, graphic_effect. Full names ("Textured PEI Plate") also work. Applies to every plate that slices, preset slices and `--load` projects alike (the CLI config is applied after the plate's own bed type). An unknown value exits with the key list. Before this PR the only way was the generic `--curr-bed-type "Textured PEI Plate"` (dashes, not underscores; `--curr_bed_type` is "Invalid option"), which gate.sh could not pass because it word-splits extra args. |
| Different bed type per plate | Gap | `--bed-type` is global: it also replaces a project plate's own bed type, so a multi-plate project cannot be sliced with plate 1 on Cool and plate 2 on PEI in one run. Slice plate by plate (`--slice N`) with different options in separate runs instead. |
| Other per-plate settings (print sequence, first/other layer sequence, spiral vase, filament map mode) | Gap | These live in the plate config of a project. Any matching CLI option is applied after the plate config (`new_print_config.apply(m_extra_config)`, Snapmaker_Orca.cpp ~6585), so it overrides every plate. A project cannot be sliced "as saved" for one plate and overridden for another in one run. |
| Plate selection | Works | `--slice N` slices plate N, `--slice 0` all plates. gate.sh preset cases always use plate 0 (one model, one plate); there is no preset-mode way to ask for several plates. |
| Filament mapping | Works for the Manual mode | `--filament-map-mode=Manual --filament-map=1,2` is used by the h2d/h2c gate cases. `--filament-volume-map` and `--filament-nozzle-map` are generic print options with the same dashed names; the gate never exercises them. |
| Nozzle volume type (standard / high flow) | Not exercised | `nozzle_volume_type` is a generic print option (`--nozzle-volume-type`), a vector of enums. No gate case or CLI test sets it, so whether the command line value parses and reaches the slice is unverified. |
| Extra args with spaces in gate.sh | Gap (by design) | gate.sh word-splits extra args (`$TEX`, `${e:-}`), so a value with spaces cannot be passed. Use short keys (as `--bed-type textured_pei` now allows) or add a short-key alias for the next such option. |
