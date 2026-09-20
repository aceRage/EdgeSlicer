# Assemble tool and Auto-fit

## What it is

EdgeSlicer adds stronger **assembly** and **auto-fit** tools so multi-part models can be mated and merged for printing without juggling them by hand.

Two related entry points:

- **Auto-Fit Assembly** (object / selection menu) — right-click a multi-selection: the largest part stays fixed; the rest mate to it by matching faces, holes, and pegs; everything merges into one print-ready object.
- **Assemble tool → Auto-fit** — pick one feature on each part, press Auto-fit, nudge with live **Rotate / Offset** sliders, then **Merge parts**.

New assembly modes include **Triangle and triangle** and **Curve and curve**. **Point and point** snaps to vertices at any zoom and has one-click **Coincide points**.

## Where to find it

- Prepare view → **Assemble** tool (toolbar / assembly tools)
- Right-click a **multi-selection** → **Auto-Fit Assembly** (or equivalent assembly command)
- Screenshot: `docs/images/assembly.png` (assembly modes, curve-and-curve mate, Auto-fit, Rotate / Offset, Merge parts)

## How to use it

### Auto-Fit Assembly (selection)

1. Select multiple parts that should become one object.
2. Right-click → **Auto-Fit Assembly**.
3. Review the result: largest part fixed; others mated by faces / holes / pegs and merged.

### Assemble tool → Auto-fit

1. Open the **Assemble** tool.
2. Choose an assembly mode (including **Triangle and triangle**, **Curve and curve**, or **Point and point**).
3. Pick one feature on each part (flat face, circular rim, a single triangle, or a curved patch). For Point and point, use **Coincide points** when you want a one-click snap.
4. Press **Auto-fit**.
5. Nudge with the live **Rotate / Offset** sliders.
6. Press **Merge parts** when the mate looks right.

Rotation is chosen by outline fit **and** a whole-mesh collision check, so pegs seat square without intersecting.

### Related assembly helpers

- **Assemble Separately / Separate** — pull parts out of an assembly in place  
- **Merge into Single Part** — merge while keeping paint and seam annotations  

## Limits / notes

| Situation | What happens |
|---|---|
| Poor feature picks | Auto-fit may not find a clean mate — try another mode or feature. |
| Collision check | Favours mates that do not intersect; adjust Rotate / Offset if needed. |
| Upstream face-and-face snapping | Edge also carries fixes so face-and-face picking uses the facet under the cursor again. |

For cut-plane joints instead of assembly mates, see [Flexi joints and Cut tool](Flexi-joints).

## Related

- [Auto-Fit Assembly](Auto-Fit-Assembly) — same selection-based flow called out on the Home index  
- [Flexi joints and Cut tool](Flexi-joints)  
- [Home](Home)  
