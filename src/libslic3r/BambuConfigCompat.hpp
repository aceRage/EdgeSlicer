#ifndef slic3r_BambuConfigCompat_hpp_
#define slic3r_BambuConfigCompat_hpp_

#include <string>
#include <vector>

namespace Slic3r {

class ConfigBase;
class ConfigOptionDef;
struct ConfigSubstitutionContext;

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
    // option keeps its compiled-in default. For an override (translate_nil_override) this is
    // exactly Bambu's meaning - the override applies to no variant - and is not lossy.
    AllNil,
    // Override only: every nil slot took the value its parent (the print settings, or the
    // enclosing object) has in that same slot - what Bambu Studio resolves it to.
    Inherited,
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

// ---- Overrides: per-object / per-part / layer-range settings in a 3MF ----------------------
//
// Bambu Studio applies such an override slot by slot (ConfigOptionVector::set_to_index, called
// from PrintObject::object_config_from_model_object and apply_to_print_region_config): a "nil"
// slot keeps whatever the print settings - or the enclosing object - already hold for that
// extruder variant. This fork applies an override as a whole vector (ConfigOption::set), so the
// nil slots are resolved here, at load time, from the parents.
//
//   parent_slots: the same option in each parent config, nearest first, one string per slot
//                 (a scalar parent is a single entry). A parent is used for a vector target only
//                 when its slot count matches; for a scalar target its first entry is the value.
//
//   all slots nil                -> AllNil: drop the key, the object simply has no override
//   vector, every nil slot has a parent value -> Inherited (clean)
//   vector, some nil slot has none -> those slots are backfilled from the override's own values
//                                   (translate_nil_array's rule) and the result is BackfilledLossy:
//                                   Bambu would have used a parent value we do not know
//   scalar                       -> the collapse rule of translate_nil_array; Collapsed only if
//                                   every non-nil slot agrees AND every nil slot's parent value
//                                   equals the collapsed one, otherwise CollapsedLossy
//
// Nullable and string options are left alone exactly as in translate_nil_array.
NilResult translate_nil_override(const ConfigOptionDef *optdef, const std::vector<std::string> &array_values,
                                 const std::vector<std::vector<std::string>> &parent_slots);

// Split a serialized (comma-joined) option value into its slots, but only when it can carry a
// Bambu "nil": returns false - and leaves slots empty - when the value holds no "nil" element or
// the option is nullable or a string type. Cheap on the common path (a substring test first).
bool split_nil_value(const ConfigOptionDef &optdef, const std::string &value, std::vector<std::string> &slots);

// Marks the values deserialized while it lives as Bambu overrides whose nil slots inherit from
// `parent` (may be nullptr: override semantics with no known parent). Scopes nest; the innermost
// parent is searched first. Used by the 3MF reader around object, part and layer-range settings.
class OverrideScope {
public:
    OverrideScope(ConfigSubstitutionContext &ctxt, const ConfigBase *parent);
    ~OverrideScope();
    OverrideScope(const OverrideScope &) = delete;
    OverrideScope &operator=(const OverrideScope &) = delete;
private:
    ConfigSubstitutionContext &m_ctxt;
};

// Human-readable one-liner naming what was done, used in the log and the substitution report.
std::string describe(NilFix fix);

} // namespace BambuConfigCompat
} // namespace Slic3r

#endif // slic3r_BambuConfigCompat_hpp_
