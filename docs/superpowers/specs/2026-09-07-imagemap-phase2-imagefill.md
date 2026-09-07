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

> **Follow-up, 2026-09-07, from hands-on testing.** Three things were reported from a 30 mm cube
> with a three-vertical-band image. (1) A flat projection painted every face - the top, the bottom
> and all four sides. Fixed: a projection now lands only on the faces it points at, and the dialog
> has a "Faces" control and a "From the negative side of the axis" box. (2) A wrap looked like the
> flat projection. The wrap's arithmetic was right - the tests in section 6.1 now prove the two are
> different paintings of the same image on the same cube - but nothing about the result *said* which
> projection had been applied, and the dialog cached the controls from their change events rather
> than reading them at Apply. Both hardened: `ImageFillDialog::params()` re-reads every control, and
> the dialog's summary line spells out the projection, the axis, the side and the face rule in
> words. A wrap also no longer paints the caps. (3) Axis Y and axis Z agreeing on a vertical-band
> image is arithmetic, not a bug; section 2.2 says why, and it is pinned by a test so nobody
> "fixes" it. Sections 2.1 and 2.2 are the whole of the new semantics.

---

## 1. The user flow, as built

1. Select a part in the 3D view or the object list.
2. Right-click → **Apply image fill...** (one new menu item, next to "Split by painted colour").
3. In the dialog:
   * **Source** — an image file (PNG, JPEG, BMP; anything wxWidgets reads), or a two- or
     three-colour gradient. A gradient's stops are taken from the first, middle and last allowed
     filament, so it needs no colour pickers and costs the project no payload at all.
   * **Projection** — *Flat, along an axis* / *Wrapped around an axis* / *The model's own texture
     coordinates* / *On all six sides (box)*, plus the axis (X/Y/Z), **which faces it may paint**,
     **which side of the axis it comes from**, and two mirror checkboxes. Section 2.1 is the whole
     of what those two new controls mean; section 2.3 is the box projection, which disables Axis,
     Faces and "From the negative side" because it uses none of them, and enables one checkbox of
     its own, *Mirror so it reads from outside (box)*.
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
enum class ImageFillProjection { Planar, Cylindrical, MeshUV, Box };
enum class ImageFillAxis       { X, Y, Z };
bool image_fill_project(const ImageFillParams&, const BoundingBoxf3&, const Vec3f &p,
                        float &u, float &v, const Vec3f *facet_normal = nullptr);
int  image_fill_box_face(const Vec3f &normal);   // 0 = +X, 1 = -X, 2 = +Y, 3 = -Y, 4 = +Z, 5 = -Z
```

* **Planar** — `u`, `v` are the point's position along the two axes that are not `axis`, normalised
  over the part's bounding box, in ascending axis order. Axis Z gives (x, y); axis X gives (y, z);
  axis Y gives (x, z).
* **Cylindrical** — `u = atan2(b, a) / 2π + 0.5` about the box's centre in the two other axes; `v`
  runs along `axis`. A point exactly on the axis returns `false` rather than guessing an angle.
  **The seam is fixed and predictable**: `u = 0` and `u = 1` meet on the **negative side of the
  first of the two other axes**, and the middle of the image (`u = 0.5`) faces the positive side of
  it. Wrapping about Z: seam on −X, middle of the image facing +X, `u = 0.25` at −Y and `u = 0.75`
  at +Y. About Y: seam on −X again, middle facing +X. About X: seam on −Y, middle facing +Y.
  Rotating the part rotates the seam with it, because the projection is in the part's own space.
* **MeshUV** — supplied by the caller, not computed. `image_fill_project` returns `false` for it.
* **Box** — tri-planar. Each facet is projected flat along the axis of its own dominant normal
  component, so one image lands on all six sides of a box. Section 2.3.

`v` runs **up**; the sampler flips it once, so image row 0 is at `v = 1`.

### 2.1 Which faces a projection paints

```cpp
enum class ImageFillFaces : int { Facing = 0, Through = 1, All = 2 };
bool image_fill_face_is_painted(const ImageFillParams&, const BoundingBoxf3&,
                                const Vec3f &normal, const Vec3f &centroid);
