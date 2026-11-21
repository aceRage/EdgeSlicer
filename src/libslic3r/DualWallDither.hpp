#pragma once

// Dual wall dithering (DwD), phase 1: the colour model and the solved pair table.
//
// Design: tests/design_dual_wall_dithering.md, sections 3.1-3.2. EXPERIMENTAL.
//
// The outer wall prints a short, dense layer sequence of translucent filaments (so layer lines
// blend), the first inner wall prints a longer sequence that is seen through it, and everything
// deeper is one backing filament. A "pair" is (outer sequence, inner sequence, backing). This file
// predicts the colour of a pair from each filament's colour and transmission distance (TD), builds
// the table of distinct colours a filament set can reach, and finds the pair nearest a target.
//
// Nothing here touches slicing: a pair is turned into the grouped manual pattern a mixed filament
// row already understands (MixedFilament.hpp, `manual_pattern`, one comma group per wall).
//
// Model (two-flux, one number per filament):
//   one-way transmission of a wall of width w:  T = exp(-k * w / TD)
//   a wall shows  R * (1 - T^2)  of its own colour and passes  T^2  of what is behind it
//   colour of one layer = outer_own + To^2 * (inner_own + Ti^2 * backing)
//   the eye averages the layers of one full period, in linear light.
// k and the widths are parameters; k is what a calibration print fits.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace Slic3r {

struct DwdFilament
{
    std::array<float, 3> srgb { 1.f, 1.f, 1.f }; // display colour, 0..1 per channel
    double               td_mm = 4.;             // filament_transmission_distance; <= 0 is treated as opaque
};

struct DwdModelParams
{
    double k              = 3.;   // a slab one TD thick passes exp(-k), about 5 %
    double outer_width_mm = 0.45;
    double inner_width_mm = 0.45;
};

struct DwdTableParams
{
    // Strict: outer period <= 4 with at most 2 layers of one filament in a row. Relaxed also
    // allows 3:1 mixes (a run of three).
    bool   relaxed_outer = false;
    // Largest table. Pairs closer than `min_delta` (Oklab distance) to one already kept are
    // dropped; the threshold is raised until the table fits.
    size_t max_pairs = 256;
    double min_delta = 0.02;
};

struct DwdPair
{
    std::vector<uint8_t> outer;       // 0-based filament index per layer, minimal period (1, 2 or 4)
    std::vector<uint8_t> inner;       // 0-based filament index per layer, minimal period (1, 2, 4 or 8)
    uint8_t              backing = 0; // 0-based filament index
    std::array<float, 3> srgb  {};    // predicted colour, 0..1
    std::array<float, 3> oklab {};
    int                  filaments_used = 0; // distinct filaments incl. backing: tool changes per layer <= this
};

// One-way transmission of a wall. 0 for td <= 0 (opaque), always within [0, 1].
double dwd_transmission(double td_mm, double width_mm, double k);

// Predicted colour (sRGB 0..1) of the given sequences over a backing. Sequences hold 0-based
// indices into `filaments`; an empty sequence or an out-of-range index yields black.
std::array<float, 3> dwd_predict(const std::vector<DwdFilament> &filaments, const std::vector<uint8_t> &outer,
                                 const std::vector<uint8_t> &inner, uint8_t backing, const DwdModelParams &model);

// Outer-wall sequences for n filaments: 23 for n = 4 (35 relaxed). One per mix, in its densest order.
std::vector<std::vector<uint8_t>> dwd_outer_sequences(size_t n_filaments, bool relaxed);

// Inner-wall sequences for n filaments: every mix of 8 layers (165 for n = 4), evenly spread.
std::vector<std::vector<uint8_t>> dwd_inner_sequences(size_t n_filaments);

// The reachable colours of a filament set over one backing filament, densest outer wall first.
// Supports 2 to 9 filaments (the pattern token limit); returns empty otherwise.
std::vector<DwdPair> dwd_build_table(const std::vector<DwdFilament> &filaments, uint8_t backing,
                                     const DwdModelParams &model, const DwdTableParams &params = {});

// Index of the table entry nearest the target colour (Oklab), or -1 for an empty table.
int dwd_nearest(const std::vector<DwdPair> &table, const std::array<float, 3> &target_srgb);

// The lightest filament: the natural backing (white in a CMY + W set).
uint8_t dwd_default_backing(const std::vector<DwdFilament> &filaments);

// The pair as a grouped manual pattern, e.g. "12,33334444,4", for a mixed row whose components
// are the 1-based physical filaments component_a and component_b. `physical_ids[i]` is the
// 1-based physical filament of model filament i. Returns "" when a filament cannot be written
// (physical id 1 or 2 that is not one of the row's components, or an id above 9).
std::string dwd_pattern(const DwdPair &pair, const std::vector<int> &physical_ids, int component_a, int component_b);

std::array<float, 3> dwd_srgb_to_oklab(const std::array<float, 3> &srgb);

} // namespace Slic3r
