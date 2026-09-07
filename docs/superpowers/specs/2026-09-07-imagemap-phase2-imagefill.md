# ImageMap Phase 2: Image Fill by painting

Date: 2026-09-07
Branch: `feat/imagemap-p2-imagefill`, from `origin/feat/imagemap-p1-solver` @ `0db218571f`
(itself from `origin/feat/ultra-preferences` @ `302d19f34d`)
Worktree: `C:\Dev\SnapmakerOrcaPhone\.claude\worktrees\imagemap-p2`

Implements Phase 2 of `docs/superpowers/specs/2026-09-07-imagemap-edgeslicer-plan.md`
(on `feat/imagemap-plan`), stacked on Phase 1
(`docs/superpowers/specs/2026-09-07-imagemap-phase1-solver.md`). Phase 1 is not merged;
this branch sits on it rather than on the release head.

> **`origin/feat/ultra-preferences` moved during this work**, from `302d19f34d` (the commit
> Phase 1 branched from) to `499440252d` — the dual-nozzle send fix. This branch was **not**
> rebased onto it, deliberately: a mid-implementation rebase would have invalidated every
> measurement below. Phase 1 and Phase 2 should be rebased together, once, before review.

---

## 1. The user flow, as built

1. Select a part in the 3D view or the object list.
2. Right-click → **Apply image fill...** (one new menu item, next to "Split by painted colour").
3. In the dialog:
   * **Source** — an image file (PNG, JPEG, BMP; anything wxWidgets reads), or a two- or
     three-colour gradient. A gradient's stops are taken from the first, middle and last allowed
     filament, so it needs no colour pickers and costs the project no payload at all.
   * **Projection** — *Flat, along an axis* / *Wrapped around an axis* / *The model's own texture
     coordinates*, plus the axis (X/Y/Z) and two mirror checkboxes.
   * **Apply to** — the whole part, or only the faces already painted with one filament by the
     existing colour-painting tool. That is the face selection: no new gizmo, exactly as the plan
     requires.
   * **Filaments the image may use** — a checklist over every loaded filament, physical and mixed
     alike. A mixed-filament row is just another id with a colour, so an image can use one without
     Image Fill knowing what a mix is.
   * **Detail (mm)** — how small a facet the image is allowed to decide. The part is subdivided
     until its facets are about this size.
   * **Preview** — a 128 × 128 thumbnail of the source **quantised through the same palette and the
     same solver the fill will use**, so what is shown is the colours the printer will try to make,
     not the picture's own, plus a line saying how many filaments the result uses and how many
     facets it will paint at the depth actually chosen.
4. **Apply**. The part's `mmu_segmentation_facets` is rewritten, and the settings are recorded on
   the part so re-opening the dialog starts where it left off.
5. Slice, preview, print. Everything downstream is unchanged, because the output *is* MMU paint:
   paint depth bounds it, colour split can turn it into geometry, the 3MF round-trips it, the
   sidebar chips and the H2C/H2D filament maps see ordinary filament ids.

### What is not in the dialog, and why

| Not there | Why |
| --- | --- |
| A 3D preview of the painted model | It needs the gizmo machinery the plan explicitly keeps out of Phase 2. The 3D view updates on Apply, and Apply is undoable. |
| Gradient colour pickers | The stops are the allowed filaments' own colours, which is the useful case. Editing stops belongs with Phase 3's `DistributionMode::ImageWeighted`, where a gradient becomes a mixed-filament row rather than a source of samples. |
| A background-filament chooser | `ImageFillParams::background` exists and round-trips; the dialog does not expose it. Unpainted (`0`) is the right default and the only one anybody asked for. |

---

## 2. The service API

`src/libslic3r/ImageFill.{hpp,cpp}` — wxWidgets-free, so the CLI, the importers and the tests all
use the same code.

### The asset store

```cpp
std::string      image_fill_sha256_hex(const uint8_t *data, size_t len);
class ImageAssetStore {
    std::string       add(std::vector<uint8_t> png_bytes);   // returns the SHA-256, dedupes
    const ImageAsset *find(const std::string &sha256) const;
    const ImageAsset *pixels(const std::string &sha256) const;   // decoded on demand
    void              retain(const std::vector<std::string> &keep);
};
```

