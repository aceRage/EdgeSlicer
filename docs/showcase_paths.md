# Showcase paths and `--showcase-render`

A **path file** scripts a Showcase video: where the camera goes and when, plus timed changes to the
lights, backdrop, floor, callouts, watermark and the layer build-up. People write them by hand,
agents write them in a loop:

1. plan a path,
2. render it with `--showcase-render`,
3. check the report and the frames,
4. refine.

Two ways to use a path file:

- **In the Showcase tab:** Video, then Path file, then **Import...**. The moves become keyframes you
  can edit, and the timed changes play with the preview and the export. **Export...** writes the
  current video as a path file.
- **On the command line:** `--showcase-render` (below).

The code is in `src/libslic3r/Showcase/ShowcasePath.{hpp,cpp}` (parsing, compiling, the timeline)
and `ShowcaseRenderCli.{hpp,cpp}` (the arguments, the report). The unit tests are in
`tests/libslic3r/test_showcase_path.cpp`. Examples ship in `resources/showcase/paths/`.

## The file

```json
{
  "format": "edgeslicer-showcase-path",
  "version": 1,
  "name": "Hero orbit",
  "fps": 30,
  "start": "scene",
  "keyframes": [ ... ],
  "moves": [ ... ],
  "events": [ ... ]
}
```

| key | | |
|---|---|---|
| `format` | required | exactly `"edgeslicer-showcase-path"` |
| `version` | required | `1`. A file with a higher version is refused, with a message saying it needs a newer EdgeSlicer. |
| `name`, `description` | optional | text |
| `fps` | optional | 10 to 120. The command line's `--fps` wins over it. |
| `duration` | optional | 1 to 60 s. Default: as long as the path. A shorter duration cuts the path (with a warning). |
| `projection` | optional | `"perspective"` or `"ortho"`. Default: the scene camera's. |
| `start` | optional | Where the camera starts. Default `"scene"` (the scene's camera). Also `"default"` (front right, the plate's objects filling the frame), a view name (below), or a pose object `{ "eye": ..., "target": ..., "fov": ... }`. |

The rules:

- Times are seconds from the start of the video.
- Positions are world millimetres, the plate's coordinates; +Z is up.
- Keys that begin with `_` or `$` are comments.
- Any other unknown key is a warning that names the key you probably meant. It is never silently ignored.
- Every error names its place in the file, for example `moves[2].deg: expected a number`.

### Poses

A pose is `eye`, `target` and optionally `up` (default: as before, starting with +Z). It frames with
at most one of these:

| key | meaning |
|---|---|
| `fov` | Perspective. The vertical angle across the frame, in degrees (1 to 150). |
| `ortho_scale` | Orthographic. The height the frame shows at the target, in mm. |
| `zoom` | The camera's own zoom: pixels per mm of the frame. It depends on the frame size, so prefer `fov`. |

A pose with none of them keeps the field of view of the pose before it (the scale, in orthographic).
`fov` in an orthographic path, or `ortho_scale` in a perspective one, is an error.

### `keyframes`: absolute poses at times

```json
{ "t": 2.0, "eye": [0, -180, 60], "target": [0, 0, 25], "fov": 30, "ease": "ease_out", "hold": 1.0 }
```

| key | meaning |
|---|---|
| `t` (or `time`) | When the camera is there. Keys must be in order. Each key must come after the previous key plus its hold. |
| `ease` | How the camera speeds up and slows down on its way to this key: `linear`, `ease_in_out` (default), `ease_in` or `ease_out`. |
| `hold` | Seconds the camera stays here before moving on. |
| `spin` | How far the model has turned, in degrees, as a turntable turns it. The lights turn the other way. |
| `glide` | `true`: the camera passes through this key without stopping. A run of glide keys is eased as one move, at an even speed. |

Between keys the camera follows a centripetal Catmull-Rom spline, at an even speed along its length.

### `moves`: high-level moves, chained

Moves start where the keyframes end, or at 0 s from the start camera when there are none. Each move
starts where the one before it left the camera. Every move takes `duration` (seconds) and `ease`
(default `ease_in_out`).

