# Object and part context menus (Edge)

## What it is

Prepare-view **right-click** menus on the object list and 3D canvas share the same Edge-aware builders. This page is the map of **Edge-only or heavily extended** items. Stock Orca / Snapmaker items (Simplify, Mesh boolean, Center, Drop, Mirror, Delete, Add part, …) stay on those menus but are documented upstream.

Menus are built in `GUI_Factories` (`MenuFactory`). Object-list and canvas right-click both use them.

## Where to find it

- Object list → right-click an **object** row or a **part** row  
- 3D Prepare view → right-click the selection  
- Multi-select objects or parts for the multi-selection menus  

Related (not a menu item): the object-list **eye** column cycles Normal → Ghost → Hidden. See [Visibility](Visibility).

## How to use it (Edge highlights)

### Object menu (one object)

| Item | What it does | Page |
|---|---|---|
| **Clone** (`Ctrl+K`) | Opens Clone dialog (copy count, auto-arrange). **Fill** runs fill-bed. | [Fill bed with copies](Fill-bed-with-copies) |
| **Fill bed with copies** | Pack copies onto the remaining bed (compact or grid). | [Fill bed with copies](Fill-bed-with-copies) |
| **Visibility** | Normal / Ghost / Hidden / Show all objects. | [Visibility](Visibility) |
| **Support groups...** | Opens the Support groups window for this object. | [Support sets and groups](Support-sets-and-groups) |
| **Repair/Remesh** | OpenVDB voxel remesh → watertight mesh (all platforms). | [Repair/Remesh](Repair-Remesh) |
| **Fix model** | Stock Netfabb repair (Windows 10+ only). | — |
| **Apply image fill...** | Project an image onto the object with allowed filaments. | [Image Fill](Image-Fill) |
| **Scale to build volume** | Fit to bed with gaps / centre options. | [Scale to build volume](Scale-to-build-volume) |
| **Split ▸ By painted colour** | MMU paint → solid parts per filament. | [Split by painted colour](Split-by-painted-colour) |

### Part menu (one part / volume)

Live builder is the BBL part menu (not the unused alternate part menu).

| Item | What it does | Page |
|---|---|---|
| **Repair/Remesh** | Same voxel remesh. | [Repair/Remesh](Repair-Remesh) |
| **Visibility** | Normal / Ghost / Hidden for that part. | [Visibility](Visibility) |
| **Separate** | Extract this part into a new object; keep world pose. | [Assemble tool](Assemble-tool) |
| **Split ▸ By painted colour** | Same colour-split. | [Split by painted colour](Split-by-painted-colour) |
| **Support group** | Assign part(s) to a named group (None, existing, New, from set, Support groups…). | [Support sets and groups](Support-sets-and-groups) |

**Apply image fill...** is on the **object** menu only in the live UI. A part inside a multi-part object has no Image Fill item.

### Multi-selection

**Several objects**

| Item | What it does | Page |
|---|---|---|
| **Assemble** | Merge into one multi-part object (stock). | — |
| **Auto-Fit Assembly** | Largest fixed; others mate by faces/holes, then merge. | [Auto-Fit Assembly](Auto-Fit-Assembly) |
| **Repair/Remesh** | Same remesh. | [Repair/Remesh](Repair-Remesh) |
| **Visibility** | Same view modes. | [Visibility](Visibility) |

**Several parts of one object**

| Item | What it does | Page |
|---|---|---|
| **Assemble Separately** | Selected parts → new assembly; keep world pose. | [Assemble tool](Assemble-tool) |
| **Merge into Single Part** | Boolean-merge selected parts (keeps paint/seams intent of the Orca port). | [Assemble tool](Assemble-tool) |
| **Visibility** | Same. | [Visibility](Visibility) |
| **Support group** | Same assignment submenu. | [Support sets and groups](Support-sets-and-groups) |
| **Repair/Remesh** | Same. | [Repair/Remesh](Repair-Remesh) |
| **Split ▸ By painted colour** | Same. | [Split by painted colour](Split-by-painted-colour) |

Assemble-view menus stay stock-like — they do **not** get Visibility, Support group, Auto-Fit, or colour-split.

## Limits / notes

| Situation | What happens |
|---|---|
| **Bake slice to mesh / Quad remesh / Round all edges / Edit cut** | On **`feat/ultra-preferences` (staging)** — see dedicated pages. Not on `main` yet. SVG/text gizmo **Bake** is still a different feature. |
| Hidden objects/parts | Hidden in the 3D view but **still slice/print**. |
| Clone | There is no Clone *submenu* — one **Clone** item opens a dialog; **Fill** inside that dialog is fill-bed. |

## Related

- [Fill bed with copies](Fill-bed-with-copies)  
- [Scale to build volume](Scale-to-build-volume)  
- [Repair/Remesh](Repair-Remesh)  
- [Split by painted colour](Split-by-painted-colour)  
- [Visibility](Visibility)  
- [Home](Home)  
