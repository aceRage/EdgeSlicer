# EdgeSlicer command-line reference

EdgeSlicer is both a GUI application and a headless slicer. The same executable does both: start it
with no action on the command line and the window opens; give it an action such as `--slice` and it
runs headless, writes its results and exits.

This reference is written from the source (`src/Snapmaker_Orca.cpp`, the `CLIActionsConfigDef`,
`CLITransformConfigDef` and `CLIMiscConfigDef` tables in `src/libslic3r/PrintConfig.cpp`, and the
error codes in `src/libslic3r/Utils.hpp`). Examples marked **verified** were run against the installed
release build 2.4.8.0 (commit `cc089f741d`) with a scratch data directory. This document describes
`main` at commit `6558ae3f60`; anything newer than the 2.4.8.0 build is described from source only and
is marked **source only**.

Options added by the "CLI gaps" change (`--list-presets`, `--list-exit-codes`, `--validate`, `--quiet`,
`--export-gcode`, the richer `result.json`, and the fixes to `--help`, `--outputdir` and `--export-3mf`)
are marked **new** and were verified against a test build of that change.

Contents

1. [Overview and invocation](#1-overview-and-invocation)
2. [Option reference](#2-option-reference)
3. [How settings are loaded](#3-how-settings-are-loaded)
4. [Plates and objects](#4-plates-and-objects)
5. [Outputs](#5-outputs)
6. [Exit codes](#6-exit-codes)
7. [Worked examples](#7-worked-examples)
8. [EdgeSlicer-specific behaviour](#8-edgeslicer-specific-behaviour)

---

## 1. Overview and invocation

### Binary

| Platform | Command |
|---|---|
| Windows | `EdgeSlicer.exe` in the install folder |
| macOS | `EdgeSlicer.app/Contents/MacOS/EdgeSlicer` |
| Linux | the `EdgeSlicer` binary (or the AppImage) |

There is one binary on every platform. The Windows executable is a GUI-subsystem program: run it from
a console and its standard output and standard error can be redirected to files or pipes (verified).
`--help` can be captured too (**new**; before that change its text went to the console instead, see
[section 2.1](#21-actions)).

### Syntax

```
EdgeSlicer [OPTIONS] [input files ...]
```

* Options start with `--` (a single `-` is also accepted). Option names use dashes
  (`--load-settings`); the underscore spelling used in the source is not recognised.
* A value is the next argument (`--slice 1`) or follows an equals sign (`--slice=1`). An unknown option
  prints `Invalid option --name`, prints the help, and exits with `-2`.
* **Boolean options take no value token.** `--no-thumbnails` switches the option on. `--no-thumbnails=1`
  is accepted. `--no-thumbnails 1` is wrong: the `1` becomes an input file and the run ends with
  "No such file" (`-3`). This is verified. The `option` placeholder that the help text prints after some
  boolean options is misleading for the same reason.
* List options take their entries in one argument separated by semicolons
  (`--load-settings "machine.json;process.json"`) or, for integer lists, by commas (`--skip-objects 3,5`).
  Repeating a list option appends to it.
* A bare `--` ends option parsing; everything after it is an input file.
* Anything that is not an option is an input file: `.3mf`, `.stl`, `.obj`, `.step`/`.stp`, or another
  format the importer knows. ZIP archives are refused (`-2`): open them in the GUI or extract them first.
  Relative input paths are resolved against the directory you ran the command in.
* Order matters for **actions** and **transforms**. They run in the order they first appear on the
  command line. Everything else (misc options, print-setting overrides) is order independent.

### GUI or headless?

The program starts the GUI when the command line contains **no action** (nothing from the Actions
table in 2.1) and no `--downward-check`. Misc options and transforms alone do not make a run headless:
`EdgeSlicer.exe --outputdir out model.stl` opens the window. Always give at least one action, usually
`--slice`. (`--min-save`, `--uptodate`, `--info`, `--export-3mf` and all other entries of the Actions
table count as actions.)

### Output streams

* Status and diagnostic log lines are written to **standard output**, at a level set by `--debug`
  (default 1, errors). The `found error ... exit` line a failing run prints goes there too. Scripts that
  parse standard output (`--export-settings -`, `--info`, `--inspect-mesh`, `--list-presets`) should add
  **`--quiet`** (**new**): it sends the log lines and the `found error` line to standard error, so
  standard output holds only the command's payload. (Builds before the change also printed a stray
  `Initializing StaticPrintConfigs` trace line first, which no option could silence; it is gone.)
* Errors and usage problems go to standard error.
* `--progress-json` adds machine-readable JSON lines on standard output (section 5.4).

### Always use a scratch data directory in scripts

`--datadir <dir>` redirects everything the program reads and writes in its data directory: user presets,
logs and settings. Point it at a fresh folder in tests and CI so a run never
touches an interactive install:

```
EdgeSlicer.exe --datadir C:/work/scratch/datadir --slice 0 ...
```

---

## 2. Option reference

There are 85 CLI-specific options in three groups, plus every print setting (section 2.4).

| Group | Count | Table |
|---|---|---|
| Actions | 26 | 2.1 |
| Transform | 13 | 2.2 |
| Misc | 46 | 2.3 |
| Print-setting overrides | about 715 | 2.4 |

The counts were taken from the option definition tables in the source. Entries that exist only as
commented-out code (for example `export_obj`, `export_amf`, `copy`, `cut`, `split`, `scale_to_fit`,
`repair`, `center`, `align_xy`, `help_fff`, `help_sla`, `gcodeviewer`) are not options and are not listed.

"Type" is the option's value type: bool takes no value token, int/float take one number, string takes one
string, list takes a semicolon-separated string, int list takes a comma-separated list. "Default" is
the value used when the option is not given. For options that need a value, the default only matters for
the documentation; the action runs only when the option is present.

### 2.1 Actions

Actions say what the run should do. A run with at least one action is headless.

| Option | Value | Type | Default | Description and notes |
|---|---|---|---|---|
| `--help`, `-h` | | bool | off | Prints the usage and option list, then continues with the other actions. The text goes to standard output, so it can be captured by a pipe or file redirect (**new**: on Windows, builds before the change attached a console and wrote there instead, which left a redirected file empty). When both streams are redirected no console is created. |
| `--slice` | `N` | int | 0 | Slice plates. `0` slices all plates, `N` slices plate N (1-based). Any other value makes the run fail with `-2` ("invalid plate id"). For input that is not a project 3MF the value is reset to 0 (there is only the generated plate set). With `--outputdir` the G-code goes to `<outputdir>/plate_<N>.gcode`, or to the name chosen with `--export-gcode`. |
| `--export-gcode` | `filename.gcode` | string | empty | **New.** The G-code file name for each sliced plate; needs `--slice` (`-2` otherwise, also for an empty name). A relative name goes under `--outputdir`, an absolute one is used as given, and missing folders are created. The token `{plate}` stands for the 1-based plate number. Without the token, slicing more than one plate (`--slice 0` on a project with several plates) writes `<name>_<N>.<ext>` (`out.gcode` becomes `out_1.gcode`, `out_2.gcode`); one plate keeps the name exactly. The 3MF export embeds these files. Example: `--export-gcode "p{plate}.gcode"`. |
| `--validate` | | bool | off | **New.** Load the input, check preset compatibility and run the per-plate checks that precede slicing, then exit without slicing. See section 2.1.2. |
| `--list-presets` | | bool | off | **New.** Print the system and user presets as JSON on standard output and exit. See section 2.1.3. |
| `--list-exit-codes` | | bool | off | **New.** Print the exit code table and exit. See section 6. |
| `--export-3mf` | `filename.3mf` | string | `output.3mf` | Writes the project as a 3MF at the end of the run, including the G-code of every plate sliced in this run. With `--outputdir` a relative name is placed in that directory; an absolute name is used as given (**new**: it used to be appended to the directory). See section 5.3. Combine with `--slice` to get sliced plates inside. Without `--slice` the project is exported unsliced. |
| `--min-save` | | bool | off | Export the 3MF without the model geometry (`SkipModel`), for a smaller file. Use with `--export-3mf`. Verified: this reduces a one-object 3MF from about 279 KB to about 110 KB. |
| `--export-bambu-3mf` | | bool | off | Write the file given to `--export-3mf` in Bambu Studio's format (like File > Export > Export Bambu 3MF). Settings Bambu Studio does not have are left out and the rest are converted to its names and types. A one-line summary is printed on standard output. |
| `--export-stl` | | bool | off | Export every object as its own STL under `<outputdir>/stl/obj_<index>_<name>.stl` (the folder is created). Runs before slicing, in command-line order. |
| `--export-stls` | `dir` | string | `stl_path` | Same, but into the given directory, as `<dir>/obj_<index>_<name>.stl`. |
| `--export-step` | `filename.step` | string | `output.step` | Export all objects, all instances, as one STEP file of solids. Parts that were imported from STEP keep their exact geometry; mesh parts are written as faceted solids. A relative name is placed under `--outputdir` when one is given. A summary line is printed. Failure exits with `-25`. |
| `--export-settings` | `file.json` | string | `output.json` | Write the effective project settings (printer, process and filament merged, with command-line overrides) as JSON. `-` writes to standard output (verified; see 2.1.1). |
| `--export-slicedata` | `dir` | string | `cached_data` | Export slicing data to a directory for later reuse. Cannot be combined with `--load-slicedata` (`-2`). |
| `--load-slicedata` | `dir` | list | `cached_data` | Load cached slicing data from a directory. Cannot be combined with `--export-slicedata` or `--repetitions`. |
| `--info` | | bool | off | Print each model's statistics on standard output: size, bounding box, facet count, manifold flag, number of parts, volume. Works on the unrepaired model. Verified. |
| `--inspect-mesh` | | bool | off | Print a JSON summary of each loaded object on standard output, then exit: bounding boxes, plus the convex-hull faces an object can be laid on, with normals, areas and centres. These are the faces the `--ground-*` options choose from. Cannot be combined with `--progress-json`, `--hub`, `--hms-lookup` or any other action. Needs an input file or `--load-assemble-list`. Verified. |
| `--inspect-paint` | | bool | off | Print a JSON summary of every painted layer already on the loaded model (supports, seam, multi-colour, fuzzy skin): per-state facet count, surface area and mesh-local bounding box. Same restrictions as `--inspect-mesh`. Read only. |
| `--uptodate` | | bool | off | For a project 3MF: refresh the 3MF's configuration values to the latest ones, using system presets supplied with `--uptodate-settings` and `--uptodate-filaments`. |
| `--load-defaultfila` | | bool | off | When some entries of `--load-filaments` are empty, use the first loaded filament in their place. See section 3.3. |
| `--normative-check` | | bool | on | Check "normative" items on a project 3MF. At present this refuses a 3MF whose settings contain post-processing scripts (`-19`). The default is on; there is no way to say "off" other than `--normative-check=0`. |
| `--no-check` | | bool | off | Skip the validity checks that run after slicing, such as the G-code path conflict check. Cannot be combined with `--strict`. |
| `--strict` | | bool | off | Turn non-critical warnings that are otherwise only logged into a failed run (non-zero exit). The only such warning today is "model needs support but support is disabled". The warnings are listed with a stable `class` in `result.json`'s `warnings` array. `strict_mode` is recorded in `result.json`. |
| `--mtcpp` | `count` | int | 1000000 | Maximum triangle count per plate. A plate over the limit fails with `-59`. |
| `--mstpp` | `seconds` | int | 300 | Maximum slicing time per plate in seconds. A plate over the limit fails with `-58`. |
| `--pipe` | `pipename` | string | empty | Send slicing progress to a named pipe. Only effective on Linux; on other platforms the option is accepted and ignored. Use `--progress-json` for a cross-platform alternative. |

#### 2.1.1 `--export-settings -`

With `-` the JSON is written to standard output, so everything that would also write there is refused:
`--progress-json`, `--info`, `--help`, `--orient`, slicing and the other exporters (`-2`). Allowed
companions are `--uptodate`, `--load-defaultfila`, `--min-save`, `--mtcpp`, `--mstpp`, `--no-check`,
`--strict`, `--normative-check` and `--pipe`. The log lines described in section 1 still appear in front
of the JSON. Verified: the output is a JSON object keyed by setting name, about 67 KB for an X1C preset set.

#### 2.1.2 `--validate` (new)

`--validate` runs everything a slice runs before it starts slicing, and stops there: it loads the input,
applies the transforms, switches the presets, checks their compatibility, and then validates every plate
(objects inside the volume, something printable on the plate, filaments against the bed type, filament
temperature spread, object and tower collisions, spiral vase settings, and the other `Print::validate`
rules). It writes no G-code and no 3MF.

* The exit codes are the ones the same run with `--slice` would have returned (`-3`, `-4`, `-5`, `-16`,
  `-17`, `-50` to `-65` and so on); `0` means every plate passed and would have been sliced.
* `--slice N` picks the plate to check, `0` (the default) checks all plates. Alone, `--slice` slices;
  together with `--validate` it only selects.
* It can be combined with `--uptodate`, `--load-defaultfila`, `--min-save`, `--mtcpp`, `--mstpp`,
  `--no-check`, `--strict`, `--normative-check`, `--pipe` and with transforms. Any action that writes a file
  (`--export-3mf`, `--export-gcode`, `--export-settings` and so on) is refused with `-2`.
* `result.json` gets `"validate_only": true`; each checked plate appears in `sliced_plates` with
  `"validated": true`, its `triangle_count`, its `bounding_box` and any validation warnings (also listed in
  the top-level `warnings` array with `class` `validation_warning`). It has no timing or usage numbers and
  no `totals`.
* It takes seconds, not the minutes slicing takes, because nothing is sliced. Checks that need the sliced
  result (G-code path conflicts, the estimates) are not run.

#### 2.1.3 `--list-presets` (new)

Prints a JSON document on standard output and exits with `0`. It lists the **system** presets of the
installation (`resources/profiles`) and the **user** presets of the data directory (`--datadir`), by type:

```
{
  "datadir": "...",
  "machine":  [ { "name", "vendor", "source", "inherits", "model", "variant" }, ... ],
  "process":  [ { "name", "vendor", "source", "inherits",
                  "compatible_printers", "compatible_printers_condition" }, ... ],
  "filament": [ { ... the same, plus "filament_id" and "filament_type" }, ... ]
}
```

* `vendor` is the profile bundle that lists the preset (`BBL`, `Snapmaker`, `OrcaFilamentLibrary`, ...);
  for a user preset it is the vendor of the system preset it inherits. `source` is `system` or `user`.
  `inherits` is the parent preset's name (empty for a base preset).
* `compatible_printers` (a list of printer preset names) and `compatible_printers_condition` (an expression
  on the printer's settings) are the preset's own declaration. A filament or process preset fits a printer
  when its list names the printer, or its list is empty and the condition holds (or both are empty).
* Within a type the entries are sorted by vendor and name.
* **Filter by printer:** `--list-presets --printer-preset "<printer name>"` (system or user printer) returns
  only that printer under `machine`, and under `process` and `filament` only the presets that are compatible
  with it, each marked `"compatible": true`; the document gains `"printer"`. Only the printer's own vendor and
  `OrcaFilamentLibrary` are searched, which is where its compatible presets live; user presets are included.
  An unknown printer name prints `preset not found: <name>` on standard error and exits `-5`.
* It takes no input files, transforms or other actions (`-2`) and writes no `result.json`.

### 2.2 Transform

Transforms change the loaded objects. They run in command-line order, so a rotation given before
`--ground-largest-face` is respected by it.

| Option | Value | Type | Default | Description and notes |
|---|---|---|---|---|
| `--arrange` | `N` | int | 0 | `0` do not arrange, `1` always arrange, any other value keeps the automatic behaviour. Automatic: models loaded from STL, OBJ and similar are arranged, a project 3MF keeps its plate layout. With `--arrange 0` an STL keeps its own coordinates, so a model that does not sit on the bed fails with `-50` (verified). Failure to arrange exits with `-21`. |
| `--orient` | `N` | int | 0 | `0` disable, `1` auto-orient every object, any other value keeps the default behaviour (no auto-orient). Failure exits with `-22`. `--orient 1` runs after all transforms and replaces any orientation they chose. |
| `--repetitions` | `count` | int | 1 | Repeat the whole model `count` times on the plate being sliced. Requires `--slice N` with a single plate: with `--slice 0` the run fails with `-2`. Cannot be combined with `--load-slicedata`. |
| `--assemble` | | bool | off | Arrange all supplied models on a plate and merge them into one object (named "Assembly"), so later actions act on it once. Needs at least one input model and cannot be combined with `--clone-objects`. |
| `--convert-unit` | | bool | off | If a model looks like it was saved in metres or inches, convert it to millimetres. |
| `--ensure-on-bed` | | bool | off | Lift objects that are partly below the bed onto the bed. |
| `--rotate` | `deg` | float | 0 | Rotate every object around Z by the angle in degrees. |
| `--rotate-x` | `deg` | float | 0 | Rotate around X. |
| `--rotate-y` | `deg` | float | 0 | Rotate around Y. |
| `--ground-largest-face` | | bool | off | Lay each object on the largest face of its convex hull and drop it onto the bed. Of equally large faces, the one already facing down is kept. Objects without a face large enough to rest on are left as they are. |
| `--ground-face-normal` | `NX,NY,NZ` | string | empty | Lay each object on the convex-hull face whose outward normal is closest to the given direction (object coordinates). `1,0,0` stands the object on its +X side. |
| `--ground-face-point` | `X,Y,Z` | string | empty | Lay each object on the convex-hull face that contains the point (object coordinates; `--inspect-mesh` reports face centres in them). The run fails with `-2` if no object has such a face. |
| `--scale` | `factor` | float | 1 | Scale the model by a factor. A factor of zero or less fails with `-2`. |

### 2.3 Misc

#### Presets, settings and filaments

| Option | Value | Type | Default | Description and notes |
|---|---|---|---|---|
| `--load-settings` | `"a.json;b.json"` | list | empty | Load a machine (printer) and a process settings file, at most one of each type. Each file must carry `name`, `from` (`system`, `User` or `user`) and `type` (`machine` or `process`). A second file of the same type fails with `-5`. |
| `--load-filaments` | `"f1.json;f2.json;..."` | list | empty | Load filament settings, one file per filament slot, in order. An empty entry (for example `"f1.json;;f3.json"`) leaves that slot to `--load-defaultfila` or the 3MF. Files must be `type: filament`. |
| `--printer-preset` | `name` | string | empty | Use a named printer preset instead of a `--load-settings` machine file. With `--list-presets` it is the printer to filter by instead (section 2.1.3). System presets come from `resources/profiles`, user presets from the data directory. An unknown name prints `preset not found` and exits `-5`. |
| `--process-preset` | `name` | string | empty | Same, for the process preset. |
| `--filament-presets` | `"n1;n2;..."` | list | empty | Same, for filaments, in extruder order. |
| `--uptodate-settings` | `"a.json;b.json"` | list | empty | System machine/process files that `--uptodate` should use. The file's name must match the system preset the 3MF inherits. |
| `--uptodate-filaments` | `"f1.json;..."` | list | empty | System filament files for `--uptodate`. |
| `--downward-check` | | bool | off | Check whether the current machine is downward compatible with the machines in `--downward-settings`. Counts as an action: it keeps the run headless on its own. |
| `--downward-settings` | `"m1.json;m2.json;..."` | list | empty | The machine files `--downward-check` compares against. |
| `--load-custom-gcodes` | `file.json` | string | empty | Load custom G-code (colour-change and similar entries) from JSON and apply it to the plate being sliced (plate 1 when slicing all). |
| `--enable-timelapse` | | bool | off | Slice as if a timelapse were enabled. |
| `--no-filament-prices` | | bool | off | Leave prices out of the G-code (`; filament cost`, `; total filament cost`, and `filament_cost`/`time_cost` in the config block) and out of the settings of an exported 3MF. Without it the CLI writes them. |

#### Objects, plates and arranging

| Option | Value | Type | Default | Description and notes |
|---|---|---|---|---|
| `--skip-objects` | `"3,5,10,77"` | int list | empty | Skip the listed objects when slicing (by object id as the project assigns them). A plate left with nothing fails with `-60`. |
| `--clone-objects` | `"1,3,1,10"` | int list | empty | Clone the objects of each input file that many times. The list must have one entry per input file and needs `--load-filaments`. Not for a 3MF input. At most 512 per file. |
| `--load-filament-ids` | `"1,2,3,1"` | int list | empty | Assign a filament slot to the objects of each input file (0 means leave alone). One entry per input file, needs `--load-filaments`, not for a 3MF input. |
| `--load-assemble-list` | `assemble_list.json` | string | empty | Build the plates from a JSON assemble list instead of input files. Cannot be mixed with input files or transforms. Mismatched plate counts fail with `-2`. |
| `--allow-multicolor-oneplate` | | bool | **on** | Arrange may put objects of several colours on one plate. |
| `--allow-rotations` | | bool | off | Arrange may rotate objects. Off by default in the CLI. |
| `--align-to-y-axis` | | bool | off | Turn each object so its long side runs along Y before placing it. Only an explicit option overrides the default, which is on for i3 printers and off for the others (as in the GUI). |
| `--avoid-extrusion-cali-region` | | bool | off | Arrange avoids the extrusion-calibration region. |
| `--skip-modified-gcodes` | | bool | off | Ignore G-code that was modified in the 3MF's printer or filament presets. |

#### Input and output

| Option | Value | Type | Default | Description and notes |
|---|---|---|---|---|
| `--datadir` | `dir` | string | empty | Load and store settings in this directory. See section 1. |
| `--outputdir` | `dir` | string | empty | Where result files go: `plate_<N>.gcode`, `result.json`, the 3MF, STEP and STL exports. Empty means the current directory for `result.json` and temporary files for G-code (section 5). The directory is created when it does not exist (**new**; it used to have to exist). |
| `--debug` | `level` | int | 1 | Log level: 0 fatal, 1 error, 2 warning, 3 info, 4 debug, 5 trace. |
| `--quiet`, `-q` | | bool | off | **New.** Send log lines and the `found error ... exit` line to standard error instead of standard output, so `--info`, `--inspect-mesh`, `--inspect-paint`, `--export-settings -`, `--list-presets` and `--list-exit-codes` print only their payload on standard output. Does not change the log level, `result.json` or `--progress-json`. |
| `--progress-json` | | bool | off | JSON lines for progress and the final result on standard output (section 5.4). |
| `--no-thumbnails` | | bool | off | Do not render plate thumbnails when exporting a 3MF (no OpenGL context needed). Recommended on servers and CI. |
| `--allow-newer-file` | | bool | off | Allow a 3MF written by a newer version to be sliced. The environment variable `SNAPMAKER_ORCA_ALLOW_NEWER_FILE` set to anything other than `0` does the same. |
| `--makerlab-name` | `name` | string | empty | MakerLab name recorded in the 3MF. |
| `--makerlab-version` | `version` | string | empty | MakerLab version recorded in the 3MF. |
| `--metadata-name` | `"n1;n2;..."` | list | empty | Metadata names to add to the 3MF. Must have as many entries as `--metadata-value` (`-2` otherwise). |
| `--metadata-value` | `"v1;v2;..."` | list | empty | Their values. |
| `--single-instance` | | bool | off | GUI launches only: send the arguments to a running instance, or activate its window. |

#### Application helpers (not slicing)

These options are for the desktop app's background features and for testing. None of them slices.

| Option | Value | Type | Default | Description and notes |
|---|---|---|---|---|
| `--hub` | | bool | off | Run the phone-access and camera-relay helper process for this data directory instead of the slicer. The slicer starts it automatically when needed. |
| `--hub-token` | `token` | string | empty | With `--hub`: the token for the phone link, used only when the data folder has none yet. |
| `--hub-phone` | | bool | off | With `--hub`: start with phone access (the LAN listener) enabled. |
| `--no-hub` | | bool | off | Use a hub that is already running as a client, but never start, restart or take over the hub. Also set by `EDGESLICER_NO_HUB=1` or a file named `edgeslicer_no_hub` next to the executable. |
| `--take-hub` | | bool | off | Normal hub behaviour even when the environment variable or marker file asks for none. Also set by `EDGESLICER_TAKE_HUB=1`. |
| `--hidden` | | bool | off | Start the GUI without a window or taskbar button. |
| `--hms-lookup` | `serial:code[:lang]` | string | empty | Print a printer's own text for an error code, from the local tables, and exit (development and test). Prints `HMS_*=` lines on standard output. |
| `--hms-add` | `code description` | list | empty | Record your own description for an error code in `<datadir>/hms/overrides.json` and exit. See `docs/hms-overrides.md`. |
| `--hms-add-lang` | `lang` | string | empty | With `--hms-add`: language of the description (default `en`). |
| `--hms-add-model` | `series` | string | empty | With `--hms-add`: the first three characters of the serial the text applies to (default all printers). |
| `--hms-add-force` | | bool | off | With `--hms-add`: replace an existing recorded description. |
| `--migrate-datadir-test` | `dir` | string | empty | Development and test only: run the first-start data directory migration against a scratch root and exit. |
| `--showcase-video-test` | `file` | string | empty | Development and test only: write a short test video (`.mp4`, `.webm`, `.gif`, `.webp`, or a folder of PNG frames) and exit. Cannot be combined with other actions. |

#### Other command-line modes

Two more modes are recognised before the option parser runs. They are not options in the tables:

* `--showcase-render <project.3mf> --out <file>` renders a Showcase video or still without a window.
  Options: `--path`, `--plate`, `--size`, `--aspect`, `--fps`, `--format`, `--quality`, `--bitrate`,
  `--still`, `--report`, `--slice`, `--dry-run`, `--datadir`. Exit codes: 0 done, 2 bad arguments,
  3 invalid path, 4 project or plate problem, 5 render failed, 6 encoder failed. Nothing else on the
  command line runs with it. Path format: `docs/showcase_paths.md`. **Source only.**
* `--fix-firewall` (Windows) is the elevated helper that the Help menu's firewall check starts. It is not
  meant to be called by hand.

### 2.4 Print-setting overrides

Every print setting that is not marked internal can be given on the command line as `--<setting-name>
<value>`, with underscores written as dashes. About 715 of the roughly 785 setting definitions accept this
(the rest are marked no-CLI in `PrintConfig.cpp`). Examples: `--layer-height 0.28`,
`--sparse-infill-density 15%`, `--wall-loops 3`, `--nozzle-temperature 215`.

* The value format is the same as in a preset JSON file (percentages as `15%`, lists as in the
  preset, booleans as `0`/`1`). Vector settings (per filament or per extruder) take a semicolon-separated
  list.
* Command-line values have the **highest priority**: they override the loaded presets and the 3MF.
* The exact accepted names are the keys in a preset file or in the output of `--export-settings`.
* A boolean setting given without a value is switched on (`--enable-prime-tower`); to switch off, write
  `--enable-prime-tower=0`.
* Verified: `--layer-height 0.28 --sparse-infill-density 15%` showed up as `; layer_height = 0.28` and
  `; sparse_infill_density = 15%` in the sliced G-code, against `0.2` from the preset.

---

## 3. How settings are loaded

The help text states the rule in one sentence, and it holds:

1. Settings given on the command line (highest priority)
2. Settings loaded with `--load-settings`, `--load-filaments` and the `--*-preset` options
3. Settings stored in the 3MF (lowest priority)

### 3.1 Input files first

Input files are read first, in order. A project 3MF (one that carries project settings) must be the
**first** input file; if it is not, the run fails with `-4`. It supplies the printer, process and
filament presets and the plate layout. A 3MF that carries no project settings (a geometry-only file such as the
bundled handy models) is treated as a plain model. STL, OBJ, STEP and later 3MFs are plain models: only
their geometry is used. Models are positioned and arranged afterwards.

For a project 3MF, the preset names are read from its `printer_settings_id`, `print_settings_id` and
`filament_settings_id`, and the system parents from `inherits_group`. Keys the 3MF does not carry (a
project saved before a setting existed) are filled from its system presets, the way the GUI does it,
for printer and process settings.

A 3MF written by a newer minor version than the CLI is refused with `-24` unless `--allow-newer-file`
(or the environment variable above) is given.

### 3.2 Overriding the printer, process and filaments

* **Machine and process:** pass them with `--load-settings` (JSON files), or by name with
  `--printer-preset` and `--process-preset`. Named presets are turned into temporary flat settings files
  that join the same lists, so both routes behave identically. Giving the same type twice fails with `-5`.
* **Filaments:** `--load-filaments` or `--filament-presets` give one filament per slot, in extruder order.
  For a project 3MF they replace the 3MF's filaments slot by slot. If every loaded slot is the same
  filament, the wipe tower is disabled after the mapping.
* **Compatibility:** when you switch the printer of a 3MF, the process in the 3MF must be compatible with
  the new printer, or the run fails with `-17`. Switching to a printer the 3MF does not list as upward
  compatible fails with `-16`. With `--uptodate`, a 3MF whose printable size exceeds the system
  printer's is rejected with `-23`.
* **User presets:** a settings file whose `from` is `User`/`user` is applied on top of the system preset
  named by its `inherits`. System files have `from: system` and are used as they are.
* **Preset lookup by name:** system presets are read from the `resources/profiles` bundles of the
  installation; user presets from `<datadir>/user/*/{machine,process,filament}`. Inheritance (`inherits`)
  is flattened, including the shared filament library.
* **Technology:** only FDM is supported. A resin (SLA) file or preset fails with `-7`.

### 3.3 `--load-defaultfila`

Lists like `--load-filaments "a.json;;c.json"` may leave slots empty. Without `--load-defaultfila` an
empty slot keeps whatever the 3MF already had for it. With it, the first non-empty entry is used
for every empty slot. If the list has entries but none can be loaded the run fails with `-5`.
`--filament-presets` entries should be non-empty.

### 3.4 `--uptodate`

For a project 3MF, `--uptodate` re-reads the system machine and process files you supply with
`--uptodate-settings`, and system filaments with `--uptodate-filaments`, and refreshes the 3MF's values
from them. Each file's `name` must equal the system preset the 3MF was created from, otherwise the run
fails with `-5`.

---

## 4. Plates and objects

* **Plates exist for project 3MFs.** A 3MF carries its plates and the objects on each. `--slice 0` slices
  every plate, `--slice N` plate N. The plate count is in the log; an out-of-range N fails with `-2`.
* **Other input** (STL, OBJ, STEP, multiple files) is arranged onto the plate set that the printer
  profile defines. Arrangement may use more than one plate where the objects do not fit on one
  (**source only**; the verified examples use one object).
* **Arrange.** Plain models are arranged by default; a project 3MF keeps its layout. `--arrange 1` forces
  an arrange of a project, `--arrange 0` suppresses it (see 2.2). `--allow-rotations`,
  `--align-to-y-axis`, `--avoid-extrusion-cali-region` and `--allow-multicolor-oneplate` tune it.
* **Orient.** `--orient 1` runs the auto-orient on every object after the other transforms.
  `--ground-*` options lay an object on a chosen face and run in order with the other transforms.
* **Transforms apply to all objects** of all loaded models. There is no per-object selector; use
  separate runs or `--skip-objects`/`--clone-objects` for per-file differences.
* **`--assemble`** merges all input models into a single object before any later step.
* **Wipe tower.** When the prime tower is enabled, each plate's tower position is clamped into the
  printable area and moved off excluded bed regions before slicing, as the GUI does.
* **Print validation.** Each plate is validated before slicing. Objects outside the volume
  (`-52`), plates with nothing printable (`-50`), colliding objects in print-by-object mode (`-63`) and
  in normal mode (`-64`), filaments unsuited to the bed type (`-61`), too large a filament temperature
  difference (`-62`) and spiral-vase conflicts (`-65`) are reported with their own codes. `--no-check`
  skips the post-slicing checks only.

---

## 5. Outputs

### 5.1 G-code

* With `--outputdir D`: `D/plate_<N>.gcode`, where N is the 1-based plate number (verified).
  The directory is created if it is missing (**new**). `--export-gcode <name>` chooses the file name instead (section 2.1).
* Without `--outputdir`: the G-code is written to a temporary location and is only kept inside an exported
  3MF. If you need the `.gcode`, give `--outputdir`.
* Prices, cost lines and the config block follow `--no-filament-prices`.

### 5.2 The 3MF

`--export-3mf name.3mf` writes the project with its plates, presets, thumbnails and (when sliced in the
same run) the G-code of each sliced plate. Thumbnails are rendered unless `--no-thumbnails`.
`--min-save` omits geometry. `--export-bambu-3mf` writes Bambu Studio's flavour.

Whether the plate data is "sliced" depends on `--slice` in the same run: `--slice 0` marks all plates,
`--slice N` only plate N. Without `--slice` nothing is sliced and the export holds the unsliced project.

### 5.3 File names and paths

| Output | Name |
|---|---|
| G-code | `<outputdir>/plate_<N>.gcode`, or the `--export-gcode` name (relative names go under `--outputdir`; `{plate}` token or `_<N>` suffix for several plates) |
| Result file | `<outputdir>/result.json`, or `result.json` in the current directory without `--outputdir` |
| 3MF | `<outputdir>/<value of --export-3mf>`, or the value as given (relative to the current directory) without `--outputdir`. A relative name is placed under `--outputdir`; an absolute path is used as given (**new**; it used to be appended to the directory), and its folder is created. |
| STEP | the `--export-step` value, placed under `--outputdir` only when it is relative |
| STL (`--export-stl`) | `<outputdir>/stl/obj_<index>_<name>.stl` |
| STL (`--export-stls dir`) | `<dir>/obj_<index>_<name>.stl` |

### 5.4 `result.json` and `--progress-json`

`result.json` is written at the end of every headless run, success or failure, on every platform
(verified). Example from a successful slice (values shortened):

```json
{
    "error_string": "Success.",
    "export_time": 0,
    "plate_index": 0,
    "prepare_time": 0,
    "return_code": 0,
    "sliced_plates": [
        {
            "filament_g": 3.30,
            "filament_mm3": 2622.8,
            "gcode": "<outputdir>/plate_1.gcode",
            "id": 1,
            "sliced_time": 0,
            "sliced_time_with_cache": 0,
            "time_s": 927.0,
            "triangle_count": 15518,
            "warning_message": "",
            "warnings": []
        }
    ],
    "strict_mode": false,
    "warnings": []
}
```

* `return_code` and `error_string` repeat the exit status and its meaning. `plate_index` names the plate
  the run stopped on (0 for a failure before slicing).
* `time_s` is the estimated print time in seconds; `filament_g` and `filament_mm3` the estimated usage.
* **New keys (added in the CLI gaps change; every key above is unchanged).** Per sliced plate:
  `layers`; `filaments` (one entry per filament slot that extruded: `index`, `preset`, `type`, `colour`,
  `extruder`, `grams`, `mm`, `mm3`, `cost`, `price_per_kg`, `model_g`, `support_g`, `flush_g`, `tower_g`);
  `extruders` (the same summed per physical extruder: `index`, `filaments`, `grams`, `mm`, `mm3`, `cost`;
  the extruder of a filament comes from the plate's `filament_map`, 1 when there is none); `cost`
  (`material`, `machine`, `total`, `display_total` rounded to cents as the cost panel shows it,
  `prices_known`, `machine_rate_known`, `machine_rate_per_h`); `bounding_box` (`min`, `max`, `size` of the
  objects in mm, relative to the plate's origin). Top level: `presets` (`printer`, `process`, `filaments`:
  the preset names used) and `totals` (`plates`, `time_s`, `layers`, `filament_g`, `filament_mm`,
  `filament_mm3`, `cost`). `time_s` per plate is the estimated print time. Costs use the filament prices of
  the presets (and the printer's time cost); with `--no-filament-prices` they are 0. `--validate` adds
  `validate_only` and per-plate `validated` instead (section 2.1.2).
* `upward_compatible_machine` and `downward_compatible_machine` appear when the 3MF or the downward
  check supplied them.
* `warnings` is an array of structured warnings (each has a `class`; for example the precise-seam and
  support warnings). A non-empty array does not make the run fail, except for non-critical ones under
  `--strict`.

With `--progress-json`, standard output also carries one JSON object per line:

```
{"event":"progress","message":"Generating G-code: layer 25","plate_count":1,"plate_index":1,"plate_percent":80}
{"event":"result", ... the same object as result.json ... }
```

`progress` lines carry `plate_index`, `plate_count`, `plate_percent` (0-100 within the plate) and either
`message` or, for a warning, `warning`. The last line is the `result` event. Verified. Log lines may still
appear on standard output between JSON lines (when `--debug` is raised), so parse only lines that start
with `{`. `--progress-json` cannot be combined with `--export-settings -`, `--inspect-mesh` or
`--inspect-paint`.

### 5.5 Other output

`--info` prints to standard output (add `--quiet` to keep log lines off it); `--list-presets` prints JSON, `--list-exit-codes` a table or JSON; `--inspect-mesh` and `--inspect-paint` print JSON to standard output;
`--export-settings -` prints JSON to standard output; `--export-step` and `--export-bambu-3mf` print one
summary line. Everything else is written to files.

---

## 6. Exit codes

The process exit status is the code below. The codes are negative. On Windows `$LASTEXITCODE` and
`%ERRORLEVEL%` show the negative number (for example `-3`); on Linux and macOS the shell shows the code
modulo 256 (`-3` becomes 253). `result.json` always holds the exact number in `return_code`.

| Code | Name | Meaning |
|---|---|---|
| 0 | `CLI_SUCCESS` | Success. |
| -1 | `CLI_ENVIRONMENT_ERROR` | Failed setting up the environment (for example the UTF-8 file system layer). |
| -2 | `CLI_INVALID_PARAMS` | Invalid parameters: unknown option, missing or bad value, an invalid combination, a bad plate number, an unsupported ZIP input. |
| -3 | `CLI_FILE_NOTFOUND` | An input file, settings file or custom G-code file was not found. |
| -4 | `CLI_FILELIST_INVALID_ORDER` | A project 3MF is not the first input file. |
| -5 | `CLI_CONFIG_FILE_ERROR` | A preset or settings file is invalid, duplicated, of the wrong type, or a named preset was not found. |
| -6 | `CLI_DATA_FILE_ERROR` | An input model could not be parsed. |
| -7 | `CLI_INVALID_PRINTER_TECH` | Unsupported printer technology (not FDM), or inputs of mixed technology. |
| -8 | `CLI_UNSUPPORTED_OPERATION` | Unsupported instruction. |
| -9 | `CLI_COPY_OBJECTS_ERROR` | Failed copying objects. |
| -10 | `CLI_SCALE_TO_FIT_ERROR` | Failed scaling to fit. |
| -11 | `CLI_EXPORT_STL_ERROR` | STL export failed. |
| -12 | `CLI_EXPORT_OBJ_ERROR` | OBJ export failed. |
| -13 | `CLI_EXPORT_3MF_ERROR` | 3MF export failed. |
| -14 | `CLI_OUT_OF_MEMORY` | Out of memory during slicing. |
| -15 | `CLI_3MF_NOT_SUPPORT_MACHINE_CHANGE` | The 3MF does not allow a printer change. |
| -16 | `CLI_3MF_NEW_MACHINE_NOT_SUPPORTED` | The selected printer is not compatible with the 3MF. |
| -17 | `CLI_PROCESS_NOT_COMPATIBLE` | The process preset is not compatible with the selected printer. |
| -18 | `CLI_INVALID_VALUES_IN_3MF` | Invalid parameter values in the 3MF. |
| -19 | `CLI_POSTPROCESS_NOT_SUPPORTED` | Post-processing scripts are not supported in the CLI (normative check). |
| -20 | `CLI_PRINTABLE_SIZE_REDUCED` | The selected printer's bed is smaller than the profile's bed. |
| -21 | `CLI_OBJECT_ARRANGE_FAILED` | Auto-arrange failed. |
| -22 | `CLI_OBJECT_ORIENT_FAILED` | Auto-orient failed. |
| -23 | `CLI_MODIFIED_PARAMS_TO_PRINTER` | A modified printer parameter was found in the 3MF that should not be changed (or the printable size exceeds the system default, with `--uptodate`). |
| -24 | `CLI_FILE_VERSION_NOT_SUPPORTED` | 3MF written by a newer version than this one. |
| -25 | `CLI_EXPORT_STEP_ERROR` | STEP export failed. |
| -50 | `CLI_NO_SUITABLE_OBJECTS` | The plate is empty, or no object is both printable and fully inside the print volume. |
| -51 | `CLI_VALIDATE_ERROR` | Incorrect slicing parameters in the 3MF. |
| -52 | `CLI_OBJECTS_PARTLY_INSIDE` | Objects lie over the edge of the bed. |
| -53 | `CLI_EXPORT_CACHE_DIRECTORY_CREATE_FAILED` | Could not create the cache directory. |
| -54 | `CLI_EXPORT_CACHE_WRITE_FAILED` | Writing cache data (or `--export-settings`) failed. |
| -55 | `CLI_IMPORT_CACHE_NOT_FOUND` | Cache data not found. |
| -56 | `CLI_IMPORT_CACHE_DATA_CAN_NOT_USE` | Cache data cannot be parsed. |
| -57 | `CLI_IMPORT_CACHE_LOAD_FAILED` | Cache data failed to load. |
| -58 | `CLI_SLICING_TIME_EXCEEDS_LIMIT` | A plate exceeded `--mstpp`. |
| -59 | `CLI_TRIANGLE_COUNT_EXCEEDS_LIMIT` | A plate exceeded `--mtcpp`. |
| -60 | `CLI_NO_SUITABLE_OBJECTS_AFTER_SKIP` | Nothing printable remains after `--skip-objects`. |
| -61 | `CLI_FILAMENT_NOT_MATCH_BED_TYPE` | Filaments are not compatible with the plate type. |
| -62 | `CLI_FILAMENTS_DIFFERENT_TEMP` | The filaments' temperature difference is too large. |
| -63 | `CLI_OBJECT_COLLISION_IN_SEQ_PRINT` | Object conflicts in print-by-object mode. |
| -64 | `CLI_OBJECT_COLLISION_IN_LAYER_PRINT` | Object conflicts (including prime tower too close to an exclusion area). |
| -65 | `CLI_SPIRAL_MODE_INVALID_PARAMS` | Settings that cannot work with spiral vase mode. |
| -69 | `CLI_MIXED_FILAMENT_INVALID` | A mixed filament is invalid (components of different types, or none of its own). |
| -100 | `CLI_SLICING_ERROR` | Slicing failed. |
| -101 | `CLI_GCODE_PATH_CONFLICTS` | G-code path conflicts were detected after slicing. |

Verified: a missing input file gives `-3`, an unknown option `-2`, a bad plate number `-2`, a plate that
holds nothing printable `-50`.

`--list-exit-codes` (**new**) prints this table from the program itself: aligned columns, or with `--quiet` JSON
`{"exit_codes": [{"code", "name", "message"}, ...]}`. The messages are the ones `result.json`'s `error_string` uses.

`--showcase-render` has its own small set of exit codes (section 2.3).

---

## 7. Worked examples

Commands use Git Bash or a POSIX shell syntax. On Windows PowerShell the same arguments work; quote
names that contain spaces. The model is `OrcaToleranceTest.stl` from `resources/handy_models`. Replace
paths with your own. Every example uses a scratch data directory.

### 7.1 Slice an STL for a Bambu Lab X1 Carbon (verified)

```
EdgeSlicer.exe --datadir scratch/datadir \
  --printer-preset "Bambu Lab X1 Carbon 0.4 nozzle" \
  --process-preset "0.20mm Standard @BBL X1C" \
  --filament-presets "Bambu PLA Basic @BBL X1C" \
  --slice 0 --no-thumbnails --outputdir out/x1c model.stl
```

Result: `out/x1c/plate_1.gcode` and `out/x1c/result.json`, exit status 0. The run took about 7 seconds. The
output directory must already exist.

### 7.2 Slice an STL for a Snapmaker U1 (verified)

```
EdgeSlicer.exe --datadir scratch/datadir \
  --printer-preset "Snapmaker U1 (0.4 nozzle)" \
  --process-preset "0.20mm Standard @Snapmaker U1 (0.4 nozzle)" \
  --filament-presets "Snapmaker PLA Basic @U1" \
  --slice 0 --no-thumbnails --outputdir out/u1 model.stl
```

For a multi-tool machine, give one `--filament-presets` entry per tool you want to load: `"A;B;C;D"`.

### 7.3 Settings overrides (verified)

```
EdgeSlicer.exe --datadir scratch/datadir \
  --printer-preset "Bambu Lab X1 Carbon 0.4 nozzle" --process-preset "0.20mm Standard @BBL X1C" \
  --filament-presets "Bambu PLA Basic @BBL X1C" \
  --layer-height 0.28 --sparse-infill-density 15% --scale 1.5 --rotate 90 \
  --slice 0 --no-thumbnails --outputdir out/ov model.stl
```

The G-code shows `; layer_height = 0.28` and `; sparse_infill_density = 15%`.

### 7.4 Export a sliced 3MF (verified)

```
EdgeSlicer.exe --datadir scratch/datadir \
  --printer-preset "Bambu Lab X1 Carbon 0.4 nozzle" --process-preset "0.20mm Standard @BBL X1C" \
  --filament-presets "Bambu PLA Basic @BBL X1C" \
  --slice 0 --no-thumbnails --export-3mf project.3mf --outputdir out/x3 model.stl
```

Produces `out/x3/project.3mf` (with G-code inside), `out/x3/plate_1.gcode` and `out/x3/result.json`.
Without `--slice` the 3MF holds the unsliced project. Add `--min-save` for a smaller file without geometry
(verified, about 110 KB against about 279 KB), or `--export-bambu-3mf` for Bambu Studio's format.

### 7.5 Slice one plate of a project 3MF (verified)

```
EdgeSlicer.exe --datadir scratch/datadir --slice 1 --no-thumbnails --outputdir out/p1 project.3mf
```

The project's own printer, process and filament presets are used. Add `--printer-preset` or
`--load-settings` to switch the printer (subject to the compatibility checks in 3.2). Slicing plate 5 of a
one-plate project fails with `-2`.

### 7.6 Batch over a folder (verified)

```
mkdir -p results
for f in models/*.stl; do
  n=$(basename "$f" .stl)
  mkdir -p "results/$n"
  EdgeSlicer.exe --datadir scratch/datadir \
    --printer-preset "Bambu Lab X1 Carbon 0.4 nozzle" --process-preset "0.20mm Standard @BBL X1C" \
    --filament-presets "Bambu PLA Basic @BBL X1C" \
    --slice 0 --no-thumbnails --outputdir "results/$n" "$f" >/dev/null 2>&1
  echo "$n exit=$?"
done
```

Each run gets its own output directory, so each has its own `result.json`. Runs sharing one data
directory are fine for slicing, but do not run several at once against one data directory if any of them
writes user presets.

### 7.7 Machine-readable progress (verified)

```
EdgeSlicer.exe --datadir scratch/datadir --progress-json ... --slice 0 --outputdir out model.stl > progress.jsonl
```

Read `progress.jsonl` line by line; the last JSON line has `"event":"result"`.

### 7.8 Inspect a model and dump settings (verified)

```
EdgeSlicer.exe --datadir scratch/datadir --info model.stl
EdgeSlicer.exe --datadir scratch/datadir --inspect-mesh model.stl
EdgeSlicer.exe --datadir scratch/datadir --printer-preset "..." --process-preset "..." --filament-presets "..." --export-settings settings.json model.stl
```

### 7.9 Lay a model on a face and slice (source only)

```
EdgeSlicer.exe --datadir scratch/datadir ... --ground-face-normal 1,0,0 --slice 0 --outputdir out model.stl
```

### 7.10 Use the result in CI

Check the exit status; on failure read `result.json`'s `error_string`. Add `--strict` to fail on
warnings that otherwise only log. An example gate:

```
EdgeSlicer.exe --datadir "$RUNNER_TEMP/dd" ... --strict --slice 0 --no-thumbnails --outputdir out model.3mf
test "$(python -c "import json;print(json.load(open('out/result.json'))['return_code'])")" = 0
```

### 7.11 List presets, validate, name the G-code (new)

```
EdgeSlicer.exe --datadir scratch/datadir --quiet --list-presets --printer-preset "Bambu Lab X1 Carbon 0.4 nozzle" > presets.json
EdgeSlicer.exe --datadir scratch/datadir --list-exit-codes --quiet > exit_codes.json
EdgeSlicer.exe --datadir scratch/datadir --printer-preset "Bambu Lab X1 Carbon 0.4 nozzle" --process-preset "0.20mm Standard @BBL X1C"   --filament-presets "Bambu PLA Basic @BBL X1C" --validate --no-thumbnails --outputdir out/check model.stl
EdgeSlicer.exe ... --slice 0 --no-thumbnails --outputdir out/s --export-gcode "part_{plate}.gcode" model.stl
```

Verified on a test build: the filtered listing for the X1 Carbon returned 1 machine, 11 process and 506
filament presets in about 2 s (the unfiltered listing, with 998 machines, 2880 process and 7276 filament
presets, about 19 s). `--validate` on the sample model exited 0 and wrote `result.json` with
`validate_only` and the plate's `bounding_box`; with `--scale 20` it exited `-50`. The slice wrote
`part_1.gcode`, and `result.json` carried `layers` 32, one `filaments` entry (3.30 g, 1090 mm,
2623 mm3, cost 0.0826) and `totals`.

---

## 8. EdgeSlicer-specific behaviour

The CLI began as the Bambu Studio / OrcaSlicer CLI. The list below covers what EdgeSlicer added or
changed, taken from the source and its change history. Upstream OrcaSlicer may have adopted similar
features since; compare with its own documentation where it matters.

**Added options and outputs**

* `--printer-preset`, `--process-preset`, `--filament-presets`: presets by name, system or user, instead of
  JSON files.
* `--progress-json` and `result.json` on every platform (the original wrote the result file on Linux only),
  with per-plate time, filament use, G-code path and structured warnings.
* `--strict` and the `warnings` array with stable classes.
* `--no-thumbnails`, `--no-filament-prices`.
* `--list-presets`, `--list-exit-codes`, `--validate`, `--quiet`, `--export-gcode`, and the richer `result.json` (section 5.4).
* `--inspect-mesh`, `--inspect-paint`, `--export-step`, `--export-bambu-3mf`, `--export-settings -` (stdout).
* `--ground-largest-face`, `--ground-face-normal`, `--ground-face-point`, `--ensure-on-bed`,
  `--align-to-y-axis`.
* The application helper options (hub, HMS, test hooks) in section 2.3.
* `--showcase-render` and `--showcase-video-test`.

**Changed behaviour**

* `--allow-rotations` defaults to off in the CLI.
* A 3MF without project settings loads as a plain model, so geometry-only files (such as the bundled
  handy models) keep the presets from the command line.
* Keys missing from an old project 3MF are filled from its system presets.
* The newer-file version check compares against EdgeSlicer's own version and refuses only genuinely newer
  files; `--allow-newer-file` and `SNAPMAKER_ORCA_ALLOW_NEWER_FILE` override it.
* ZIP archives are refused from the command line.
* A prime tower left on an excluded bed region is moved clear of it, as the GUI does.
* The `-50` message names the real reason (empty plate, or nothing printable and inside the volume).
* Robustness fixes: filament slots grow to the number of presets passed; presets that set no filament
  diameter no longer crash; `result.json` is written even when a path is not valid UTF-8; progress
  callbacks are thread safe.

**Not provided**

* There is no `--export-obj`, `--export-amf`, `--cut`, `--split`, `--repair`, `--scale-to-fit` or
  `--center`: the code for some remains in the source but the options are disabled. Verified: `--cut 3` and `--scale-to-fit 10,10,10` both fail with `Invalid option` (`-2`). Their definitions in `PrintConfig.cpp` sit inside comment blocks, and the handlers in `Snapmaker_Orca.cpp` are unreachable (the `--cut` one only prints "Cut operation is not supported yet").

---

*Option counts: Actions 26, Transform 13, Misc 46 (85 CLI options); about 715 print-setting overrides.*