`Model::image_assets` is an `ImageAssetStore` **by value**. It is not an `ObjectBase`, holds no
`ObjectBase`, and therefore consumes no id from the global counter — the plan's section 5.4 rule,
and the thing Bar A's `; model label id` check gates. Two parts using the same picture share one
entry, because the key is the content.

Every stored image is a PNG. The dialog normalises whatever the user picked through `wxImage` and
re-encodes before storing, because (a) `PNGReadWrite` decodes 8-bit RGB/RGBA PNG and nothing else,
and (b) a fixed encoding is what makes the content hash — and therefore the 3MF round trip —
reproducible rather than merely equivalent.

### Projections

```cpp
enum class ImageFillProjection { Planar, Cylindrical, MeshUV };
enum class ImageFillAxis       { X, Y, Z };
bool image_fill_project(const ImageFillParams&, const BoundingBoxf3&, const Vec3f &p,
                        float &u, float &v);
```

* **Planar** — `u`, `v` are the point's position along the two axes that are not `axis`, normalised
  over the part's bounding box, in ascending axis order. Axis Z gives (x, y); axis X gives (y, z);
  axis Y gives (x, z).
* **Cylindrical** — `u = atan2(b, a) / 2π + 0.5` about the box's centre in the two other axes, so
  `+x` is the middle of the image and `-x` is the seam; `v` runs along `axis`. A point exactly on
  the axis returns `false` rather than guessing an angle.
* **MeshUV** — supplied by the caller, not computed. `image_fill_project` returns `false` for it.

`v` runs **up**; the sampler flips it once, so image row 0 is at `v = 1`.

### Texture coordinates

```cpp
using ImageFillUVs = std::vector<std::array<Vec2f, 3>>;   // THREE per triangle
```

Per **face**, not per vertex. A glTF exporter splits a vertex at every UV seam and the reader has
to weld those back before the mesh is printable; that destroys a per-vertex UV array but leaves a
per-face one exactly right. Inside a triangle a glTF `TEXCOORD_0` is linear, so subdividing the UV
triangle alongside the position triangle is exact.

### Quantise, then solve

```cpp
ImageFillPalette image_fill_quantise(const std::vector<std::array<float,3>> &samples, size_t max_colors);
void             image_fill_solve(ImageFillPalette&, const std::vector<std::array<float,3>> &filament_colors,
                                  const std::vector<int> &filament_ids);
```

Phase 1's own report says a solve costs about 0.3 s per model colour, so the image is reduced to at
most 256 representatives **before** the solver is asked anything: a fixed 5-bit-per-channel bucket
pass, ordered busiest-first with the bucket key breaking ties, and everything past the cap merged
into its nearest kept representative so the counts still add up. No randomness and no iteration
count, so two runs on one image give the same palette in the same order — which is half of what
makes Bar B's determinism claim true.

The solver call is the Phase 1 solver, used with **`ColorSolverConstraints{min_components = 1,
max_components = 1}`**. Phase 2's writer is `mmu_segmentation_facets`, which holds one id per
triangle; the constraint says "a facet can only be one filament" and the solver answers "which one",
measured in Oklab with the chroma-dependent axis weights (`OklabSoftCap4Dark4`). That is exactly the
improvement the plan asks for over `ObjColorMatch`'s CIE76 `obj_color_distance`. Lifting the
constraint is the whole of Phase 3's change: the same call then returns mixes. The candidate set is
cached per palette in a `thread_local ColorSolverCandidateCache`, so a batch pays for the
enumeration once.

### Subdivision

```cpp
void image_fill_subdivide(const Vec3f &a, const Vec3f &b, const Vec3f &c, int depth,
                          std::vector<ImageFillLeaf> &out);
TriangleSelector::TriangleSplittingData
     image_fill_encode(size_t n_original_triangles, int depth, const std::vector<int> &states);
int  image_fill_depth_for_detail(float max_edge_mm, float detail_mm, int cap, size_t n_triangles);
```

The subdivision is `TriangleSelector::perform_split`'s three-side case, transcribed: with
`special_side == 0` the vertex list becomes `[v0, m01, v1, m12, v2, m20]` and the four children are
`(v0,m01,m20)`, `(m01,v1,m12)`, `(m12,v2,m20)`, `(m01,m12,m20)`. `image_fill_encode` writes the same
tree into `TriangleSelector`'s bitstream, including the reversed child order it keeps for
PrusaSlicer 2.3.1 compatibility and the nibble-chain form for states ≥ 3.

