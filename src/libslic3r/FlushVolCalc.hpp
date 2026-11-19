#ifndef slic3r_FlushVolCalc_hpp_
#define slic3r_FlushVolCalc_hpp_

#include "libslic3r.h"
#include "Config.hpp"


namespace Slic3r {

extern const int g_min_flush_volume_from_support;
extern const int g_flush_volume_to_support;
// Orca #11778: relaxed from 800 so printers that need more purge (high flow, U1 colour changes) can keep it;
// the clamp only guards against extreme values typed in the UI.
inline constexpr int g_max_flush_volume = 20000;
// The upper end of the "Suggestion: Flushing Volume in range [min, max]" guidance (the old cap).
inline constexpr int g_typical_max_flush_volume = 800;

class FlushVolCalculator
{
public:
    FlushVolCalculator(int min, int max, float multiplier = 1.0f);
    ~FlushVolCalculator()
    {
    }

    int calc_flush_vol(unsigned char src_a, unsigned char src_r, unsigned char src_g, unsigned char src_b,
        unsigned char dst_a, unsigned char dst_r, unsigned char dst_g, unsigned char dst_b);

private:
    int m_min_flush_vol;
    int m_max_flush_vol;
    float m_multiplier;
};


}

#endif