```

A projection is a **direction, not a solid**. The first build of Phase 2 painted every facet, because
a side facet's centroid still has an (x, y) and so still samples a pixel — which is why a flat
projection along Z put the picture on the top face **and** on the bottom **and** smeared it down all
four sides. The rule now, decided once per **original** facet (every leaf of a facet shares its
plane, so the subdivision refines the sampling and not the geometry):

| `faces` | painted when | in the dialog |
| --- | --- | --- |
| `Facing` (default) | `normal · direction > 1e-4` | "Only the faces it points at" |
| `Through` | `\|normal · direction\| > 1e-4` | "Project through (both sides)" |
| `All` | always | **not offered** — it is the old behaviour, kept only for the importers, the tests and an old params string |

**The rule does not apply to a Box projection at all**, and the dialog disables the control there.
Under a tri-planar projection each facet is projected along the axis it most nearly faces, so it
faces its own projection by construction and there is nothing left to exclude. `Through` in
particular would be a no-op, which is why it is disabled rather than offered; `collect()` forces
`faces = Facing` for Box so the params string a box fill records says what it actually did.

* **`direction`** is `+axis`, or `−axis` when `axis_negative` (the dialog's "From the negative side
  of the axis"). So a flat projection along Z lands on what faces up; tick the box and it lands on
  what faces down instead.
* **For a cylindrical projection** the direction is not the axis but the facet's own **outward
  radial** — so a wrap paints the surfaces facing away from the axis and leaves the end caps
  unpainted, their radial component being exactly zero. `axis_negative` selects the inward-facing
  surfaces instead, which is what a bore wants; asking for the inside of a solid part is refused
  with a message rather than silently painting everything.
* **A facet parallel to the direction is painted by neither `Facing` nor `Through`.** A cube's four
  sides under a projection along Z score exactly `0`, and `1e-4` is the epsilon that keeps a
  grazing facet on the same side of the question as an exactly parallel one. Stated plainly: the
  side faces stay unpainted, and if you want the picture on them you aim the axis at them.
* **`Through` does not mirror.** The far face takes the same `u`, `v` its position gives, so it
  reads mirrored when you look at it from behind. "Mirror horizontally" is the control that turns
  that round; making `Through` mirror by itself would take the choice away.
* **Painting the whole part is a replacement**, so the faces the image misses come out *unpainted* —
  that is what "the image is on this face and nowhere else" has to mean. Applying to a **face
  selection** is still a merge, and the cull now feeds the merge mask too: a facet the projection
  does not reach keeps the paint it already had. So the way to put one picture on the top and
  another on the front is to paint each face with the colour tool first and apply per selection.
* **The annotation always records the rule.** `image_fill_params` gains `pf` (the `faces` value) and
  `an` (`axis_negative`) — and, for the box projection, `bm` (`box_mirror`, written only when set,
  so a string without it reads as the unmirrored default). The `proj` reader's clamp is `min(3, .)`
  now, not `min(2, .)`: with the old clamp every saved box fill would have read back as a mesh-UV
  fill and repainted the part differently on reload. `pf` is written even when it is the default: a string without it is a
  string from before the rule existed, and it must not be mistaken for one that chose today's
  default on purpose. Such a string reads back as `Facing`, so re-applying an old annotation does
  the right thing rather than reproducing the bug — the painting already stored is untouched until
  the user applies again.

### 2.2 Why axis Y and axis Z look the same, and why that is not a bug

Reported, checked, and deliberately left alone. `u` and `v` are the two axes that are **not** the
projection axis, in ascending order: Z gives (x, y), Y gives (x, z), X gives (y, z). For an image
whose colour depends only on `u` — vertical bands, the shape of `bands_rgb.png` — axis Z and axis Y
are therefore *the same function of position*: both read the colour off **x**. With `faces = All`
the two paintings are identical facet for facet, which is what the test
`axis Y matches axis Z for a vertical-band image` pins. Axis **X** reads the colour off **y** and
does differ.

What the face rule changes about this: with the default `Facing`, axis Z paints the top face and
axis Y paints the +Y face, so the two now differ *by which faces they reach* even though they agree
about the colour at any given point. The same test pins that too.

### 2.3 The Box projection — one image on all six sides

The owner's finding after the first hands-on test: *"there is currently no way to hit all 3 planes
with an image, since there is no multiplanar or tri-planar wrapping."* A flat projection paints one
face; a wrap paints a band and leaves the caps; neither puts a picture on a whole box. `Box` does.

**How a facet chooses its face.** `image_fill_box_face(normal)` takes the axis of the normal's
**dominant component** and its sign, giving one of six faces — `0 = +X, 1 = -X, 2 = +Y, 3 = -Y,
4 = +Z, 5 = -Z`. The point is then projected flat along that axis, exactly as `Planar` does for
its one axis.

* It is the **facet** normal, not a per-vertex one, and it is computed **once per original facet**
  and reused for all `4^depth` of its leaves. That is what stops a triangle being cut in half by a
  projection seam: a seam can only ever fall along a triangle edge, never across the middle of a
  face. The test `no leaf of a facet disagrees with its own normal` pins it on an icosphere.
* A **tie** — a facet at exactly 45 degrees between two axes — goes to the **lower axis index**
  (X, then Y, then Z). Arbitrary, but fixed, so two runs on one mesh always agree.
* A **degenerate** facet (zero-length normal) belongs to no face and `image_fill_project` returns
  `false` for it, rather than guessing.
* On a **curved** part the result is a blend by dominant axis: the sphere is divided into six
  patches whose boundaries follow triangle edges. **No facet is ever left unpainted** — which is
  the one thing a flat projection can never promise, and the test measures both sides of it.

**The orientation rule, per face.** Two things are fixed and one is a choice.

* `v` — **"up" in the image** — is **+Z** on the four side faces (dominant axis X or Y) and **+Y**
  on the top and the bottom (dominant axis Z). Always, in both mirror modes. This is what makes a
  picture stand up the right way as you walk around the part rather than lying on its side.
* `u` — **"right" in the image** — is the remaining world axis, and **by default runs in its
  positive direction on every face**.
* `box_mirror` (the dialog's *Mirror so it reads from outside (box)*) flips `u` on exactly the
  three faces where the default reads backwards.

| face | `u` (image right) | `v` (image up) | `u` when `box_mirror` |
| --- | --- | --- | --- |
| **+X** | +Y | +Z | +Y *(unchanged)* |
| **-X** | +Y | +Z | **-Y** |
| **+Y** | +X | +Z | **-X** |
| **-Y** | +X | +Z | +X *(unchanged)* |
| **+Z** (top) | +X | +Y | +X *(unchanged)* |
| **-Z** (bottom) | +X | +Y | **-X** |

**Why that is the default, and what the checkbox is for.** The default column is exactly what the
`Planar` projection of each axis already gives: `Planar` X is `(y, z)`, Y is `(x, z)`, Z is
`(x, y)`, taken in ascending axis order. So a box fill's **+X, -Y and +Z faces are identical, leaf
for leaf, to a flat projection along X, Y and Z** — which the test
`the top face agrees, leaf for leaf, with a flat projection along +Z` pins as a consequence rather
than a restatement. The price is that on the opposite three faces you are looking at that same
projection **from behind**, so it reads mirrored — the same thing `Through`'s far face does, and
for the same reason.

Ticking the box flips `u` on `-X`, `+Y` and `-Z`, which is the same statement as **`u = v x n` on
every face**: with `v` up the screen and `n` pointing at you, `v x n` is screen-right, and that is
the condition for text to read the right way round from outside. So: **leave it off** when you want
the box faces to agree with flat projections and do not care which way round the back reads;
**tick it** when the image has text or a logo and should read correctly on all six sides.

`flip_u` and `flip_v` still apply globally on top, as they do for every other projection.

**What `axis` and `axis_negative` do: nothing.** Every face is its own axis. The dialog disables
both controls, and the test `the axis and the negative-side flag make no difference to it` asserts
the painting is byte-identical across all three axes and both signs — so a disabled control cannot
be a control that silently matters.

**What to expect, on a cube, with the two sample images.**

* **`quadrants.png`** (a 2x2 diagonal checker: yellow top-left and bottom-right, dark top-right and
  bottom-left). Every face gets the whole checker. With the box **unticked**, the yellow runs on
  the "upper-left to lower-right" diagonal on the top, the front (-Y) and the right (+X), and on
  the *other* diagonal on the back (+Y), the left (-X) and the bottom — because a horizontal flip
  turns this checker into its opposite. That flip is the visible signature of the default rule.
  **Tick the box** and all six faces show yellow on the same diagonal as seen from outside.
* **`bands_rgb.png`** (three vertical bands: red, green, blue, left to right). The colour depends
  only on `u`, so every face gets three stripes running **top to bottom** on the four sides (`v` is
  +Z there) and **parallel to Y** on the top and the bottom. **Unticked**: red is at low X on the
  front, low Y on the right, and low X on the top and the bottom — so the stripes line up
  continuously across the front/top edge — but the back and the left read blue-green-red left to
  right, i.e. reversed. **Ticked**: every face reads red-green-blue left to right when you look at
  it square-on from outside. Either way the picture **restarts** at the vertical corner edges — a
  tri-planar projection repeats the image per face and is not continuous around the part; that is
  what it is, not a defect.

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
count, so two runs on one image give the same palette in the same order. The unit tests pin that
(§6.1); note that it does not make the whole SLICE reproducible - §6.3 measures where that stops.

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

**A face selection is a merge, not a replacement.** `image_fill_encode` takes an optional
`selected` mask and the part's existing painting; an original triangle the caller did not select
keeps whatever it had, copied bit-for-bit out of the old bitstream. `serialize()` writes its
entries in ascending triangle order and the bitstream in that same order, so a triangle's bits run
from its own start index to the next entry's, which is what makes a verbatim copy possible at all.
Without this, "apply the image only to these faces" would erase every other stroke on the part.

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

Everything below was produced on this branch, in its own build tree
(`.claude/worktrees/imagemap-p2/build`, VS 2022 x64 Release, `BUILD_TESTS=ON`, deps from
`C:/Dev/SnapmakerOrca/deps/build/OrcaSlicer_dep/usr/local`), installed to
`snorca_hubtest/inst_imgp2_cand`, against a baseline installed from the **phase 1** branch's own
build tree at `snorca_hubtest/inst_imgp1_base`. Every slice ran on an isolated `--datadir` copied
from `snorca_hubtest/dd_lan`. The user's live install and data directory were never touched.

### 6.1 Unit tests

`libslic3r_tests`:

```
test cases:   642 |   640 passed | 2 failed as expected
assertions: 73035 | 73033 passed | 2 failed as expected
```

The two expected failures are the pre-existing pair in `tests/libslic3r/test_mixed_filament.cpp`.
Phase 1 was **628 cases** with the same two, so this branch adds **14**, all of them in
`tests/libslic3r/test_image_fill.cpp` and all of them in the default run:

| case | what it pins |
| --- | --- |
| asset hash | the five fixtures' SHA-256s against `make_fixtures.py`'s own output, the empty-input hash, dedupe, `retain()`, and that the decoded pixels are the ones written. `bands3.png` (three vertical bands) and `wrap4.png` (four bands offset by half a band, so each side of a cube falls in the middle of one) were added for the projection tests |
| projections | known points to known `u`,`v` for planar on each axis, cylindrical on Z and on X, the flips, and MeshUV declining to guess |
| the quadrants | end to end: a 2 x 2 picture on a 4 x 4 mm quad at depth 3 must put each colour in the right quadrant, read back through the fork's decoder |
| mesh UVs | the same quad with UVs that rotate the picture a quarter turn, so a projection that ignored them could not pass both |
| subdivision | 4^n leaves whose areas sum to the original; a painted cube through `ColorSplit::extract_color_patches` with zero open edges at four depths; the encoder round-tripping through `TriangleSelector`; the nibble chain; the depth caps |
| palette and solver | determinism, counts that add up, the obvious filament for an obvious colour, only allowed filaments, per-face colours, and two identical runs of `image_fill_compute` |
| params | the `k=v;k=v` round trip, gradients, refusal of a string with no version marker, and a gradient painting with no asset at all |
| 3MF | apply, save, reload: the painted facets bit-identical, the asset byte-identical under the same hash, the annotation recoverable, and re-applying it reproducing the same painting |
| `[glb]` | the before/after of §6.5 |
| `[barb]` | writes the Bar B project of §6.3, asserting the painting first |
| `[selection]` | a face selection is a merge: the unselected facets' bits are carried over verbatim, only four of the cube's twelve triangles carry anything, and a selection nothing is painted with is refused |
| `[faces]` flat | a flat fill along +Z on a cube touches the top face and no other; `axis_negative` moves it to the bottom; `Through` gives top **and** bottom and still not the sides; `All` gives all six; and the predicate itself is checked against a top, a bottom and a side normal |
| `[faces]` wrap | a wrap about Z with `wrap4.png` gives the four sides four **different** filaments (-X white, -Y red, +X green, +Y blue, one colour all the way across each face) and leaves both caps unpainted; asking for the inward surfaces of a solid cube is refused; and flat vs wrapped on one cube with one image touch different facets, and disagree on well over a fifth of the leaves even with the culling switched off |
| `[faces]` axes | axis Y and axis Z produce the **identical** painting for a vertical-band image with `faces = All` (both read the colour off x - section 2.2), axis X does not, and with the face rule on Y and Z part company because they land on different faces |

### 6.2 Bar A - N = 2 new keys

`snorca_hubtest/bar_a_imgp2.py`, `OrcaToleranceTest.stl` on "Bambu Lab P1S 0.4 nozzle" /
`0.20mm Standard @BBL X1C` / `Generic PLA`:

```
base lines 24560, cand lines 24562
base sha256 (timestamp dropped):                 18f37a944317a99395770e6bf29f20f22c474601549329fdc53dc24dacb81ae7
cand sha256 (timestamp dropped):                 4e3575799be794de08e32439404a06c3badbd878e7ef10f4daceec2f25739bdb
cand sha256 (timestamp + new keys dropped):      18f37a944317a99395770e6bf29f20f22c474601549329fdc53dc24dacb81ae7

