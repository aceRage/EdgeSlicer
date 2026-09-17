# Flexi joints and Cut tool

## What it is

EdgeSlicer extends the **Cut tool** so you can join the halves of a cut part with a **print-in-place Flexi joint** instead of a plug: double ring, ball and socket, chain link, hinge (chosen knuckle count, halves can fold shut), screw-top **thread**, or quarter-turn **bayonet** lock.

The cut plane can also bend into a **curved cut**, and flat or curved cuts can remove a **cut thickness** (kerf) so reassembled halves leave room for glue or a joint.

Joints survive saving and reopening a project. The slicer warns when a gap-closing setting would fuse them.

## Where to find it

- Prepare view → **Cut tool** (toolbar / object manipulation)
- In the Cut tool UI: Flexi joint type and clearance, curved-cut controls, cut thickness / kerf options
- Assembly-related screenshots: `docs/images/assembly.png` in the repo (assemble modes; Flexi/cut behaviour is in the Cut tool itself)

## How to use it

### Add a Flexi joint on a cut

1. Select the part and open the **Cut tool**.
2. Place the cut (flat or curved — see below).
3. Choose a **Flexi joint** instead of a plug: double ring, ball and socket, chain link, hinge, thread, or bayonet.
4. Set **clearance** and orient the joint to suit the part.
5. For a **hinge**, set the number of knuckles; the halves can fold shut after printing.
6. Apply the cut. Keep both halves; connectors (including Flexi joints) sit on the cut surface.
7. Save the project if you need the joint to persist across sessions.

### Curved cut

1. In the Cut tool, bend the cut plane into a **curved sheet**.
2. Drag control points; right-click-drag to snap them to the model.
3. Set density as needed.
4. Set each half to **Visible**, **Ghost**, or **Hidden** so you can read the result from both sides.
5. Both halves are always kept. Connectors and Flexi joints sit on the curved surface.

### Cut thickness (kerf)

1. For a flat or curved cut, set a non-zero **cut thickness** to remove a slab of material.
2. Centre it on, above, or below the cut as needed (glue gap or joint clearance).
3. Connectors grow to span the gap.
4. Leave thickness at **zero** (default) for a normal zero-kerf cut.

## Limits / notes

| Situation | What happens |
|---|---|
| Gap-closing settings that would fuse the joint | Slicer **warns** — check clearance / related settings before printing. |
| Project reopen | Flexi joints are preserved with the project. |
| Hardware validation | Curved cut and Flexi joints (including thread and bayonet) are listed as **not yet printed on hardware by the maintainer** — check the first print carefully. |
| Zero cut thickness | Behaviour matches a normal cut (no kerf). |

Other notes:

- Prefer Flexi joints when you want print-in-place articulation or a lock without a separate plug.
- Use cut thickness when glue or joint clearance needs a gap between halves.

## Related

- [Assemble tool and Auto-fit](Assemble-tool) — mate features and merge parts  
- [Auto-Fit Assembly](Auto-Fit-Assembly) — multi-selection face/hole/peg mating  
- [Home](Home) — EdgeSlicer wiki index  
