#include "BambuConfigCompat.hpp"
#include "Config.hpp"

#include <algorithm>
#include <map>

namespace Slic3r {
namespace BambuConfigCompat {

static const std::string NIL = "nil";

bool has_nil(const std::vector<std::string> &array_values)
{
    return std::any_of(array_values.begin(), array_values.end(),
                       [](const std::string &v) { return v == NIL; });
}

std::string describe(NilFix fix)
{
    switch (fix) {
    case NilFix::None:            return "unchanged";
    case NilFix::Collapsed:       return "collapsed per-extruder list to a single value";
    case NilFix::CollapsedLossy:  return "collapsed a per-extruder list whose values differ - only one value was kept";
    case NilFix::Backfilled:      return "filled not-applicable extruder slots from the value already in the list";
    case NilFix::BackfilledLossy: return "filled not-applicable extruder slots whose neighbours differ - the filled slots are a best guess";
    case NilFix::AllNil:          return "every extruder slot was not-applicable - the setting was left at its default";
    }
    return "unchanged";
}

static std::string join(const std::vector<std::string> &values)
{
    std::string out;
    for (size_t i = 0; i < values.size(); ++i) {
        if (i) out += ",";
        out += values[i];
    }
    return out;
}

NilResult translate_nil_array(const ConfigOptionDef *optdef, const std::vector<std::string> &array_values)
{
    NilResult r;
    if (optdef == nullptr || array_values.empty() || ! has_nil(array_values))
        return r;

    // A nullable option deserializes "nil" natively into its own nil sentinel. Touching it here
    // would destroy the very information the nullable type exists to carry.
    if (optdef->nullable)
        return r;

    // In a string list "nil" is just text the user typed, not a not-applicable marker, and the
    // string paths quote and escape each element rather than comma-joining them. Leave them be.
    if (optdef->type == coString || optdef->type == coStrings)
        return r;

    r.original = join(array_values);

    std::vector<std::string> non_nil;
    non_nil.reserve(array_values.size());
    for (const std::string &v : array_values)
        if (v != NIL)
            non_nil.push_back(v);

    if (non_nil.empty()) {
        // Nothing survives. Never fabricate a number - especially not 0, which for a speed or
        // acceleration would emit `G1 F0` and stall the printer. Let the option keep its default.
        r.fix = NilFix::AllNil;
        return r;
    }

    const bool all_equal = std::all_of(non_nil.begin() + 1, non_nil.end(),
                                       [&](const std::string &v) { return v == non_nil.front(); });

    if (optdef->is_scalar()) {
        if (all_equal) {
            r.fix   = NilFix::Collapsed;
            r.value = non_nil.front();
            return r;
        }
        // Documented lossy rule: majority of the non-nil slots, first non-nil slot breaks a tie.
        std::map<std::string, int> tally;
        for (const std::string &v : non_nil)
            ++tally[v];
        std::string best  = non_nil.front();
        int         best_n = tally[best];
        for (const std::string &v : non_nil) {
            const int n = tally[v];
            if (n > best_n) {
                best   = v;
                best_n = n;
            }
        }
        r.fix   = NilFix::CollapsedLossy;
        r.value = best;
        return r;
    }

    // Vector option that is not nullable: keep the slot count so per-extruder indexing stays
    // correct, and fill each nil from the nearest real neighbour.
    std::vector<std::string> filled = array_values;
    std::string last_seen;
    for (size_t i = 0; i < filled.size(); ++i) {
        if (filled[i] != NIL)
            last_seen = filled[i];
        else if (! last_seen.empty())
            filled[i] = last_seen;
    }
    // Leading nils have no preceding value; take the first real value that follows.
    for (size_t i = 0; i < filled.size() && filled[i] == NIL; ++i)
        filled[i] = non_nil.front();

    r.fix   = all_equal ? NilFix::Backfilled : NilFix::BackfilledLossy;
    r.value = join(filled);
    return r;
}

} // namespace BambuConfigCompat
} // namespace Slic3r