NEW CONFIG LINES IN THE CANDIDATE (N = 2):
   '; image_fill_detail = 0'
   '; image_fill_params = '

RESULT: identical apart from the timestamp line and the 2 new key line(s).
label/M624/M625 lines: base 129, cand 129, identical: True
   base: ; model label id: 15        cand: ; model label id: 15
BAR A: PASS
```

Two things worth naming. The baseline hash is **the same value the phase 1 report recorded**
(`18f37a94...`), which is an independent check that the baseline install and the harness are the
ones that document describes. And `; model label id: 15` is unchanged, along with all 129
label/`M624`/`M625` lines - the automated proof that no `ObjectBase`-derived member was added.
(PR 10 was measured shifting that number from 15 to 23.)

### 6.3 Bar B - a real three-filament slice of an applied image

The project is a 30 mm cube with `stripes3.png` applied to it through the real `image_fill_apply`
at subdivision 4, written by `libslic3r_tests.exe "[barb]"` - the CLI cannot make one, because
`Snapmaker_Orca.cpp` hands `read_from_file` a null `ObjImportColorFn` and so a command-line import
gets no colour matching at all. 12 facets x 256 leaves = **3072 painted leaves, 3 filaments**.

Sliced twice by `snorca_hubtest/bar_b_imgp2.py` on P1S / `0.20mm Standard @BBL X1C` with three
`Generic PLA` spools:

| | run 1 | run 2 |
| --- | --- | --- |
| G-code lines | 107 928 | 107 916 |
| tool-change lines | 304 | 304 |
| tools used | T0, T1, T2 | T0, T1, T2 |
| estimated time | 30 214.0 s | 30 213.9 s |
| filament | 71.889 g | 71.890 g |

* **The facets are painted with the allowed ids.** The project's `3D/3dmodel.model` carries
  `paint_color` on all 12 triangles, each a four-way split tree 341-597 characters long, and the
  states in them are `4`, `8` and `0C` - filaments 1, 2 and 3, the three the dialog allowed.
* **Tool changes appear where the image changes colour.** T2 x 149, T1 x 76, T0 x 76 across 304
  tool-change lines, on a part that would have none without the image.
* **The annotation and the asset survive the slice.** The exported `--export-3mf` carries
  `image_fill_params`, `Metadata/image_fill/<sha>.png` and `Metadata/image_fill/manifest.json`, and
  12 `paint_color` trees in `3D/Objects/image fill cube_1.model`.
* **The two runs are NOT bit-identical - and neither are two runs of the phase 1 build on the same
  file.** This is the one acceptance item that does not come back clean, so here is the whole of
  it. Differences are `M73` progress lines, the header's estimated time by one second, and one
  top-surface infill fragment. Slicing the same project twice on **each** build and comparing every
  pair, with `M73` and the time estimate dropped:

  | pair | differing lines | of |
  | --- | --- | --- |
  | phase 1 run 1 vs phase 1 run 2 | 313 | 107 220 (0.292 %) |
  | phase 2 run 1 vs phase 2 run 2 | 210 | 107 237 (0.196 %) |
  | phase 1 run 2 vs phase 2 run 1 | **180** | 107 213 (0.168 %) |
  | phase 1 run 1 vs phase 2 run 1 | 490 | 107 220 (0.457 %) |

  The cross-build pair with the *smallest* difference (180 lines) is smaller than either build's
  own run-to-run difference, so the two builds are indistinguishable from the noise. The
  non-determinism is a pre-existing property of the MMU-painted path on this box, not something
  Phase 2 introduced - which is worth someone's attention on its own, but is not this branch's.
  Note the contrast with Bar A: the single-filament `OrcaToleranceTest.stl` slice **is** exactly
  reproducible on both builds. It is the multi-material path that is not.

### 6.4 The plan's Bar B - the support corpus, feature off

The plan's own Phase 2 acceptance also names **Bar B** in its original sense:
`scripts/support_group_identity.py` over `tests/data/support_corpus`, comparing every case's G-code
between the two builds through the fork's own Slice Compare engine. Run with the phase 1 install as
the baseline and this branch's as the candidate, all 20 cases:

```
20 cases: config_rows=2, layers changed=0, a_only=0, b_only=0,
          segments=100.0000%, dirty_layers=0, est_seconds identical
