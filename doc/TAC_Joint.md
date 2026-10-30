# TAC Joint

TAC Joint adds a built-in cut connector based on a matching pair of contributed
STL meshes. The original files live in `resources/cut_connectors/tac_joint/`;
the application embeds the meshes and needs no external files at runtime.

## Usage

1. Select an object and open the Cut tool (`C`).
2. Choose a flat cut and open Add connectors / Edit connectors.
3. Select **TAC Joint** and place it on the cut face.
4. Adjust the parameters, confirm the connectors, and perform the cut.

The cut produces a matching socket in each half and a separate printable
connector. The blue preview represents the socket. The connector is laid flat
beside the parts for printing while retaining its assembled pose.

| Parameter | Default | Meaning |
|---|---:|---|
| Size | 100% | Original connector: approximately 4.09 × 3.34 × 11 mm |
| Side clearance | 0.12 mm | Nominal clearance per side |
| End clearance | 0.30 mm | Clearance at each end |
| Cut offset | 0 mm | Midpoint cut; 5.5 mm engagement per half without kerf |
| Rotation | 0° | Rotation in the cut plane, adjustable through 360° |

**Reset TAC Joint** restores these defaults. When editing multiple connectors,
only the edited field changes; reset replaces all parameters.

Default values preserve the original STL geometry. Side clearance is a nominal
dimension adjustment, not a constant surface-normal offset around rounded
profiles. Scaling preserves the configured clearances in millimetres.
Geometric validation rejects interference and insufficient engagement.

## Supported cuts and persistence

Flat cuts support kerf, which reduces engagement without lengthening the pin.
Both halves may be kept as parts of one object. Parameters and rotation survive
project save/load, cut recipes, copying, and undo.

Curved/drawn cuts, groove cuts, mixing with Flexi joints, individual part
assignment, and mirroring are explicitly blocked for TAC Joint. The body
boundary check samples the socket mesh, so narrow internal voids between
samples can be missed. Connector/socket fit uses separate geometric validation.

## Development and validation

Geometry and validation are in `src/libslic3r/TacJoint.cpp`; cut integration is
in `CutUtils.cpp`, and UI controls are in `GLGizmoCut.cpp`.

Run `python scripts/generate_tac_joint.py --check` to verify reproducible
embedded mesh data. The generator uses only the Python standard library.
Follow the platform build instructions in the repository README. With
`BUILD_TESTS` enabled, build the `libslic3r_tests` target and run it with the
Catch2 filter `[TACJoint]`. Related regression filters are `[PushFit]`,
`[CutRecipe]`, `[CutMirror]`, and `[DrawCut]`.

Physical printing and network printing are not covered by these software tests.
See also the [German usage guide](TAC_Joint.de.md).
