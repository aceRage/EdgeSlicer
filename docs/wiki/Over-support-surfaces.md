# Over-support surfaces

## What it is

A bottom face resting on support is normally treated as a **bridge**, so it takes bridge flow and bridge speed and often looks different from the walls around it.

**Over-support surfaces** (off by default) gives those faces — and the wall segments printed over support — their own surface type, extrusion role, and preview colours (*Bottom surface over support*, *Wall over support*). They print at **Over-support flow** and **Over-support speed**, so the underside can match the perimeters framing it.

True bridges over air are unchanged. With the switch off, G-code stays byte-identical to the previous behaviour.

## Where to find it

- Process tab → **Support**
- **Over-support surfaces** (checkbox, off by default)
- **Over-support flow** and **Over-support speed** (when enabled)

All three keys are **per part**, so one part in an object can use them while another does not. Works with both the classic and Arachne wall generators.

## How to use it

1. Enable supports for the object as usual.
2. On Process → Support, turn on **Over-support surfaces**.
3. Set **Over-support flow** as needed for the underside look.
4. Set **Over-support speed**. Use **`0`** to follow the outer wall speed so the underside matches the perimeters around it.
5. Slice and check the preview colours for *Bottom surface over support* and *Wall over support*.
6. If only some parts should use it, set the keys per part (object list / part overrides).

## Limits / notes

Because supports are generated **after** slicing, “is there support under this face?” is a **reconstruction** of what the support generator will do. It can **over-claim**.

| Situation | What happens |
|---|---|
| Build-plate-only supports, small-overhang removal, sharp-tail heuristics | Not fully modelled — a fringe may print as bottom shell instead of a bridge. |
| Over-claim is wrong for you | Use a **support blocker** as the escape hatch. |
| Top Z distance is **0 mm** | Feature stands down entirely. |
| Supports off | Feature stands down. |
| Manual support types outside enforcers | Feature stands down. |
| Switch off | G-code is byte-identical to without the feature. |

Other notes:

- True bridges over open air are never reclassified.
- Per-part keys let you mix behaviour inside one object.

## Related

- [Support sets and groups](Support-sets-and-groups) — named support recipes and per-part interface groups  
- [Support Filament Matching](Support-Filament-Matching) — colour-matched supports and brims  
- [Home](Home) — EdgeSlicer wiki index  
