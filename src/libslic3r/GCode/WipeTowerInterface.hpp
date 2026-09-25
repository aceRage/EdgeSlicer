#ifndef slic3r_GCode_WipeTowerInterface_hpp_
#define slic3r_GCode_WipeTowerInterface_hpp_

// Tower interface options, shared by both prime tower generators (WipeTower for Bambu Lab
// printers, WipeTower2 for every other printer) and by the pre-slice footprint estimate.
//
// Bambu Studio has one developer switch, enable_tower_interface_features, for a bundle of
// behaviours it applies on "interface layers" of its per-adhesion-category tower planner (where a
// different material category starts printing on the tower). Three of them are options here, each
// off by default and independent of the others:
//   A  wipe_tower_interface_temp         wait for filament_tower_interface_print_temp before the
//                                        interface purge, set the normal temperature again after it;
//   B  wipe_tower_interface_run_in       start the purge filament_tower_interface_pre_extrusion_dist
//                                        outside the tower and run in through a wall gap;
//   C  wipe_tower_interface_extra_prime  push filament_tower_interface_pre_extrusion_length + 2 mm
//                                        through the standing nozzle before the purge.
// Neither generator has Bambu's planner, so an interface is a tool change picked by
// wipe_tower_interface_trigger (every change, material change, material family change), never on
// the tower's first layer. Bambu's firmware purge (M620.13 in the H2 change_filament_gcode) is not
// part of this; its placeholders stay stubbed off in GCode.cpp.
//
// B needs the wall gaps (wipe_tower_wall_gap) and is off without them: the run-in line crosses the
// tower's outer wall only through a gap cut on the interface layer and the GAP_LAYERS - 1 layers
// below, so neither the line nor the blob it drags is pulled over a wall. The tower's reserved area
// (reported through the brim width by both generators and by estimate_wipe_tower_footprint()) grows
// by run_in_reserve(), and the start is clamped to the printable bed.

#include <functional>
#include <string>
#include <vector>

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/Polyline.hpp"

namespace Slic3r {

class ConfigBase;

namespace TowerInterface {

// The interface layer and the layers below it whose outer wall gets the run-in gap
// (Bambu Studio WipeTower.cpp:3183 pre_access_layer).
constexpr int   GAP_LAYERS       = 4;
// Bambu Studio primes pre_extrusion_length + 2 mm before an interface (WipeTower.cpp:3966).
constexpr float EXTRA_PRIME_BASE = 2.f;
// ... standing still, at this feedrate (mm/min).
constexpr float PRIME_FEEDRATE   = 100.f;

struct FilamentKind
{
    std::string type;
    bool        is_support = false;
};

// Material family for the "material family change" trigger: PLA and PLA-CF are one family, so are
// ABS and ASA (one Bambu adhesion category), PETG, PET-CF and PCTG, the PA and PPA grades, the PC
// grades, TPU and TPE, PVA and BVOH. A support filament of a family is a family of its own.
std::string material_family(const std::string &filament_type, bool is_support);

// Whether a tool change from `from` to `to` is a tower interface under `trigger`
// (a TowerInterfaceTrigger value).
bool triggers(int trigger, const FilamentKind &from, const FilamentKind &to);

// The temperature an interface purge waits for: `configured` (filament_tower_interface_print_temp),
// -1 meaning `range_high` (nozzle_temperature_range_high); `normal` when that resolves to nothing.
int interface_temperature(int configured, int range_high, int normal);

struct Settings
{
    bool temp        = false;
    bool run_in      = false; // already false when wipe_tower_wall_gap is off
    bool extra_prime = false;
    int  trigger     = 1;     // titMaterialChange

    bool any() const { return temp || run_in || extra_prime; }
    // Reads the process options; a key the config lacks takes its declared default.
    static Settings from_config(const ConfigBase &config);
};

FilamentKind filament_kind(const ConfigBase &config, unsigned int filament);
float        run_in_distance(const ConfigBase &config, unsigned int filament);

// Room the run-in needs beyond the tower body on every side: the longest run-in distance of a
// filament that can be switched to at an interface between two of `filaments`, plus half of
// `line_width`; 0 when no run-in can happen. The generators and the estimate all use this, so the
// area reserved before slicing covers every run-in the generator can print.
double run_in_reserve(const ConfigBase &config, const std::vector<unsigned int> &filaments, double line_width);
// The same from values the generators already hold, indexed by filament.
double run_in_reserve(const Settings &settings, const std::vector<FilamentKind> &kinds, const std::vector<float> &distances,
                      const std::vector<unsigned int> &filaments, double line_width);

// Box of the printable area every nozzle reaches (printable_area, narrowed by each
// extruder_printable_area on multi-nozzle machines, like Bambu's shared print bed).
BoundingBoxf shared_printable_box(const ConfigBase &config);

// Largest t in [0, dist] for which to_bed(from + t * dir) stays in `bed`. to_bed must be affine.
float clamp_to_bed(const Vec2f &from, const Vec2f &dir, float dist, const std::function<Vec2f(const Vec2f &)> &to_bed, const BoundingBoxf &bed);

// A wall gap: cut where a horizontal ray from `pos` leaves the wall polygon, going left or right.
struct GapPoint
{
    Vec2f pos;
    bool  is_left = true;
};

// The wall `polygon` cut open gap_length along the wall on both sides of each gap point's
// crossing (the nearest one along its ray). insert_skip_polygon gets the wall with the gap end
// points inserted. Implemented in WipeTower2.cpp beside the gap code it generalises.
Polylines cut_wall_gaps(const Polygon &polygon, const std::vector<GapPoint> &points, float gap_length, Polygon &insert_skip_polygon);

} // namespace TowerInterface
} // namespace Slic3r

#endif // slic3r_GCode_WipeTowerInterface_hpp_
