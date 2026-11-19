#ifndef slic3r_Airbrush_BlueNoise_hpp_
#define slic3r_Airbrush_BlueNoise_hpp_

// Airbrush (tests/design_airbrush_paint.md, D6): the ordered blue-noise threshold mask the bake
// compares coverage against. Ordered dithering is local and deterministic: changing the paint in
// one place never reshuffles specks anywhere else, which error diffusion would.
//
// The mask is generated in code (void-and-cluster, Ulichney 1993) rather than shipped as a file, so
// there is no third-party texture and no licence question (design section 7). It is the same
// generator the P0 calibration card used (tests/libslic3r/test_airbrush_p0_card.cpp, PR #779),
// at 64 x 64 instead of 32 x 32 so a tile repeats less often.

#include <cstdint>
#include <vector>

namespace Slic3r {
namespace Airbrush {

// Side of the square, toroidal mask.
static constexpr int BLUE_NOISE_SIZE = 64;

// The mask: BLUE_NOISE_SIZE^2 thresholds, row-major, each (rank + 0.5) / N, so the values are a
// permutation of the N evenly spaced levels in (0, 1). "Lit when t < coverage" lights round(coverage
// * N) cells of a full tile, spread as evenly as the method can. Built once, on first use, and
// identical on every run of one build (integer LCG start, ties broken by lowest index). Energies are
// doubles from std::exp, so a different libm could break a near-tie differently: a re-bake on
// another platform may move specks; the saved paint itself never changes on load.
const std::vector<float> &blue_noise_mask();

// The generator itself, exposed for the tests. `size` must be a power of two.
std::vector<float> make_blue_noise_mask(int size);

// The threshold at integer cell (i, j) for a tile offset derived from `seed`.
float blue_noise_threshold(int64_t i, int64_t j, uint32_t seed);

// A small stable integer hash (FNV-1a style), used for tile offsets and stroke scatter.
uint32_t airbrush_hash(uint32_t a, uint32_t b, uint32_t c = 0);

} // namespace Airbrush
} // namespace Slic3r

#endif // slic3r_Airbrush_BlueNoise_hpp_
