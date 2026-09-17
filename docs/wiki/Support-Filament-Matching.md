# Support Filament Matching

## What it is

**Support Filament Matching** (opt-in) prints supports, interfaces, ironing, and brims in the colour of the surface they touch, instead of a single shared support filament.

A related setting, **Brim filament → Nearest wall**, does the same idea for brims alone: each brim uses the filament of the wall it sits against.

Use this when a multicolour print should keep support and brim colours consistent with nearby surfaces, so leftover interface colour is less visible after removal.

## Where to find it

- **Support Filament Matching** — Process settings for multimaterial / support filament options (opt-in; off by default until you enable it).
- **Brim filament → Nearest wall** — Brim / multimaterial filament-for-features controls (brims only).

Exact row labels sit with the other multimaterial filament-for-feature settings in the process tab. Screenshots: `docs/images/support-matching.png` and `docs/images/brim-match.png` in the repo.

## How to use it

1. Load the filaments you want on the project (including any colours that touch support or brim).
2. Turn on **Support Filament Matching**.
3. Slice and check the preview: supports, interfaces, ironing, and brims should follow the colour of the surface they touch.
4. For brims only (without full matching), set **Brim filament** to **Nearest wall**.

No special painting is required for matching — it follows contact with the model surface.

## Limits / notes

| Situation | What happens |
|---|---|
| A **support group** sets its own interface filament | Support Filament Matching is turned **off for that object**. See [Support sets and groups](Support-sets-and-groups). |
| Single-filament prints | Matching has nothing to switch between; leave it off. |
| Soluble / dedicated interface workflows | Prefer an explicit interface filament (or a support set / group) instead of colour matching. |

Other notes:

- Matching is opt-in so existing profiles keep their previous support/brim filament behaviour until you enable it.
- Preview colours in the slice view are the best quick check before you print.
- For per-part interface recipes that conflict with matching, use support groups and accept that matching is disabled on that object.

## Related

- [Support sets and groups](Support-sets-and-groups) — per-part interface settings; disables matching when a group has its own interface filament  
- [Outer wall filament](Outer-wall-filament) — separate filament for outer walls  
- [Home](Home) — EdgeSlicer wiki index  