| `op` | parameters | what it does | default duration |
|---|---|---|---|
| `orbit` | `deg` (positive = counter-clockwise from above), `elevation` (degrees up or down along the way) | Round the target at the same distance. | 1 s per 45 degrees |
| `turntable` | `deg` | The model turns; the lights stay fixed to it. Seen from outside, the camera goes the other way. | 1 s per 45 degrees |
| `dolly_in`, `dolly_out` | `factor` (> 1, default 1.5) or `distance` (mm) | Towards or away from the target. The field of view stays, so the subject grows or shrinks. In orthographic it scales instead. | 2 s |
| `zoom` | `factor` (> 1 closer, < 1 wider) | Narrows or widens the view; the camera stays put. | 2 s |
| `pan` | `right` and `up` (mm on the frame), or `by` ([x, y, z] world mm) | Moves the eye and the target together. | 2 s |
| `focus_object` | `object` (0-based index or name), `margin` (default 1.15) | Frames that object from the same direction. | 2 s |
| `focus_filament` | `filament` (1-based slot), `margin` | Frames the parts printed in that filament. | 2 s |
| `fly_to` | `view`, `margin` | To a built-in view of the plate: `front`, `back`, `left`, `right`, `top`, `bottom`, `iso`. Or to one of the scene's saved views, by name. | 2 s |
| `move_to` | a pose | To that pose. | 2 s |
| `wait` | | Stands still. | 1 s |

Moves compile to keyframes **deterministically**: the same file and project always give the same
keys. An orbit or turntable becomes a key every 15 degrees, as a glide run, so it eases in and out
once and keeps its radius.

The whole video is at most 60 s and 512 keyframes. Compile errors name the move and what exists:

- an unknown object or view lists the ones there are;
- a filament not on the plate says which filaments are;
- a `dolly_in` that would pass the target says so.

### `events`: timed changes

Each event has a `type` and its timing, one of these forms:

- `"at": t`, optionally with `"transition": s`. From then on, blended in over the transition.
- `"from": a, "to": b`, optionally with `"fade": s`. Only inside that range, fading in and out. A
  fade is at most half the range.

Events apply in time order, and in file order for the same time. Colours blend, angles take the
short way round, and other values switch half way.

| `type` | keys |
|---|---|
| `lighting` | `rig` (`soft`, `classic`, `dramatic`, `key_only`, `sun`), `azimuth`, `elevation` (degrees), `key_intensity` (0-3), `temperature` (2000-10000 K), `fill`, `rim` (0-3, 0 = off), `environment` (an environment id), `env_intensity`, `env_rotation`, `exposure` (EV, -3 to 3), `shadows`, `shadow_intensity`, `shadow_softness` (0.5-4), `ssao` |
| `background` | `kind` (`solid`, `vertical_gradient`, `radial_gradient`, `image`, `transparent`, `environment`), `color1`, `color2` (`"#RRGGBB"`) |
| `floor` | `kind` (`none`, `shadow_catcher`, `solid`, `grid`, `texture`, `image`), `color`, `shadow_strength`, `reflection`, `contact_shadow` (0-1) |
| `look` | `preset`: a whole built-in look (`studio_white`, `dark_gradient`, `build_plate`, `desk_wood`, `outdoor`) for the backdrop, floor and lights |
| `callouts` | `action` (`show`, `hide`, `highlight`), and which callouts: `filaments` (slots) and/or `labels` (callout keys such as `"f2"` or `"f2@12.40"`, or a callout's title). Naming neither means all of them. A highlight draws an accent ring round the label and its dots, and dims the others. |
| `watermark` | `enabled`, `text`, `anchor` (`top_left` ... `bottom_right`), `opacity` (0-1), `size` (% of the height). A range shows the mark inside it. |
| `build` | Needs `from` and `to`. `start` and `end` give how much of the print is shown, 0 to 1 (default 0 to 1). Also `ease`, `source`: `auto`, `toolpaths` or `clip`, and `infill` and `supports` (`true` or `false`; unset = the scene's choice, which is both on). `"follow": true` frames the camera on what is built so far (the Showcase tab's "Static with zoom out": the same angle, zoomed in on the first layers, ending on the path's own framing). Before the range the print shows `start`; after it, `end`. |

**The layer build-up.**

- **With a slice.** On a sliced plate (Auto or `toolpaths`), the slice's layers grow, the top layer
  laid move by move.
- **Without a slice, or with `clip`.** The model is cut by a horizontal plane at the height reached.
  The cut is drawn as a solid cap, and only what is built casts a shadow.
- **Timing.** Every layer takes the same time, as on the printer.
- **Infill and supports.** With toolpaths the build-up lays every extrusion. `"infill": false`
  leaves out sparse and internal solid infill and internal bridges; `"supports": false` leaves out
  supports, their interfaces and transitions, and all ironing (support ironing is
  written with the same role as the model's; the model's ironing only retraces a top surface). The clip of an unsliced model has neither. With
  `--showcase-render`, add `--slice` so a project without a slice gets toolpaths.
- **Not drawn.** The machine's own extrusions (the start G-code's purge line, prime blobs) are left
  out of every toolpath view in Showcase.
- **Callouts in a build-up.** A callout's spot that is not built yet gets no leader; the label shows
  without one until it is. With "As used" (a scene setting) a callout appears only once its filament
  has been printed. A dashed leader with a hollow dot (stills and the live view) means the spot is
  behind the model from that view; a video fades such a leader instead.

## `--showcase-render`

```
EdgeSlicer --showcase-render <project.3mf> --out <file> [--path <path.json>] [--plate N]
           [--size 1080] [--aspect 16:9] [--fps 30] [--format mp4|webm|webm_alpha|gif|webp|png]
           [--quality draft|high|best] [--bitrate 0-100] [--still <seconds>] [--report report.json]
           [--slice] [--dry-run] [--datadir <dir>]
```

- **What it renders.** Without `--path`, it renders the scene's own video, as set in the Showcase
  tab. With `--path`, the path replaces the camera path and timeline. The scene's look (lights,
  backdrop, floor, materials, callouts, watermark) is the project's. That includes the callouts'
  fields (material family, hex code and swatch apart), their separator, "each colour once" and the
  leader style, and the video's callout position (animated or static) and appearance (always or as
  used), which the Showcase tab sets. The scene's camera path may also be `static` or
  `static_zoom_out`; an exported path writes those as a `wait` move (and a following `build`).
