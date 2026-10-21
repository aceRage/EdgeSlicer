# Showcase tab: design (2026-10-18)

This is a design note only: no code and no builds. It covers **Showcase**, a new tab next to Preview that makes presentation renders and orbit videos of how the *printed* model will look. You choose the lighting, backdrop and floor, and can add filament callouts.

Owner vision: 2026-10-17/18. Foundation: the viewer-extras stack (`feat/viewer-extras-p1..p5`, worktree `C:/Dev/wt_viewer`). Unless this note says "main", file and line references point at that worktree.

---

## 0. TL;DR

- **Hosting.** Showcase is a third page that hosts `m_plater`, after Preview. It has its own `GLCanvas3D` of a new type, `CanvasShowcase`, hosted the way `AssembleView` is. The one shared GL context lets it draw both the model volumes and the libvgcode toolpaths.
- **Rendering.** Every export goes through one offscreen pipeline, which turns the realistic-view code we already have into a reusable renderer:
  - FBO at export size, plus 2x supersampling and MSAA;
  - Phong lighting with per-filament finish parameters;
  - a shadow map with a world-fixed light;
  - SSAO with its radius scaled to the export size;
  - a shadow-catcher floor;
  - an optional bundled environment map for reflections.
- **Printed look.** Two render sources:
  - **Model** (default, no slice needed): the mesh with procedural layer lines at the real layer heights.
  - **Toolpaths** (needs a slice): libvgcode with per-fragment *rounded/flattened* cross-sections and a Smoothness slider. This fixes today's razor-edged extrusion look.
- **Callouts.** Generated from object, part, painted and colour-change filament assignments. They are laid out in margin columns with leader lines that never cross. Labels are drawn by **one CPU 2D rasteriser** (`wxGraphicsContext`), so the screen, PNG and video match.
- **Videographer.** Orbit, turntable and spline-keyframe paths, rendered offline frame by frame.
  - **Windows:** raw frames are piped to the bundled LGPL `ffmpeg.exe`. It already has `h264_mf`, NVENC/QSV/AMF, `libopenh264`, `libvpx-vp9`, `gif` and `libwebp_anim` (checked against the build in `build/Snapmaker_Orca/resources/tools/go2rtc/ffmpeg.exe`).
  - **Mac:** AVFoundation/VideoToolbox, because we ship no ffmpeg on Mac.
- **Storage.** Scenes are saved in the 3MF under `Metadata/showcase/`, which old readers ignore. They are left out of "Export Bambu 3MF".
- **Phases.** S1 covers the tab, backdrop/floor/hide-plate and still export. S5 (toolpath realism) comes last. The total is roughly 30–40 agent-days, starting after the viewer stack merges.

---

## 1. What the user sees

### 1.1 Layout (in words)

```
[Home][Prepare][Preview][Showcase][Device][Stream][Printers][Project][Calibration]

+-------------------------------------------------------------+------------------------+
| Toolbar: Preset ▾ | Source: (Model | Toolpaths) | Plate ▾ |  | Right panel (tabs or   |
|          Camera ▾ (saved views) | Safe frame ▢ | Render ▶  |  | collapsible sections)  |
+-------------------------------------------------------------+                        |
|                                                             |  Scene                 |
|   Live viewport (WYSIWYG at the chosen aspect ratio,        |  Lighting              |
|   letterboxed; outside the frame is dimmed)                 |  Background            |
|                                                             |  Floor                 |
|   Callout labels + leader lines drawn live; drag to move    |  Materials             |
|                                                             |  Camera                |
|                                                             |  Callouts              |
+-------------------------------------------------------------+  Video                 |
| Timeline (Video only): keyframes ◆, scrubber, ▶ preview     |  Export                |
+-------------------------------------------------------------+------------------------+
```

- The viewport is **interactive at draft quality**. Shadows and SSAO are on and supersampling is off. After about 300 ms idle it refines to full quality (progressive).
- **Safe frame:** the viewport shows the exact export aspect ratio, for example 16:9, 1:1, 4:5 or 9:16 for phone and Instagram.
- **Plate selector:** a Showcase scene belongs to one plate. "All plates" lays the plates out side by side; that is a stretch goal.

### 1.2 Panels

| Panel | Controls |
|---|---|
| **Scene** | Hide plate / show plate / replace plate with floor; hide wipe tower, supports, brim (each toggle); show only selected objects; explode (reuse the Assemble view explosion ratio); object spacing unchanged otherwise |
| **Lighting** | Preset (see 1.3); key light azimuth/elevation/intensity/colour temperature; fill and rim on/off and intensity; environment map ▾ + intensity + rotation; shadow softness; AO strength; exposure; light "follows camera" vs "fixed in world" (default fixed) |
| **Background** | Solid colour · vertical/radial gradient (2–3 stops) · image (fit/fill/tile, blur) · environment map (blurred) · **transparent** (PNG/WebM alpha) |
| **Floor** | None · **shadow catcher** (invisible, only shadows/AO, composites onto any background) · solid colour · built-in texture (wood, concrete, cutting mat, paper, fabric, our build plates) · user image · reflection strength (glossy floor, planar reflection) · size/fade radius |
| **Materials** | One row per filament in use: swatch, inferred finish ▾ (Matte, Standard, Silk, Glossy/PETG, Translucent, Metallic, Glow, CF), roughness, sheen; "Layer lines" (Off / Subtle / Real / Exaggerated); Toolpath Smoothness slider (S5) |
| **Camera** | Perspective/ortho, focal length (mm equivalent), saved views (named, per scene), "frame model", roll lock, depth of field (stretch goal), safe-frame aspect |
| **Callouts** | Auto-generate, style ▾, content checkboxes, per-callout edit/hide/re-anchor, layout (columns L/R, top/bottom, legend), font/size |
| **Video** | Path type, duration, fps, easing, loop, keyframes, start/end hold, preview |
| **Export** | Still: resolution preset or custom (≤ 8192 per side), quality (draft / high / best = SSAA 1x/2x/3x), format PNG/JPEG/WebP, transparent background, include callouts, copy to clipboard, "use as project cover". Video: container/codec, quality, size, output path |

### 1.3 Render presets

