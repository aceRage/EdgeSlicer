# Scale to build volume

## What it is

**Scale to build volume** fits the selection to the printer’s build volume with a dialog for uniform or per-axis scaling, edge gap, top gap, and auto-centre.

Available on the object menu for every printer. Upstream had a label that never reached the BBL-style object menu; Edge wires it up with `ScaleToVolumeDialog`.

## Where to find it

- Object menu → **Scale to build volume**

## How to use it

1. Select the object to scale.
2. Object menu → **Scale to build volume**.
3. Choose **Scale mode**: Uniform or Non-uniform.
4. Set **Gap from bed edges** and **Gap below maximum height**.
5. Optionally **Centre on the bed**.
6. Confirm with **Scale**.

## Limits / notes

| Situation | What happens |
|---|---|
| Non-uniform scale of a **rotated** part | Warns before it would shear. |
| Already larger than volume | Scale down to fit with the chosen gaps. |

## Related

- [Fill bed with copies](Fill-bed-with-copies)  
- [Object menu](Object-menu)  
- [Preferences → Extras](Preferences-Extras) — drop-to-bed / bottom-referenced Z  
- [Home](Home)  
