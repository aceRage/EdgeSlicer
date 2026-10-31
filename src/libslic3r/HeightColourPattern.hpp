#ifndef slic3r_HeightColourPattern_hpp_
#define slic3r_HeightColourPattern_hpp_

// ---------------------------------------------------------------------------
// Height colour pattern (tests/design_height_colour_pattern.md, phase 1).
//
// "Paint 10 mm starting at Z 2 mm, skip 10 mm, repeat" used to mean one Height
// range modifier per band, edited one at a time. A pattern is only the AUTHORING
// of those ranges: expand_height_colour_pattern() turns the description into
// ordinary ModelObject::layer_config_ranges entries (layer_height + extruder), and
// the slicer sees nothing but ranges. The G-code is therefore exactly what a
// hand-made set of ranges gives, and Bambu Studio / Orca open the file with the
// bands intact.
//
// The HeightColourPattern record is what lets the group be regenerated, removed as
// one unit, or noticed as hand-edited. It lives on the ModelObject and, in the 3MF,
// in the EdgeSlicer-only sidecar Metadata/edgeslicer_height_patterns.json, which is
// written only when some object has a pattern.
//
// All Z values are object-local millimetres (0 = the object's bottom, raft
// excluded), like the keys of layer_config_ranges.
// ---------------------------------------------------------------------------

#include <string>
#include <utility>
#include <vector>

#include <cereal/types/vector.hpp>

namespace Slic3r {

class ModelObject;

// What goes into the gap between two bands.
//  ObjectFilament: nothing is written; the gap prints with the object's own
//                  filament (D2 default, fewest tool changes).
//  NextInSequence: the gap after band i prints with the filament of band i+1 (the
//                  next entry of the sequence). The gap never takes a sequence entry:
//                  bands always run the whole sequence, F1 F2 F3 F1 F2 F3 ...
//  Fixed:          every gap uses gap_filament.
enum class HeightPatternGapMode : int {
    ObjectFilament = 0,
    NextInSequence = 1,
    Fixed          = 2,
};

// One range the pattern wrote, with the values it wrote, so a later edit by hand
// can be told from the pattern's own output.
struct HeightPatternRange
{
    double lo            = 0.;
    double hi            = 0.;
    int    extruder      = 0;   // 1-based filament id written as the "extruder" key
    double layer_height  = 0.;  // written as the "layer_height" key (mandatory, Slicing.cpp)

    template<class Archive> void serialize(Archive &ar) { ar(lo, hi, extruder, layer_height); }
};

struct HeightColourPattern
{
    int                      id        = 0;    // unique within one ModelObject, 1-based
    // As typed (mm). The snapped values are in `generated`.
    double                   start_z   = 0.;
    double                   band_h    = 0.;
    double                   gap_h     = 0.;
    // How many times the WHOLE sequence is repeated (every repetition uses every filament, in order);
    // 0 = repeat until the top of the object. Records written before 2.4.8.x counted single bands here;
    // see repeat_is_sequences.
    int                      repeat    = 0;
    // True for records that count whole sequences. The sidecar writes "repeat_seq": true for them; a
    // record without it is read as the old band count and converted on load (the bands it generated
    // are stored and stay exactly as they were).
    bool                     repeat_is_sequences = true;
    std::vector<int>         sequence;         // 1-based filament ids, cycled per band
    int                      gap_mode  = int(HeightPatternGapMode::ObjectFilament);
    int                      gap_filament = 0; // HeightPatternGapMode::Fixed only
    bool                     snap_to_layers = true;
    // Which direction the bands run in: 0 = Z (height), the only one there is today. Reserved for
    // horizontal and angled colour patterns; saved in the sidecar only when it is not 0, so files
    // without it are exactly the Phase 1 format.
    int                      axis      = 0;
    // The ranges the last apply wrote (the keys this pattern owns).
    std::vector<HeightPatternRange> generated;

    bool empty() const { return generated.empty() && sequence.empty(); }