changed_config on every case, and nothing else:
    {"key": "image_fill_detail",  "a": "", "b": "0"}
    {"key": "image_fill_params",  "a": "", "b": ""}
RESULT: 20 case(s) differ        (the gate requires ZERO changed config rows)
```

The corpus covers grid, snug, ledge, soluble and dense interfaces, `over_support_off`, ironing,
organic and classic trees in four variants, a raft, no support at all, and the three handy models -
up to 593 layers and 708 354 segments (`handy_bunny_tree_organic`). **Every segment of every layer
of every case is identical**, including the estimated print time to three decimals. The gate reports
"differ" purely because its structural criterion is *zero* changed config rows, and N = 2 by
construction; the plan's wording anticipates exactly this ("plus exactly the N new
`; <key> = <default>` lines that new `PrintConfig` keys legitimately add").

Worth noting for whoever reads the JSON: `image_fill_params` shows `"a": "", "b": ""` - the values
agree and the *row* is what is new.

### 6.5 The glTF before/after

`tests/data/image_fill/agent_plaque.glb` and `agent_medallion.glb`, written for this branch by
`make_glb.py` from `struct` and `zlib` so nothing about them comes from the reader they measure.
Both are deliberately low-poly with a detailed 64 x 64 texture - the case the GLB import status
document's gap 4 names. Both paths run in one binary on one import, and both are scored with the
**old** path's own metric (CIE76 dE from `ObjColorMatch`) against the finest available sampling of
the texture, area-weighted. The filaments are chosen by `obj_color_auto_match` from a four-spool
start, exactly as a headless import does, so both sides get the same spools.

| file | triangles | leaves | | painted | filaments | mean dE | max dE |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `agent_plaque.glb` | 12 | 192 | **before** | 192 | 4 | 50.27 | 139.56 |
| | | | **after** | 192 | 4 | **31.70** | 139.56 |
| `agent_medallion.glb` | 64 | 1024 | **before** | 1024 | 8 | 48.49 | 170.94 |
| | | | **after** | 1024 | 8 | **1.90** | **4.74** |

**37 % less colour error on the plaque, 96 % less on the medallion**, with the same filaments and
nothing left unpainted. The two differ because of what 16 samples per triangle can resolve: the
plaque's twelve triangles are enormous (each covers a third of a texture carrying six colours and
an 8-pixel checker), so 16 samples still cannot see the checker; the medallion's wall quads each
span 1/16 of the texture's width, so 16 samples resolve its bands almost exactly. That is the
honest shape of this change - it helps most where the mesh is fine enough to carry the detail, and
a user who wants more re-applies Image Fill from the object menu, where the subdivision control is.

**The pre-existing bug this found.** `png::decode_colored_png` fills its output buffer bottom-up
(`PNGReadWrite.cpp:163-166` walks the rows backwards), while `GLTF.cpp`'s `TextureImage::sample`
maps `v` straight onto the row index because glTF's UV origin is the image's *top*-left. So every
PNG `baseColorTexture` this fork has ever imported was sampled **vertically mirrored**. JPEG
textures were always right - libjpeg hands rows back top-down - which is why the JPEG fixtures
never caught it. Fixed in `decode_png_image`, where the decoder's convention is known, so `sample`
keeps its one honest rule. This is a second behaviour change to GLB import, and it makes the result
correct rather than merely different.

### 6.6 GUI

A hidden scratch instance of this build (`snorca_hubtest/run_control_app.py`, its own install at
`inst_imgp2_cand`, its own data dir copied from `dd_lan`) starts on the Bar B project and stays up.
So the new dialog and the new menu item link and initialise inside the real application.

**Not verified by hand, and this needs saying plainly: nobody clicked anything.** There is no
automation in this repository that can open a wxWidgets dialog and read it - `run_control_app.py`
only starts the app hidden, and the phone-control API covers slicing and sending, not dialogs. So
the dialog's own behaviour rests on (a) the model-level API the eight unit tests do exercise, which
is everything `Apply` calls, and (b) the fact that `Plater::apply_image_fill` is a thin adapter
between the two. What was **not** observed: the dialog rendering, the preview thumbnail updating as
controls change, the file picker accepting a JPEG, the filament checklist, Apply changing the 3D
view, and the undo of an apply.

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
7. **A face selection has to be painted on whole facets.** The selection is matched by comparing
   the paint tool's leaves against the part's own triangles, so a stroke that only covers part of a
   facet is refused with a message that says so rather than being rounded up or down. Painting a
   selection with the bucket-fill or the seed-fill tool, which work per facet, is the way to make
   one. Matching a sub-facet selection would mean intersecting two subdivision trees, which is
   Phase 3's shape of problem, not this one's.
8. **The multi-material slice is not reproducible run to run.** §6.3 measures it and shows it
   predates this branch, but nobody has chased it down. It matters more now than it did, because
   Image Fill makes multi-material slices something an ordinary user reaches for.
9. **Nothing paid attention to how long a big fill takes.** `image_fill_compute` walks the leaves
   twice and does one linear pass over the palette per leaf, so at the 4 000 000-leaf ceiling that
   is on the order of a billion distance computations. On a cube it is instant; on a dense mesh at
   a fine detail setting it will be a visible pause with no progress bar. Bucketing the
   sample-to-palette lookup by the quantiser's own 5-bit key would collapse it to a hash lookup,
   and that is the first thing to do if anyone complains.
10. **The dialog was not clicked.** See §6.6: there is no automation in this repository that can
   drive a wxWidgets dialog, so the dialog's own behaviour is argued from the model-level API the
   tests do exercise, plus a hidden instance proving the changed binary starts and stays up.
