#ifndef slic3r_HeightPatternInput_hpp_
#define slic3r_HeightPatternInput_hpp_

// ---------------------------------------------------------------------------
// Height colour pattern, phase 2: the dialog's model (tests/design_height_colour_pattern.md).
//
// Everything the "Height colour pattern" dialog and the object list need that is not drawing:
// turning what the user typed (mm or layers) into a HeightColourPattern, validating it, the live
// "snapped result" text, the overlap check against existing ranges, and the wording of the
// object-list row. The GUI code stays thin and this file is unit-tested.
// ---------------------------------------------------------------------------

#include "HeightColourPattern.hpp"
#include "Slicing.hpp"

#include <string>
#include <utility>
#include <vector>

namespace Slic3r {

class ModelObject;

enum class HeightPatternUnit : int {
    Mm     = 0,
    Layers = 1,
};

// What the dialog fields hold. start/band/gap are in `unit`.
struct HeightPatternInput
{
    HeightPatternUnit unit           = HeightPatternUnit::Mm;
    double            start          = 2.;
    double            band           = 10.;
    double            gap            = 10.;
    int               repeat         = 3;
    bool              to_top         = false;
    std::vector<int>  sequence;                 // 1-based filament ids
    int               gap_mode       = int(HeightPatternGapMode::ObjectFilament);
    int               gap_filament   = 0;
    bool              snap_to_layers = true;
};

// Empty string = usable. Otherwise a sentence for the dialog. `max_filament` as in
// expand_height_colour_pattern (0 = do not check the upper bound).
std::string validate_height_pattern_input(const HeightPatternInput &in, int max_filament);

// The record the input describes. Layers are turned into millimetres here: start N means "N layers
// below the first band" (read off the real layer grid when there is one), band and gap are N *
// layer_height. The result is what expand_height_colour_pattern() takes; `generated` stays empty.
HeightColourPattern height_pattern_from_input(const HeightPatternInput &in, double layer_height,
                                              const std::vector<double> &layer_tops);

// The reverse, for Edit: always in millimetres, since the record stores millimetres.
HeightPatternInput height_pattern_input_from_pattern(const HeightColourPattern &p);

// Existing ranges that overlap `ranges`, apart from the ones the pattern `own_pattern_id` wrote
// (those are about to be regenerated). Ascending.
std::vector<std::pair<double, double>> find_height_pattern_clashes(const ModelObject                   &object,
                                                                   const std::vector<HeightPatternRange> &ranges,
                                                                   int                                    own_pattern_id);

// Is this exact range an untouched output of one of the object's patterns? Those are shown as one
// grouped row instead of one row each. A range the user has edited is not owned any more and
// shows as an ordinary range.
bool height_pattern_owns_range(const ModelObject &object, const t_layer_height_range &range);

// The band ranges only (a gap range written for a gap filament is not a band), in order.
std::vector<HeightPatternRange> height_pattern_band_ranges(const HeightPatternExpansion &expansion);

// One short line for the dialog: "14 bands, last band cut short by the top of the object".
std::string height_pattern_short_summary(const HeightPatternExpansion &expansion);

// "6 bands: 2.00-12.00, 22.00-32.00, ... (last band ends at 112.00 of 120.00 mm)". Snapped values.
std::string height_pattern_summary(const HeightPatternExpansion &expansion, double object_top);

// "Overlaps 2 existing height ranges: 40.00-50.00, 60.00-70.00".
std::string height_pattern_clash_text(const std::vector<std::pair<double, double>> &clashes);

// Number of bands a record wrote (gaps are not bands).
int height_pattern_band_count(const HeightColourPattern &p);

// Object-list row text: "Height colour pattern (3 filaments x 6 bands)".
std::string height_pattern_row_label(const HeightColourPattern &p);

// Everything the dialog shows live.
struct HeightPatternPreview
{
    HeightPatternExpansion                 expansion;
    std::string                            error;     // input or expansion error; nothing to apply
    std::string                            summary;   // see height_pattern_summary
    std::vector<std::pair<double, double>> clashes;
    bool ok() const { return error.empty(); }
};

// `object` may be null (no clash check). `own_pattern_id` is 0 for a new pattern.
HeightPatternPreview preview_height_pattern(const ModelObject          *object,
                                            const HeightPatternInput   &in,
                                            int                         own_pattern_id,
                                            const std::vector<double>  &layer_tops,
                                            double                      layer_height,
                                            double                      object_height,
                                            int                         max_filament);

// What is wrong with a pattern (the badges of its object-list row). The GUI words these itself.
struct HeightPatternHealth
{
    bool             modified = false;
    std::vector<int> missing_filaments;
    bool             layer_height_stale = false;
    bool ok() const { return !modified && missing_filaments.empty() && !layer_height_stale; }
};
HeightPatternHealth height_pattern_health(const ModelObject &object, const HeightColourPattern &p,
                                          int filament_count, double layer_height);

// ---- filament changes the pattern adds (phase 3) ----------------------------------------------------

struct HeightPatternChangeInput
{
    int                 base_filament = 1;     // the object's own filament (what the gaps print with)
    // flush_volumes_matrix of the project: filament_count x filament_count, row = from, column = to, mm^3.
    std::vector<float>  flush_matrix;
    int                 filament_count = 0;
    double              flush_multiplier = 1.;
    std::vector<double> density;                // g/cm^3, index = filament id - 1
};

struct HeightPatternChangeEstimate
{
    int    changes      = 0;       // filament changes one object gets from the pattern
    double purge_mm3    = 0.;      // flush volume of those changes (needs the matrix)
    double purge_g      = 0.;      // ... as weight (needs the densities as well)
    bool   purge_known  = false;   // the matrix covered every change
};

// Walk the object bottom to top: it starts on the filament at Z 0 (a band that starts at 0 is not a change),
// every band or gap that prints another filament than the one before is one change, and the object's own
// filament comes back in every gap with no range of its own and above the last band. An estimate: it counts
// one object alone, and the slicer merges changes of objects that sit at the same layer.
HeightPatternChangeEstimate estimate_height_pattern_changes(const std::vector<HeightPatternRange> &ranges,
                                                            double                                  object_top,
                                                            const HeightPatternChangeInput         &input);

// ---- bands the user edited by hand (phase 3) --------------------------------------------------------

enum class HeightPatternBandStatus : int {
    Removed,            // no range at the band's place any more (moved, resized or deleted)
    FilamentChanged,    // same place, another filament
    LayerHeightChanged, // same place and filament, another layer height
};

struct HeightPatternBandDiff
{
    int                     band_number = 0;   // 1-based among the bands; 0 for a gap range
    double                  lo = 0., hi = 0.;
    HeightPatternBandStatus status = HeightPatternBandStatus::Removed;
    int                     expected_extruder = 0;
    int                     actual_extruder   = 0;   // 0 when removed
};

// Only the ranges that differ from what the pattern wrote, in Z order. Empty = untouched.
std::vector<HeightPatternBandDiff> height_pattern_modified_ranges(const ModelObject &object, const HeightColourPattern &pattern);

// Put back only those ranges: every other range of the pattern is left exactly as it is (including
// ones the user changed on purpose elsewhere). A hand-made range that overlaps a restored band is
// replaced by it. Returns how many were restored, -1 for an unknown pattern id.
int reset_height_pattern_modified_ranges(ModelObject &object, int pattern_id);

} // namespace Slic3r

#endif /* slic3r_HeightPatternInput_hpp_ */
