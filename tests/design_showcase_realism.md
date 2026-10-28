# Showcase realism: design (2026-10-25)

Design only. Nothing here is built yet. It covers how to take the Showcase tab from a good-looking raster render toward a photo, in four phases:

1. cheap realism in the existing raster renderer;
2. real bead geometry from the toolpaths;
3. an offline progressive path tracer;
4. a Blender export with a companion add-on.

It builds on `tests/design_showcase_tab.md` (S1 to S5, decisions D1 to D17). Repo state read: main at 5bbeb60507 (after #699 and #703).

---

## 0. TL;DR

- **The biggest blocker is not a missing effect.** Every Showcase shader tone-maps and writes 8-bit sRGB itself, into `GL_RGBA8` buffers. So nothing after the draw (depth of field, bloom, grain, motion blur, a photo composite) can see real light levels. **Phase 1 starts with an HDR scene buffer and one post-processing pass.** It is the first PR.
- **The second blocker is that Showcase has two looks.** Mesh mode lights through `showcase_material` (GGX, image-based light, finishes). Toolpath mode (the sliced look and the build-up) lights through libvgcode's Preview Phong, which has no environment light, no finishes and no translucency. Toolpath mode already has the extrusion-aligned silk streak (Kajiya-Kay along `seg_dir`), but under the wrong light. **Phase 1 brings the toolpath shader up to the material model.** After that, the silk sheen along the real extrusion direction is a small step.
- **Phase 2 does not need a mesh for the live view.** libvgcode already instances one primitive per segment. Upgrading that to a ray-cast tube impostor gives real silhouettes at no memory cost. Real tube meshes are generated only for export (Blender, path tracer), with level of detail (LOD) and culling.
- **Phase 3: start on the CPU, with Embree + Open Image Denoise (OIDN).** Both are Apache-2.0, both ship arm64 macOS builds, and both use the TBB we already ship. This path runs on every machine, including the Mac (GL 4.1, no compute) and the headless `--showcase-render`. A GPU tracer comes second, and as a **fragment-shader tracer, not GL 4.3 compute**, so it also runs on the Mac.
- **Phase 4 can come before phases 2 and 3.** A GLB plus a JSON side file, a GPL-3.0-or-later add-on with one panel, and "Open in Blender". It gives photo-real stills to anyone with Blender for an M-sized effort.
- **Recommended order:** 1a HDR/post → 1b toolpath parity + silk → 1h HDRI and backplates → 1c-1f finishes and imperfections → 1g camera effects → 4 Blender → 2 beads → 3 path tracer.

---

## 1. Where Showcase is today

### 1.1 The pieces, what they do, and where they fall short

Files: `src/slic3r/GUI/Showcase/ShowcaseRenderer.cpp` (draw order in `draw_scene`, export in `render_export`), `resources/shaders/140/showcase_*.fs`, `src/libslic3r/Showcase/ShowcaseMaterial.*`, the libvgcode smooth path in `src/libvgcode/src/Shaders.hpp`.

| Piece | What it does now | Where it falls short of a photo |
|---|---|---|
| **Frame order** (`draw_scene`) | Backdrop → plate or floor → shadow map → contact shadow + mirror → objects (material, toolpaths or Phong fallback) → SSAO | Each pass writes final 8-bit sRGB. There is no HDR stage and no post stage. |
| **Framebuffers** | `GL_RGBA8` colour + D24S8, MSAA renderbuffer resolved to a texture; export supersampled 1-3x and downsampled on the CPU (`downsample_rgba`) | 8-bit after tone mapping: bright highlights clip before any blur, and translucent blending happens in display space, not linear light |
| **Tone mapping** | **Khronos PBR Neutral** (not ACES as the brief said), inside `showcase_material.fs` and `showcase_background.fs`; exposure in EV | Done per shader. The floor and toolpaths have their own curves. No filmic "highlight roll-off" choice. |
| **Image-based light** | 6 bundled Poly Haven CC0 environments, baked to GGX-prefiltered cube atlases: **128 px a face, 5 mips, RGBM range 16**, about 1.3 MB in total; diffuse light from 9 SH coefficients; the manifest stores the sun direction | 128 px is fine for reflections on rough plastic, but blurry as a backdrop (background mode 5). RGBM 16 clips the sun, so there are no sharp sun highlights and the shadow comes from the analytic key light, not the HDRI. No user HDRI yet (S2b in the old design). |
| **Material** (`showcase_material.fs`) | GGX specular + Lambert diffuse; per finish: roughness, F0, coloured highlight (metal_tint), anisotropy (silk), clear coat (gloss), alpha (transparent), groove shade, fibre speckle (CF). Karis analytic split-sum, no LUT | The anisotropy tangent is always **horizontal** (`cross(up, n)`). That is right for walls but wrong for top skins, whose lines run along the infill direction. No sheen lobe for matte or CF fuzz. No flakes. No subsurface light. |
| **Finishes** (`ShowcaseMaterial.cpp`) | Standard, Matte, Silk, Gloss, Transparent, Metallic, CarbonFiber; inferred from the preset name and type (D4); PETG infers to Gloss | No Sparkle/glitter finish. No "translucent natural PETG" as distinct from clear. |
| **Layer lines (mesh mode)** | Procedural: the normal is tilted by a sin bead profile at the object's real layer heights (uniform, or an R32F table), a groove darkens, and the stripes fade below 1.5 px a layer to stop moire | Perfectly regular: no wobble, no seam, no elephant's foot, no colour drift. These are what make a print read as real. |
| **Toolpath look** (libvgcode smooth path, S5/D13) | Each segment is an instanced primitive; the fragment shader gives a superellipse bead normal, skin flatten and overlap, groove crease, and moire LOD. **Silk = Kajiya-Kay streak along the true extrusion direction `seg_dir`**. | Lit by Preview's two fixed eye-space lights + ambient + shadow map. **No IBL, no GGX, no finishes, no translucency, clamped to 0..1.** The silhouette is the instanced box (a faceted outline). |
| **Shadows** | One shadow map from the world-fixed key light (D14), 5x5 PCF, slope bias; shadow catcher floor writes shadow as alpha | Hard-edged at constant softness (no contact hardening). Fill and rim cast nothing. Translucent filaments cast fully black shadows (depth-only map). |
| **Contact shadow** | Top-down orthographic footprint (MAX blend), separable 9-tap Gaussian (`showcase_blur.fs`), read by the floor | Good. Only on the floor, not between objects. |
| **SSAO** | The realistic view's SSAO (`ssao.fs`), with its radius scaled to the export size | Screen-space, so it darkens only what is on screen. No specular occlusion: reflections in crevices stay bright. |
| **Reflections** | Planar mirror of the objects at half size, blurred through mips by the floor's roughness; the objects reflect only the environment | No reflections between objects. No reflection of the floor in the object. |
| **Translucency** | Translucent volumes are sorted back to front per volume, back faces then front, with alpha raised at the Fresnel rim | No thickness, so a thick wall looks as clear as a thin one. No refraction, no tinted shadow, no glow through thin walls. Sorting errors appear on interlocking parts (R12). |
| **Anti-aliasing** | Export: SSAA x MSAA (`plan_export`, 1 GB budget). Live: MSAA. **Showcase does not run FXAA**: the FXAA pass belongs to the editing canvas and `render()` returns before it for `CanvasShowcase` | Fine for stills. The live view shimmers on fine layer lines while orbiting. |
| **"Blur"** | Two things: the contact-shadow Gaussian, and the backdrop picture's mip-and-disc soft focus | Not a camera blur. There is no depth of field on the model. |
| **Floor / backdrop** | Solid, gradients, image (fit/fill/tile), environment, transparent; photo floors (6 CC0 tiles), rotation and flip (#703) | A photo backdrop is pasted flat behind the model. The camera is not matched to it (horizon, focal length), and its light does not light the model. |
| **Video** | Paths, events, layer build-up from real toolpaths, callouts; the encoder is LGPL ffmpeg on Windows and AVFoundation on Mac | Each frame is a sharp instant: no motion blur, no grain, no gate weave. |
| **Callouts** | ID pass over the mesh volumes (`render_id_pass`), drawn on the CPU (D6) | Not a realism issue. Phase 2 must keep their anchors valid (see 3.5). |

### 1.2 The GL floor we design against

| Platform | What we get | Consequence |
|---|---|---|
| Windows / Linux | Core context, highest of 4.6 down to 3.2 (`s_core_gl_versions` in `OpenGLManager.cpp`). Intel UHD/Iris Xe and every discrete GPU give 4.6 on Windows | Compute shaders are available here |
| macOS | **Core 4.1, no compute, no SSBO, no image load/store**, `GL_MAX_SAMPLES` often 4, unified memory; OpenGL is deprecated (a layer over Metal) | Everything in phase 1 must be fragment-shader only. A GL 4.3 compute tracer would be Windows/Linux only. |
| Showcase minimum | `ShowcaseRenderer::is_supported()`: GL 3.2 core + ARB framebuffers | Every new effect needs a "below this tier: off" rule |

Things we can count on at 3.2/4.1 core: RGBA16F and RGBA32F render targets (MSAA too), texture buffers (`samplerBuffer`, already used by libvgcode), seamless cube maps, `gl_FragDepth`, MRT, and `texelFetch`.

---

## 2. Phase 1: cheap realism (raster, all platforms)

Everything here stays in the existing renderer, works on Mac GL 4.1, and degrades on an Intel iGPU. Each item is a separate PR.

### 2.1 1a: the HDR scene buffer and one post pass (foundation; the first PR)

**What the user sees:** nothing changes at default settings. That is the test. Highlights stop clipping before blur, translucent filaments blend in linear light, and a "Look" section gains Tone (Neutral / Filmic) and Exposure.

**Technical approach**
- Scene colour becomes `GL_RGBA16F` (live and export, MSAA where allowed). Depth stays D24S8.
- `showcase_material`, `showcase_background` (env mode) and `showcase_floor` stop tone-mapping. They output linear radiance × exposure. Colour, gradient and image backdrops are converted sRGB→linear on output, so they come back out of the post pass unchanged. Tone mapping, if applied to them at all, uses an exact inverse.
- libvgcode smooth path (`smooth_mode != 0` only, so Preview is untouched): skip the `clamp(c, 0, 1)` and output linear.
- New `showcase_post.fs` (a full-screen quad, as SSAO uses):
  - tone map: PBR Neutral as now, plus an optional AgX-style filmic curve;
  - linear→sRGB;
  - the hooks for 1g (vignette, grain, chromatic aberration).

  DoF and bloom run before it, on linear data.
- SSAO keeps working on the resolved depth. It multiplies the linear colour before tone mapping, which is more correct anyway.
- Export readback stays RGBA8 after the post pass. A video frame accumulator (1g motion blur) reads the 16F buffer instead.
- `plan_export`/`export_memory_estimate` count 8 bytes per pixel for colour, not 4.

**Files:** `ShowcaseRenderer.cpp/.hpp` (framebuffers, `draw_scene`, `render_export`, a new `draw_post`), `showcase_material.fs`, `showcase_background.fs`, `showcase_floor.fs`, new `showcase_post.fs`, `src/libvgcode/src/Shaders.hpp` (smooth path tail), `ShowcaseScene.hpp/.cpp` (Look: tone curve, JSON + 3MF round trip), `ShowcaseScene.cpp` `export_memory_estimate`.

**Cost:**
- +4 bytes per pixel on colour.
- At a 1080p High export (3840x2160 internal, 4x MSAA), colour grows from 133 to 265 MB. Total ~0.6 GB, still inside the 1 GB budget.
- At 4K High, auto-degrade drops MSAA sooner.
- The post pass is < 1 ms at 1080p on an iGPU.
- Mac and Intel iGPU: fine; RGBA16F MSAA is core 3.2.

**Risks:**
- A subtle colour shift on every existing scene.
- Floor alpha (shadow catcher, transparent export) must still composite straight alpha correctly through the post pass.

**Size / model:** M, **Opus** (it touches every shader and the libvgcode tail).

**As built (PR feat/showcase-hdr-post, 2026-10-26).** Two findings changed the plan above:
- **The backdrop, plate and floor stay display-referred.** PBR Neutral reaches white only at infinite radiance, so a white backdrop pushed back through an inverse curve is radiance in the hundreds. The shadow catcher's alpha (and every floor fade) blended against that in linear light all but vanishes. So the frame is two layers sharing one depth buffer: the RGBA8 scene buffer (backdrop, plate, floor, drawn and blended exactly as before) and the RGBA16F objects layer (the materials as linear light, premultiplied by coverage). `showcase_post.fs` applies exposure, the tone curve (Neutral or Filmic) and sRGB to the objects layer and lays it over the scene buffer with the blending the objects used before. With MSAA both layers are multisample textures and `showcase_post_ms.fs` tone-maps and composites each sample before averaging (resolving linear light first and tone-mapping the average moved edge pixels by up to 9/255 in the first golden run). One translucent layer over the backdrop is unchanged; translucent over opaque now mixes in linear light. SSAO still runs on the finished colour after the post pass (unchanged look; moving it before the curve is a later, visible change). The environment backdrop tone-maps in its own shader with the scene's curve.
- **The toolpaths stay in the scene buffer until 1b.** Their Preview lighting is display-referred, and PBR Neutral has no inverse for saturated highlights (pure blue at 255 is never an output of the curve), so they cannot pass through it unchanged. libvgcode is untouched in 1a. 1b gives them real linear lighting and moves them into the objects layer.
- The live view renders off-screen (the window's size and sample count) and is copied to the window. Without float buffers, the post shader or with `EDGESLICER_SHOWCASE_NO_HDR=1`, everything draws as before, each shader tone-mapping itself.
- Checks: `tests/showcase/golden.sh` (6 stills + a short PNG-sequence video against the shared `_baseline`), `tests/libslic3r/test_showcase_tone.cpp`.

**Checks:**
- Golden stills via `--showcase-render` of 5 fixture scenes (Studio white, Dark gradient, Build plate, Desk wood, transparent PNG) before and after. Mean ΔE < 1 and no pixel > 4 at default settings.
- The transparent-PNG alpha matches byte-for-byte where there is no object.
- Mac hand test (one still).

### 2.2 1b: toolpath mode on the same material model

**What the user sees:** the sliced look and the build-up video get the same environment light, finishes, gloss and translucency as mesh mode, so cutting from mesh to toolpaths in a video no longer changes the lighting.

**Technical approach:** in the libvgcode smooth path (`smooth_mode != 0`), replace `smooth_lit()` with the material function from `showcase_material.fs`:
- GGX + IBL (SH diffuse, prefiltered cube), with the three world-fixed lights;
- finish parameters per extruder through the existing `look_a/look_b[16]` arrays, extended with roughness, F0, metal tint, clear coat and alpha.

To avoid two copies drifting, move the shared GLSL into one include string that both build from:
- `ShowcaseMaterialGlsl.hpp` in libslic3r;
- libvgcode gets it injected through a new `Viewer::set_fragment_prelude()`, so libvgcode stays free of Showcase knowledge.

Translucency in toolpath mode comes later (it needs order-independent transparency (OIT) for millions of segments). v1 shows translucent filaments opaque but with the 1c thickness glow faked from the bead width.

**Files:** `src/libvgcode/src/Shaders.hpp`, `src/libvgcode/include/Viewer.hpp`, `src/libvgcode/src/ViewerImpl.cpp` (uniforms, env texture unit), `ShowcaseRenderer.cpp` (`draw_toolpaths`: bind env, SH, lights), new `src/libslic3r/Showcase/ShowcaseMaterialGlsl.hpp`.

**Cost:**
- About 2x the fragment work of today's smooth path. That is fine on discrete GPUs.
- On an Intel iGPU, a 5M-segment print orbits at about half today's frame rate. The live view already uses the S5d moire LOD; add "every n-th layer while moving" (old design §6).

**Risks:**
- libvgcode is upstream code. Keep every change behind `smooth_mode` so a later upstream port stays mechanical.
- Uniform array limits on the Mac (16 extruders × 4 vec4) are fine.

**Size / model:** M, **Opus**.

**Checks:** a golden still of the toolpath look before and after (expected to change: review side by side); a Preview identity check (smooth_mode 0: pixel-identical screenshot of Preview); `libslic3r_tests` for the GLSL include assembly.

**As built (feat/showcase-1b, 2026-10-27).**
- The shared GLSL is not a C++ string: it stays in `showcase_material.fs`, between `// ---- BEGIN/END SHOWCASE_MATERIAL_COMMON ----` markers (`shade_material()`, `material_output()` and what they use). `Showcase::toolpath_prelude()` (libslic3r `ShowcaseMaterialGlsl`) cuts it out at run time and appends `showcase_toolpath.glsl` (`external_lit()`, `external_output()`, per-extruder `sc_mat_a/b[16]`). One copy, so the mesh and the toolpaths cannot drift; `test_showcase_glsl.cpp` checks the assembly against the shipped files and that no prelude global clashes with libvgcode's segments shader (it parses `Shaders.hpp`).
- libvgcode: `Viewer::set_fragment_prelude()` compiles a second segments program (`EXTERNAL_LIGHTING` defined, prelude after `#version`); `set_use_fragment_prelude()` selects it only while the smooth look is on. Preview never sets it, and the built-in program is unchanged.
- The toolpaths go into the HDR objects layer (opaque, alpha 1) and the post pass, as the mesh. Fallbacks: no float buffers or `EDGESLICER_SHOWCASE_NO_HDR=1` → the material model tone-mapped in the shader; prelude fails to build or `EDGESLICER_SHOWCASE_CLASSIC_TOOLPATHS=1` → Preview's lighting as before. The export report says which ("toolpaths: material, HDR layer" ...).
- The silk lobe's tangent in toolpath mode is already `seg_dir` here (the analytic lights); 1c adds the bent IBL lookup.
- No toolpath shadows yet: libvgcode's own `shadow_shade()` is passed in, and Showcase does not give it a shadow map (as before 1b).

### 2.3 1c: silk sheen along the real extrusion direction

**What the user sees:** silk prints show the sliding "liquid metal" band that follows the lines. On walls it runs around the part; on top skins it runs along the infill lines and flips 90° layer to layer, as a real silk print does.

**Technical approach**
- **Toolpath mode (after 1b):** anisotropic GGX with tangent = `seg_dir` (already passed per segment) for the lights, and a bent-reflection lookup for IBL:
  - stretch the reflection vector along the tangent, then sample the prefiltered cube at a roughness raised across it;
  - this is the standard approximation; there is no anisotropic prefiltered map.

  The current Kajiya-Kay term becomes the fallback when IBL is off.
- **Mesh mode (no slice needed):**
  - walls keep the horizontal tangent (correct for perimeters);
  - top and bottom faces (|n.z| > 0.9) take a tangent at the object's `solid_infill_direction` (or `infill_direction` for sparse layers), rotated by 90° on odd layers when the top pattern is rectilinear or monotonic;
  - for concentric patterns, use the radial-perpendicular direction around the face's centroid;
  - patterns we cannot model (Hilbert, Archimedean chords) fall back to an isotropic highlight.

  The settings come from the object's print config when the Showcase scene is built (one uniform per object).
- **Multi-colour silk** (dual/tri-colour coextruded): out of scope here, but the finish table should reserve the name.

**Files:** `showcase_material.fs`, `ShowcaseMaterialGlsl.hpp`, `ShowcaseRenderer.cpp` (per-object skin angle and pattern uniform), `ShowcaseMaterial.cpp/.hpp` (`skin_tangent_mode()` from the config, unit-testable).

**Cost:** a few ALU ops; free on all tiers.

**Risks:** the top-skin direction from the config is a guess when `align_infill_direction_to_model` is on (it rotates with the object). Read the effective angle the slicer uses (`align_infill_direction_to_model` plus the object rotation), or, when the plate is sliced, take it from the toolpaths.

**Size / model:** S, **Sonnet**.

**Checks:** unit tests for the tangent mode from config combinations; a golden still of a silk vase (walls) and a silk flat plaque (top skin) at a raking light; a hand check that the band moves the right way while orbiting.

### 2.4 1d: subsurface scattering and thin-wall translucency

**What the user sees:**
- light glows through thin walls and edges (a lamp shade, a vase, a translucent PETG cup);
- thick parts stay dense;
- coloured translucent filament throws a tinted, lighter shadow;
- plain PLA gets a faint softness at the edges, so it no longer looks like painted metal.

**Technical approach (three cheap tricks, all fragment-only):**
1. **Thickness pass.** Before the objects, draw back-face depth of the translucent and "SSS" materials into an R16F/R32F texture at half resolution. In the material pass, thickness = back depth − front depth along the view ray. Transmittance = `exp(-sigma_t * thickness)`, tinted by the albedo, where `sigma_t` comes from a new finish parameter `density`:
   - clear PETG is low;
   - natural PLA is medium;
   - opaque PLA is very high, so the term is off.

   The transmitted light is the key light from behind plus the environment SH in the −n direction.
2. **Wrap lighting + translucency term** (the "fast SSS" of game engines) for opaque plastics with a small `sss` weight: diffuse with a wrap factor, plus `pow(saturate(dot(v, -(l + n·distortion))), p) * thickness_falloff`. It reuses the thickness from 1. where available. Otherwise it uses a constant from the finish.
3. **Coloured shadows.**
   - Add an RGB transmittance shadow map: translucent casters are drawn into a second colour texture, multiplied, alongside the depth map.
   - The floor and objects multiply the shadow by this colour when the receiver is behind a translucent caster.
   - Memory is one extra RGBA8 at the shadow map size (4096²: 64 MB; 2048²: 16 MB live).

**Files:** `ShowcaseRenderer.cpp` (thickness pass, transmittance map), new `showcase_thickness.fs`, `showcase_material.fs`, `showcase_floor.fs`, `ShowcaseMaterial.cpp/.hpp` (`density`, `sss` per finish), `src/slic3r/GUI/GLCanvas3D.cpp` (only if the shadow pass needs a hook for the colour map; prefer a Showcase-side pass).

**Cost:**
- One extra geometry pass for translucent objects only (usually one or two volumes).
- Half-res thickness at 1080p is 4 MB.
- iGPU: fine. Mac: fine.

**Risks:**
- Thickness from a single back face is wrong for nested shells, such as a cup seen through two walls. Accept it and document.
- The transmittance shadow map doubles shadow draw calls.

**Size / model:** M, **Sonnet** (plain shader work with clear references).

**Checks:** a golden still of a translucent PETG cup on a white floor (glow at the rim, tinted shadow); a 0.8 mm vs 5 mm wall test part (the thin wall glows, the thick one does not); unit tests for the transmittance maths.

### 2.5 1e: glitter, matte carbon-fibre fuzz, translucent PETG

| Finish | What the user sees | Approach | Cost |
|---|---|---|---|
| **Sparkle** (new; "glitter, sparkle, galaxy, starlight") | Tiny flakes that wink as the camera or the light moves | Stochastic flakes: a 3D cell hash in **object space** (so flakes stick to the part in video) at ~0.15 mm cells. Each cell has a random flake normal and a random on/off with density from the finish; a tight GGX lobe per flake (light + IBL at low roughness). LOD: when a cell is under ~1 px (`fwidth`), fade to the average "sparkle haze" (a broader, weaker lobe), so videos do not fizz. | ~30 ALU; fine everywhere |
| **Matte / CF fuzz** | Velvet-like grazing brightening on carbon-fibre and matte filaments; fibres catch light along the lines | A sheen lobe (Charlie/Estevez-Kulla, as `KHR_materials_sheen`) added to Matte and CarbonFiber; CF keeps its fibre speckle but aligned along the extrusion tangent from 1c (short streaks, not dots) | small |
| **Translucent PETG** (new name `translucent`; PETG "natural", "clear", "transparent" still go to `transparent`) | Milky glass: soft see-through, bright rim, a blurred view of what is behind | The 1d thickness and transmittance, plus **screen-space refraction**: copy the opaque scene (HDR, mipmapped) before translucent objects, and sample it offset by the refracted normal, at a mip set by roughness. This is the one place translucent objects are better drawn after the opaque copy. | one colour copy + mips (~16 MB at 1080p) |

Inference rules (`infer_finish`):
- add the words for Sparkle;
- keep PETG → Gloss for opaque colours;
- "natural", "clear" and "transparent" → Translucent or Transparent (owner decision D-R4).

New JSON names are stable and never translated, per the existing rule.

**Files:** `ShowcaseMaterial.cpp/.hpp` (enum, names, params, inference), `showcase_material.fs`, `ShowcaseRenderer.cpp` (opaque copy for refraction), `ShowcaseView.cpp` (finish dropdown), `tests/libslic3r/test_showcase_materials.cpp`.

**Risks:**
- Sparkle aliasing in video is the classic failure. The object-space cells plus the LOD fade are required, not optional.
- Screen-space refraction cannot show what is off screen; at the frame edge it falls back to the environment.

**Size / model:** M, **Sonnet**.

**Checks:**
- inference unit tests (new words; "MatterHackers" still not matte);
- golden stills of a sparkle Benchy and a CF bracket;
- **a 3 s turntable of the sparkle part, checked for flicker:** the mean absolute frame-to-frame difference stays within 1.5x that of a matte part.

### 2.6 1f: subtle imperfections

**What the user sees:** an "Imperfections" slider (Off / Subtle (default for presentation presets) / Visible). The "Technical" presets keep it Off.

| Imperfection | Approach | Data it needs |
|---|---|---|
| **Layer wobble** (Z-banding) | Per layer, a hash gives a small XY offset that tilts the wall normal (mesh mode) or shifts the bead (toolpath mode, a vertex offset in the instanced segment) by ±0.02-0.05 mm, smoothed over 3-10 layers | layer index (already there: the layer table, libvgcode layer id) |
| **Seam bumps** | A bump and a slight gloss change along the seam line | **Toolpath mode:** libvgcode already knows seam points (`EOptionType::Seams`); add a small bump at each seam vertex (a radial bulge on the segment touching it). **Mesh mode, when sliced:** a per-layer seam XY table (R32G32F texture, one entry per layer and object) taken from the same seam points; the shader bumps walls within ~0.4 mm of that point. **Not sliced:** no seam (we do not guess). |
| **Elephant's foot** | The first 1-3 layers bulge out by up to `0.15 mm − elefant_foot_compensation` (never negative), with a slightly glossier, flatter first layer (the bed side) | first layer height, `elefant_foot_compensation`, object Z = 0 |
| **Colour variance** | Low-frequency 3D noise of ±1-2% value and ±1° hue per filament, plus a tiny per-layer shift, so large flat areas are not one flat colour | none |

Every term is driven by object-space or layer coordinates, never screen space, so it is stable in videos. Each one fades with the same "pixels per layer" LOD as the layer lines.

**Files:** `showcase_material.fs`, `ShowcaseMaterialGlsl.hpp`, libvgcode smooth path (wobble and seam bump in the vertex stage, behind `smooth_mode`), `ShowcaseRenderer.cpp` (seam table from libvgcode), `ShowcaseScene.hpp/.cpp` (Imperfections level, JSON/3MF), `ShowcaseView.cpp`.

**Cost:** small ALU; one small texture per object.

**Risks:**
- "Subtle" is a taste call. The owner signs off on the default strength from a still.
- Seam extraction couples Showcase to libvgcode option data. Keep it read-only.

**Size / model:** M, **Sonnet**.

**Checks:** unit tests (elephant's foot amount from config, seam table build from a known G-code fixture); a golden still at Off identical to before; a side-by-side owner card at Subtle and Visible.

### 2.7 1g: camera effects

**What the user sees:** a "Camera" section:
- aperture (f-number) and focus (click the model to focus; the ID pass's `world_at` gives the point);
- bokeh shape (disc / 6 blades);
- grain, vignette and chromatic aberration (each a 0-100 slider, all default 0 except a light vignette in the presentation presets);
- in videos, motion blur (off / 180° shutter / 360°).

| Effect | Approach | Cost (1080p) | iGPU / Mac |
|---|---|---|---|
| **Depth of field + bokeh** | Physical circle of confusion (CoC) from depth, f-number, focal length (from the FOV and a 36 mm sensor) and the focus distance. Half-res gather DoF (a separate near/far field, a ring-sampled disc or hexagon kernel of ~48 taps, brightness-weighted so HDR highlights bloom into bokeh balls), upsampled and composited by CoC. Needs 1a. | 1.5-3 ms discrete, 6-10 ms iGPU live | iGPU live view uses 24 taps; export uses full taps at internal size |
| **Grain** | Luma-weighted film grain after tone mapping, blue-noise texture (64² tile, CC0 or generated), new seed per video frame, scaled to the output size, not the internal SSAA size | < 0.2 ms | fine |
| **Vignette** | A cos⁴-style falloff in the post pass, relative to the safe frame (so live = export) | free | fine |
| **Chromatic aberration** | A radial R/B offset of up to ~1.5 px at the corners (output pixels) | free | fine |
| **Motion blur (video)** | Accumulate N sub-frames per output frame over the shutter interval: N = 8 at 180°, adaptive (2-16) from the camera's screen-space speed. The path evaluator already gives a camera pose at any time; the build-up and events are evaluated at each sub-time. Accumulate in an RGBA16F (or CPU float) buffer before the post pass, then post once. | N x the frame cost | same, just slower; the CLI shows the estimate first (S4's "time one frame x N") |
| Bloom (optional, small) | A 5-level mip-chain bloom on linear HDR, weight 0.02-0.05 | ~1 ms | fine |

**Files:** `showcase_post.fs`, new `showcase_dof.fs`, `ShowcaseRenderer.cpp` (passes, accumulation), `ShowcaseViewVideo.cpp` and `src/libslic3r/Showcase/ShowcaseVideo.*` (sub-frame times, shutter), `ShowcaseScene.hpp/.cpp` (camera settings, JSON/3MF), `ShowcasePath.*` (events may animate focus and aperture: the "focus pull"), `docs/showcase_paths.md`, `ShowcaseView.cpp`.

**Risks:**
- Gather DoF has edge halos at depth discontinuities. Use the standard near/far separation and a max-CoC dilate.
- Motion blur multiplies video export time. Default it Off, and show the time estimate.
- Callouts are drawn over the finished image (D6), so they are never blurred. That is correct.

**Size / model:** M, **Sonnet** (DoF itself is the hardest part; Opus if the first attempt shows halos).

**Checks:** unit tests (CoC from f-number and focus, sub-frame times and shutter, adaptive N); golden stills at f/2.8 and f/16; a short orbit video with motion blur on and off (frame-difference smoothness); grain-seed determinism (the same frame index gives the same grain).

### 2.8 1h: HDRI environment presets, backplates, a shadow catcher onto a photo

**What the user sees:**
- **Environment presets** ("Studio softbox", "Workshop bench", "Window daylight", "Overcast street", "Sunset"…) light the model *and* fill the background sharply. Sun environments give a sharp sun highlight and a crisp sun shadow.
- **Backplate mode:** a real photo behind the model (a desk, a workshop shelf) taken with the HDRI. The model sits on it with its shadow and a faint reflection, lit by that HDRI. The camera starts matched to the photo (its horizon and focal length) and can only orbit within limits that stay believable.
- **"Use my photo":** the user's own picture as a backplate with a shadow catcher. A simple horizon line and floor-tilt control matches the camera. Lighting comes from a chosen preset or the user's HDRI.
- **User HDRI** (`.hdr`/`.exr`), finally doing S2b.

**Technical approach**
- **Two textures per environment:**
  1. the existing 128 px prefiltered atlas for lighting (unchanged);
  2. a **backdrop equirect** at 2k (bundled) or 4k (downloaded), loaded as RGBA16F, for the background, sharp reflections at roughness < 0.1, and sun extraction.
- **HDR loading:** a C++ Radiance RGBE reader (~150 lines; the Python baker already has one to port), and OpenEXR, which is **already in `deps/`**, for `.exr`.
- **User HDRIs:** prefilter at load in fragment passes (equirect → cube → GGX mips, 64 samples a texel at 128 px, ~0.3 s), with SH projection on the CPU.
- **The sun becomes a real light.** The manifest already stores `sun_dir`. The sun's colour and solid angle are measured at bake time. The key light becomes the sun in sun environments (`LightRig::Sun`), so the shadow map and the sharp highlight agree with the picture. Its brightness is removed from the IBL map (clamped) so it is not counted twice.
- **Backplates** (Poly Haven ships matching backplate photos for some HDRIs; for the rest, the 4k equirect itself projected through the camera is the plate). Each preset's manifest entry gets:
  - `backplate` (file);
  - `camera`: height above the floor in mm-scaled units, pitch, focal length;
  - `horizon` (0-1 in the image);
  - `floor_tint` (for the catcher's reflection).

  The renderer draws the plate as the background (mode 3 already letterboxes images), the floor as a **shadow + reflection catcher** (shadow as alpha as now, plus the mirrored model at low strength, Fresnel-weighted), and the objects with the matched IBL rotation.
- **Camera match for user photos:** two drag handles on the live view (the horizon line, and the "floor depth" of a reference point), solved for pitch and camera height at the chosen focal length. This is enough for a believable tabletop shot; full vanishing-point calibration is out of scope.

**Licensing**

| Source | Licence | Notes |
|---|---|---|
| Poly Haven HDRIs, textures, backplates | **CC0**: no attribution required; redistribution and commercial use allowed ([polyhaven.com/license](https://polyhaven.com/license)) | We already bundle 6 (README in `resources/textures/showcase/environments/` lists each source). Assets come with no warranty, and some depict real places. |
| Poly Haven **API** | **Separate terms.** The [API page](https://polyhaven.com/our-api) says free for non-commercial use and commercial by agreement (and a unique User-Agent is required); the [GitHub Public-API README](https://github.com/Poly-Haven/Public-API) says free for any purpose with a "Powered by Poly Haven" credit; [corporate](https://polyhaven.com/corporate) mentions a revenue share for integrations | **Do not call the Poly Haven API from the app.** The assets are CC0, so we download the chosen files once, keep the originals and our bakes on our own mirror (a GitHub release asset in the existing edgeslicer-deps mirror), and download on demand from there. No API terms apply, and we do not depend on their uptime. |

**Size budget**

| Option | Install size added | Offline | Notes |
|---|---|---|---|
| A. Bundle nothing new | 0 | presets download | First-run surprise: "downloading…" before a still |
| **B. Bundle 3 presets at 2k (studio, workshop, outdoor) + their backplates; the rest on demand (recommended)** | ~15-20 MB (a 2k RGBE `.hdr` is ~5-7 MB; a backplate JPEG at 2.5k is ~1 MB) | the 3 work offline | Comparable to one font pack; the installer today is far larger |
| C. Bundle 8 at 4k + plates | ~150-200 MB | all | Too big for a slicer installer |
| D. Lower-res plates in the installer, 4k plates on demand | ~10 MB | 2k quality offline | Can combine with B |

Downloads land in `%APPDATA%\EdgeSlicer\showcase\environments\` (the app writes there at the user's request; agents never write there), with a SHA-256 from our manifest and a cancel button.

**Files:** `ShowcaseAssets.hpp/.cpp` (manifest: backdrop, plate, camera, sun colour, `download_url`, sha256), `ShowcaseTextures.cpp/.hpp` (16F equirect upload, user HDRI prefilter passes), new `src/libslic3r/Showcase/ShowcaseHdr.cpp/.hpp` (RGBE reader, EXR via OpenEXR, SH projection, sun extraction: GL-free and unit-tested), new `showcase_prefilter.fs`, `showcase_background.fs` (equirect + plate), `showcase_floor.fs` (reflection catcher), `ShowcaseRenderer.cpp`, `ShowcaseView.cpp` (preset gallery, downloads, horizon handles), `scripts/showcase/bake_showcase_assets.py` (2k outputs, sun measurement), `resources/textures/showcase/environments/*`.

**Cost:**
- A 2k RGBA16F equirect is 16 MB of VRAM (4k: 64 MB).
- The prefilter of a user HDRI takes ~0.3 s on discrete GPUs and ~1 s on an iGPU, once per load.
- Mac: the 4k plate is fine (16384 limit).

**Risks:**
- Camera match quality decides whether a backplate looks real or pasted. Keep the camera locked near the plate's pose by default, with an "unlock" option.
- Double-counted sun.
- The download UX on a slow line.

**Size / model:** L overall. Split it:
- 1h-1: HDR loader, 2k backdrops, sun light (M, Sonnet);
- 1h-2: backplates + catcher + camera match (M, Opus);
- 1h-3: downloads + user HDRI (S-M, Sonnet).

**Checks:** RGBE/EXR reader unit tests on tiny fixtures; SH projection against the Python baker's numbers for the 6 bundled maps (tolerance 1e-3); a golden still per bundled preset; a backplate still reviewed by the owner (the one real "does it look like a photo" gate); a download test against a local mock server only.

### 2.9 Phase 1 summary

| Item | Size | Model | Depends on |
|---|---|---|---|
| 1a HDR buffer + post pass | M | Opus | none |
| 1b toolpath material parity | M | Opus | 1a |
| 1c silk along the extrusion | S | Sonnet | 1b (toolpaths), none (mesh) |
| 1d SSS / thin-wall translucency | M | Sonnet | 1a |
| 1e sparkle, sheen, translucent PETG | M | Sonnet | 1a, 1d |
| 1f imperfections | M | Sonnet | 1b for toolpath mode |
| 1g camera effects | M | Sonnet (DoF may need Opus) | 1a |
| 1h HDRI, backplates, catcher | L (3 PRs) | Sonnet / Opus | 1a |

**Minimum spec rule for all of phase 1:**
- GL 3.2 core: everything on, but live DoF and the thickness pass at half resolution.
- Legacy context: the tab stays disabled, as today.
- Intel iGPU: the live view auto-switches to "draft" while orbiting (no DoF, no thickness pass, every 2nd layer in toolpath mode) and refines on idle, as the old design §6 planned.

**Owner decisions, phase 1**

| # | Decision | Options | Recommendation |
|---|---|---|---|
| D-R1 | Default tone curve | Keep PBR Neutral / switch to filmic | **Keep Neutral** (filament colours stay true); filmic as an option |
| D-R2 | HDRI packaging | A / **B** / C / D (table above) | **B**: 3 at 2k bundled, the rest from our mirror |
| D-R3 | Where downloads come from | our mirror / Poly Haven API | **Our mirror**; never the API |
| D-R4 | PETG inference | Gloss as now / "natural, clear" → Translucent | **Translucent for natural and clear words only** |
| D-R5 | Imperfections default | Off / Subtle in presentation presets | **Subtle in presentation presets, Off in Technical** |
| D-R6 | Vignette default | 0 / light | **Light in presentation presets** |
| D-R7 | Sparkle as a finish | add / skip | **Add** (glitter filaments are common) |

---

## 3. Phase 2: real bead geometry

### 3.1 What exists, and what is missing

- libvgcode draws **one instanced primitive per toolpath segment**: positions, height/width/angle and colour come from texture buffers.
- The S5 smooth path shades a superellipse bead normal in the fragment shader. That already *is* the instancing strategy, at ~32 bytes a segment.
- What it cannot give:
  - **Silhouettes:** the outline is the proxy's flat facets, visible on curved walls at grazing angles and in close-ups.
  - **Junctions:** corner joints are overlapping boxes.
  - **Real geometry** for a path tracer or Blender.

### 3.2 What the user sees

- Close-ups of the sliced look show round beads with real outlines, rounded corners and the gaps between infill lines.
- Exports to Blender and path-traced stills use the actual extruded geometry.

### 3.3 Technical approach: two tiers

**Tier 1, the live view and raster export: a ray-cast tube impostor (no new memory)**
- Keep libvgcode's instanced proxy, but make it a slightly larger box around the segment.
- In the fragment shader, intersect the view ray with the bead: a swept superellipse between the segment's two ends, approximated as a rounded box (a capsule when the bulge is 2). Analytic ray/rounded-box intersection is ~40 ALU.
- Discard misses, write `gl_FragDepth`, and take the normal from the hit.
- Joints become round naturally, because two neighbouring rounded segments overlap at a shared sphere-like end.
- Behind `smooth_mode >= 2` ("Bead geometry: on"), so Preview never pays for it.
- **Cost:** writing `gl_FragDepth` disables early-Z for these fragments. Expect 1.5-2.5x the 1b fragment cost.
  - Live on an iGPU: impostors only while idle, the 1b look while orbiting.
  - Export: always on when enabled.
  - Mac 4.1: fine (`gl_FragDepth` is core).

**Tier 2, real meshes for export (Blender, path tracer CPU fallback)**
- A new GL-free `BeadMesher` in `src/libslic3r/Showcase/` reads the same move list libvgcode is built from (`GCodeProcessorResult` moves: position, width, height, role, extruder, layer).
- It sweeps a superellipse cross-section with k sides along each extrusion run:
  - shared rings between consecutive segments;
  - mitred or rounded joints by turn angle;
  - end caps at retractions and travels.
- It outputs chunks, one per (object, extruder, role group, block of 16 layers), each with its move-index range. That range is what the build-up and Blender use.

**LOD and culling (the memory problem)**

| Strategy | What it drops | Saving on a typical part |
|---|---|---|
| **Role culling** | Sparse infill, internal solid infill and internal perimeters are invisible in a finished print. Keep them only in the top ~10 layers of a build-up frame, or when "show internals" is on. | 50-70% of segments |
| **Sides by role** | External perimeter 8-12 sides; top skin 6; everything else 4 (a flattened diamond, as today) | 30-50% of vertices |
| **Segment merging** | Collinear runs (direction change < 2°) become one segment | 20-60% on straight walls |
| **Distance LOD** (Blender/path tracer: per chunk, from the export camera path's nearest approach) | k = 4 when a bead is under 2 px at the closest camera of the path | variable |

**Memory per million kept segments** (ring sharing, 16 B a vertex: float3 position + octahedral normal; 32-bit indices):

| Sides k | Vertices / segment | Triangles / segment | MB per 1M segments |
|---|---|---|---|
| 4 | 4 | 8 | ~160 |
| 6 | 6 | 12 | ~240 |
| 8 | 8 | 16 | ~320 |
| 12 | 12 | 24 | ~480 |

A Benchy at 0.2 mm is roughly 0.3-0.5M extrusion segments, and a large full-plate print 5-15M. After role culling and merging, a large print keeps ~2-5M, i.e. **0.5-1.5 GB at k = 6-8. That is too much to hold.**

**Limits:**
- the mesher streams chunks to the GLB writer and never holds the whole mesh;
- a hard cap of 1.5 GB of CPU memory for the mesher (the 1 GB GPU export budget is unaffected, since tier 2 is not drawn by GL);
- above the cap it lowers k for non-external roles first, then the culling band;
- the export dialog shows the estimate before it starts.

**For the path tracer (phase 3):**
- Embree's **round linear curves** draw tubes analytically from points and radii, at ~32 bytes a segment with no triangles.
- Their cross-section is round, not the flattened superellipse. To get an elliptical bead, use a user-geometry rounded box (the same intersection as tier 1) or triangles at k = 6 for external perimeters only.
- **Recommendation: user geometry with the tier 1 intersection.** One shape definition then serves raster and ray tracing, and the memory is the segment list itself.

### 3.4 Interaction with the layer build-up

- **Tier 1:** inherits libvgcode's view range, so the build-up works unchanged. The newest layer can get a "wet" gloss boost (a finish tweak on the top N moves) as a cheap touch.
- **Tier 2 / Blender:** chunks carry a move range. The build-up becomes chunk visibility per frame. Inside the current chunk, the add-on uses the `move_index` vertex attribute with a geometry-nodes "delete where move_index > t" (see §5).
- **Path tracer:** filters segments by move index at BVH build time per frame. The BVH is rebuilt per frame for a build-up video, which costs ~50-300 ms at a few million segments with Embree; acceptable next to the render time.

### 3.5 Interaction with callouts

- The ID pass draws **mesh volumes**, not toolpaths (`render_id_pass` loops over `m_volumes`). Beads lie within ~±0.2 mm of the mesh surface, so anchors and occlusion tests stay valid.
- Keep the ID pass on meshes. Do not move it to beads; it would cost a full extra bead pass per callout update for no visible gain.
- During a build-up, S5c already hides anchors above `build_height_at()`. Unchanged.

### 3.6 Phase 2 summary

**Files:**
- `src/libvgcode/src/Shaders.hpp`, `ViewerImpl.cpp`, `include/Viewer.hpp` (impostor mode, behind `smooth_mode`);
- new `src/libslic3r/Showcase/ShowcaseBeadMesher.cpp/.hpp`;
- the GLB writer from §5;
- `ShowcaseRenderer.cpp` (toggle, live/idle LOD);
- `ShowcaseView.cpp` ("Bead geometry" toggle, memory estimate);
- `tests/libslic3r/test_showcase_beads.cpp`.

**Performance and GPU limits:** tier 1 adds no memory and costs fragment work, as above. Tier 2 is CPU-only and streamed; the GPU never holds it.

**Risks:**
- An upstream libvgcode port conflicts with the impostor shader (keep it in a separate shader string selected by mode).
- Mesher correctness at sharp corners and on arcs (G2/G3 are already linearised by the processor).
- Overhangs and bridges sag in reality and not in ours. Ignore that in v1.

**Size / model:**
- tier 1 impostor: M, **Opus**;
- tier 2 mesher + LOD: L, **Opus**;
- culling/LOD tuning: S, Sonnet.

**Checks:**
- unit tests (mesher vertex and triangle counts per k; watertightness of a straight run; the memory estimate within 5% of the actual; role culling keeps external perimeters; the move-range chunks cover every kept move exactly once);
- golden stills of a toolpath close-up with impostors off and on (silhouette check);
- a memory check exporting the largest fixture (stays under the cap);
- a Preview identity screenshot.

**Owner decisions, phase 2**

| # | Decision | Options | Recommendation |
|---|---|---|---|
| D-R8 | Impostor beads in the live view | always / idle only / export only | **Idle only on iGPU, always on discrete** (auto) |
| D-R9 | Internals in exports | culled / kept | **Culled**, with a "show internals" toggle for cut-away shots |
| D-R10 | Mesher memory cap | 1 GB / 1.5 GB / 3 GB | **1.5 GB**, auto-lowering detail above it |

---

## 4. Phase 3: offline progressive path tracer

### 4.1 What the user sees

- A **Render** button next to Export in the Showcase panel, with the choices "Preview (fast)", "Final" and "Time limit: N min".
- Clicking it opens the frame in a render window inside the tab:
  - the image refines live;
  - a progress bar shows samples per pixel, elapsed time and the estimate;
  - **Cancel** keeps the best image so far;
  - **Save** writes PNG/JPEG/WebP (and 16-bit PNG or EXR for people who grade).
- After the last sample, OIDN denoises and the denoised image replaces the noisy one (a "noisy / denoised" toggle).
- For videos, "Render video (path traced)" uses a per-frame sample budget and shows the total estimate first.
- The CLI gains `--engine raster|pathtrace --spp N --time-limit S`.

### 4.2 GPU, CPU or both

| Option | Runs on | For | Against |
|---|---|---|---|
| A. GL 4.3 compute | Windows/Linux GL 4.3+ (Intel iGPUs included); **not Mac** | fast, SSBOs | needs a second implementation for Mac anyway; Windows GPU watchdog (TDR, 2 s) forces small dispatches |
| **B. Fragment-shader tracer** (BVH and triangles in texture buffers, ping-pong RGBA32F accumulation) | **every GL 3.3/4.1 core context, including the Mac** | one GPU code path everywhere; proven approach (e.g. three-gpu-pathtracer, MIT, WebGL2) | the watchdog still applies (tile the frame into ~5-20 ms draws); no recursion (loop with a fixed bounce count); register pressure on iGPUs |
| **C. CPU: Embree 4 + TBB** | **every platform, including headless CLI and VMs without a GPU** | robust, deterministic, no driver bugs; native curve/user-geometry support for beads; we already ship TBB | slower than a discrete GPU; heavy work on laptops |
| D. Metal (Mac) | Mac | fastest on Apple GPUs | a third backend; OpenGL→Metal is a bigger project |
| E. Hardware RT (Vulkan, OptiX, DXR) | RTX/RDNA2+ | fastest | new graphics API; out of scope |

**Recommendation:** **C first, B second, skip A, and D only if the Mac becomes the main target.**
- C is the reference, the CLI and the fallback. B accelerates the GUI on machines with a usable GPU.
- Both share one scene description, the material model (the same BSDF maths in C++ and GLSL, tested against each other) and OIDN.
- The owner's GL 4.3 compute idea would leave the Mac without a GPU path. B covers the Mac too, at a similar effort.

### 4.3 Technical approach

- **Scene build** (`ShowcasePathScene`, GL-free):
  - meshes from the model volumes (mesh mode), or bead user-geometry from §3 (toolpath mode);
  - the floor (a large disc with the photo texture or catcher flag);
  - the environment (the full-res equirect from 1h, with an importance-sampling CDF);
  - the world-fixed lights as area lights (the analytic key/fill/rim become soft-boxes with a size, so shadows soften with distance, as in a photo);
  - the camera (the pose, FOV, f-number and focus from 1g; motion blur by time-sampled camera poses).
- **BVH:**
  - C: Embree builds it (`RTC_BUILD_QUALITY_HIGH` for stills, `MEDIUM` per video frame).
  - B: our own binned-SAH BVH built on the CPU with TBB, flattened into an RGBA32F texture buffer (two texels a node). About 0.2-0.5 s for 1M triangles; refit only for rigid moves.
- **Integrator:**
  - unidirectional path tracing with next-event estimation and MIS (lights + environment), Russian roulette after 3 bounces, max 8 bounces (16 through transmission);
  - stratified/blue-noise sampling (Sobol + Owen scrambling), the seed per pixel fixed by the frame index for video stability.
- **BSDF** (one "filament" principled model, mirroring §2):
  - diffuse + GGX (isotropic/anisotropic with the extrusion tangent) + clear coat + sheen + flakes (the object-space cells of 1e, as real micro-facets) + rough dielectric transmission with volume absorption (Beer's law, `density`) for Transparent/Translucent;
  - a random-walk subsurface option for SSS plastics later.
  - The layer-line and imperfection bumps come from the same functions as the raster shader, ported to C++ (shared headers where possible).
- **Denoiser:** OIDN `RT` filter with albedo + normal AVOs (first-hit), on the CPU device (all platforms) or the Metal device on Apple Silicon. "Clean aux" mode for stills; for video, the same filter per frame with higher spp to limit flicker.
- **Progressive loop:**
  - it runs as a Plater job (worker thread, like other jobs) with an atomic cancel flag;
  - C: tiles of 32x32 handed out by TBB, one sample pass at a time;
  - B: a pass per GL frame tiled to stay under ~16 ms a draw (watchdog-safe), and the live view keeps working;
  - the preview updates every ~250 ms through a texture upload into the tab.
- **Callouts and watermark:** composited after denoising by the existing CPU overlay path (D6), exactly as for raster exports.

### 4.4 Dependencies

| Library | Licence | Version (2026-10) | Package size (from the GitHub release assets, all GPU devices included) | Mac arm64 | Notes |
|---|---|---|---|---|---|
| **Embree** | Apache-2.0 | 4.4.1 (2026-04-02) | Windows x64 zip 74 MB; macOS arm64 zip 55 MB ([releases](https://github.com/RenderKit/embree/releases)) | **yes** (an official arm64 macOS build) | Build from source in `deps/`, with only the ISAs we need (SSE2, AVX2; NEON on arm64) and only the triangle + user + round-curve geometry types, and link TBB to ours. Expected installed size after trimming ~15-25 MB (**measure in the spike**). |
| **Open Image Denoise** | Apache-2.0 ([repo](https://github.com/RenderKit/oidn)) | 2.5.1 (2026-08-18) | Windows x64 zip 53 MB; macOS arm64 tar.gz 49 MB ([releases](https://github.com/RenderKit/oidn/releases)) | **yes**: arm64 CPUs (Apple silicon), plus a Metal GPU device on M1+ with **macOS Ventura or newer** (Metal build needs Xcode 15+, CMake 3.21+) | The CPU device needs **ISPC ≥ 1.30** and **TBB** at build time. Ship only `OpenImageDenoise` + `OpenImageDenoise_core` + `device_cpu` (+ `device_metal` on Mac). Skip the CUDA/HIP/SYCL devices, which make up much of the zip. The README notes that dropping unused filters/weights "significantly decreases" the binary; build with only the `RT` filter. Expected ~20-35 MB (**measure**). |

Combined, about **+40-60 MB per platform** before compression. That is the main cost of phase 3 and an owner decision (D-R12). An alternative is an on-demand "Path tracing components" download, which is the same mechanism as the HDRI packs. Both are permissive licences, compatible with our AGPL-3.0 distribution; include their NOTICE files in `resources/licenses`.

### 4.5 Expected render times (estimates; the spike measures them)

1920x1080, Benchy-sized mesh scene (~250k triangles), studio HDRI, 6 bounces.

| Machine | Engine | Rays/s (est.) | "Preview" 32 spp + OIDN | "Final" 256 spp + OIDN |
|---|---|---|---|---|
| 8-core desktop CPU (Ryzen 7 / i7, AVX2) | C Embree | ~40-80 M | ~10-20 s | ~1.5-3 min |
| 4-core laptop CPU (Intel U-series) | C Embree | ~10-20 M | ~40-80 s | ~5-12 min |
| Mac mini M-series (8-10 cores) | C Embree (NEON) | ~40-70 M | ~10-20 s | ~2-3 min |
| RTX 3060-class | B fragment | ~200-400 M | ~2-4 s | ~15-30 s |
| Intel Iris Xe | B fragment | ~30-60 M | ~15-30 s | ~2-4 min (slower than its own CPU in some scenes; auto-pick the faster by timing 1 spp) |
| Apple M-series GPU via GL 4.1 | B fragment | ~60-150 M | ~5-10 s | ~45-90 s |

- OIDN adds ~0.3-1 s a frame on the CPU at 1080p, and ~0.1 s on Metal.
- Toolpath scenes with millions of bead primitives are ~1.5-3x slower.
- **Video:** a 10 s, 30 fps clip at 64 spp is 300 frames × ~5-40 s, i.e. **25 min to 3 h**. So path-traced video is a CLI/"leave it running" feature, with a clear estimate and resumable frames (one numbered PNG per frame, skipping finished ones on restart).

### 4.6 UI

- **Render** button: next to "Export image"; disabled with a reason on unsupported contexts (C always works, so it is never fully unavailable).
- **Render panel** (replaces the options panel while rendering):
  - quality (Preview / Final / Custom spp / Time limit);
  - engine (Auto / CPU / GPU);
  - denoise (on);
  - progress (spp, elapsed, remaining);
  - **Cancel**, **Save**, and **Back to live view**.
- The live view freezes while rendering on the GPU. With CPU rendering the UI stays responsive, and BelowNormal thread priority keeps the PC usable.
- A notification when a long render finishes while the tab is hidden.

### 4.7 Phase 3 summary

**Files:**
- `deps/Embree/`, `deps/OIDN/` (+ `deps/ISPC` build tool on Windows/Mac/Linux), `deps/CMakeLists.txt`, `deps/deps-*.cmake`;
- `src/libslic3r/Showcase/PathTracer/` (scene, BSDF, sampler, integrator, Embree backend, denoise wrapper: GL-free, testable);
- `src/slic3r/GUI/Showcase/ShowcasePathTraceJob.cpp/.hpp`, GPU backend `ShowcasePathTraceGL.cpp` + `resources/shaders/140/showcase_pt_*.fs`;
- `ShowcaseView.cpp` (button, panel);
- `ShowcaseRenderCli.cpp/.hpp` + the GUI render job (`--engine`);
- the installer and CMake install rules for the DLLs/dylibs (and the Mac bundle signing list);
- `resources/licenses`.

**Performance and GPU limits:**
- C uses all cores at BelowNormal priority, with memory of scene × ~2 (the BVH).
- B needs RGBA32F targets (core), texture buffers up to `GL_MAX_TEXTURE_BUFFER_SIZE` (≥ 64M texels on modern drivers; the Mac reports 256M). A scene above that falls back to C.
- Intel iGPU: B can be slower than C; auto-select by a 1-spp timing.

**Risks:**
- dependency weight and build complexity (ISPC on three platforms, the Mac universal build);
- matching raster and path-traced looks (users will compare);
- video flicker from per-frame denoising;
- GPU watchdog resets with B;
- Mac code signing of the new dylibs (the 2.4.6 Mac lessons apply).

**Size / model:**
- spike (Embree + OIDN in deps, one still from the CLI, measured sizes and times): M, **Opus**;
- C integrator + materials + UI: L, **Opus**;
- B GPU backend: L, **Opus**;
- render-time video + resume: M, Sonnet.

**Checks:**
- unit tests: BSDF energy conservation (white furnace test: the albedo-1 diffuse and dielectric sum to ~1 under a uniform environment); BSDF C++ vs GLSL agreement on a grid of directions (B vs C); BVH correctness (ray hits vs brute force on random scenes); the sampler is deterministic per seed;
- a reference image: C at 4096 spp on a fixture scene, stored once; B and C at 256 spp + OIDN must match it within a perceptual threshold;
- a cancel test (the job stops within 0.5 s);
- headless `--showcase-render --engine pathtrace` on the VM;
- a Mac hand test card.

**Owner decisions, phase 3**

| # | Decision | Options | Recommendation |
|---|---|---|---|
| D-R11 | Engine order | GL 4.3 compute first / **CPU Embree first, then fragment GPU** / GPU only | **CPU first, then fragment GPU**; no compute backend |
| D-R12 | Ship Embree + OIDN | in the installer (+40-60 MB) / on-demand component download | **In the installer on Windows and Mac**, if the measured size stays ≤ 60 MB; otherwise on demand |
| D-R13 | Path-traced video | GUI / CLI only / both | **Both, with resumable frames**; GUI shows the time estimate first |
| D-R14 | OIDN Metal device on Mac | ship / CPU only | **Ship** (it needs macOS 13+; fall back to CPU below) |

---

## 5. Phase 4: Blender export and the companion add-on

### 5.1 What the user sees

- **Export → "Blender package…"** writes a folder or zip holding the scene, materials, camera path and environment.
- **"Open in Blender"** (shown when Blender ≥ 4.2 is found) does the same and launches Blender with the scene already set up for Cycles.
- In Blender, one **EdgeSlicer** panel (sidebar, "N" key) offers:
  - Look preset (matches Showcase's presets);
  - Quality (Preview / Final / Best);
  - Device (Auto);
  - Render image / Render animation;
  - output folder and "Open output".

  Nothing else is needed to get a Cycles render that matches the Showcase framing.

### 5.2 The package (format `edgeslicer-blender-package`, version 1)

| File | What it holds |
|---|---|
| `scene.glb` | Meshes (mesh mode) or bead chunks (phase 2, toolpath mode), one node per object/volume/chunk, Z-up converted to glTF Y-up. Materials use core PBR + `KHR_materials_clearcoat`, `_transmission`, `_volume`, `_ior`, `_sheen`, `_specular`, `_anisotropy`, `_emissive_strength`. **All are imported by Blender's glTF importer**, along with `KHR_mesh_quantization`, `EXT_mesh_gpu_instancing` and `EXT_meshopt_compression` ([importer extension list](https://github.com/KhronosGroup/glTF-Blender-IO/blob/main/docs/blender_docs/scene_gltf2.rst)). Our extra parameters (finish id, layer table, flake density, density/absorption, seam table, move ranges) go in glTF `extras`, which the importer keeps as custom properties. |
| `showcase.json` | Lights (world-fixed key/fill/rim as area lights with size and temperature), environment id + rotation + exposure, floor (type, texture, catcher), backdrop, camera (FOV/ortho, f-number, focus point), output size and aspect, imperfections, plus the package version and the EdgeSlicer version |
| `camera_path.json` | The **compiled keyframes** of the Showcase path (`ShowcasePath` already compiles moves to keys deterministically), sampled per frame at the path's fps, plus the events timeline (lights, build-up fraction per frame). The add-on never re-implements the move compiler. |
| `environment/<id>.hdr` (+ plate) | The full-res HDRI and backplate (from the bundle or our mirror) |
| `textures/` | Floor photos, the user's backdrop or floor images |
| `overlays/` | Callouts + watermark as transparent PNG (stills; video: an optional PNG sequence) |
| `README.txt` | What this is, how to open it without the add-on (plain glTF import) |

The GLB is also useful on its own (Sketchfab, three.js, Keyshot), so offer "glTF only" as a second export choice.

### 5.3 The add-on (`edgeslicer_render`)

**Licence: GPL-3.0-or-later.** The extensions platform requires it for add-ons, and requires CC0 for any bundled assets ([licences page](https://projects.blender.org/blender/blender-manual/raw/branch/main/manual/advanced/extensions/licenses.rst), [platform about page](https://extensions.blender.org/about/)).
- It is a separate program written fresh in Python.
- No AGPL C++ is copied into it. The package format is the interface.
- Our assets in it are Poly Haven CC0 or our own CC0 node presets.

**What it does**
1. **Import:**
   - `bpy.ops.import_scene.gltf` on `scene.glb`;
   - then replace each material with a node group from the add-on's library, chosen by `extras.finish`;
   - the glTF material stays as the fallback when the finish is unknown.
2. **Material library** (a node group per finish, all Principled BSDF based):

   | Finish | Node group |
   |---|---|
   | Standard / Matte / Gloss | roughness, coat, sheen settings |
   | Silk | anisotropic specular, with the tangent from a `tangent` attribute we export per vertex: the extrusion direction for beads, the horizontal or skin direction for meshes |
   | Transparent / Translucent PETG | transmission + volume absorption from `density`, IOR 1.57 |
   | Metallic | coloured specular |
   | CF | sheen + fibre noise |
   | Sparkle | Voronoi-cell flakes as a normal perturbation |

   Layer lines on meshes come from a bump driven by object-space Z through the layer table, stored as an attribute or a 1D image. Beads need none.
3. **Scene:**
   - World: the HDRI with rotation and exposure from `showcase.json`;
   - lights: area lights;
   - floor: a plane with the photo texture, or a **shadow catcher** (Cycles `is_shadow_catcher`) for transparent and backplate shots, with the backplate in the compositor;
   - camera: FOV/ortho and DoF (f-stop, focus object at the focus point);
   - render size and fps.
4. **Camera path:** keyframes from `camera_path.json`, one per frame, with linear interpolation (the keys are already eased).
5. **Build-up:** chunk visibility keyframes, plus a geometry-nodes modifier on the current chunk that deletes points with `move_index > t`, with `t` keyframed from the per-frame build fraction.
6. **Render settings** (Cycles):

   | Quality | Samples | Notes |
   |---|---|---|
   | Preview | 64 adaptive | OpenImageDenoise (Blender's built-in denoiser) |
   | Final | 512 adaptive | |
   | Best | 2048 | |

   Device: GPU automatically when Blender reports OptiX/CUDA/HIP/Metal/oneAPI, otherwise CPU. Filmic/AgX view transform matched to Showcase's tone choice (Neutral maps to Blender's "Khronos PBR Neutral" view, available in Blender 4.2+).
7. **Overlays:** for stills, the callout PNG is composited on top in the compositor, so callouts match the EdgeSlicer export.

**One panel: "Render like EdgeSlicer"**
- the package path (filled in by Open in Blender);
- Look preset;
- Quality;
- an "Imperfections" toggle;
- **Render image**, **Render animation**, **Open output folder**.

An "Advanced" foldout links to the standard Blender panels for people who want to tweak; nothing more.

### 5.4 "Open in Blender"

- **Finding Blender** (newest version ≥ 4.2 wins; the user can set a path in Preferences → Showcase):
  - Windows: the `blendfile` open command in the registry (`HKCR\blendfile\shell\open\command`), then `C:\Program Files\Blender Foundation\Blender *\blender.exe`, then Steam's default library;
  - Mac: `/Applications/Blender*.app`, then a Spotlight query for the bundle id `org.blenderfoundation.blender`;
  - Linux: `PATH`, Flatpak (`org.blender.Blender`), Snap.
- **Launching:** `blender --python <EdgeSlicer resources>/blender/edgeslicer_launch.py -- --package <dir>`. The launcher script:
  - puts the add-on bundled with EdgeSlicer on `sys.path` and registers it for this session only, so nothing has to be installed;
  - imports the package, builds the scene and opens the panel;
  - offers a one-click "Install permanently" in the panel, using Blender's own extension install.

  We never write into Blender's config folder ourselves.
- The version is checked with `blender --version`. Below 4.2, the menu says so.

### 5.5 Versioning

- Package: `format` + integer `version`, like the path files (`PATH_FORMAT_VERSION`).
- The add-on reads every version up to its own and refuses newer ones with "update the EdgeSlicer add-on". The add-on is semver; its minor version goes up with each package version.
- The EdgeSlicer release notes say which add-on version they bundle.
- Blender support: the **4.2 LTS minimum** (the first with extensions) and the newest LTS; test both on the VM.
- Blender API changes (node socket names change between versions) are isolated in one `compat.py`.

### 5.6 Distribution

| Channel | How | When |
|---|---|---|
| **Inside the EdgeSlicer install** (`resources/blender/`) | Used by Open in Blender with no install step | v1 |
| **Zip in our GitHub release assets** | Built with `blender --command extension build`, checked with `blender --command extension validate <zip>` (the [manual's](https://projects.blender.org/blender/blender-manual/raw/branch/main/manual/advanced/extensions/getting_started.rst) commands), on the VM | v1 |
| **extensions.blender.org** | Upload through Blender ID, held for review ([about](https://extensions.blender.org/about/)); gives in-Blender install and updates. Needs `blender_manifest.toml` (schema 1.0.0, id, version, name, tagline ≤ 64 chars, maintainer, type "add-on", `blender_version_min = "4.2.0"`, `license = ["SPDX:GPL-3.0-or-later"]`). No network permission needed (we declare only `files`). | after v1 is stable (owner decision D-R16) |

The add-on lives in its own folder in this repo (`tools/blender_addon/edgeslicer_render/`) so it ships with the matching EdgeSlicer version, or in a separate repo if the owner prefers (D-R17).

### 5.7 Phase 4 summary

**Files:**
- new `src/libslic3r/Showcase/ShowcaseGltf.cpp/.hpp` (GLB writer, GL-free; no new dependency: GLB is JSON + a binary chunk);
- `ShowcasePackage.cpp/.hpp` (package writer);
- `ShowcasePath.*` (export compiled keys per frame);
- `src/slic3r/GUI/Showcase/ShowcaseView.cpp` (export menu, Open in Blender);
- new `src/slic3r/Utils/BlenderLocator.cpp/.hpp`;
- `resources/blender/` (launcher + add-on copy);
- `tools/blender_addon/edgeslicer_render/` (`__init__.py`, `importer.py`, `materials.py`, `scene.py`, `panel.py`, `compat.py`, `blender_manifest.toml`, node library `.blend` with CC0 assets only);
- `tests/libslic3r/test_showcase_gltf.cpp`;
- `tests/blender/` (headless import tests).

**Performance:**
- The GLB of a mesh scene is the mesh size, a few MB to 100 MB.
- Bead scenes are streamed from the mesher (§3) and capped.
- Blender itself needs the user's GPU/RAM; Cycles on an Intel iGPU is slow (CPU fallback); we say so in the panel.

**Risks:**
- Blender API churn between versions (node names).
- Users without a GPU face long Cycles renders.
- The look differs from Showcase (expected; we match framing and lighting, not pixels).
- Blender's glTF importer may change its custom-property handling.

**Size / model:**
- GLB + package writer: M, **Sonnet**;
- Open in Blender + locator: S, Sonnet;
- add-on v1 (import, materials, scene, camera, panel): M, **Opus** (the material node work needs care);
- build-up in Blender: S, Sonnet;
- extensions platform submission: S.

**Checks:**
- unit tests: GLB validity (the Khronos glTF validator, run offline on fixtures in the test script, if it is not shipped as an exe) and round-trip of `extras`; compiled keys per frame equal the Showcase video's camera poses at the same times;
- headless Blender on the VM: `blender -b --python tests/blender/import_and_render.py -- <fixture package>`, rendering 16 spp at 320x180, asserting the objects, materials, camera keyframe count and a non-black output, on 4.2 LTS and the newest LTS;
- `extension validate` on the zip;
- an owner hand test: Open in Blender, then Render image.

**Owner decisions, phase 4**

| # | Decision | Options | Recommendation |
|---|---|---|---|
| D-R15 | Package shape | folder / single zip (`.edgeshow`) | **Folder** for Open in Blender; zip for "Export…" |
| D-R16 | Extensions platform listing | v1 / later / never | **Later**, after two releases without format changes |
| D-R17 | Where the add-on code lives | this repo / new repo `aceRage/EdgeSlicerBlender` | **This repo** (versions move together) |
| D-R18 | Callouts in Blender | stills only / stills + video PNG sequence / none | **Stills only** in v1 |

---

## 6. Cross-phase tests and visual checks

- **GL-free unit tests** go in `tests/libslic3r/test_showcase_*.cpp`, as the existing five files do: materials, HDR readers, bead mesher, path-tracer maths, GLB writer, path keys.
- **The golden-image harness** (new, used by every phase): `tests/showcase/golden.sh` runs `EdgeSlicer --showcase-render` on the fixture projects (`tests/showcase/fixtures/*.3mf`: cube, Benchy, silk vase, PETG cup, CF bracket, sparkle part, a 2-colour toolpath plate). It compares against stored PNGs with a perceptual metric (FLIP or SSIM in a small Python script), and writes a side-by-side HTML for the owner when something changes on purpose.
  - Windows baseline from the shared `_baseline` install.
  - Mac goldens are kept separately; never compare Mac with Windows pixels, the same rule as the G-code gates.
- **Video stability check:** the mean frame-to-frame difference on a 3 s turntable, compared with a matte reference (catches sparkle and grain fizz, and denoiser flicker).
- **Performance check:** frame time of the live view on the owner's laptop class (Intel iGPU) for the 5M-segment fixture, before and after each phase-1 PR. A budget table lives in the PR.
- **Hand-test cards** (`pr_status.json`): every PR that changes the look gets a still pair for the owner; the Mac mini session does a Mac still for 1a, 1b, 1h and phase 3.

---

## 7. All owner decisions

D-R1 to D-R18 are listed in their phases.

### 7.1 Owner decisions taken (2026-10-26)

The owner accepted every recommendation on 2026-10-26.

| # | Decision | Owner's choice (2026-10-26) |
|---|---|---|
| D-R1 | Default tone curve | **Keep Khronos PBR Neutral as the default**; filmic selectable as an option |
| D-R2 | HDRI packaging | **B**: bundle 3 HDRIs at 2k, the rest on demand (1h, not 1a) |
| D-R3 | Where downloads come from | **Our own mirror**; never the Poly Haven API (1h, not 1a) |
| D-R4 | PETG inference | As recommended: Translucent for "natural"/"clear" words only |
| D-R5 | Imperfections default | As recommended: Subtle in presentation presets, Off in Technical |
| D-R6 | Vignette default | As recommended: light in presentation presets |
| D-R7 | Sparkle finish | As recommended: add |
| D-R8 | Impostor beads live | As recommended: idle only on iGPU, always on discrete |
| D-R9 | Internals in exports | As recommended: culled, with a "show internals" toggle |
| D-R10 | Mesher memory cap | As recommended: 1.5 GB, auto-lowering detail |
| D-R11 | Path tracer engine order | As recommended: CPU (Embree + OIDN) first, then the fragment-shader GPU tracer; no compute backend |
| D-R12 | Ship Embree + OIDN | As recommended: in the installer on Windows and Mac if measured ≤ 60 MB, otherwise on demand |
| D-R13 | Path-traced video | As recommended: GUI and CLI, resumable frames, estimate first |
| D-R14 | OIDN Metal device | As recommended: ship (macOS 13+, CPU below) |
| D-R15 | Package shape | As recommended: folder for Open in Blender, zip for Export |
| D-R16 | Extensions platform listing | As recommended: later, after two releases without format changes |
| D-R17 | Add-on location | **This repo** (`tools/blender_addon/edgeslicer_render/`) |
| D-R18 | Callouts in Blender | As recommended: stills only in v1 |
| Order | Phase order | **Phase 4 (Blender) goes ahead of phases 2 and 3**, as in §8 |

The ones that blocked the first PRs:

1. **D-R1** tone curve default (keep PBR Neutral?) blocks 1a.
2. **D-R2/D-R3** HDRI packaging and download source block 1h.
3. **D-R11/D-R12** engine order and dependency weight block the phase 3 spike.
4. **D-R17** add-on location blocks phase 4.
5. **Phase order itself:** is Blender (phase 4) allowed to jump ahead of beads and the path tracer? (Recommended: yes.)

---

## 8. Recommended order and the first PR

| Step | Work | Size | Model | Why here |
|---|---|---|---|---|
| 1 | **1a HDR buffer + post pass** | M | Opus | Everything after it needs linear light |
| 2 | 1b toolpath material parity → 1c silk along the extrusion | M + S | Opus, Sonnet | The build-up video is Showcase's signature; it should not look like Preview |
| 3 | 1h-1 2k backdrops + the sun as a real light | M | Sonnet | The biggest single "photo" jump for stills |
| 4 | 1d translucency → 1e finishes → 1f imperfections | 3 x M | Sonnet | Material credibility |
| 5 | 1g camera effects | M | Sonnet | Polish; DoF needs 1a |
| 6 | 1h-2 backplates + catcher + camera match; 1h-3 downloads + user HDRI | M + S-M | Opus, Sonnet | Needs 1h-1 and 1g |
| 7 | **Phase 4 Blender** (GLB + package, add-on, Open in Blender) | 2 x M + S | Sonnet, Opus | Photo-real for anyone with Blender, without our own tracer |
| 8 | Phase 2 tier 1 impostor beads | M | Opus | Close-ups; no memory cost |
| 9 | Phase 2 tier 2 mesher (+ Blender bead export) | L | Opus | Needed by the tracer and by bead-accurate Blender scenes |
| 10 | Phase 3 spike, then CPU tracer, then GPU fragment tracer | M, L, L | Opus | The heaviest dependency and effort, last |

### First PR: "Showcase: HDR scene buffer and post pass" (branch `feat/showcase-hdr-post`)

**In scope:**
- RGBA16F colour for the live view and the export (MSAA and the SSAA downsample unchanged; `export_memory_estimate` at 8 B/px);
- tone mapping and sRGB conversion moved out of `showcase_material.fs`, `showcase_background.fs` and `showcase_floor.fs` into a new `showcase_post.fs`, with Khronos PBR Neutral unchanged as the only curve for now;
- the libvgcode smooth path stops clamping when `smooth_mode != 0`;
- translucent blending in linear light;
- straight-alpha output preserved for transparent exports;
- the golden-image harness (`tests/showcase/golden.sh` + 5 fixtures), because this PR is the one that must prove "no visible change".

**Out of scope:** new curves, DoF, grain, vignette (the post pass has empty hooks only), any material change.

**Done when:**
- golden stills match the pre-PR images (mean ΔE < 1);
- the transparent PNG alpha is identical outside objects;
- a Preview screenshot is identical (smooth_mode 0);
- `libslic3r_tests` pass;
- a Mac still is hand-checked by the Mac session.

**Build:** app targets (for the CLI goldens) via edgebuild, once, at the end.

---

## Sources

- Poly Haven licence (CC0, redistribution allowed): https://polyhaven.com/license
- Poly Haven API terms: https://polyhaven.com/our-api, https://github.com/Poly-Haven/Public-API, https://polyhaven.com/corporate
- Open Image Denoise (Apache-2.0, arm64 + Metal, ISPC/TBB requirements, filter size note): https://github.com/RenderKit/oidn; release assets 2.5.1: https://github.com/RenderKit/oidn/releases
- Embree (Apache-2.0, arm64 macOS build) release assets 4.4.1: https://github.com/RenderKit/embree/releases
- Blender extension manifest, permissions, build/validate commands: https://projects.blender.org/blender/blender-manual/raw/branch/main/manual/advanced/extensions/getting_started.rst
- Blender add-on licence requirement (GPL-3.0-or-later; CC0 assets): https://projects.blender.org/blender/blender-manual/raw/branch/main/manual/advanced/extensions/licenses.rst
- Blender extensions platform (GPL compliant, review queue): https://extensions.blender.org/about/
- Blender glTF importer extension list: https://github.com/KhronosGroup/glTF-Blender-IO/blob/main/docs/blender_docs/scene_gltf2.rst
- Fragment-shader path tracing prior art (MIT): https://github.com/gkjohnson/three-gpu-pathtracer
