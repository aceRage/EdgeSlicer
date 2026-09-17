# Fill bed with copies

## What it is

**Fill bed with copies** packs as many copies of the selection as will fit on the current plate, using a dialog for gaps, margins, rotation, and layout mode.

It fills around objects already on the plate. **Grid** lays an exact regular block; **Compact** packs more tightly (Edge’s compact pack no longer gives up early and leaves empty strips). Filling plate 2 or later puts copies on that plate.

## Where to find it

- Object menu → **Fill bed with copies**  
- Object menu → **Clone** (`Ctrl+K`) → dialog button **Fill** (tooltip “Fill bed with copies”)  

Entry also sits under Clone in the object menu family described in [Object menu](Object-menu).

## How to use it

1. Select the object (or instance) you want to duplicate across the bed.
2. Choose **Fill bed with copies**, or open **Clone** and click **Fill**.
3. In the dialog set:
   - **Minimum gap between copies**
   - **Allow rotation** (on/off)
   - **Layout**: Compact or Grid
   - **Minimum distance from bed edge**
   - Optional **Keep the front edge clear** / **Distance from the front edge** (calibration-line margin)
4. Confirm with **Fill**.
5. Check the live count / arrangement on the plate, then slice.

## Limits / notes

| Situation | What happens |
|---|---|
| Objects already on the plate | Fill packs around them. |
| Grid layout | Exact regular block; hardware validation of Grid is still listed as unchecked by the maintainer — verify the first print. |
| Compact layout | Improved vs early bail-out that left empty strips. |
| Plate 2+ | Copies go on the active / chosen later plate. |

## Related

- [Scale to build volume](Scale-to-build-volume)  
- [Object menu](Object-menu)  
- [Home](Home)  
