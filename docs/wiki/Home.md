# Welcome to the EdgeSlicer Wiki

EdgeSlicer is an OrcaSlicer-based slicer (via Snapmaker Orca) with bleeding-edge features aimed at supporting many printers in one app.

Some Prepare-menu features (Bake slice to mesh, Quad remesh, Round all edges, Edit/Copy cut) are documented from staging branch `feat/ultra-preferences` and may not be on `main` yet.

This wiki explains **EdgeSlicer-specific** features: what each one is, where to find it, and how to use it. For settings that come from upstream OrcaSlicer unchanged, use the [OrcaSlicer Wiki](https://www.orcaslicer.com/wiki/).

**Current release:** [v2.3.8.0-edge](https://github.com/aceRage/EdgeSlicer/releases/tag/v2.3.8.0-edge) · [GitHub](https://github.com/aceRage/EdgeSlicer) · [Releases](https://github.com/aceRage/EdgeSlicer/releases)

---

## What is Edge-only?

These areas are custom to EdgeSlicer (or heavily extended here). Each page follows the same short shape: **What it is**, **Where to find it**, **How to use it**, and **Limits / notes**.

### Support

| Page | What it covers |
|---|---|
| [Support sets and groups](Support-sets-and-groups) | Named support presets, per-part support groups, the Groups window |
| [Over-support surfaces](Over-support-surfaces) | Bottom/wall faces over support printed like walls, not bridges |

### Edge Hub (phone and remote)

| Page | What it covers |
|---|---|
| [Edge Hub overview](Edge-Hub) | Tray hub, phone page, QR / token link, hidden slicer windows |
| [Phone access and the companion app](Phone-access) | LAN phone UI, iOS/Android app, Reprints |
| [Remote access (Tailscale)](Remote-access-Tailscale) | HTTPS link, allow-list, home vs away |
| [Printer event notifications](Printer-notifications) | Tray, Web Push, Pushover, webhooks, native app push |
| [Store G-Code Files](Store-G-Code-Files) | Archive of sent G-code, retention, phone Reprints |

### Multicolor and materials

| Page | What it covers |
|---|---|
| [Support Filament Matching](Support-Filament-Matching) | Supports/interfaces/brims in the colour of the surface they touch |
| [Outer wall filament](Outer-wall-filament) | Separate filament for outer walls |
| [Paint depth](Paint-depth) | Bound painted multi-material claims by walls or distance |
| [Split by painted colour](Split-by-painted-colour) | Painted regions → separate parts |
| [Spool Manager (Spoolman)](Spool-Manager) | Inventory, slot bindings, usage deduction |

### Assembly and modeling

| Page | What it covers |
|---|---|
| [Auto-Fit Assembly](Auto-Fit-Assembly) | Mate multi-selection by faces / holes / pegs |
| [Assemble tool and Auto-fit](Assemble-tool) | Feature pick, rotate/offset, merge |
| [Flexi joints and Cut tool](Flexi-joints) | Print-in-place joints, curved cut, cut thickness |
| [glTF / GLB import](glTF-import) | Meshes, materials, vertex colours, textures |
| [Sculpt](Sculpt) | Brush sculpting on the Prepare toolbar |
| [Image Fill](Image-Fill) | Project an image onto a part; optional dithering |
| [Fill bed with copies](Fill-bed-with-copies) | Compact / grid packing dialog |
| [Scale to build volume](Scale-to-build-volume) | Fit an object to the bed with gaps |
| [Visibility (Normal / Ghost / Hidden)](Visibility) | Eye column; Ghost X-ray; Hidden still slices |


### Object / part context menu

| Page | What it covers |
|---|---|
| [Object and part context menus](Object-menu) | Edge right-click map (object, part, multi-select) |
| [Fill bed with copies](Fill-bed-with-copies) | Compact/Grid pack dialog; also Clone → Fill |
| [Scale to build volume](Scale-to-build-volume) | Fit to bed with gaps and centre |
| [Repair/Remesh](Repair-Remesh) | OpenVDB watertight remesh; staging dialog |
| [Bake slice to mesh](Bake-slice-to-mesh) | Staging — bake sliced outer wall to mesh |
| [Quad remesh](Quad-remesh) | Staging — QuadriFlow quad remesh |
| [Round all edges](Round-all-edges) | Staging — fillet all edges |
| [Edit cut and Copy cut](Edit-cut) | Staging — edit/copy stored cut recipe |
| [Split by painted colour](Split-by-painted-colour) | Paint regions → separate parts |

### Print quality

| Page | What it covers |
|---|---|
| [Process settings (Edge overview)](Process-settings) | Map of Edge process tweaks vs upstream Orca |
| [Offset layers](Offset-layers) | Interlocking odd walls (experimental) |
| [Z contouring](Z-contouring) | Smooth shallow top staircasing (Edge notes) |
| [Locked Zag infill](Locked-Zag-infill) | Separate skin/skeleton patterns; skin follows surface |
| [Fuzzy skin (Edge notes)](Fuzzy-skin) | Unfuzz overhangs; width floor |

### Workflow and devices

| Page | What it covers |
|---|---|
| [Preferences → Extras](Preferences-Extras) | Project, presets, Bambu Network, Spoolman, phone, G-code archive |
| [Compare Slices](Compare-Slices) | Diff two slices: settings, time, filament, toolpaths |
| [Stream tab](Stream-tab) | Multi-camera wall (Snapmaker, Moonraker, Bambu LAN, RTSP/ONVIF) |
| [Devices list](Devices) | Multiple printers of the same model as separate entries |
| [Bambu Lab in EdgeSlicer](Bambu-Lab) | Profiles, LAN send, dual-nozzle, mixed nozzles, status |

### Getting started

| Page | What it covers |
|---|---|
| [Download and install](Download-and-install) | Windows / Linux / macOS packages, side-by-side with Snapmaker Orca |
| [Data directory and migration](Data-directory) | `%APPDATA%\EdgeSlicer`, legacy UltraOne / Snapmaker_Orca copy |
| [Supported printers](Supported-printers) | Snapmaker, Bambu, Flashforge, Orca vendor library — status table |
| [Lineage and licence](Lineage) | Fork chain, AGPL-3.0, what is / is not redistributed |

---

## How pages are written

Every feature page aims to answer:

1. **What it is** — one short paragraph.
2. **Where to find it** — menu, tab, or right-click path.
3. **How to use it** — the usual steps, not every edge case.
4. **Limits / notes** — what it will not do, and when stock Orca behaviour still applies.

Screenshots live under `docs/images/` in the repo when a page needs them.

---

## Upstream documentation

EdgeSlicer keeps Snapmaker Orca / OrcaSlicer behaviour for shared settings. Prefer:

- [OrcaSlicer Wiki](https://www.orcaslicer.com/wiki/) — printer, material, and process settings
- [OrcaSlicer Calibration Guide](https://www.orcaslicer.com/wiki/guides/calibration_guide)

---

## Contributing

Bug reports and feature requests: [GitHub Issues](https://github.com/aceRage/EdgeSlicer/issues).  
Wiki edits: keep Edge-only pages here; do not duplicate full upstream setting docs. Prefer the same concise “what / how used” shape as the pages above.
