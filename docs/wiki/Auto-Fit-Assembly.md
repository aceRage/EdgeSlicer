# Auto-Fit Assembly

## What it is

**Auto-Fit Assembly** mates a multi-selection into one print-ready object: the largest part stays fixed, the rest mate by matching faces, holes, and pegs, then everything merges.

For feature-pick Auto-fit inside the Assemble tool (Triangle/Curve/Point modes, Rotate / Offset, Merge parts), see [Assemble tool and Auto-fit](Assemble-tool).

## Where to find it

- Object list / Prepare view → select multiple parts → right-click → **Auto-Fit Assembly**

## How to use it

1. Select the parts that should become one object.
2. Right-click → **Auto-Fit Assembly**.
3. Check that the largest part stayed fixed and the others seated on faces / holes / pegs.
4. Slice or continue editing the merged object.

## Limits / notes

| Situation | What happens |
|---|---|
| Ambiguous geometry | Mating may need the Assemble tool’s feature-pick Auto-fit instead. |
| Paint / seams | Prefer **Merge into Single Part** when you need annotations preserved — see [Assemble tool](Assemble-tool). |

## Related

- [Assemble tool and Auto-fit](Assemble-tool)  
- [Home](Home)  
