# Round all edges

> Staging: available on `feat/ultra-preferences` (OpenVDB). Not on `main` yet.

## What it is

**Round all edges...** fillets every edge of the selected parts by a radius. The part is **rebuilt**, so painted data is cleared.

## Where to find it

- Object / part / multi-selection menus → next to Repair/Remesh and Quad remesh → **Round all edges...**
- Hidden without OpenVDB (`voxel_ops_available()`)
- Also: Edit gizmo can expose a Round-all control (not only the context menu)

## How to use it

1. Select the part(s).
2. Right-click → **Round all edges...**
3. In the **Round all edges** dialog:
   - **Radius**
   - **Voxel size**
   - **Round inside corners too**
   - **Keep the bottom flat** / **Bottom slab height**
4. Confirm with **Round**.

Tooltip: *Fillet every edge of the selected parts by a radius (the part is rebuilt, so painted data is cleared).*

## Limits / notes

| Situation | What happens |
|---|---|
| Paint / seams / colours | Cleared by the rebuild. |
| Keep the bottom flat | Preserves bed contact when you need a flat first layer. |
| vs Repair/Remesh | Remesh makes watertight; Round fillets edges. |

## Related

- [Repair/Remesh](Repair-Remesh)  
- [Quad remesh](Quad-remesh)  
- [Object menu](Object-menu)  
- [Home](Home)  
