# Repair/Remesh

## What it is

**Repair/Remesh** rebuilds a part (or object) as a **watertight** mesh using an OpenVDB **voxel** remesh (triangle output). Fine detail below about **0.1 mm** may be lost.

It is Edge’s cross-platform repair path. Stock Snapmaker/Orca on Windows also keeps **Fix model** (Netfabb); Remesh is shown on all platforms.

This is **not** “quad remesh” — there is no quad-remesh command on current `main`. SoftFever Orca’s Smooth Mesh subdivision is not in Edge.

## Where to find it

- Object menu → **Repair/Remesh**  
- Part menu → **Repair/Remesh**  
- Multi-selection menus (several objects or several parts) → **Repair/Remesh**  

Windows also shows **Fix model** (Netfabb) below Remesh when available.

## How to use it

1. Select the broken or non-manifold object/part(s).
2. Right-click → **Repair/Remesh**.
3. Wait for the remesh to finish.
4. Inspect the result; re-apply paint/seams if detail was lost.
5. Prefer **Simplify Model** (stock) when you only need decimation, not a full SDF rebuild.

## Limits / notes

| Situation | What happens |
|---|---|
| Fine detail | Features smaller than ~0.1 mm may disappear. |
| Repair/Remesh vs Simplify Model | Remesh = SDF rebuild, watertight. Simplify = stock decimation. |
| Repair/Remesh vs Fix model | Remesh always available. Fix model = Windows Netfabb only. |
| Quad remesh | Not implemented on current `main`. |

## Related

- [Object menu](Object-menu)  
- [Mesh boolean](https://www.orcaslicer.com/wiki/) (stock; Manifold backend in Edge)  
- [Home](Home)  
