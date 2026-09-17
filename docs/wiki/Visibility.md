# Visibility (Normal / Ghost / Hidden)

## What it is

**Visibility** controls how each **object** or **part** appears in the Prepare view:

- **Normal** — fully shown  
- **Ghost** — X-ray / see-through, so you can work around or inside assemblies without losing context  
- **Hidden** — not drawn (still in the project)

An **eye** column in the object list shows and changes visibility without hunting through menus.

Use Ghost when aligning or inspecting nested / overlapping parts; use Hidden to clear clutter while you edit something else.

## Where to find it

- Object list → **eye** column (per object or part)
- Object / part context menus that set visibility to Normal, Ghost, or Hidden (Prepare view)

Visibility is a Prepare-view / modeling aid. It does not by itself change which geometry is sliced unless you leave parts hidden in a way your workflow treats as excluded — treat Hidden as “out of the way for editing,” and confirm selection / slice scope before you print.

## How to use it

1. Open the object list so objects and parts are visible.
2. Click the **eye** (or use the visibility menu) on an object or part.
3. Choose:
   - **Normal** — restore full display  
   - **Ghost** — X-ray so neighbours stay visible underneath  
   - **Hidden** — remove it from the view temporarily  
4. Combine with multi-part assemblies: Ghost the shell while you edit an insert; Hidden for plates or helpers you do not need on screen.
5. Switch back to **Normal** before final layout checks if you want the true on-bed appearance.

Related Prepare helpers that often go with visibility:

- Move panel **align & distribute**
- **Bottom-referenced Z** / **keep imported Z** (drop-to-bed toggle)
- Double-click to select a part

See also [Preferences → Extras](Preferences-Extras) for drop-to-bed and bottom-referenced Z toggles.

## Limits / notes

| Situation | What happens |
|---|---|
| Ghost | Display only (X-ray) — geometry is still there for editing and, when selected appropriately, for slicing. |
| Hidden | Cleared from the view; confirm you still intend that part in the slice / export. |
| Per object vs per part | You can set visibility on a whole object or on individual parts inside it. |
| Cut tool halves | The Cut tool can also set halves to Visible / Ghost / Hidden while placing a curved cut — see [Flexi joints](Flexi-joints). |

## Related

- [Assemble tool and Auto-fit](Assemble-tool)  
- [Flexi joints and Cut tool](Flexi-joints)  
- [Preferences → Extras](Preferences-Extras)  
- [Home](Home)  
