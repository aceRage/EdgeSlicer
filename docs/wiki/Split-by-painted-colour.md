# Split by painted colour

## What it is

**Split ▸ By painted colour** turns a multi-material painted model into **separate parts**, one closed shell per painted filament region, placed back where they were.

Opens the **Split by painted colour** dialog for depth and island options.

## Where to find it

- Object menu → **Split** → **By painted colour**  
- Part menu → **Split** → **By painted colour**  
- Multi-part selection → **Split** → **By painted colour**  

## How to use it

1. Paint the model with the colour painting tool (MMU / multi-filament regions).
2. Right-click → **Split** → **By painted colour**.
3. In the dialog, set options as needed:
   - **Depth override** (mm) or **Unlimited depth**
   - **Cap flat tops/bottoms at solid shell depth**
   - **Absorb enclosed islands**
   - **Keep base-colour sparse infill**
   - **Solid colour interfaces** (`interface_shells`)
4. Confirm the split and check that each region is its own part in the object list.
5. Assign filaments / print as separate parts or re-assemble as needed.

Related: [Paint depth](Paint-depth) bounds how far a painted claim goes *before* you split.

## Limits / notes

| Situation | What happens |
|---|---|
| No paint | Nothing useful to split — paint first. |
| Stock Split | **To objects** / **To parts** remain; colour-split is Edge-only. |
| After split | Parts keep their place on the bed; treat them as independent volumes. |

## Related

- [Paint depth](Paint-depth)  
- [Object menu](Object-menu)  
- [Image Fill](Image-Fill)  
- [Home](Home)  