**The depth is uniform across the volume, by construction.** That is the reason there are no
T-joints: a shared edge's midpoint is computed from the same two endpoints on both sides, so
neighbouring triangles agree at every level. An *adaptive* depth would leave T-joints for
`get_facets_strict` to triangulate away, which works but is harder to reason about and to test;
uniform is the choice, and the cost is that a mesh with wildly uneven triangles subdivides its
already-fine facets too. `image_fill_depth_for_detail` derives the depth from the target facet size,
caps it at 6, and drops levels until `4^depth × n_triangles` fits under `IMAGE_FILL_MAX_LEAVES`
(4 000 000).

Nothing writes the bitstream into the model directly: `image_fill_apply` deserialises it into a
`TriangleSelector` and stores `selector.serialize()`, so what is kept is the canonical encoding the
fork's own writer would have produced, and the encoder is validated against the fork's decoder on
every single call.

### Applying

```cpp
ImageFillResult image_fill_compute(mesh, existing_painting, params, assets,
                                   filament_colors, filament_ids, uvs = {});
ImageFillResult image_fill_apply  (ModelVolume&, params, assets, filament_colors, filament_ids, uvs = {});
bool            image_fill_params_of(const ModelVolume&, ImageFillParams &out);
ImageFillResult image_fill_from_face_colors(mesh, face_colors, filament_colors, filament_ids,
                                            background = 0, depth = 0);
```

`image_fill_from_face_colors` is the importers' entry point: `face_colors` holds `4^depth` entries
per triangle in the same leaf order, so `depth == 0` is the plain "one colour per triangle" case the
OBJ and glTF readers have always had, and a positive depth is the same reader sampling its texture
per sub-facet.

---

## 3. The 3MF layout

| Path | Content |
| --- | --- |
| `Metadata/image_fill/<sha256>.png` | one file per referenced asset, the bytes verbatim, **stored** (`MZ_NO_COMPRESSION`) — the payload is already a PNG, so deflating it again costs time and buys nothing, and "stored" is the plainest guarantee the bytes come back as written |
| `Metadata/image_fill/manifest.json` | `{"version": 1, "assets": [{"sha256", "path", "bytes"}, ...]}`, sorted by hash so two saves of one project match |
| `<metadata key="image_fill_params" value="...">` on each volume | the annotation, inside the existing `Metadata/model_settings.config` — **no new 3MF element** |

Writing: `_BBS_3MF_Exporter::_add_image_fill_to_archive`, called just after the custom-G-code file.
Only assets some volume's `image_fill_params` actually names are written, so a project does not
carry every picture the user ever tried.

Reading: `_BBS_3MF_Importer::_extract_image_fill_asset_from_archive`, from a branch placed **before**
the generic `Metadata/*.png` thumbnail branch. Every payload is re-hashed on the way in and a file
whose name and content disagree is logged and stored under its real hash rather than trusted, so a
tampered or truncated archive cannot make the annotation point at the wrong picture — the annotation
simply finds no image, and the part keeps the painting it already has.

The annotation string is `k=v;k=v`, starting `v=1`: `img` (hash), `proj`, `axis`, `fu`/`fv`,
`sub`, `det`, `bg`, `sel`, `f` (comma-separated allowed ids), and the gradient's `g`/`g3`/`gd`/
`ga`/`gb`/`gc`. A string without `v=` is refused rather than half-parsed.

---

## 4. The config keys: two, and why

| Key | Type | Class | Default | Why |
| --- | --- | --- | --- | --- |
| `image_fill_params` | `coString` | `PrintObjectConfig` | `""` | One application's settings, riding `ModelVolume::config` — a `ModelConfigObject` that **already exists**. This is what makes Image Fill re-editable after a reload without adding an `ObjectBase`-derived member anywhere. Descriptive only: the slicer reads the painting, never this. |
| `image_fill_detail` | `coFloat` | `PrintObjectConfig` | `0` | The project's default target facet size in millimetres for a new application. It belongs to the project rather than to `AppConfig` because the right value depends on the part (a 200 mm plaque and a 20 mm token want different numbers) and because reopening a project should offer the default it was made with. `0` means "use the model's own triangles", which is why this key changes nothing for anyone who never opens the dialog. |

