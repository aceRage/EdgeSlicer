#include "BlueNoise.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>

namespace Slic3r {
namespace Airbrush {

uint32_t airbrush_hash(uint32_t a, uint32_t b, uint32_t c)
{
    uint32_t h = 2166136261u;
    for (uint32_t v : { a, b, c }) {
        h ^= v + 0x9E3779B9u + (h << 6) + (h >> 2);
        h *= 16777619u;
    }
    return h;
}

std::vector<float> make_blue_noise_mask(int size)
{
    assert(size > 0 && (size & (size - 1)) == 0);
    const int    S     = size;
    const int    N     = S * S;
    const double sigma = 1.9;
    // Toroidal Gaussian kernel by offset.
    std::vector<double> kern(size_t(N), 0.);
    for (int dy = 0; dy < S; ++dy)
        for (int dx = 0; dx < S; ++dx) {
            const int ddx = std::min(dx, S - dx), ddy = std::min(dy, S - dy);
            kern[size_t(dy * S + dx)] = std::exp(-double(ddx * ddx + ddy * ddy) / (2. * sigma * sigma));
        }
    // The kernel is negligible past ~6 sigma; only that window is updated, which keeps the
    // generator O(N * window) instead of O(N^2) per step.
    const int R = std::min(S / 2, int(std::ceil(6. * sigma)));
    auto add_energy = [&](std::vector<double> &e, int idx, double sign) {
        const int x0 = idx % S, y0 = idx / S;
        for (int dy = -R; dy <= R; ++dy)
            for (int dx = -R; dx <= R; ++dx) {
                const int x = (x0 + dx + S) & (S - 1), y = (y0 + dy + S) & (S - 1);
                e[size_t(y * S + x)] += sign * kern[size_t(((dy + S) & (S - 1)) * S + ((dx + S) & (S - 1)))];
            }
    };
    // Tightest cluster: the set cell with the highest energy; largest void: the unset cell with the
    // lowest. Ties go to the lowest index, so the result never depends on luck.
    auto extreme = [&](const std::vector<char> &bits, const std::vector<double> &e, char want, bool highest) {
        int best = -1;
        for (int i = 0; i < N; ++i)
            if (bits[size_t(i)] == want && (best < 0 || (highest ? e[size_t(i)] > e[size_t(best)] : e[size_t(i)] < e[size_t(best)])))
                best = i;
        return best;
    };

    // Initial pattern: ~10% ones from a fixed LCG, then relaxed until stable.
    std::vector<char> proto(size_t(N), 0);
    uint32_t          lcg  = 0x2545F491u;
    int               ones = 0;
    while (ones < N / 10) {
        lcg         = lcg * 1664525u + 1013904223u;
        const int i = int((lcg >> 8) % uint32_t(N));
        if (!proto[size_t(i)]) {
            proto[size_t(i)] = 1;
            ++ones;
        }
    }
    std::vector<double> e(size_t(N), 0.);
    for (int i = 0; i < N; ++i)
        if (proto[size_t(i)])
            add_energy(e, i, 1.);
    for (int iter = 0; iter < 4 * N; ++iter) {
        const int c       = extreme(proto, e, 1, true);
        proto[size_t(c)]  = 0;
        add_energy(e, c, -1.);
        const int v       = extreme(proto, e, 0, false);
        proto[size_t(v)]  = 1;
        add_energy(e, v, 1.);
        if (v == c)
            break;
    }

    std::vector<int> rank(size_t(N), -1);
    // Phase 1: rank the prototype's ones by removing tightest clusters.
    {
        std::vector<char>   bits = proto;
        std::vector<double> en   = e;
        for (int r = ones - 1; r >= 0; --r) {
            const int c      = extreme(bits, en, 1, true);
            bits[size_t(c)]  = 0;
            add_energy(en, c, -1.);
            rank[size_t(c)] = r;
        }
    }
    // Phases 2 and 3: fill the voids up to all ones. Past half the tile the "largest void" of the
    // zeros is the same cell as the "tightest cluster" of the zeros taken as minority pixels, so a
    // single rule (lowest energy among the zeros) covers both on a symmetric kernel.
    {
        std::vector<char>   bits = proto;
        std::vector<double> en   = e;
        for (int r = ones; r < N; ++r) {
            const int v     = extreme(bits, en, 0, false);
            bits[size_t(v)] = 1;
            add_energy(en, v, 1.);
            rank[size_t(v)] = r;
        }
    }
    std::vector<float> t(static_cast<size_t>(N));
    for (int i = 0; i < N; ++i)
        t[size_t(i)] = (float(rank[size_t(i)]) + 0.5f) / float(N);
    return t;
}

const std::vector<float> &blue_noise_mask()
{
    // C++11 guarantees thread-safe initialisation of a function-local static.
    static const std::vector<float> mask = make_blue_noise_mask(BLUE_NOISE_SIZE);
    return mask;
}

float blue_noise_threshold(int64_t i, int64_t j, uint32_t seed)
{
    const std::vector<float> &m  = blue_noise_mask();
    const uint32_t            h  = airbrush_hash(seed, 0xA1B2C3D4u);
    const int64_t             ox = int64_t(h & uint32_t(BLUE_NOISE_SIZE - 1));
    const int64_t             oy = int64_t((h >> 8) & uint32_t(BLUE_NOISE_SIZE - 1));
    const int64_t             x  = (i + ox) & int64_t(BLUE_NOISE_SIZE - 1);
    const int64_t             y  = (j + oy) & int64_t(BLUE_NOISE_SIZE - 1);
    return m[size_t(y * BLUE_NOISE_SIZE + x)];
}

} // namespace Airbrush
} // namespace Slic3r
