# Per-part brim — design

Date: 2026-09-27
Branch: `feat/per-part-brim`
Status: approved by owner (2026-09-27), pre-implementation

## 1. Goal

Make brim controllable per **part** (ModelVolume) instead of only per object / globally.
When printing an assembly of dissimilar parts, some parts need a brim for bed adhesion and
others do not. Today `brim_type`, `brim_width`, and `brim_object_gap` live only in
`PrintObjectConfig`; every read in `src/libslic3r/Brim.cpp` is `object->config().brim_*`, and
the bottom-layer polygons are merged across all of an object's regions before brim is computed.

## 2. Scope and semantics

- Keys that become part-capable: `brim_type`, `brim_width`, `brim_object_gap`.
- Fallback is per key: **part → object → global** (approved: option B scope, option A semantics).
- A part wins in both directions: it can opt *into* a brim when the object has none
  (`brim_type == no_brim` / `brim_width == 0` at object level), and opt *out* when the object
  has one.
- Brim-ears parameters (`brim_ears*`) and painted brim stay object-level-only in v1. A
  part-level `brim_type` of `brim_ears` or `painted` is honored as a type choice, but the ear /
  paint parameters come from the object-level config.
- Raft behavior is unchanged (raft suppresses brim as today).

## 3. Existing infrastructure reused

- `ModelVolume::config` (Model.hpp:910) already stores per-part keys and round-trips through
  both 3MF writers/readers (`Format/3mf.cpp:2208/3164`, `Format/bbs_3mf.cpp:5718/8609`).
  Edge ↔ Edge serialization needs no new code.
- The **Objects Table already has a per-part Brim (type) column**
  (GUI_ObjectTable.cpp:1889/2027) that reads/writes `volume->config` with the
  part → object → global fallback and auto-erases a part key equal to the object value.
  It becomes functional once slicing consumes the key.
- The **per-part Settings panel** (`TabPrintPart`, Tab.cpp:4330) is built from
  `PrintRegionConfig().keys()` + the curated `SettingsFactory::part_support_keys()` list
  (the support-groups precedent, plans/2026-09-02-support-sets-and-groups.md §2.3/§3.5).
- Invalidation precedent: `PrintObject::object_config_from_model_object` (PrintObject.cpp:3697)
  folds part-level data into the diffed object config so the existing PrintApply config-diff
  re-slices (support_top_z_distance does exactly this).
- Bambu export: `BambuExport::convert` (Format/BambuExport.cpp:907) already walks part configs
  (bbs_3mf.cpp:8606), drops unknown keys, and reports them
  ("N setting(s) not supported by Bambu Studio left out").

## 4. Key plumbing (libslic3r + GUI)

- New `const std::vector<std::string>& Slic3r::part_brim_keys()` in
  `src/libslic3r/PrintConfig.cpp` (next to `part_support_keys()`, PrintConfig.cpp:9363),
  declared in PrintConfig.hpp: `{ "brim_type", "brim_width", "brim_object_gap" }`.
- `TabPrintPart` constructor (Tab.cpp:4336): append `part_brim_keys()` to the key list so the
  part Settings panel renders the three keys exactly like the object-level panel.
- `SettingsFactory` (GUI_Factories.cpp): treat the brim keys like the curated support keys so
  part-level settings bundles include them and object-only bundles don't leak them onto parts.
- Objects Table: no code change expected for `brim_type`; width/gap editing lives in the part
  Settings panel (approved UI choice: mirror the object level, no new table columns).

## 5. Slicing (Brim.cpp) — Approach 1: per-part first-layer footprints

Approved approach: compute each part's first-layer footprint separately instead of attributing
the object's merged bottom-layer slices (Approach 2, rejected: parts sharing a PrintRegion have
merged slices and cannot be attributed).

1. For each `ModelVolume` that `is_model_part()` of a `PrintObject`, slice the volume mesh at
   the first-layer slice height in object coordinates to get that part's first-layer footprint
   (ExPolygons). Elephant-foot compensation applies per part as it does per object today
   (first-layer slices already include it; the part footprint computation must match the same
   compensation so brim lands where the extrusions land).
