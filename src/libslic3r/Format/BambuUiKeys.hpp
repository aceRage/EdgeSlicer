#ifndef slic3r_Format_BambuUiKeys_hpp_
#define slic3r_Format_BambuUiKeys_hpp_

// The option lines Bambu Studio puts on its Process / Filament / Printer / Plate / Part tabs, with
// the view level that shows each one. Feeds the "Bambu" settings view (Basic | Advanced | Bambu,
// see SettingsView.hpp and tests/design_view_modes.md).
//
// The data (BambuUiKeys.cpp) is GENERATED from a Bambu Studio release tag by
// scripts/gen_bambu_ui_keys.py and checked in; the build never needs a Bambu Studio checkout.
// Keys are Bambu's spelling: map them to ours through BambuKeyAliases::by_bambu() and
// inferred_equivalents().

#include <vector>

namespace Slic3r {
namespace BambuUiKeys {

enum class Tab { Process, Filament, Printer, Plate, Part };

// Bambu Studio's level: Simple lines show in its Simple view, Advanced lines with its Advanced switch
// on, Develop lines only in Preferences > Developer mode.
enum class Level { Simple, Advanced, Develop };

struct Entry
{
    const char *key;   // Bambu Studio's key
    Tab         tab;
    Level       level;
    const char *page;  // Bambu's page title (reference only; the Bambu view keeps our layout)
    const char *group; // Bambu's group title (reference only)
    int         line;  // line in Bambu's src/slic3r/GUI/Tab.cpp at the tag
};

struct InferredEquivalent
{
    const char *bambu;
    const char *ours;
};

const char *bambu_version(); // e.g. "02.09.01.70"
const char *bambu_commit();  // short commit of the tag

const std::vector<Entry>              &entries();
const std::vector<InferredEquivalent> &inferred_equivalents();
// Bambu keys shown at Simple or Advanced that EdgeSlicer does not define (reviewed, not ported yet).
const std::vector<const char *>       &reviewed_missing();
// Bambu keys whose lines Bambu Studio hides on every Bambu Lab printer.
const std::vector<const char *>       &bbl_printer_hidden();

} // namespace BambuUiKeys
} // namespace Slic3r

#endif // slic3r_Format_BambuUiKeys_hpp_