    template<class Archive> void serialize(Archive &ar)
    {
        ar(id, start_z, band_h, gap_h, repeat, sequence, gap_mode, gap_filament, snap_to_layers, generated, axis, repeat_is_sequences);
    }
};

// ---- expansion (pure) ------------------------------------------------------

struct HeightPatternExpansion
{
    std::vector<HeightPatternRange> ranges;    // ascending, non-overlapping; layer_height filled in
    int                             bands = 0; // band ranges among `ranges` (the rest are gap ranges)
    bool                            clipped_at_top = false; // the last band was cut short by the object top
    // Repeat counted in whole sequences (repeat > 0): the bands the full repeats would have made, and
    // whether the object top stopped the last repetition before every filament was used.
    int                             repeats = 0;            // the pattern's repeat (whole sequences); 0 = to the top
    int                             planned_bands = 0;
    bool                            cut_off = false;
    int                             last_filament = 0;      // filament of the last band written
    int                             last_sequence_index = 0; // 1-based position of that band's filament in the sequence
    int                             sequence_length = 0;
    int                             dropped = 0;            // bands that collapsed to nothing when snapped
    std::string                     error;                  // non-empty: nothing was produced
    bool ok() const { return error.empty(); }
};

// Number of bands `repeat` whole repetitions of a sequence of `sequence_length` filaments make:
// repeat * sequence_length, whatever the gap mode (gaps sit between bands and use no sequence entry).
// repeat <= 0 gives 0 (to the top). Used by the expansion and by the dialog's count line.
int height_pattern_planned_bands(int repeat, int sequence_length, HeightPatternGapMode gap_mode);

// The legacy record counted bands in `repeat`; the sequences needed to give at least that many bands.
int height_pattern_repeat_from_legacy_bands(int bands, int sequence_length, HeightPatternGapMode gap_mode);

// The top Z of every layer of the object, ascending, from the interleaved
// [lo, hi, lo, hi, ...] vector that generate_object_layers() returns.
std::vector<double> height_pattern_layer_tops(const std::vector<double> &object_layers);

// Expand a pattern into ranges.
//  layer_tops    the object's real layer grid (see above); the last entry is the object top.
//                May be empty, then `object_height` is the top and nothing snaps.
//  layer_height  written into every range's "layer_height" key.
//  max_filament  the project's filament count (physical + mixed); 0 = do not check. A band or gap
//                that names a higher filament is refused (an ordinary range with such an extruder
//                silently prints filament 1, which is exactly what a pattern must not do).
// The pattern's `generated` is ignored and not modified.
HeightPatternExpansion expand_height_colour_pattern(const HeightColourPattern &pattern,
                                                    const std::vector<double> &layer_tops,
                                                    double                     layer_height,
                                                    double                     object_height = 0.,
                                                    int                        max_filament = 0);

// ---- applying to an object -------------------------------------------------

struct HeightPatternApplyResult
{
    bool                                       ok = false;
    std::string                                error;
    // Existing ranges (not owned by the pattern being applied) that overlap the new bands.
    // With replace_overlaps == false nothing is changed and this lists the clashes.
    std::vector<std::pair<double, double>>     clashes;
    HeightPatternExpansion                     expansion;
    int                                        pattern_id = 0;
};

// Write `pattern` into the object: erases the ranges a previous apply of the same
// pattern id wrote, expands, resolves clashes with other ranges, inserts the ranges
// and stores the record (new id if pattern.id is 0 or unknown). Everything or nothing.
HeightPatternApplyResult apply_height_colour_pattern(ModelObject                &object,
                                                     HeightColourPattern         pattern,
                                                     const std::vector<double>  &layer_tops,
                                                     double                      layer_height,
                                                     bool                        replace_overlaps,
                                                     double                      object_height = 0.,
                                                     int                         max_filament = 0);

// Filament ids used by the object's patterns (bands and gaps as generated) that are above
// `filament_count`, e.g. after a project was loaded with fewer filaments. Sorted, unique.
std::vector<int> height_colour_pattern_missing_filaments(const ModelObject &object, int filament_count);

// A band prints at the layer height written into its range, so a pattern generated for one layer
// height changes the surface (and the layer count) of its bands once the object's real layer height
// is something else. True when any generated range carries a layer height other than `layer_height`:
// the cue to regenerate (Reset) rather than slice.
bool height_colour_pattern_layer_height_mismatch(const ModelObject &object, double layer_height);

// Erase the ranges the pattern owns that are still there, and the record. Hand-made
// ranges are never touched. Returns false if the id is unknown.
bool remove_height_colour_pattern(ModelObject &object, int pattern_id);

// Forget the record, keep the ranges as plain ranges (the "Detach" of a modified pattern).
bool detach_height_colour_pattern(ModelObject &object, int pattern_id);

const HeightColourPattern *find_height_colour_pattern(const ModelObject &object, int pattern_id);

// D8: is the pattern still exactly what it generated?
struct HeightPatternState
{
    bool modified = false;
    // The owned ranges that are gone, moved or carry other settings.
    std::vector<std::pair<double, double>> changed;
};
HeightPatternState height_colour_pattern_state(const ModelObject &object, const HeightColourPattern &pattern);

// Sidecar (de)serialisation, Metadata/edgeslicer_height_patterns.json.
// `objects` holds one vector per object, in model order; objects without patterns give "[]".
std::string height_patterns_to_json(const std::vector<const std::vector<HeightColourPattern> *> &objects);
// Returns one vector per object listed in the file as (1-based object index, patterns). A broken
// file or entry is dropped, never half-applied.
std::vector<std::pair<int, std::vector<HeightColourPattern>>> height_patterns_from_json(const std::string &json);

} // namespace Slic3r

#endif /* slic3r_HeightColourPattern_hpp_ */
