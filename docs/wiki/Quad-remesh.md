# Quad remesh

> Staging: available on `feat/ultra-preferences` (builds with QuadriFlow). Not on `main` yet.

## What it is

**Quad remesh...** rebuilds selected parts as an **even grid of quads** at a target face count. Needs a **closed** mesh. The part is stored as triangles (two per quad). Painted supports, seams, and colours are **cleared**.

## Where to find it

- Object / part / multi-selection menus → next to **Repair/Remesh** → **Quad remesh...**
- Hidden when QuadriFlow is not in the build (`quad_remesh_available()`)
- Also: **Sculpt** gizmo panel → **Quad remesh...** (same action)

## How to use it

1. Select a closed part (or object / multi-selection that includes remeshable parts).
2. Right-click → **Quad remesh...** (or use the Sculpt panel button).
3. In the **Quad remesh** dialog:
   - **Target faces**
   - **Preserve sharp features**
4. Confirm with **Remesh**.
5. Re-apply paint/seams if you still need them.

Tooltip: *Rebuild the selected parts as an even grid of quads at a target face count (needs a closed mesh).*

## Limits / notes

| Situation | What happens |
|---|---|
| Open / non-manifold mesh | Refused with *This part cannot be quad remeshed:* + reason. |
| Paint / seams / colours | Cleared by remesh. |
| vs Repair/Remesh | Repair/Remesh = OpenVDB triangle voxel remesh. Quad remesh = QuadriFlow quad grid. |
| Build without QuadriFlow | Menu item hidden. |

## Related

- [Repair/Remesh](Repair-Remesh)  
- [Round all edges](Round-all-edges)  
- [Object menu](Object-menu)  
- [Sculpt](Sculpt)  
- [Home](Home)  
