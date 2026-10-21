#ifndef slic3r_Showcase_ShowcaseCallouts_hpp_
#define slic3r_Showcase_ShowcaseCallouts_hpp_

// Showcase tab, phase S3: filament callouts (design tests/design_showcase_tab.md, section 3). This is
// the GL- and wxWidgets-free half, so the unit tests and the 3MF code can use it:
//   - what the scene stores (Callouts, Callout, the label fields D7 and the style),
//   - which callouts a plate gets: one per filament (and per colour or tool change) the plate shows,
//     from object, part, painted, height-range and layer-change filament assignments (plan_callout_targets),
//     and which of them a visible point belongs to (CalloutRegions::classify, used on the ID pass),
//   - where an anchor goes in a region seen on screen (find_anchor_pixels: the pixel deepest inside),
//   - colour names (D8: the Snapmaker colour library, then the user's text, then "≈ nearest name"),
//   - the label text (build_callout_label),
//   - the layout: left and right columns whose leaders never cross, a numbered legend when crowded
//     (layout_callouts).
// The GUI rasterises the result with wxGraphicsContext (D6) for the screen and every export alike.

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace Slic3r {

class Model;
struct FilamentColorInfo;
namespace CustomGCode { struct Info; }

namespace Showcase {

// New values go at the end: the JSON names and the panel's choices follow this order.
enum class CalloutSource : int { Object = 0, Part, Paint, HeightRange, ToolChange, ColorChange };
enum class CalloutShape  : int { Minimal = 0, Card, Pill, Technical };
enum class LeaderStyle   : int { Straight = 0, Elbow };
enum class CalloutLayout : int { Auto = 0, Columns, Legend };
// Built-in looks (design 3.4). Applying one sets the shape, colours, leader and size.
enum class CalloutStylePreset : int { Minimal = 0, CardLight, CardDark, Pill, Technical };

const char *to_string(CalloutSource v);
const char *to_string(CalloutShape v);
const char *to_string(LeaderStyle v);
const char *to_string(CalloutLayout v);
bool from_string(const std::string &s, CalloutSource &v);
bool from_string(const std::string &s, CalloutShape &v);
bool from_string(const std::string &s, LeaderStyle &v);
bool from_string(const std::string &s, CalloutLayout &v);

// Where a leader starts: a point on an object, in the coordinates of one of its instances, so it
// moves with the object when the object is moved or turned.
struct CalloutAnchor
{
    int                   object_idx   = -1;
    int                   instance_idx = 0;
    std::array<double, 3> local{ { 0., 0., 0. } };

    bool operator==(const CalloutAnchor &o) const { return object_idx == o.object_idx && instance_idx == o.instance_idx && local == o.local; }
    bool operator!=(const CalloutAnchor &o) const { return !(*this == o); }
};

// Up to this many leaders from one label to the separate regions of its filament ("multileader").
constexpr int MAX_CALLOUT_LEADERS = 3;

struct Callout
{
    // What it labels (see make_callout_key()): one filament, or the filament after a colour or
    // tool change at a height.
    std::string   key;
    int           filament = 1;                        // 1-based slot; a mixed filament's virtual id too
    CalloutSource source   = CalloutSource::Object;    // where the filament first comes from
    double        z_from   = -1.;                      // ToolChange / ColorChange: from this height (mm)
    std::string   color;                               // ColorChange: the colour after it, "#RRGGBB"

    std::vector<CalloutAnchor> anchors;                // the first is the main one
    bool          user_anchor = false;                 // placed by hand: regenerating keeps it

    bool          pinned = false;                      // the label stays where the user dragged it
    std::array<double, 2> label_pos{ { 0., 0. } };     // pinned: the label's top-left, 0..1 of the frame

    std::string   title;        // the user's first line, replacing the automatic one when set
    std::string   color_name;   // the user's name for the colour (D8, used where the library has none)
    std::string   note;         // an extra line under the label
    bool          hidden = false;

    bool operator==(const Callout &o) const;
    bool operator!=(const Callout &o) const { return !(*this == o); }
};

// D7: what a label shows. Weight and price are off by default (resellers may not want prices in
// shared images); both need a sliced plate.
struct CalloutFields
{
    bool brand      = true;
    bool product    = true;
    bool color_name = true;
    bool hex        = true;   // the hex code and the swatch
    bool type       = true;   // the filament type and the finish
    bool weight     = false;
    bool price      = false;

    bool operator==(const CalloutFields &o) const;
    bool operator!=(const CalloutFields &o) const { return !(*this == o); }
};

struct CalloutStyle
{
    CalloutShape shape     = CalloutShape::Card;
    std::string  font_face;              // empty = the UI font (Technical: a fixed-width font)
    bool         bold      = false;
    float        size_pct  = 2.2f;       // text height, % of the image height (so it scales with it)
    // Text and leader colours chosen to stand out from the backdrop behind them (design 3.4).
    bool         auto_colors = true;
    std::string  text_color  = "#1E1E1E";
    std::string  line_color  = "#1E1E1E";
    std::string  background  = "#FFFFFF";   // Card and Pill
    float        background_opacity = 0.92f;
    LeaderStyle  leader      = LeaderStyle::Elbow;

    bool operator==(const CalloutStyle &o) const;
    bool operator!=(const CalloutStyle &o) const { return !(*this == o); }
};

std::vector<CalloutStylePreset> callout_style_presets();
void                            apply_callout_style_preset(CalloutStyle &style, CalloutStylePreset preset);

struct Callouts
{
    bool                 enabled = false;
    CalloutLayout        layout  = CalloutLayout::Auto;
    int                  max_per_column = 8;   // Auto: more than this in a column gives the legend
    CalloutFields        fields;
    CalloutStyle         style;
    std::vector<Callout> items;

    const Callout *find(const std::string &key) const;
    Callout       *find(const std::string &key);

    bool operator==(const Callouts &o) const;
    bool operator!=(const Callouts &o) const { return !(*this == o); }
};

// "f2" (filament 2), "f2@12.40" (filament 2 from 12.4 mm, a tool change), "f2@12.40#FF0000" (filament
// 2 changed to red at 12.4 mm). Stable across saves, so user edits stay with the right callout.
std::string make_callout_key(int filament, double z_from = -1., const std::string &color = std::string());

// A loaded scene names objects by index: anchors on objects that are gone are dropped, and so are
// callouts of filament slots the project no longer has. Returns how many anchors and callouts went.
int prune_callouts(Callouts &callouts, size_t object_count, int filament_count);

// --- which callouts a plate gets ------------------------------------------------------------------

struct CalloutTarget
{
    std::string      key;
    int              filament = 1;
    CalloutSource    source   = CalloutSource::Object;
    double           z_from   = -1.;
    std::string      color;
    std::vector<int> objects;   // the plate's objects showing it, in model order

    bool operator==(const CalloutTarget &o) const
    {
        return key == o.key && filament == o.filament && source == o.source && z_from == o.z_from && color == o.color && objects == o.objects;
    }
};

// What a visible point of an object shows, before height ranges and layer changes.
struct BaseRegion
{
    int           filament = 1;
    bool          object_default = true;   // the object's own filament (a height range may replace it)
    CalloutSource source = CalloutSource::Object;
};

// The filament assignments of a plate's objects, and the layer changes of the plate.
class CalloutRegions
{
public:
    // `objects`: model indices of the plate's objects. `changes`: the plate's layer changes (null =
    // none). Model parts only; modifiers, negative volumes and support blockers draw nothing.
    CalloutRegions(const Model &model, const std::vector<int> &objects, const CustomGCode::Info *changes);

    // The distinct base regions of an object: its parts' filaments, painted filaments.
    const std::vector<BaseRegion> &base_regions(int object_idx) const;
    // Which callout a point shows: an object's base region at print height z (mm).
    CalloutTarget classify(int object_idx, const BaseRegion &base, double z) const;
    // Every callout the plate shows, sorted by filament, then height.
    std::vector<CalloutTarget> targets() const;

private:
    struct Range  { double lo, hi; int filament; };
    struct Change { double z; bool tool; int extruder; std::string color; };
    struct Object { int idx; double height; std::vector<BaseRegion> bases; std::vector<Range> ranges; };
    const Object *object(int object_idx) const;

    std::vector<Object> m_objects;
    std::vector<Change> m_changes;   // sorted by z
};

std::vector<CalloutTarget> plan_callout_targets(const Model &model, const std::vector<int> &objects, const CustomGCode::Info *changes);

// --- anchors from the ID pass ---------------------------------------------------------------------

struct AnchorPixel
{
    int    x = 0, y = 0;   // origin top-left
    int    area = 0;       // pixels in its region
    double clearance = 0.; // distance to the region's edge, pixels
};

// `class_map` (w x h, row-major, top row first) says which class each pixel shows, -1 = none. Per
// class: its connected regions (4-neighbour), largest first, at most `max_regions`, each with its
// pole of inaccessibility (the pixel furthest from the region's edge, design 3.3). Regions smaller
// than `min_area` are left out unless the class has nothing bigger.
std::vector<std::vector<AnchorPixel>> find_anchor_pixels(const std::vector<int> &class_map, int w, int h, int num_classes,
                                                         int max_regions = MAX_CALLOUT_LEADERS, int min_area = 0);

// --- colour names (D8) ----------------------------------------------------------------------------

struct NamedColor
{
    const char *name;   // English, marked for translation
    const char *hex;
};
// The built-in list of about 150 names the "≈ nearest" fallback picks from.
const std::vector<NamedColor> &named_colors();

// CIEDE2000 colour difference of two sRGB colours (0..255 per channel).
double delta_e2000(const std::array<int, 3> &a, const std::array<int, 3> &b);

struct ColorNameMatch
{
    std::string name;            // English
    bool        exact   = false; // the hex is exactly that named colour
    double      delta_e = 0.;
};
ColorNameMatch nearest_color_name(const std::string &hex);

// The Snapmaker colour library's name for a colour: the filament's own family first (`family`
// may be null), then any library filament of exactly that colour. `lang` like "de_DE"; falls back
// to "en", then any name. Empty when the library has none.
std::string library_color_name(const FilamentColorInfo *family, const std::vector<FilamentColorInfo> &library,
                               const std::vector<std::string> &colors, const std::string &lang);

// D8: the library's name, else the user's, else "≈ " + the nearest built-in name (`translate`
// turns that English name into the UI language; null = keep English). Empty for an invalid hex.
std::string resolve_color_name(const std::string &library_name, const std::string &user_name, const std::string &hex,
                               const std::function<std::string(const char *)> &translate = nullptr);

// --- label text -----------------------------------------------------------------------------------

// What the GUI knows about the filament a callout names.
struct LabelFilament
{
    std::string              vendor;
    std::string              preset_name;
    std::string              type;          // filament_type
    std::string              finish;        // the finish's name in the UI language
    bool                     standard_finish = true;   // not worth saying ("PLA", not "PLA · Standard")
    std::vector<std::string> colors;        // "#RRGGBB"; several for a multi-colour filament
    std::string              color_name;    // resolved by resolve_color_name()
    double                   weight_g = -1.;   // < 0: unknown (not sliced)
    double                   price    = -1.;
    bool                     mixed    = false;
    std::string              mixed_parts;   // a mixed filament: "PLA Basic 60% + PLA Matte 40%"
};

// The UI-language words a label uses.
struct LabelWords
{
    std::string mixed_title = "Mixed filament";
    std::string from_height = "from %1% mm";   // %1% = the height
    std::string currency    = "$";
    std::string grams       = "g";
};

struct CalloutLabel
{
    std::string              title;        // brand + product (or the user's first line)
    std::string              color_name;   // shown after the title
    std::vector<std::string> swatches;     // empty without the hex field
    std::string              hex;
    std::string              details;      // type · finish · weight · price · from height
    std::string              note;

    bool empty() const { return title.empty() && color_name.empty() && hex.empty() && details.empty() && note.empty(); }
};

// "Polymaker PolyTerra PLA @BBL X1C 0.4 nozzle" with vendor "Polymaker" -> "PolyTerra PLA".
std::string product_name(const std::string &preset_name, const std::string &vendor);
std::string format_weight(double grams, const std::string &unit = "g");
std::string format_price(double amount, const std::string &currency);
CalloutLabel build_callout_label(const Callout &callout, const LabelFilament &filament, const CalloutFields &fields, const LabelWords &words);
// The first line as drawn: title, then " – " and the colour name.
std::string callout_first_line(const CalloutLabel &label);

// Ink that reads on a backdrop of that luminance (0 black .. 1 white): near-black or near-white.
std::string auto_ink(double luminance);
double      hex_luminance(const std::string &hex, double fallback = 1.);

// --- layout ---------------------------------------------------------------------------------------

struct PointD
{
    double x = 0., y = 0.;
};
struct RectD
{
    double x = 0., y = 0., w = 0., h = 0.;
    double right() const { return x + w; }
    double bottom() const { return y + h; }
    bool   empty() const { return w <= 0. || h <= 0.; }
    bool   contains(const PointD &p) const { return p.x >= x && p.x <= x + w && p.y >= y && p.y <= y + h; }
};

struct LayoutLabel
{
    bool   has_anchor = true;
    PointD anchor;           // the main anchor, frame pixels, origin top-left
    double w = 0., h = 0.;   // the label's size
    bool   pinned = false;
    PointD pinned_pos;       // pinned: the top-left, frame pixels
};

struct LayoutParams
{
    double              frame_w = 0., frame_h = 0.;
    double              margin  = 0.;   // between the frame's edges and the labels
    double              gap     = 0.;   // between two labels of a column
    double              landing = 0.;   // Elbow: the horizontal end of a leader
    double              model_center_x = -1.;   // < 0: the middle of the anchors
    LeaderStyle         leader = LeaderStyle::Elbow;
    CalloutLayout       mode   = CalloutLayout::Auto;
    int                 max_per_column = 8;
    double              legend_badge = 0.;   // the legend: the number's width in front of each row
    std::vector<RectD>  reserved;            // kept free of labels (the watermark)
};

struct PlacedLabel
{
    RectD               box;
    int                 side = 0;     // -1 left column, 1 right column, 0 pinned or in the legend
    std::vector<PointD> leader;       // from the anchor to the label; empty in the legend
    int                 number = 0;   // the legend's number, 1-based
};

struct CalloutLayoutResult
{
    bool                     legend = false;
    std::vector<PlacedLabel> labels;          // one per LayoutLabel, in order
    RectD                    legend_box;      // legend: the key's box
    bool                     bent = false;    // a column's leaders were bent to keep them apart
};

CalloutLayoutResult layout_callouts(const std::vector<LayoutLabel> &labels, const LayoutParams &params);

// True when the segments [a, b] and [c, d] meet (touching included).
bool segments_cross(const PointD &a, const PointD &b, const PointD &c, const PointD &d);
bool polylines_cross(const std::vector<PointD> &a, const std::vector<PointD> &b);

} // namespace Showcase
} // namespace Slic3r

#endif // slic3r_Showcase_ShowcaseCallouts_hpp_
