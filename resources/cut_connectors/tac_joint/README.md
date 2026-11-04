# TAC Joint reference geometry (version 1)

The two binary STLs are the user's unmodified `connector.stl` and
`connector cutter.stl`, supplied on 2026-10-30. Coordinates are millimetres.
`reference.json` records their SHA-256 hashes, triangle counts, measured
dimensions, and their common source-to-connector rigid transform.

Run `python scripts/generate_tac_joint.py` from the repository to reproduce
`src/libslic3r/TacJointData.inc` and `reference.json`. Run the same command with
`--check` to verify reproducibility without writing. Only the Python standard
library is needed. The application uses the embedded arrays, so no desktop
files or runtime mesh import are required.

The frame is derived from the connector's large planar faces and centered on
the connector. Both sources use exactly the same transform. Local Z follows
the original 11 mm longitudinal axis; local X is approximately source +X and
local Y approximately source -Z. Do not independently center the cutter.

Default parameters preserve the source shapes. Cutter dimension adjustment
uses `B_new = scale * (B - 2 * nominal_clearance) + 2 * clearance`; the nominal
side/end clearances are 0.12/0.30 mm. The cutter's reference center follows the
connector's uniform scale; its deviations from that center use the adjusted
dimension factors. Offset translates both meshes along local Z.

This dimensions-based adjustment does not guarantee a constant normal
clearance around rounded profiles. The geometry validator checks actual
containment, not only bounds. Zero side clearance is invalid for this pair;
at scale 1 even 0.001 mm side clearance produces interference despite larger
cutter bounds. New reference shapes must receive a new asset version and a
corresponding compatibility implementation.