Both are on `PrintObjectConfig`, which makes a value stored on a `ModelVolume` **inert for slicing**:
`region_config_from_model_volume` (`PrintObject.cpp:3819`) applies only `PrintRegionConfig` keys from
a volume's config, so these two can never split or merge a print region. `PrintObject::
invalidate_state_by_config_options` gets an explicit no-op branch for them — without it the legacy
fallback at the end of that chain would invalidate every step for an edit that cannot change a single
extrusion.

**Not added, deliberately:** a subdivision-level key (it is per application, and lives in
`image_fill_params`), a leaf-budget key (`IMAGE_FILL_MAX_LEAVES` is a safety rail, not a preference),
and anything about the solver (Phase 1 added none and Phase 2 keeps that).

---

## 5. The glTF/GLB unification

The plan's requirement: *"`GLTF.cpp`'s texture path and `Model.cpp`'s `paint_volume_from_face_colors`
are refactored onto the same service so GLB import becomes Image Fill with mesh UVs."*

What was done, precisely:

* **`Model.cpp`'s `paint_volume_from_face_colors`** no longer writes through `CONST_FILAMENTS`' hex
  table and `set_triangle_from_string`. It builds a state array and hands it to
  `image_fill_encode`, then stores it through a `TriangleSelector`. One writer for imports and for
  the dialog. The `filament_id <= 1` skip is kept exactly — id 1 is the part's own filament and was
  never painted.
* **`GLTF.cpp`** samples the `baseColorTexture` at **16 points per triangle** instead of one:
  `GLTF_TEXTURE_SUBDIVISION = 2`, the same 4:1 midpoint split, walked in UV space. The new
  `GltfInfo::sub_face_colors` / `sub_face_depth` carry them, paired through the degenerate-face
  removal in blocks.
* **`Model::import_multi_volume_face_color_deal`** gains an overload that takes those. The colour
  dialog (or `obj_color_auto_match`, headless) still decides **which** filaments exist and what
  colour each one stands for — the palette is the mean of the imported colours mapped to each id, so
  a filament nobody chose can never appear. The ImageFill service then decides, per **sub-facet**,
  which of those the finer sample is nearest to, in Oklab.
* The overload **declines** (returns `false` having touched nothing) when the arrays do not line up
  or when fewer than two filaments were chosen, and `read_from_file` then falls back to the old
  per-facet call. So the failure mode of the new path is exactly the shipped behaviour.

**What was *not* unified, and why.** The texture decode and the UV walk stay inside `GLTF.cpp`
rather than moving into the service. The UVs only line up with the vertices *before* welding, and
welding is mandatory (a glTF cube arrives as 12 loose triangles otherwise). Moving the walk into the
service would mean carrying pre-weld UVs out of the reader, which is the same information in a
less-safe shape. The consequence a reader should know: **a GLB import does not put its texture into
`Model::image_assets`**, so it cannot be re-opened in the Image Fill dialog and re-projected — the
importer has no PNG re-encoder (libslic3r has a decoder only) and a glTF texture may be a JPEG.
Re-applying Image Fill to an imported part works; it just starts from a picture the user supplies.

---

## 6. Measurements

*(Filled in from the acceptance runs; see the session report for the raw logs.)*

---

## 7. What was not done

1. **No 3D preview in the dialog.** Section 1 says why.
2. **The gradient stops are not editable.** Section 1 says why.
3. **A GLB's texture is not stored as an asset.** Section 5 says why.
4. **Paletted, greyscale and 16-bit PNGs are still not decodable** by `PNGReadWrite`. The dialog
   works around it by re-encoding through `wxImage`; a project written by something else that
   carries such a PNG degrades to "no image", not to a crash.
5. **Adaptive subdivision.** The depth is uniform per volume. Section 2 says why, and what it costs.
6. **Phase 3's per-fill-segment resolution.** Out of scope by the plan: Image Fill is still limited
   by facet size, only now the facet size is the user's choice rather than the modeller's.
7. **The dialog was not clicked.** See the acceptance section of the session report: there is no
   automation in this repository that can drive a wxWidgets dialog, so the dialog's own behaviour is
   argued from the model-level API the tests do exercise, plus a hidden instance proving the changed
   binary starts and stays up.
