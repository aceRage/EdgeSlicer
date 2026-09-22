#ifndef slic3r_BambuConfigCompat_hpp_
#define slic3r_BambuConfigCompat_hpp_

#include <string>
#include <vector>

namespace Slic3r {

class ConfigOptionDef;

// Bambu Studio stores many per-extruder settings as NULLABLE vectors and writes the literal
// string "nil" into the slots of extruders the setting does not apply to, e.g.
//     "top_surface_acceleration": ["100", "nil", "100", "nil", "nil"]
// This fork types a number of those same options as scalars, and types others as plain
// (non-nullable) vectors. Either way ConfigOption*::deserialize() throws
// "Deserializing nil into a non-nullable object" on the first nil it meets, and
// PresetCollection::load_presets treats a parse failure as a reason to delete the file.
// The helpers below translate the array BEFORE it reaches deserialize().
//
// The cardinal rule: a "nil" slot means "this setting does not apply to that extruder". It does
// NOT mean zero. Substituting 0 for a speed or acceleration is what produces `G1 F0` and stalls
// a printer, so no code path here may ever invent a 0.
namespace BambuConfigCompat {

enum class NilFix {
    // The array carried no "nil"; nothing was changed.
    None,
    // Every non-nil slot held the same value and the fork wants a scalar: collapsed to it.
    Collapsed,
    // The non-nil slots genuinely disagreed and the fork only has a scalar. The documented
    // rule below picked one; the value the user sees may differ from Bambu Studio's.
    CollapsedLossy,
    // The fork's option is a vector but not nullable: nil slots were backfilled from a
    // neighbouring real value so per-extruder structure and slot count survive.
    Backfilled,
    // Same as Backfilled, but the surviving neighbours disagreed, so the fill is a guess.
    BackfilledLossy,
    // Every slot was nil: there is no value to keep. The caller drops the key and the
    // option keeps its compiled-in default.
    AllNil,
};

struct NilResult {
    NilFix      fix { NilFix::None };
    // The translated value, already in the comma-joined form set_deserialize() expects.
    // Only meaningful when fix is neither None nor AllNil.
    std::string value;
    // The original array as Bambu wrote it, comma-joined, for the substitution report.
    std::string original;

    bool changed() const { return fix != NilFix::None; }
    bool lossy()   const { return fix == NilFix::CollapsedLossy || fix == NilFix::BackfilledLossy; }
    bool drop()    const { return fix == NilFix::AllNil; }
};

// True if any element is the literal "nil".
bool has_nil(const std::vector<std::string> &array_values);

// Translate a nil-bearing array for the option described by optdef.
//
//   optdef == nullptr            -> None (unknown key; the caller drops it anyway)
//   option is nullable           -> None (deserialize() handles nil natively; leave it alone)
//   no "nil" present             -> None
//   all slots nil                -> AllNil
//   scalar option                -> Collapsed / CollapsedLossy
//   vector, non-nullable option  -> Backfilled / BackfilledLossy
//
// Lossy rule for a scalar target: the value held by the majority of the non-nil slots wins;
// on a tie the FIRST non-nil slot wins. Bambu writes the primary extruder first, so the first
// slot is the value a single-extruder machine actually prints with.
//
// Lossy rule for a vector target: each nil slot takes the nearest preceding non-nil value, or,
// when the array starts with nil, the first non-nil value that follows. The slot count is
// preserved so per-extruder indexing stays correct.
NilResult translate_nil_array(const ConfigOptionDef *optdef, const std::vector<std::string> &array_values);

// Human-readable one-liner naming what was done, used in the log and the substitution report.
std::string describe(NilFix fix);

} // namespace BambuConfigCompat
} // namespace Slic3r

#endif // slic3r_BambuConfigCompat_hpp_