2. Resolve each part's effective `(brim_type, brim_width, brim_object_gap)` via the
   part → object → global fallback (part wins both directions).
3. Generate brim area per part from its own footprint with its own width/gap, then union —
   exactly like today's per-object brim areas union at print level.
4. **Every** part's footprint + gap enters `no_brim_area`, including brimless parts, so a
   neighbor's brim never crosses a clean part. (This extends the existing per-object
   `no_brim_area` rule, Brim.cpp:230-233, to part granularity within an object.)
5. Where two brimmed parts touch: areas union; differing widths extend differently from each
   footprint. Where widths differ and brims merge, the wider brim defines the outer edge.
6. Cross-object merge, auto-brim width rounding (Brim.cpp:207-212, per object — extended to
   resolve per part), support-first-layer brim, and sequential (by-object) printing paths are
   unchanged in structure; they consume the same brim/no-brim maps.

`PrintObject::has_brim()` must return true if any part resolves to a brim, so the object enters
the top-level brim island logic.

## 6. Invalidation

- Extend `object_config_from_model_object` to fold the part-level brim keys into the returned
  `PrintObjectConfig` as a composite (e.g. per-part resolved brim signature serialized into an
  Ultra-namespaced key, following the support-groups comment pattern at PrintObject.cpp:3708-3718),
  so the existing PrintApply config-diff (PrintApply.cpp:1806) invalidates the brim step
  (`invalidate_state_by_config_options` already maps `brim_*` keys to the brim step) whenever a
  part's brim settings change.
- No new invalidation machinery.

## 7. Bambu 3MF export — generalize up to object level

Bambu Studio ignores per-part brim metadata. On `export bambu 3mf`:

- If **no** part carries an explicit brim key: unchanged behavior.
- If any part does: flatten to object level in the exported file —
  - `brim_type`: `outer_only` if the brimmed parts disagree, else their common type
    (`no_brim` only if every part says `no_brim`);
  - `brim_width`: max width among brimmed parts;
  - `brim_object_gap`: max gap among brimmed parts;
  - the part-level brim keys are dropped from part metadata;
  - the export report gains a note: "per-part brim flattened to object level; Bambu Studio does
    not support per-part brim" (mechanism: BambuExport.cpp:701-714 `dropped`/`notes`).
- Rationale (owner, 2026-09-27): Edge-only specialty keys must generalize back to
  Bambu-compatible keys on export, accepting that the per-part behavior itself won't work in
  Bambu Studio.

## 8. Out of scope (v1)

- Per-part brim-ears parameters / painted brim parameters.
- Per-part brim on height-range (layer) settings — `TabPrintLayer` deliberately not widened
  (same boundary as support groups).
- Objects Table numeric columns for width/gap.
- Skirt, raft brim, prime tower brim: untouched.

## 9. Testing

libslic3r unit tests (extend the brim test file or add one):

1. Fallback chain: part with only `brim_width` set inherits type/gap from object; part with
   nothing inherits everything; part wins over object both directions.
2. Geometry: two-part object, one brimmed one not — brim around part A only, no brim material
   within part B's footprint + gap.
3. Adjacent brimmed parts with different widths — union, wider edge from the wider part.
4. Object `no_brim` + one part `outer_only` → that part gets a brim.
5. 3MF round-trip: part brim keys survive save/load (Edge format).
6. Bambu export flattening: mixed part keys → correct object-level flatten + report note.
7. Invalidation: changing a part brim key re-slices the brim step (config-diff test via
   PrintApply, mirroring the support-groups tests if present).

Manual GUI verification (owner): part Settings panel shows the three keys with orange dirty
markers; Objects Table Brim column affects slicing; sliced preview shows brim only on chosen
parts.

## 10. Risks / open points resolved during implementation

- Exact source of per-part first-layer footprints: prefer reusing PrintObject's existing
  per-volume slice intermediates if they survive to the brim step; otherwise slice the volume
  meshes directly at first-layer z. Must match elephant-foot compensation of the real first
  layer. (Implementation detail; behavior spec: brim hugs the actual first-layer extrusions.)
- `auto_brim` per part: width analysis runs per brimmed part; rounding to flow width as today.