- **Format.**
  - From `--format`, else from the `--out` extension (`.mp4`, `.webm`, `.gif`, `.webp`).
  - `--format png`, or an `--out` without an extension, gives a folder of numbered PNG frames.
  - `--still <t>` renders the one frame at that time as `.png`, `.jpg` or `.webp`.
- **Size.**
  - `--size` is the shorter side.
  - `--aspect` is the frame's shape; the default is the scene's export size.
  - The camera's zoom is per pixel of the frame. The scene remembers the height of the tab's frame,
    so the command line frames the same shot at any size.
- **Slicing.** `--slice` slices the plate first, as Preview does (its G-code stays in the temporary folder). A build-up then lays the
  real toolpaths, and a scene set to show toolpaths shows them. Without it the model is shown, and
  a build-up cuts it with a plane.
- **How it runs.**
  - The project is loaded in a hidden instance that starts no network plug-in, page server or hub.
    It contacts no printer, registers no file types and shows no window.
  - It renders through the same renderer as the tab's export, and exits.
  - Use `--datadir` with a scratch folder to keep your own settings untouched.
- **Encoders.**
  - Windows: the bundled ffmpeg (H.264 through Media Foundation, OpenH264 or the GPU's encoder).
  - macOS: AVFoundation for MP4; a Homebrew ffmpeg for the rest.
  - The PNG sequence works everywhere.
- **Checks.** The arguments and the path file's syntax are checked before anything loads. Objects,
  filaments and views are checked against the project.

**Exit codes:**

| code | meaning |
|---|---|
| 0 | done |
| 2 | bad arguments |
| 3 | the path file is invalid (it does not parse or does not compile for this project) |
| 4 | the project does not load, or has no such plate |
| 5 | OpenGL or a frame failed |
| 6 | the encoder failed |

`--report report.json` writes what happened:

```json
{ "ok": true, "exit_code": 0, "project": "...", "path": "...", "path_name": "Hero orbit", "out": "...",
  "plate": 1, "kind": "video", "format": "mp4", "encoder": "ffmpeg h264_mf ...", "width": 1920, "height": 1080,
  "fps": 30, "frames": 270, "duration": 9.0, "keyframes": 12, "events": 3, "source": "model", "build": "clip",
  "seconds": 41.3, "warnings": [], "errors": [] }
```

`--dry-run` loads the project and compiles the path, renders nothing, and writes the report. Use it to
check a path against a project quickly. Stdout ends with `SHOWCASE_RENDER_OK exit=0` or
`SHOWCASE_RENDER_FAILED exit=<code>`; warnings and errors go to stderr.

## An agent's loop

1. Write a path. Start from an example in `resources/showcase/paths/`.
2. Check it: `--dry-run --report r.json`. Fix what `errors` name; read the `warnings`.
3. Render stills at the key moments: `--still 0`, `--still 4.5` and so on, at `--size 540` and
   `--quality draft`. Look at them.
4. Render the video. Check `frames` and `duration` in the report.
