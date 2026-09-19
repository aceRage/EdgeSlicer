# Edit cut and Copy cut

> Staging: available on `feat/ultra-preferences`. Not on `main` yet.

## What it is

When an object still has a **stored cut recipe** from the Cut tool:

- **Edit cut...** — reopen that cut in the Cut gizmo so you can change it  
- **Copy cut to...** — submenu of other objects; set the same cut up on another object, ready to cut it the same way  

These sit near **Invalidate cut info** on the object menu.

## Where to find it

- Object menu → **Edit cut...** (only if `has_selected_editable_cut()`)  
- Object menu → **Copy cut to...** → pick another object (only if the cut is copyable)  

## How to use it

### Edit cut

1. Select an object produced (or still holding) an editable cut.
2. Right-click → **Edit cut...**
3. Adjust the cut in the Cut gizmo (plane, Flexi joints, thickness, etc.).
4. Apply when done.

Tooltip: *Edit the cut that produced this object.*

### Copy cut to another object

1. Select the object that has the cut recipe.
2. Right-click → **Copy cut to...** → choose the target object.
3. Cut the target with the same setup.

Tooltip: *Set this cut up on another object, ready to cut it the same way.*

## Limits / notes

| Situation | What happens |
|---|---|
| No stored editable cut | Edit cut is not shown. |
| Invalidate cut info | Clears cut metadata (already on `main`); after that, Edit/Copy may no longer apply. |
| Flexi joints / curved cut | See [Flexi joints](Flexi-joints). |

## Related

- [Flexi joints and Cut tool](Flexi-joints)  
- [Object menu](Object-menu)  
- [Home](Home)  