Each preset is a JSON bundle of Lighting, Background, Floor and Camera defaults. Presets are built in and read-only; the user can save their own copies.

| Preset | Background | Floor | Lighting | Notes |
|---|---|---|---|---|
| **Studio white** | white→light grey radial | shadow catcher | soft key 45° + fill + rim, studio env | product shot, default |
| **Studio black** | near-black gradient | glossy black, 15 % reflection | rim-heavy, low fill | silk/metallic pop |
| **Desk wood** | blurred desk photo env | oak texture | warm 4000 K key, window env | maker-desk look |
| **Dark gradient** | charcoal→black vertical | none (shadow catcher off) | single key + strong rim | dramatic |
| **Outdoor** | sky env | concrete/grass texture | sun (hard shadow) + sky fill | daylight |
| **Blueprint** | blue grid | none | flat, toon-ish edges (X-Ray off) | technical; pairs with Technical callouts |
| **Build plate** | current Prepare background | our real plate (PartPlate logo/texture) | current realistic view | "as on the printer" |
| **Transparent** | alpha | shadow catcher | Studio white lights | for compositing in other tools |

---

## 2. Rendering approach (GL 3.2 core / Mac GL 4.1 core)

### 2.1 What we already have (viewer-extras stack)

| Piece | Where | Reuse in Showcase |
|---|---|---|
| Phong per-pixel lighting, shadow map sampler on unit 5 | `resources/shaders/140/phong.fs:1-120` (two **eye-space** lights `LIGHT_TOP_DIR`, `LIGHT_FRONT_DIR`) | Base of the Showcase material shader; lights become uniforms |
| SSAO post pass (Orca #13704 + normals from #15809) | `GLCanvas3D.cpp:10049` `_render_ssao_pass`; `ssao.fs:66` radius **2–4 pixels** | Reuse; radius must scale with export/screen ratio |
| Shadow map 2048², fitted to objects + plate | `GLCanvas3D.cpp:10127` `_render_shadows`, size at `:10218`, light dir **eye-space** at `:10139` | Reuse; make light world-fixed, size 4096 for export, fit to floor not plate |
| Plate as shadow receiver | `printbed_shadow.fs` (receiver-only PCF 5×5) | Becomes the shadow-catcher floor shader |
| Offscreen MSAA FBO + resolve + readback | main `GLCanvas3D.cpp:8077` `render_thumbnail_framebuffer` (samples = `GL_MAX_SAMPLES/2`) | Template for the export FBO; thumbnails stay as is |
| Thumbnail / phone capture | main `RemoteAccess.cpp:770`, `Plater.cpp:21273` → `GLVolume::simple_render` (MMU-painted colours via `mmuseg_models`, main `3DScene.cpp:569`) | Shows the colour path already works offscreen |
| Quick view-options eye button | `GLCanvas3D.cpp:8187-8276` | Showcase gets its own panel; the eye button stays Prepare/Preview |
| Realistic flags are **global app_config reads** | `_is_realistic_view_enabled()` `GLCanvas3D.cpp:10023` (`SETTING_OPENGL_*`, `AppConfig.hpp:28-38`) | Must be refactored into a `RenderSettings` struct passed in, so Showcase can differ from the user's viewport prefs |
| Toolpaths (libvgcode) | main `src/libvgcode/src/Shaders.hpp:13` (vertex), lighting **per vertex** at `:69/:169`, fragment `:226` | S5 moves lighting to the fragment stage (see 2.6) |
| Disabled legacy env map | main `Technologies.hpp:30` `ENABLE_ENVIRONMENT_MAP 0` | Don't revive; it is a 2D sphere-map hack. Do IBL properly (2.4) |
| Toolpath OBJ export (Prusa origin) | main `GCodeViewer.cpp:2081` `ToolpathsObjExporter` | Proof the toolpath geometry is reachable host-side; not used for rendering |

Two findings matter for the design:

1. **The light is a headlight.** `_render_shadows` derives `dir_to_light` from the *view* matrix (`:10139-10141`), and `phong.fs` lights are eye-space constants. So the light, and the shadows, rotate with the camera. That is fine for editing, but an orbit video would show shadows sliding around the model. Showcase needs **world-space lights**, a uniform set per scene, while editing views keep the headlight.
2. **SSAO is pixel-radius.** At 4K export with 2x SSAA (7680 px wide), a 2–4 px kernel is about 4x smaller relative to the model than on screen. Scale the radius by `export_px / reference_px`, or define it in world units at the focus distance.

### 2.2 Export pipeline (one function, used by stills, video frames and the cover)

```
ShowcaseRenderer::render(const ShowcaseScene&, Camera, w, h, quality) -> RGBA8 (+ depth, ID optional)
  1. shadow pass        world light, 4096² (export) / 2048² (viewport), fit to casters ∪ floor disc
  2. background pass    full-screen quad: colour / gradient / image / blurred env; alpha 0 if transparent
  3. floor pass         shadow catcher or textured/reflective plane (planar reflection = second scene draw mirrored, optional)
  4. opaque scene       Model source: GLVolumes (simple_render path, MMU colours) with showcase material shader
                        Toolpath source: libvgcode viewer drawn into the same FBO with showcase uniforms
  5. translucent scene  sorted back-to-front per object (v1), see 2.5
  6. resolve MSAA → SSAO (radius scaled) → tone map + exposure → optional FXAA
  7. downsample SSAA (box/Lanczos in a shader) → glReadPixels via 2 PBOs (async for video)
  8. CPU overlay        callouts (wxGraphicsContext on the RGBA image), optional watermark
```

- **Supersampling plus MSAA.** On Apple Silicon `GL_MAX_SAMPLES` is commonly 4, while Intel/AMD/NVIDIA Windows drivers allow 8 or more, so MSAA alone is not a consistent quality bar. The quality levels are:
  - Draft = 1x SSAA with 4x MSAA;
  - High = 2x SSAA with 4x MSAA;
  - Best = 3x SSAA with 4x MSAA, capped by memory (2.8).
- **SSAO needs a resolved (non-MSAA) read buffer.** `glCopyTexSubImage2D` from a multisampled FBO is an error, so step 6 has to blit-resolve colour and depth first. Today's on-screen path copies from the back buffer, which has the same problem whenever a multisampled default framebuffer is in use.
- **Size cap.** Each side is capped at 8192 internal pixels, so 4K with 2x SSAA is the practical ceiling. No tiled rendering in v1: tiles break the screen-space passes at their seams.
- **Gamma.** Render linear and tone map at the end (simple filmic/ACES-fit curve and exposure). Write sRGB. Filament hex colours are sRGB, so convert them to linear on input.

### 2.3 Shadow-catcher floor

- The floor is a large disc or quad at Z = 0, under the objects' footprint union, with a radial alpha fade at the edge.
- **Catcher mode** outputs only `(0,0,0, shadow·AO·strength)`, multiplied over the background. This is what makes "transparent background with soft contact shadow" work for compositing. `printbed_shadow.fs` already computes the occlusion term; the catcher is that shader plus an alpha output and contact AO.
- **Contact AO:** SSAO alone gives a thin rim. Add a cheap analytic contact shadow: a blurred footprint texture of the objects' XY silhouette rendered from above, at about 512². This gives the soft "sitting on the table" look that product shots need.
- **Glossy floor:** a planar reflection, which is a second scene draw with a mirrored view matrix into a half-resolution texture, blurred by roughness. It costs one extra scene pass and is optional, off in most presets.
- **Hide plate:** skip `PartPlate::render` and `Bed3D` (the bed texture lives in `PartPlate::render_logo`, per the bed-texture memory). The "Build plate" preset keeps them.

### 2.4 Environment / HDRI lighting on GL 4.1

**IBL is feasible on GL 4.1** (no compute shaders needed):

- **Feature support.** Cubemaps, `GL_TEXTURE_CUBE_MAP_SEAMLESS` (core 3.2), RGBA16F render targets and mipmapped cubemaps are all core 3.2. Prefiltering is done with fragment passes into each cube face and mip.
- **Missing on Mac 4.1:** compute shaders, image load/store (4.2) and `glTexStorage` (4.2). None of these is required.
- **Recommended v1: bundled, pre-baked environments.** 6–8 environments (studio softbox, studio rim, window daylight, overcast, sunset, workshop, night city), CC0 from Poly Haven.
  - Each ships as a **pre-filtered RGBM PNG cubemap**: 6 faces × 5 roughness mips at 128–256 px, plus a 32 px irradiance cube.
  - That is about 1–2 MB each, so no HDR decoder and no runtime prefilter.
  - The shader adds `diffuse += irradiance(n)` and `spec += prefiltered(reflect, roughness) · F(Fresnel-Schlick)`, using a 2D BRDF LUT (a 64² PNG baked once).
- **User HDRI (later, S2b):**
  - load a Radiance `.hdr` with a small RGBE reader (about 150 lines; we have no stb_image, only PNG/JPEG via libpng/libjpeg, `CMakeLists.txt:831`, `libslic3r/CMakeLists.txt:788`);
  - convert equirect → cube in a fragment pass;
  - prefilter (GGX importance-sampled, 64 samples/texel at 128 px) in about 100–300 ms on the GPU, once per load.
- **Fallback (no env, or GL 2.x legacy context):** three-point analytic lights only. Showcase requires the core profile. Under the legacy macOS 2.1 fallback the tab is shown disabled, with a reason.

### 2.5 Material model per filament ("PBR-ish")

The material is a Cook–Torrance GGX specular with a Lambert diffuse, plus three extras:

- **sheen** (silk);
- **anisotropy** along the extrusion or layer direction (silk, the key look);
- **transmission tint** (translucent).

Parameters come from the filament's *finish*.

| Finish | Inferred from (preset name/type keywords) | Roughness | Spec F0 | Extras |
|---|---|---|---|---|
| Matte | "Matte", "Matt", PLA-Matte, "Aero" | 0.85 | 0.02 | stronger layer-line darkening, minimal highlight |
| Standard PLA/ABS/ASA | default for PLA/ABS/ASA/PC | 0.55 | 0.04 | — |
| Glossy (PETG/PCTG) | PETG, PCTG, "Gloss" | 0.30 | 0.05 | slight clear-coat lobe |
| Silk | "Silk", "Shiny", "Satin" | 0.35 | 0.06 metallic-tinted | **anisotropic** highlight, tangent = line/layer direction; colour-tinted spec |
| Metallic | "Metal", "Bronze", "Copper", "Steel" fill | 0.40 | 0.5 tinted | sparkle noise (optional) |
| Translucent / Transparent | "Translucent", "Clear", "Transparent", "Crystal" | 0.25 | 0.04 | alpha 0.35–0.7, tint, Fresnel edge brighten; no refraction |
| CF / GF | "-CF", "-GF" types | 0.9 | 0.03 | micro-noise normal (fibre speckle) |
| Glow / Galaxy / Sparkle | "Glow", "Galaxy", "Sparkle", "Glitter" | 0.6 | 0.04 | glitter: hashed sparkle in specular |
| TPU / Flex | FLEX, TPU | 0.7 | 0.03 | — |

- **Inference only, no new profile key in v1.** `filament_type` (main `PrintConfig.cpp:2916`) gives the polymer, and `filament_settings_id`/`filament_vendor` (`:3010`, `:3019`) give the name keywords. The user can override the finish per filament slot, and the override is saved in the scene. A later profile key (`filament_finish`) is decision D4.
- **Multi-colour filaments:** `filament_multi_colors` with `filament_colour_mode` (split/gradient), main `PrintConfig.cpp:2519-2527`. Render as a gradient along Z (gradient mode), or as sections along the object's longest axis (split mode). This is a visual approximation; label it "≈".
- **Transparency v1:** sort per object, draw back faces then front faces, with no order-independent transparency. Weighted-blended OIT (one RGBA16F + one R16F MRT) is possible on 3.2 if users ask; it's out of scope until then.

### 2.6 The printed look: layer lines (S5), and the owner's toolpath observation

#### 2.6.1 Two sources

| | **Model mesh + procedural lines** (default) | **Real toolpaths** (libvgcode) |
|---|---|---|
| Needs a slice | No (layer height from config; variable layer height profile if present) | Yes |
| Geometry cost | Mesh triangles (usually 10⁴–10⁶) | 8 vertices × segments (often 10⁶–10⁷ segments); instanced, buffers already on GPU after slicing |
| Shows | Layer stripes on walls, top-surface line texture (fake), colour from extruder/paint | True seams, true top/ironing pattern, infill through translucent parts, supports, brim, colour changes exactly |
| Wrong about | Seams, real top pattern, overhang droop, small features below line width | Over-sharp by default (below); huge models are slower |
| Aliasing | Procedural, filtered by `fwidth` (fades out when a layer < ~1.5 px) | Geometry aliasing (needs SSAA) |
| Export time (4K, 2x SSAA, mid GPU, rough) | 50–150 ms/frame | 150–800 ms/frame for a 5M-segment print |
| Mac | Cheap | libvgcode uses `samplerBuffer` (core 3.1); fine on 4.1, already ships |

#### 2.6.2 Procedural layer stripes (model mode)

- **Where layers are:** a 1D lookup of layer boundaries built from the object's real layer Z list. Use `PrintObject` layers if sliced, else `generate_object_layers` from the layer-height profile. It is stored as an R32F texture (2D-wrapped past 16 k entries) and binary-searched in the shader, or as a z→phase texture at 0.01 mm.
- **Profile:** the per-fragment normal is perturbed by `d/dz` of a rounded profile (semi-ellipse), scaled by wall-ness `(1 − |n·z|)`. Depth slider: Off / Subtle (0.15) / Real (0.35) / Exaggerated (0.7).
- **Antialiasing:** the amplitude is multiplied by `saturate(1.5 − fwidth(z)/layer_h)`, so stripes fade out instead of moiré-ing on far shots. The honest consequence: **at full-model framing a 0.2 mm layer is under 1 px**, so layer lines only read in close-ups and in 4K/SSAA renders. The UI should say so.
- **Top surfaces:** a line normal pattern in the top-infill direction:
  - the angle comes from `top_surface_pattern` and the infill direction;
  - monotonic or concentric patterns are approximated by straight lines;
  - the pattern is applied only to faces with `n·z > 0.95` that are top-most.

#### 2.6.3 Owner observation: toolpaths look razor-edged; tops should blend, walls should soften

**Root cause (main `Shaders.hpp`):**

- The libvgcode vertex shader builds an 8-vertex camera-facing diamond per segment. It takes the normal as `normalize(pos − endpoint_pos)`, i.e. radiating from the segment axis to each diamond corner (`:166`).
- It lights *per vertex* (`direct_lighting` at `:69`, applied at `:169`).
- So each segment has a hard ridge along its top centre and two dark V-creases with its neighbours. SSAO then deepens the creases.

Real top skin is a near-continuous, slightly glossy sheet with faint ridges. Real walls have rounded layer bulges, not knife edges.

**Options**

| # | Option | What it does | Quality | GL 4.1 cost | Big models |
|---|---|---|---|---|---|
| A | **Analytic per-fragment cross-section normal** | Pass `across ∈ [−1,1]` (and `up`) as varyings; compute the normal in the fragment shader from an elliptical profile `n = normalize(right·across·k + up·sqrt(1−across²))`; move lighting to the fragment stage | Removes the diamond facets; real rounded beads | +~15 ALU/fragment, no extra geometry | Unchanged vertex cost; fragment-bound only at 4K SSAA |
| B | **Flatten + widen skin roles** (wider overlap / flattened cross-section) | For Top/Bottom solid infill and ironing: width × (1 + overlap) (e.g. 1.15) and profile exponent → flat; vertical extent unchanged | Closes the visible gaps and creases; adjacent lines merge | Vertex-shader math only | Free |
| C | Normal smoothing across adjacent toolpaths | Needs neighbour lookup (which segment is next to me in XY), not available in libvgcode's data | Best "true" blend | Needs a per-layer spatial index or a G-buffer; heavy CPU prep | Poor (memory, prep time) |
| D | Screen-space blur / normal filtering on skin | Write normals + role ID to an MRT, bilateral-blur normals where role = skin, then shade (deferred) | Good on tops, but smears edges/seams, view-dependent, flickers in video | +1 RGBA16F MRT + 2 blur passes; at 8K internal ≈ 260 MB + bandwidth | Resolution-bound, not model-bound |
| E | Replace top skin with mesh surface + line normal map | Hide top-solid toolpaths; draw the model's top faces with the procedural line normal map (2.6.2) | Clean, glossy, controllable | Two sources in one frame → z-fighting at skin/wall boundary; mesh top ≠ printed top (slicing steps) | Fine, but correctness issues with variable layer height and ironing |
| F | **"Smoothness" slider** (0 = crisp toolpaths today, 1 = fully smooth) | Drives A+B: profile exponent, skin overlap/flatten, ridge amplitude, SSAO weight on skin roles | User picks the look | Uniforms only | Free |

**Recommendation: A + B + F, all in the libvgcode shaders, with finish-aware defaults.** Reject C and E for v1, and keep D as a later experiment.

- **Per-vertex data:** A and B need the extrusion role per vertex. libvgcode already knows it (`PathVertex::role`, main `libvgcode/include/PathVertex.hpp:62`) but doesn't upload it to the shader. Add one `R8UI` buffer texture (role 5 bits + finish 3 bits). That's 1 byte/vertex, i.e. **about 10 MB for 10 M vertices**. The colour float has no spare mantissa bits, so a separate buffer is cheaper than repacking.
- **Walls:** semi-elliptical bead with Smoothness → "bulge" (exponent 2 → 6). Optionally a micro-ridge stripe at the layer seam (the groove between beads) with an adjustable depth, so walls read as soft layers, not saw teeth.
- **SSAO:** weight it down on skin roles; the creases there were the main artefact.
- **Finish-aware defaults:**

| Finish | Smoothness | Skin overlap | Wall bulge | Specular look |
|---|---|---|---|---|
| Matte | 0.5 | 1.10 | rounded, soft | diffuse; layer grooves stay visible (matte hides less) |
| Standard | 0.6 | 1.12 | rounded | mild |
| Silk | 0.8 | 1.18 | flatter | **anisotropic along `line_dir`** (tangent is free in toolpath mode); this is what makes silk read as silk |
| Glossy PETG | 0.75 | 1.15 | rounded | sharper highlight, slight clear-coat |
| Translucent | 0.7 | 1.15 | rounded | alpha + Fresnel; infill visible through walls (toolpath mode's unique advantage) |

- **Also needed:**
  - a **world-fixed light**;
  - Preview's dim-layer/grey uniforms off;
  - the full layer range;
  - travel, retraction and wipe moves hidden;
  - seam markers hidden.

  Showcase sets these explicitly, rather than inheriting whatever the Preview legend has toggled.
- **Cost:** A moves lighting from 8 vertex invocations per segment to every covered fragment. Prepare/Preview at 1080p with a 5 M-segment print is fragment-light already (most segments are sub-pixel), so expect **≤10 % frame time** in the viewport. Export is offline, so it doesn't matter there.
- **Rollout:** keep the existing per-vertex path as the default in Preview, behind `#ifdef` or a uniform. Turn on the smooth path in Showcase and in Preview's realistic view only after the owner signs off on the look (D13).

---

## 3. Callouts

### 3.1 Sources (auto-generate)

| Source | Data | Region for anchor |
|---|---|---|
| Object extruder | `ModelObject::config "extruder"` | whole object |
| Part / modifier extruder | `ModelVolume::config "extruder"` | volume |
| MMU / colour painting, Image Fill | `TriangleSelector` states per extruder (rendered via `mmuseg_models`) | each painted connected component above an area threshold |
| Height-range modifiers | `layer_config_ranges` with extruder | Z band of the object |
| Colour change / filament change at layer | `custom_gcode_per_print_z` (M600/ToolChange) | Z band → label "from 12.4 mm" |
| Mixed filament (virtual) | `MixedFilamentManager` (main `libslic3r/MixedFilament.hpp:63, 228`) | label lists components + ratio |
| Support / interface filament | `support_filament`, `support_interface_filament` | supports (only when shown) |
| After slicing (authoritative usage) | `GCodeProcessorResult` per-extruder used length/weight | for weight/price fields |

**Merge rule:** one callout per *filament per visually separate region*. If one filament has more than 3 regions, use one callout with multiple leaders (a "multileader", as AutoCAD does), up to 3, and the rest go to the legend.

### 3.2 Label content

```
Polymaker  PLA Pro – Galaxy Black        ■ #1A1A1A
PLA · Silk · 38 g · $0.95
```

| Field | Source | Default |
|---|---|---|
| Brand | `filament_vendor` | on |
| Product name | preset name minus vendor and `@printer` suffix (`GetFilamentMatchName`, main `FilamentColorLibrary.hpp`) | on |
| Colour name | 1) Snapmaker colour library (`resources/profiles/Snapmaker/filament/filaments_colours.json`, via `FilamentColorLibrary.cpp:111`); 2) the user's text; 3) nearest named colour from a built-in ~150-name list, shown with "≈" | on |
| Hex + swatch | `filament_colour` (main `PrintConfig.cpp:2512`), multi-colour → split swatch | on |
| Type / finish | `filament_type`, inferred finish | on |
| Weight | slice result per extruder | off |
| Price | slice weight × resolved price (`CostOverrides.hpp:203` `set_preset`/family overrides) | off (resellers may not want prices in shared images) |
| Free text | user | — |

