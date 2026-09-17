# Visibility (Normal / Ghost / Hidden)

## What it is

**Visibility** controls how each **object** or **part** appears in the Prepare view:

- **Normal** — fully shown  
- **Ghost (X-ray)** — see-through; clicks pass through so you can work on what is behind  
- **Hidden** — not drawn in the 3D view (**still slices and prints**)  
- **Show all objects** — reset every object to Normal (object menu)

An **eye** column in the object list cycles Normal → Ghost → Hidden without opening the menu.

## Where to find it

- Object list → **eye** column  
- Object / part / multi-selection context menu → **Visibility** submenu  

Assemble-view menus do **not** include Visibility.

## How to use it

1. Select an object or part (or use the eye column on that row).
2. Choose **Normal**, **Ghost (X-ray)**, or **Hidden**.
3. Use **Show all objects** when many items were hidden and you want a full reset.
4. Ghost a shell while editing an insert; hide helpers you do not need on screen — remember Hidden parts still go to the printer unless you remove or disable them another way.

## Limits / notes

| Situation | What happens |
|---|---|
| Hidden | Cleared from the view only — **still included in slice/print**. |
| Ghost | Display aid; geometry remains. |
| Per object vs per part | Set on a whole object or on individual parts. |
| Cut tool | Cut halves can also be Visible / Ghost / Hidden while placing a curved cut — see [Flexi joints](Flexi-joints). |

## Related

- [Object menu](Object-menu)  
- [Assemble tool](Assemble-tool)  
- [Flexi joints](Flexi-joints)  
- [Home](Home)  
