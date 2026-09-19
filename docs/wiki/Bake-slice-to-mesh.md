# Bake slice to mesh

> Staging: available on `feat/ultra-preferences`. Not on `main` yet.

## What it is

**Bake slice to mesh...** rebuilds the object as the **outer wall the slicer will actually print**, so you can re-slice that mesh at another layer height (or export it).

It uses the current slice’s extrusion paths (including fuzzy skin), not a separate “smooth contour only” path in the live UI.

## Where to find it

- Prepare **object** context menu → **Bake slice to mesh...**
- Object list object row or canvas right-click on a full object / instance

**Not** on the part menu, multi-selection menus, or Assemble view.

**Enable gate:** exactly one object, and the plate must already be sliced through perimeters (`can_bake_slice_to_mesh` / `slice_bake_available`).

## How to use it

1. Slice the plate so perimeters exist for the object.
2. Right-click the object → **Bake slice to mesh...**
3. In the **Bake slice to mesh** dialog:
   - **Result:** Replace object / Add as new object / Export STL...
   - **Resolution** (mm)
   - **Smooth vertical steps**
   - **Close small gaps** / **Gap radius**
4. Confirm with **Bake**.

Tooltip: *Rebuild this object as the outer wall the slicer will actually print, so it can be re-sliced at another layer height.*

## Limits / notes

| Situation | What happens |
|---|---|
| Not sliced yet / no perimeters | Menu item stays disabled. |
| Multi-object or part selection | Bake is not offered. |
| Contour vs extrusion UI | Dialog can build contour options, but live UI uses **extrusion paths** (fuzzy skin included). |
| vs SVG/text gizmo **Bake** | Different feature — gizmo Bake only drops SVG/text editability into an uneditable part. |

## Related

- [Object menu](Object-menu)  
- [Repair/Remesh](Repair-Remesh)  
- [Quad remesh](Quad-remesh)  
- [Home](Home)  
