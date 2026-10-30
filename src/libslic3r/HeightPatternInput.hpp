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

} // namespace Slic3r

#endif /* slic3r_HeightPatternInput_hpp_ */
