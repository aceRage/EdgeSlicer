# Support sets and groups

## What it is

**Support sets** are named presets of the Support-category process settings. You save a support recipe once and apply it to any project.

**Support groups** let different parts of the same object use different support-interface settings (filament, pattern, layers, ironing, and related values). Group values live on the parts themselves, so a project `.3mf` carries them. Stock OrcaSlicer ignores the unknown keys and still opens the file.

Together they replace one-size-fits-all support settings when a multi-part object needs different interfaces (for example a soluble interface on one part and a matching-colour interface on another).

## Where to find it

- **Support set** — Process tab → Support. The **Support set** row sits at the top of that page (*Apply*, pop-out editor, save, delete, and **Groups**).
- **Support group (per part)** — Object list → right-click part(s) → **Support group** submenu (put parts in a named group, empty or from a support set).
- **Support groups window** — Object menu → **Support groups…**, or the **Groups** button on the Support set row.

Grouped parts show a `[group]` badge in the object list.

## How to use it

### Save and apply a support set

1. Tune the Support-category settings the way you want them for this recipe.
2. On the Support page, save them as a named **Support set**.
3. Later, choose that set and **Apply**. The values are copied into the process preset; the preset then shows as modified, the same way a hand edit would.

A set stores its **interface filament as a type** (*Same as the part*, a loaded material, or a soluble type such as PVA / BVOH / HIPS / …), so the set can travel between printers instead of depending on a fixed filament slot.

### Put parts in a support group

1. Select one or more parts in the object list.
2. Right-click → **Support group** → put them in a named group:
   - **Empty** — seeded from the object’s current settings, or
   - **From a support set** — created straight from a set’s values.
3. Open **Support groups…** (or **Groups**) to rename, delete, re-apply a set, or select the parts in a group.

### What you can set per group

Per group you can control:

- Interface filament  
- Interface pattern  
- Top / bottom interface layers and spacing  
- Support ironing (pattern, flow, line spacing)

Support style, threshold angle, and top Z distance are **stored** per group but remain **object-wide in effect**. The support **base** geometry is never per group — only the interface (and its ironing) follow the group.

## Limits / notes

Warnings appear in the Support groups window and during slicing when something will not follow a group:

| Situation | What happens |
|---|---|
| Classic tree supports | Interface *layer count* is taken object-wide (organic trees and normal supports do not do this). |
| Group asks for a soluble interface | Forces **0 mm** top Z distance on the whole object. |
| Group has its own interface filament | Turns **Support Filament Matching** off for that object. |
| Interface filament on a different nozzle, or not loaded | Called out in the UI / slice warnings. |

Other limits:

- Support **base** geometry is always object-wide, never per group.
- Applying a support set is a value copy, not a live link: after Apply, editing the process settings does not rewrite the saved set until you save again.
- Projects with groups remain openable in upstream OrcaSlicer; group keys are simply ignored there.

## Related

- [Over-support surfaces](Over-support-surfaces) — bottoms and walls over support printed like walls, not bridges  
- [Support Filament Matching](Support-Filament-Matching) — colour-matched supports (disabled for an object when a group sets its own interface filament)  
- [Home](Home) — EdgeSlicer wiki index  
