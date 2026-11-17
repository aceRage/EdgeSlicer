#ifndef slic3r_HeadClearance_hpp_
#define slic3r_HeadClearance_hpp_

// EdgeSlicer: print-head clearance model.
//
// A per-printer description of the space the moving print head occupies, relative to the nozzle
// tip: the nozzle itself as a radius-versus-height profile (land plus cone), and everything else
// (heater block / sock, part-cooling ducts, carriage, X gantry) as axis-aligned boxes. It is data,
// not a printer setting: one JSON file per head under resources/head_clearance/, matched to a
// printer preset by its printer_model. The file format is exactly what parse_head_clearance_json()
// accepts: schema, id, name, source, calibrated, printer_models[], nozzle { profile [[h, r]...],
// estimated }, boxes [{ name, min [x,y,z], max [x,y,z], spans_x, estimated }].
//
// The idea and the box-stack representation follow PancakeSlicer's head envelopes
// (engine/resources/nonplanar/heads, schema orca.head_envelope.v1, AGPL-3.0,
// https://github.com/ZimengXiong/PancakeSlicer). The Bambu X1 and H2D numbers are adapted from
// that data; the format and this loader are EdgeSlicer's own.
//
// Frame: origin at the nozzle tip, +X right, +Y toward the back of the printer, +Z up, mm.
//
// Self-contained on purpose (no Print / GCode dependencies), so other passes (by-object
// clearance, curved tops) can use the same data.

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "Point.hpp"

namespace Slic3r {

class PrintConfig;

struct HeadClearanceBox
{
    std::string name;
    Vec3d       min { Vec3d::Zero() };
    Vec3d       max { Vec3d::Zero() };
    // The box runs across the whole X range regardless of min.x / max.x (an X gantry beam).
    bool        spans_x { false };
    // The numbers are an estimate (photos, proportions), not a measurement or a published value.
    bool        estimated { true };
};

// Curved tops (CurvedTops.hpp) printed on real hardware with this head: the rise and the steepest
// surface slope that a print checked, and on which printer models. Optional "curved_tops" object of
// the head file; an older parser ignores it.
struct HeadClearanceCurvedTopsVerified
{
    std::vector<std::string> printer_models;
    double                   rise_mm { 0. };
    double                   angle_deg { 0. };
    std::string              on;   // date, as written in the file
    std::string              note;
};

struct HeadClearance
{
    static constexpr const char *SCHEMA = "edgeslicer.head_clearance.v1";

    std::string              id;
    std::string              name;
    std::string              source;
    std::vector<std::string> printer_models;
    // True only once somebody has checked the numbers on a real printer.
    bool                     calibrated { false };

    // Nozzle outline: (height above the tip, outer radius) pairs, heights strictly increasing from
    // 0, radii non-decreasing. Between points the outline is a straight cone. Nothing of the nozzle
    // exists outside the last radius: the boxes describe what is above / beside it.
    std::vector<std::pair<double, double>> nozzle_profile;
    bool                                   nozzle_estimated { true };

    std::vector<HeadClearanceBox> boxes;

    // Curved tops printed with this head (empty when nobody has).
    std::optional<HeadClearanceCurvedTopsVerified> curved_tops_verified;

    // Height above the tip of the nozzle's underside at horizontal distance r from the nozzle axis.
    // 0 inside the tip land; +infinity beyond the profile's largest radius.
    double nozzle_underside_at(double r) const;
    double nozzle_max_radius() const { return nozzle_profile.empty() ? 0. : nozzle_profile.back().second; }
    // Lowest box bottom (+infinity without boxes).
    double lowest_box_z() const;
    // The steepest surface slope (degrees) the nozzle's flank clears when the tip rests on it: the
    // smallest angle, seen from the edge of the tip land, of the profile points above it. Points no
    // higher than land_tolerance above the tip count as part of the land (a chamfer that small only
    // touches the neighbouring bead, as the flat land itself does). 90 when nothing is above the land.
    double nozzle_flank_angle_deg(double land_tolerance) const;
    bool   matches_printer_model(const std::string &printer_model) const;
    // Curved tops have been printed with this head on this printer model.
    bool   curved_tops_verified_for(const std::string &printer_model) const;
};

// Parses one head file. Returns nullopt and fills *error on a malformed or inconsistent file.
std::optional<HeadClearance> parse_head_clearance_json(const std::string &json_text, std::string *error = nullptr);

// Loads every *.json in dir. Files that fail to parse are skipped and reported in *errors.
std::vector<HeadClearance> load_head_clearances(const std::string &dir, std::vector<std::string> *errors = nullptr);

using HeadClearancePtr  = std::shared_ptr<const HeadClearance>;
using HeadClearancePtrs = std::vector<HeadClearancePtr>;

// Every head shipped in resources_dir()/head_clearance (loaded once per resources dir, then cached),
// in file name order.
HeadClearancePtrs shipped_head_clearances();

// The ONE place that decides which head model a print uses (design: tests/design_curved_tops_any_printer.md,
// section 2.1). The curved-tops planner, the Z contouring collision warning and the curved-tops
// verifier all go through it.
//
// Sources, in order of precedence:
//   1. user values in the printer preset - not yet: the keys come with the "any printer" phase (P2);
//   2. a head file whose printer_models lists the printer_model; when two files list it, the first in
//      file name order (load order) wins;
//   3. nothing: nullptr. There is no generic default; a consumer that needs a head refuses.
HeadClearancePtr resolve_head_clearance(const std::string &printer_model, const HeadClearancePtrs &heads);
// Same, against shipped_head_clearances().
HeadClearancePtr resolve_head_clearance(const std::string &printer_model);
// Same, for a print's config (its printer_model).
HeadClearancePtr resolve_head_clearance(const PrintConfig &config);

// Kept for older callers: resolve_head_clearance(printer_model).
std::shared_ptr<const HeadClearance> head_clearance_for_printer_model(const std::string &printer_model);

} // namespace Slic3r

#endif // slic3r_HeadClearance_hpp_