### 3.3 Placement, leaders and anti-overlap

1. **Anchor.** Render a low-resolution (512 px) ID pass: object/volume/extruder per pixel, which reuses the picking render. Run a distance transform on each region's mask and take the pixel furthest from the region boundary (its "pole of inaccessibility"). Read its depth and store the anchor in **object-local 3D**, so it follows transforms and camera moves.
2. **Side assignment.** Anchors left of the model's screen centre go to the left column, the rest to the right. Top/bottom bands are used for wide, flat models. Swaps are balanced so neither column overflows.
3. **Column packing.** Sort by anchor screen-Y, then do a 1D sweep: push labels down to remove overlap, then relax upwards to minimise leader length. Keeping the same order as the anchors guarantees **no crossings within a column**. Leaders are straight or "elbow" (horizontal landing, as in AutoCAD's MLEADER).
4. **Too many** (more than about 8 per column): switch to **legend mode**. Numbered dots go on the model, with a key box.
5. **User edits.** Drag a label, which pins its position in normalised frame coordinates. Drag an anchor dot to re-anchor it with a ray pick. Edit or hide text. The auto layout only moves unpinned labels.
6. **Occlusion.** Test the anchor against the depth buffer. An occluded anchor gets a dashed leader (stills) or fades out (video, with about 10-frame hysteresis so it doesn't flicker).

### 3.4 Styles

The styles are Minimal (dot, hairline, text), Card (rounded box, swatch, shadow), Pill tag, Technical (AutoCAD-style landing + underline, mono font) and Legend.

- **Size:** font size is a percentage of image height, so callouts scale with resolution.
- **Colour:** text and line colour switch automatically to contrast with the backdrop luminance sampled behind each label.

### 3.5 One rasteriser for screen, still and video

Callouts are drawn on the CPU with `wxGraphicsContext` (Direct2D/GDI+ on Windows, CoreGraphics on Mac, Cairo on Linux). They are drawn into the read-back RGBA image for export. For the viewport, they're drawn into a texture that is blitted over the GL view.

- **Why not ImGui:** ImGui's font atlas is baked at UI scale, so 4K exports would show blurry or resized text, and ImGui draws only to the window framebuffer path.
- **Cost:** about 2–5 ms per 1080p frame, which is fine for video.
- **Fonts:** UI font by default; any system font is allowed. Font names are saved in the scene, with fallback to the UI font.

---

## 4. Videographer

### 4.1 Paths

| Path | Definition | Notes |
|---|---|---|
| **Orbit** | camera circles the target at fixed elevation/distance; start angle, sweep (360° default), direction | world light fixed → shadows stay put, highlights travel (looks real) |
| **Turntable** | the *model* spins, camera and lights fixed | highlights stay, shadow rotates; "product on a lazy Susan" |
| **Orbit + bob** | orbit with sinusoidal elevation (± n°) | more dynamic |
| **Spline keyframes** | user sets camera keyframes (position, target, focal length, roll) from the viewport; centripetal Catmull–Rom for position/target, slerp for orientation; arc-length re-parameterised for constant speed | timeline with ◆ markers, per-key ease |
| **Dolly / reveal** | preset: push in from wide to close-up on a chosen callout | uses keyframes |
| **Layer build-up** (toolpath source, stretch) | animate Preview's top layer from 0 → N while orbiting | "print growing" clip creators love |

- **Timing:** duration (2–60 s), fps (24/30/60), start/end hold frames.
- **Easing:** linear, ease-in-out (smoothstep / cubic), ease-in, ease-out.
- **Seamless loop:** for a 360° orbit, frame N is not a duplicate of frame 0.
- **Live preview:** the timeline ▶ plays the path in the viewport in real time at draft quality.

### 4.2 Encoding

Frames are rendered offline, one at a time (quality is independent of real-time speed), on the GL thread.

- **Readback:** double-buffered PBO.
- **Hand-off:** frames go into a bounded queue (3 frames) to an **encoder worker thread**.

| Platform | Encoder | Notes |
|---|---|---|
| Windows | bundled `ffmpeg.exe` (LGPL, `resources/tools/go2rtc/`, licence note there) as a child process via `boost::process` (pattern: main `MediaPlayCtrl.cpp:989`, `RemoteHub.cpp:1994` lookup) | `-f rawvideo -pix_fmt rgba -s WxH -r fps -i - -vf vflip` |
| macOS | **AVFoundation `AVAssetWriter` + VideoToolbox H.264/HEVC** (native, hardware) | we ship no ffmpeg on Mac (memory ffmpeg-replacement-parked, 2026-10-09); a Homebrew ffmpeg, if present, unlocks WebM/GIF |
| Linux | `ffmpeg` from PATH / AppImage-bundled if we add it | else PNG sequence |
| all | PNG sequence (zip or folder) | always available |

**Formats** (Windows encoders verified in the bundled build: `h264_mf`, `h264_nvenc`, `h264_qsv`, `h264_amf`, `libopenh264`, `libvpx-vp9`, `libsvtav1`, `gif`, `libwebp_anim`):

| Preset | Command essentials | Use |
|---|---|---|
| **MP4 H.264 (default)** | try `h264_mf -quality 100 -b:v <rate>`, fallback `libopenh264 -b:v <rate>` (no CRF in openh264); `-pix_fmt yuv420p -movflags +faststart`; even W/H | social, phone |
| WebM VP9 | `libvpx-vp9 -crf 28 -b:v 0 -row-mt 1 -deadline good` | web, smaller |
| **WebM VP9 + alpha** | `-pix_fmt yuva420p` with Transparent background | compositing / stickers |
| GIF | two-pass `palettegen`/`paletteuse`, ≤ 800 px, ≤ 20 fps | forums, Discord |
| Animated WebP | `libwebp_anim -q 80 -loop 0` | Discord/web |

- **Bitrate presets:** 1080p30 at 12 Mb/s, 4K30 at 40 Mb/s, set from a "Quality" slider.
- **Progress and cancel:**
  - a non-modal progress dialog shows frame n/N, ETA and a thumbnail of the current frame;
  - **Cancel** stops rendering, closes the pipe and kills ffmpeg, deletes the partial file and keeps the scene.
  - Rendering runs in bounded slices on `wxEVT_IDLE` or a timer, so the UI stays responsive. The GL context is single-threaded, so rendering stays on the main thread.
- **Failure:** if ffmpeg is missing or fails, the user gets an error with the ffmpeg stderr tail logged, plus an offer to "Save PNG sequence instead".
- **Rough timings:**
  - 10 s at 30 fps is 300 frames;
  - at 1080p High (2x SSAA), Model source, with shadows and SSAO, about 80–150 ms per frame, so **about 30–60 s total**;
  - 4K and Toolpaths: several minutes.

---

## 5. Data: saving scenes in the 3MF

Showcase follows the house pattern for EdgeSlicer-only data (`Metadata/image_fill/`, `Metadata/cut_recipe.xml`, main `bbs_3mf.cpp:186-208`): files under `Metadata/` that nothing in the core model references. Upstream and old readers ignore them.

```
Metadata/showcase/scenes.json          // {"schema":1, "scenes":[...], "active":"<id>"}
Metadata/showcase/<sha256>.png|jpg     // backdrop / floor / user images, content-hashed, de-duplicated
Metadata/showcase/env_<sha256>.hdr     // only for user HDRIs (S2b), size-capped
```

`scenes.json` per scene:

- `id`, `name`, `plate_index`;
- `source` (model|toolpaths);
- `preset_base` and an overrides diff;
- `lighting` {lights[], env id + rotation + intensity};
- `background`, `floor` (image refs by hash);
- `materials[]` per filament slot {finish, roughness, colour_name override};
- `camera` {type, eye, target, up, focal, aspect}, plus saved `views[]`;
- `callouts[]` {target: {object_idx, volume_idx|-1, extruder, region_seed (object-local point)}, anchor_local xyz, label_pos (normalised, pinned?), text overrides, hidden};
- `style`;
- `video` {path type, params, keyframes[], fps, duration, easing, format};
- `export` {w, h, quality, transparent}.

Rules:

- **Backward and forward compatibility:**
  - unknown keys are kept on load and written back;
  - a newer `schema` opens read-only with a notice;
  - a missing or invalid file → no scenes, never fatal.
- **Not in "Export Bambu 3MF".** It is gated on `m_bambu_compat` (main `bbs_3mf.cpp:6793`), like texture displacement (`:8381`).
- **Image cap:** images are downscaled to ≤ 4096 px and re-encoded as JPEG unless they have alpha, with a warning above 20 MB total.
- **Object references** use the 3MF object/volume order the writer already uses for `model_settings.config`. A callout whose target is gone is dropped on load, with one log line.
- **Built-in presets and environments are not embedded.** They are referenced by id with a version; if the id is unknown, fall back to Studio white.
- **Optional cover:** "Use as project cover" writes the render into the existing `/Auxiliaries/.thumbnails/thumbnail_3mf.png` (main `bbs_3mf.cpp:169`). Off by default (D10).
- **Tests:** a round-trip unit test in `tests/libslic3r` (write → read → equal, unknown-key preservation, missing-object drop). Serialization belongs in libslic3r (`ShowcaseScene.hpp/.cpp`, no GL), so the CLI and tests can use it.

---

## 6. Performance, LOD, GPU budget, Mac

**GPU memory at export**: RGBA8 colour plus D24S8 depth, with SSAO copies.

| Output | Quality | Internal px | FBO (MSAA 4x) | Resolve + SSAO copies | Shadow 4096² | Env | ≈ Total |
|---|---|---|---|---|---|---|---|
| 1080p | High (2x) | 3840×2160 | 265 MB | ~100 MB | 64 MB | ~10 MB | **~0.45 GB** |
| 4K | High (2x) | 7680×4320 | 1.06 GB | ~400 MB | 64 MB | ~10 MB | **~1.5 GB** → drop MSAA at 2x SSAA: ~0.6 GB |
| 4K | Draft (1x) | 3840×2160 | 265 MB | ~100 MB | 64 MB | ~10 MB | ~0.45 GB |

- **Budget:** at most 1 GB per export.
- **Auto-degrade:** if the budget is exceeded, or `glCheckFramebufferStatus` or an allocation fails, fall back to SSAA 2x without MSAA, then to 1x. Say so in the export log, and free everything after the export: Showcase holds no export-size buffers when idle.
- **Viewport:** draft quality while orbiting; refine on idle. That is the same idea as upstream #15674 (scene texture reuse while idle, deferred in our port plan as 4B-4). If 4B-4 lands first, Showcase reuses it.
- **LOD (planned later):** the viewport *may* use LOD meshes while moving, but **export never uses LOD**. Toolpath mode in the viewport can draw only every n-th layer while moving (libvgcode view range) and everything on idle.
- **Big toolpath prints:** export time scales with segment count; show an estimate before a toolpath video starts (time one frame, × N).
- **Mac specifics:**
  - core 4.1 only, no compute, `GL_MAX_SAMPLES` often 4 (Apple Silicon), `GL_MAX_TEXTURE_SIZE` / renderbuffer size 16384;
  - unified memory, so large FBOs compete with system RAM;
  - the unsized-depth renderbuffer trap is already fixed in the thumbnail path (main `GLCanvas3D.cpp:8134`); reuse sized formats everywhere;
  - Retina: export sizes are in real pixels, independent of the backing scale;
  - the video encoder is AVFoundation, per §4.2;
  - a hand test on the Mac mini is needed before each phase merges (memory mac-local-testing).
- **Integrated GPUs (Intel UHD):** 4K/2x/MSAA may exceed the budget; auto-degrade covers it. Test on the owner's laptop class.

---

## 7. Phases

Each phase is a separate PR with owner hand-test steps. All of them depend on the **viewer-extras stack (p1–p5) being merged to main**.

| Phase | Scope | Size | Depends on |
|---|---|---|---|
| **S1: tab + still** | `CanvasShowcase` + ShowcaseView in Plater (like `AssembleView`, main `Plater.cpp:11510`); tab after Preview (`MainFrame.hpp:253` enum renumber + audit of the 33 `tp*` uses); `RenderSettings` refactor so realistic view reads a struct, not app_config; world-fixed light option; export FBO with SSAA/MSAA + SSAO radius scale + resolve; background (colour/gradient/image/transparent); floor (none/catcher/colour/built-in textures); hide plate/wipe tower/supports; camera aspect safe frame + saved views; PNG/JPEG export, clipboard; 3 presets (Studio white, Dark gradient, Build plate); `ShowcaseScene` JSON + 3MF round trip | **L (8–10 d)** | viewer stack |
| **S2: lighting + materials** | 3-point lights UI; bundled pre-filtered env maps (6–8) + BRDF LUT; GGX material + finish inference + per-filament overrides; anisotropic silk; translucent v1; contact shadow; glossy floor reflection; remaining presets; **procedural layer lines on the mesh (2.6.2)** | **L (7–9 d)** | S1 |
| S2b (optional) | user `.hdr` import + GPU prefilter | M (3 d) | S2 |
| **S3: callouts** | auto-generate from object/part/paint/height-range/colour-change; ID-pass anchors; column layout + leaders + legend mode; drag/pin/edit; styles; wxGraphicsContext overlay (screen + export); colour-name lookup; weight/price fields | **L (6–8 d)** | S1 (S2 not needed) |
| **S4: videographer** | orbit/turntable/bob/spline keyframes; timeline UI; easing; offline frame loop + PBO + encoder thread; ffmpeg pipe (Windows), AVFoundation (Mac), PNG sequence; GIF/WebP/VP9-alpha; progress/cancel/ETA; callouts per frame with occlusion fade | **L (6–8 d)** | S1; S3 for callouts in video |
| **S5: toolpath realism** | Toolpath source in Showcase; libvgcode role buffer; per-fragment cross-section normals (A), skin flatten/overlap (B), Smoothness slider (F), finish defaults, SSAO skin weight, anisotropic silk along `line_dir`; optional same look in Preview realistic view; layer build-up animation (stretch) | **L (6–9 d)** | S2 (materials), libvgcode host; owner look sign-off |

- **Smallest useful slice:** S1 alone already gives "nice product shot of my model on white with a soft shadow, 4K PNG".
- **Order:** S3 can run in parallel with S2 once S1 merges; they touch different files (overlay vs shaders).
- **Gates and tests:**
  - no G-code impact, so no gcode-gate; S5 touches only libvgcode *shaders*, not the processor;
  - unit tests for scene JSON, finish inference, label layout (pure 2D, testable in `tests/slic3rutils` or libslic3r) and spline/easing maths;
  - GL output is checked by owner hand test with a reference scene set.

---

## 8. Risks and unknowns

| # | Risk | Impact | Mitigation |
|---|---|---|---|
| R1 | Tab enum renumber (`tpMonitor` 3→4 …) breaks code or config that stored tab indices, or the conditional `InsertPage(tpMonitor…)` paths (main `MainFrame.cpp:1608-1669`) | wrong tab opens, hub "open tab" commands | grep audit of all 33 uses + any persisted index; alternative D1(b) avoids it |
| R2 | Realistic-view code reads global app_config; refactor touches hot render code shared with Prepare/Preview | regressions in viewport | refactor first as its own small PR with no behaviour change |
| R3 | SSAO / FXAA on FBO paths assume the default framebuffer | black or garbage export | explicit resolve, sized formats, `report_gl_errors` per stage (as thumbnails do) |
| R4 | Mac GL 4.1 driver quirks (Metal translation): MSAA resolve of depth, cubemap seamless, PBO stalls | Mac-only artefacts | Mac mini hand test each phase; feature-detect, fall back to no-MSAA |
| R5 | Toolpath export of huge prints (>20 M segments) is slow or runs out of memory at 4K SSAA | long waits | estimate + warn; draft quality default for toolpath video |
| R6 | Finish inference wrong (e.g. "Silk" in a non-silk name) | odd look | per-filament override, saved in scene |
| R7 | Colour names unavailable for most vendors | "≈ Dark Grey" labels | user edit, saved; D8 |
| R8 | Mac has no ffmpeg; AVFoundation is new Obj-C++ code | Mac video slips | PNG sequence fallback from day one; Mac hand test |
| R9 | ffmpeg `h264_mf` behaviour varies by Windows build/GPU | failed encode | automatic fallback to `libopenh264`, log stderr |
| R10 | 3MF size growth from embedded images | big projects | cap/re-encode images, de-dup by hash, warn |
| R11 | Procedural layer lines alias in video | shimmering | `fwidth` fade + SSAA; default "Subtle" |
| R12 | Transparency without OIT shows sorting errors on interlocking parts | glitches on translucent multi-part | per-object sort v1; OIT later if needed |
| R13 | Callout anchors on painted regions jump between frames in video | jitter | anchors stored object-local once; only label side switching uses hysteresis |
| R14 | Upstream Orca may later add something similar | merge conflicts | none found: OrcaSlicer PR title search for render/screenshot/turntable/video/HDR/PBR on 2026-10-18 found only the realistic-view family (#13704, #14080, #14702, #14724, #15770, #15809, #16291, #15883); BambuStudio and PrusaSlicer PR searches found nothing. Keep Showcase in new files; touch GLCanvas3D minimally |
| R15 | libvgcode is AGPLv3 (header of `Shaders.hpp`); Bambu-derived parts AGPL too | none for us (EdgeSlicer is AGPL) | note only |
| U1 | How fast is the per-fragment toolpath shading on the owner's laptop iGPU at 1080p? | decides whether Preview adopts it | measure in S5 before enabling outside Showcase |
| U2 | Is 128–256 px pre-filtered env enough for glossy/silk close-ups? | visible blockiness on mirror-like floors | 512 px top mip for the floor-reflection preset only |

---

## 9. Owner decisions

**Status 2026-10-18:** owner approved D1–D17 as recommended, except D12, which is changed to a user-branding watermark (see the row).

| # | Decision | Options | Recommendation |
|---|---|---|---|
| **D1** | Where Showcase lives | (a) own tab after Preview; (b) a "Showcase" mode button inside Preview; (c) a dialog | **(a)**, as the vision asks. (b) is the fallback if the tab renumber audit (R1) turns up problems |
| **D2** | Default render source | Model mesh + procedural layer lines / real toolpaths | **Model by default** (no slice needed, fast); Toolpaths opt-in, auto-offered when a slice exists |
| **D3** | Lighting tech for v1 | bundled pre-filtered env maps + 3-point lights / user HDRI from day one / analytic lights only | **Bundled env maps (CC0) + 3-point lights in S2**; user HDRI as S2b |
| **D4** | Where the filament finish comes from | inferred from name/type + per-scene override / new `filament_finish` profile key | **Inference + override now**; consider a profile key later (profile change, validators) |
| **D5** | Video encoders | Windows bundled ffmpeg; Mac AVFoundation; Linux PATH ffmpeg; PNG sequence everywhere | **Yes as listed**; MP4 H.264 default via `h264_mf` → `libopenh264` fallback; no new Mac ffmpeg download (consistent with 2026-10-09 decision) |
| **D6** | Callout text rendering | CPU `wxGraphicsContext` overlay for screen + export / ImGui in GL | **wxGraphicsContext**, so screen, still and video match at any resolution |
| **D7** | Default label fields | — | **Brand, product, colour name, hex+swatch, type/finish ON; weight and price OFF** |
| **D8** | Colour names when unknown | hex only / nearest named colour with "≈" / ask user | **Snapmaker library → user text → "≈ nearest name"** |
| **D9** | 3MF storage | `Metadata/showcase/` sidecar with embedded images / external file references | **Sidecar, images embedded (capped, de-duplicated), excluded from Bambu export** |
| **D10** | Showcase render as 3MF cover | auto / opt-in / never | **Opt-in checkbox, off by default** (covers are also the printer/Bambu thumbnails' sibling; don't surprise) |
| **D11** | Max export size | 8K cap, no tiling / tiled up to 16K | **8192 internal px per side, no tiling in v1** |
| **D12** | Watermark | off / on by default / optional | **OWNER 2026-10-18: user branding watermark** – the user's own logo image and/or text, adjustable (position/corner, size, opacity, margin, text font/colour), saved as reusable presets, applied to stills and video; off by default. No forced "Made with EdgeSlicer". |
| **D13** | Toolpath smoothing (owner observation) | A+B+F (analytic cross-section normals, skin flatten/overlap, Smoothness slider, finish-aware defaults) / D screen-space normal blur / E mesh top skin + line normal map / leave as is | **A+B+F** in libvgcode shaders; Showcase first, Preview realistic view after your look sign-off; reject C and E, keep D as a later experiment |
| **D14** | Light behaviour in Showcase | world-fixed / camera headlight like Prepare | **World-fixed** (required for believable orbit video); headlight stays in Prepare/Preview |
| **D15** | Transparency quality | per-object sort (v1) / weighted-blended OIT | **Per-object sort v1**, OIT only if users hit it |
| **D16** | Phase order | S1 → S2 ∥ S3 → S4 → S5 | **As listed**; procedural layer lines in S2 so the "printed look" arrives before toolpath mode |
| **D17** | Plate scope | per-plate scenes / one scene for all plates | **Per plate**; "all plates side by side" later |
