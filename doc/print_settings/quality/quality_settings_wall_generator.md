# Wall Generator

The Wall Generator defines how the outer and inner walls (perimeters) of the model are printed, and how parts
thinner than a wall are printed.

- [Classic](#classic)
- [Arachne](#arachne)
- [Thin features](#thin-features)
  - [Fine detail](#fine-detail)
  - [Thinnest line for thin features](#thinnest-line-for-thin-features)
  - [Skip thin features thinner than](#skip-thin-features-thinner-than)
  - [Fine detail speed](#fine-detail-speed)
  - [Skip features thinner than](#skip-features-thinner-than)
  - [Thinnest line](#thinnest-line)
    - [Thinnest line, first layer](#thinnest-line-first-layer)
  - [Drop centre lines shorter than](#drop-centre-lines-shorter-than)
- [Wall count changes](#wall-count-changes)
  - [Minimum odd and even wall line width](#minimum-odd-and-even-wall-line-width)
  - [No centre line in corners sharper than](#no-centre-line-in-corners-sharper-than)
  - [Allowed width drift before changing wall count](#allowed-width-drift-before-changing-wall-count)
  - [Wall count change length](#wall-count-change-length)
  - [Walls that absorb width changes](#walls-that-absorb-width-changes)
- [Old names](#old-names)

All percentages on this page are of the nozzle diameter (the smallest one on the printer). The settings page shows
what they are in millimetres for the selected printer, under the Thin features group.

## Classic

The Classic wall generator is a simple and reliable method used in many slicers. It creates as many walls as possible (limited by [Wall Loops](strength_settings_walls#wall-loops)) by extruding along the model's perimeter using the defined [Line Width](quality_settings_line_width).
This method does not vary extrusion width and is ideal for fast, predictable slicing.

A part thinner than two lines is not printed with Classic unless [Detect thin wall](strength_settings_walls#detect-thin-wall)
or [Fine detail](#fine-detail) is on. With either, such a part is printed as one open line of its own width, down to
a third of the nozzle.

![wallgenerator-classic](https://github.com/SoftFever/OrcaSlicer/blob/main/doc/images/WallGenerator/wallgenerator-classic.png?raw=true)

## Arachne

The Arachne wall generator dynamically adjusts extrusion width to follow the shape of the model more closely. This allows better handling of thin features and smooth transitions between wall counts.

![wallgenerator-arachne](https://github.com/SoftFever/OrcaSlicer/blob/main/doc/images/WallGenerator/wallgenerator-arachne.png?raw=true)

> [!NOTE]
> [A Framework for Adaptive Width Control of Dense Contour-Parallel Toolpaths in Fused Deposition Modeling](https://www.sciencedirect.com/science/article/pii/S0010448520301007?via%3Dihub)

### What a thin part becomes

A wedge seen from above, 0.1 mm wide at one end and 1.3 mm at the other, shows every setting of this page at once.
With the defaults on a 0.4 mm nozzle at 0.2 mm layers (X1C "0.20mm Standard", Precise wall on):

| Part width | Printed as |
|---|---|
| under 0.19 mm | nothing ([Skip features thinner than](#skip-features-thinner-than)) |
| 0.19 - 0.42 mm | one 0.38 mm line: widened to the [Thinnest line](#thinnest-line), so thin text prints bolder than drawn |
| 0.42 - 0.70 mm | one line as wide as the part |
| 0.70 - 1.16 mm | two lines sharing the part (the split is set by the [odd minimum](#minimum-odd-and-even-wall-line-width)) |
| from 1.16 mm | two outer lines and a centre line, then more walls |

Precise wall takes about 0.09 mm off every part before these limits are compared, so they read about 0.09 mm wider
here than the settings say.

## Thin features

### Fine detail

Off / Balanced / Fine text / Custom. Lets parts narrower than a wall, such as thin text strokes, print with a thinner
line than the [Thinnest line](#thinnest-line) **without changing how the other walls are printed**: walls from about
0.45 mm up are printed exactly as with Off.

| Level | Thinnest line for thin parts | Skips parts under | On a 0.4 nozzle, 0.2 mm layers |
|---|---|---|---|
| Off | Thinnest line (85 %) | Skip features thinner than (25 %) | 0.38 mm line, parts from 0.19 mm |
| Balanced | 60 % | Skip features thinner than (25 %) | 0.28 mm line, parts from 0.19 mm |
| Fine text | 45 % | 15 % | 0.22 mm line, parts from 0.15 mm |
| Custom | the two values below | | |

- A line is never made narrower than the layer is tall. At 0.28 mm layers Fine text gives 0.28 mm lines; lower the
  layer height for the finest text.
- The first layer keeps [Thinnest line, first layer](#thinnest-line-first-layer).
- With the **Classic** generator any level other than Off turns on thin-wall detection, so the option also works on
  presets that ship with Classic (Bambu Lab, Snapmaker U1, Elegoo CC / CC2).
- It is a per-part setting: add it to an object, or to a modifier box around the text only, to leave the rest of
  the object as it is.
- Thin lines print no faster than [Fine detail speed](#fine-detail-speed).
- Off is what every existing preset and project uses; nothing changes until you choose a level.

### Thinnest line for thin features

Used while Fine detail is on (shown with Arachne). The narrowest line a part thinner than a wall is printed with; a
part between this and the [Thinnest line](#thinnest-line) is printed at its own width. It does not move the point
where a wall splits into two lines. Values above the Thinnest line act as the Thinnest line.

Choosing Balanced or Fine text writes that level's value here. Changing it sets Fine detail to Custom.

### Skip thin features thinner than

Used while Fine detail is on. Parts thinner than this are not printed. 0 means the same as
[Skip features thinner than](#skip-features-thinner-than); a larger value acts as that. Like the line above, a named
level writes it and editing it makes the level Custom.

### Fine detail speed

Used while Fine detail is on. Wall lines narrower than the [Thinnest line](#thinnest-line) print no faster than this
(default 50 mm/s). A line narrower than the nozzle needs a slower speed to come out clean. 0 leaves their speed alone.

### Skip features thinner than

Arachne only. Parts of the model thinner than this are not printed at all. Anything thicker gets at least one line.
Lower it to keep hair-thin details; they are printed wider than they are (see Thinnest line).

### Thinnest line

Arachne only. The narrowest line the slicer prints. A detail thinner than this is printed this wide, so very fine text
comes out bolder than drawn.

Unless the [odd / even minimums](#minimum-odd-and-even-wall-line-width) are set, it also decides when a thin wall is
printed as one wide line or as two narrow ones: at 85 % a wall up to about 1.6 line widths is one line, at 45 % it
becomes two from about 1.0. That is why lowering it for text also changes every 0.47 - 1.0 mm wall of the part; use
[Fine detail](#fine-detail) for thin text without that.

#### Thinnest line, first layer

The same for the first layer only, where it does all of the jobs above (Fine detail and the odd / even minimums do
not apply on the first layer). Wider lines stick to the bed better; the nozzle diameter is a good value.

### Drop centre lines shorter than

Arachne only. Open centre lines shorter than this many times their own width are not printed, which saves short
starts and stops. Higher values remove more and longer lines.

> [!NOTE]
> Top and bottom surfaces always use 0.5, to avoid visual gaps.
> Use the One Wall Threshold (in Advanced settings) to adjust how aggressively a region is considered a top surface. This option only appears when this setting exceeds 0.5, or if single-wall top surfaces are enabled.

## Wall count changes

Arachne only.

### Minimum odd and even wall line width

Cura's two minimums, which here used to be folded into the Thinnest line. 0 (the default) means "the same as the
Thinnest line", which is how walls were always printed.

- **Minimum odd wall line width:** when a wall printed with one line (or an odd number of lines) becomes two: once
  two lines would each be at least this wide. Lower splits thinner walls into two narrow lines.
- **Minimum even wall line width:** when a centre line is added between two (or an even number of) wall lines: once
  the gap between them is at least this wide. Lower adds thin centre lines sooner.

### No centre line in corners sharper than

In a wedge or corner that opens faster than this angle no extra centre line is added. Lower: fewer short centre
lines, but small unfilled tips in sharp corners. Higher: sharper corners are filled, with more short lines.

### Allowed width drift before changing wall count

How far a line may be squeezed or stretched to avoid adding a wall and dropping it again a moment later. Higher:
fewer starts and stops and less travel, but a less even line width, which can under- or over-extrude.

### Wall count change length

Where a part gets thinner and loses a wall, the change is spread over this length. Longer is smoother but leaves the
lines off their ideal width for longer.

### Walls that absorb width changes

When a wall does not fit a whole number of lines, this many lines, counted from the middle, are made wider or
narrower. 1 keeps all the outer lines at their set width.

## Old names

These settings kept their keys (profiles and projects are unchanged); only the names in the settings changed. The
search box still finds them by the old name.

| Old name | New name | Key |
|---|---|---|
| Minimum feature size | Skip features thinner than | `min_feature_size` |
| Minimum wall width | Thinnest line | `min_bead_width` |
| First layer minimum wall width | Thinnest line, first layer | `initial_layer_min_bead_width` |
| Minimum wall length | Drop centre lines shorter than | `min_length_factor` |
| Wall transitioning threshold angle | No centre line in corners sharper than | `wall_transition_angle` |
| Wall transitioning filter margin | Allowed width drift before changing wall count | `wall_transition_filter_deviation` |
| Wall transition length | Wall count change length | `wall_transition_length` |
| Wall distribution count | Walls that absorb width changes | `wall_distribution_count` |

<!-- Screenshots to add (needs the app, not made by the agent): thin_fins_wedge.stl in the preview's Line width view
     with Fine detail Off / Balanced / Fine text, and with Thinnest line 85 % / 45 %. -->
