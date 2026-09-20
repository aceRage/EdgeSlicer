# Repair/Remesh

## What it is

**Repair/Remesh** rebuilds a part (or object) as a **watertight** mesh using an OpenVDB **voxel** remesh (triangle output). Fine detail may be lost depending on voxel size.

It is Edge’s cross-platform repair path. Stock Snapmaker/Orca on Windows also keeps **Fix model** (Netfabb); Remesh is shown on all platforms.

On **`feat/ultra-preferences` (staging)**, Remesh opens a **Repair by remeshing** dialog instead of running immediately (as on `main`).

This is **not** [Quad remesh](Quad-remesh).

## Where to find it

- Object menu → **Repair/Remesh**  
- Part menu → **Repair/Remesh**  
- Multi-selection menus → **Repair/Remesh**  

On staging, the same helper also offers **[Round all edges...](Round-all-edges)** and **[Quad remesh...](Quad-remesh)** when those backends are available.

Windows also shows **Fix model** (Netfabb) when available.

## How to use it

### Staging (`feat/ultra-preferences`)

1. Select the broken or non-manifold object/part(s).
2. Right-click → **Repair/Remesh**.
3. In **Repair by remeshing** set:
   - **Voxel size**
   - **Keep the bottom flat** / **Bottom slab height**
   - **Preserve sharp edges** / **Feature angle**
4. Confirm and inspect the result; re-apply paint/seams if needed.

### `main` (until staging merges)

1. Select the part(s).
2. Right-click → **Repair/Remesh** — remesh runs immediately (no dialog).

Prefer **Simplify Model** (stock) when you only need decimation, not an SDF rebuild.

## Limits / notes

| Situation | What happens |
|---|---|
| Fine detail | Smaller features can disappear depending on voxel size. |
| Repair/Remesh vs Simplify Model | Remesh = SDF rebuild, watertight. Simplify = stock decimation. |
| Repair/Remesh vs Fix model | Remesh always available. Fix model = Windows Netfabb only. |
| vs Quad remesh | Quad remesh builds a quad grid (staging); Remesh is triangle voxels. |

## Related

- [Quad remesh](Quad-remesh)  
- [Round all edges](Round-all-edges)  
- [Object menu](Object-menu)  
- [Home](Home)  
