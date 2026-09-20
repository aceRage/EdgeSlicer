# Compare Slices

## What it is

**Compare Slices** diffs two slices (or sliced files) so you can see what changed between them: **settings**, **time** and **filament** estimates, plus a **per-layer toolpath** comparison.

Use it when you tweak a preset or project and want a clear before/after without guessing from memory.

## Where to find it

- **View** menu → **Compare Slices**
- Screenshot in the repo: `docs/images/compare-slices.png` (time and filament deltas, changed settings, per-layer toolpath overlay)

## How to use it

1. Produce the first slice (or keep a sliced file you already have).
2. Change settings or the model as needed and slice again — or pick a second sliced file.
3. Open **View → Compare Slices**.
4. Choose the two slices / sliced files to compare.
5. Review:
   - which **settings** differ
   - **time** and **filament** deltas
   - the **per-layer toolpath** overlay for path-level differences
6. Use that to decide whether the change is worth keeping, or to document a regression.

## Limits / notes

| Situation | What happens |
|---|---|
| You only have one slice | You need a second slice or sliced file to compare against. |
| Large projects | Per-layer overlays can be dense — focus on layers or regions that matter for the change. |
| Upstream Orca | This is an EdgeSlicer workflow feature; stock Orca does not ship the same Compare Slices window. |

Other notes:

- Comparing a slice to a saved sliced file is useful for “what did this release / preset change?” checks.
- Pair with [Preferences → Extras](Preferences-Extras) habits (for example Auto-Save) when you want an easy trail of project states to re-open and re-slice.

## Related

- [Preferences → Extras](Preferences-Extras) — Edge-only preferences (project, presets, archive, …)  
- [Home](Home) — EdgeSlicer wiki index  
