#pragma once

// EdgeSlicer, Fine detail (tests/design_arachne_thin_features.md, P2; owner decisions D1-D5 of 2026-11-17).
//
// The process / object / modifier option "Fine detail" (key fine_detail) lets features narrower than a wall print
// with a thinner line than min_bead_width, without moving the point where a wall splits into two lines. No GUI and
// no user-visible strings here, so the rules can be unit tested (tests/libslic3r/test_fine_detail.cpp).
//
// How the level and the raw keys relate (D1):
//  - The slicer reads the LEVEL for Off / Balanced / Fine text and the raw keys thin_feature_line_width /
//    thin_feature_min_size only for Custom. So a named level always means the same thing, whatever the raw keys say
//    (a project or a CLI override that only sets fine_detail works), and Off ignores the raw keys altogether: Off with
//    any raw values is the walls as before.
//  - The settings tab keeps them in agreement (changes_for_edit()): choosing Balanced or Fine text writes the level's
//    values into the raw keys, so they always show what the level does; editing a raw key while a named level is
//    selected switches the level to Custom and keeps the edit. The writes go through ConfigManipulation::apply(), the
//    path the settings tab already uses when one choice sets other keys (spiral vase, support style), so the process
//    tab, object settings and modifier settings all store them.

#include "PrintConfig.hpp"

#include <string>
#include <vector>

namespace Slic3r {
namespace FineDetail {

// D2 (owner, 2026-11-17): the floors, in percent of the smallest nozzle diameter on the printer (as every Arachne
// percent key). The one place to tune them after the owner's test print. A feature percent of 0 means "Skip features
// thinner than (min_feature_size) as set".
constexpr double balanced_line_percent     = 60.;
constexpr double balanced_feature_percent  = 0.;
constexpr double fine_text_line_percent    = 45.;
constexpr double fine_text_feature_percent = 15.;

struct Floors
{
    // 0 = not lowered (min_bead_width / min_feature_size as set).
    double line_percent{0.};
    double feature_percent{0.};
};

// The floors a level stands for. Custom takes the raw values; Off is {0, 0}.
Floors floors(FineDetailLevel level, double raw_line_percent, double raw_feature_percent);
Floors floors(const PrintRegionConfig &config);

// Whether the region asks for Fine detail at all.
bool enabled(const PrintRegionConfig &config);

// Whether two regions get the same walls and thin-line speed from Fine detail (Layer.cpp groups regions for one
// perimeter run by this). Two regions with Fine detail Off are the same whatever their raw keys say.
bool same_walls(const PrintRegionConfig &a, const PrintRegionConfig &b);

// D4 (a): the Classic generator detects thin walls where Detect thin wall is on or Fine detail is.
bool classic_thin_walls(const PrintRegionConfig &config);

// The narrowest Arachne spacing (mm) whose line is not narrower than the layer is tall: the rounded-rectangle line
// width is spacing + h (1 - pi/4), so width >= h needs spacing >= h pi/4.
double min_spacing_for_layer(double layer_height);

// The thin-feature line spacing (mm) WallToolPaths uses for a floor of `floor_mm` (0 = none) on a layer with the
// Off floor `min_bead_width_mm`: never above the Off floor, never narrower than the layer allows. Returns
// min_bead_width_mm when floor_mm is 0.
double effective_thin_bead_width(double floor_mm, double min_bead_width_mm, double layer_height);
// Same for the "skip thinner than" limit: never above min_feature_size. Returns min_feature_size_mm when 0.
double effective_thin_feature_size(double floor_mm, double min_feature_size_mm);

// The percent keys of the Thin features / Wall count changes groups in millimetres for one nozzle, for the settings
// tab's "= x mm" note (P1). Spacings, as Arachne works in them; line_width() turns one into the width the preview
// and the G-code show. Fine detail values are the effective ones (clamped as the slicer clamps them), 0 when Off.
struct Millimetres
{
    double nozzle{0.};
    double layer_height{0.};
    double skip_thinner_than{0.};
    double thinnest_line{0.};
    double thinnest_line_first_layer{0.};
    double odd_minimum{0.};   // = thinnest_line when the key is 0
    double even_minimum{0.};  // = thinnest_line when the key is 0
    double fine_detail_line{0.};
    double fine_detail_skip{0.};
};
// `print` holds the process keys (missing keys take their defaults); `nozzle` is the smallest nozzle diameter.
Millimetres millimetres(const DynamicPrintConfig &print, double nozzle);
// Spacing to the line width shown in the preview (Flow's rounded rectangle).
double line_width(double spacing, double layer_height);

// The raw keys a named level writes.
const std::vector<std::string> &raw_keys();
// Every key the Fine detail group owns (the level, the raw keys, the speed).
const std::vector<std::string> &all_keys();

// The GUI rule described above. `config` already holds the edited value of `changed_key`. Returns the keys to write
// (empty when nothing has to change); never touches a key outside fine_detail and raw_keys().
DynamicPrintConfig changes_for_edit(const DynamicPrintConfig &config, const std::string &changed_key);

} // namespace FineDetail
} // namespace Slic3r
