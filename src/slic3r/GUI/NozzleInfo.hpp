#pragma once

// What the slicer knows about a Bambu printer's installed nozzle, kept free of wx so it can be unit
// tested (slic3rutils_tests, [NozzleInfo]).
//
// Orca #12814: firmware that does not report its nozzle leaves the diameter at 0, which means
// "unknown". The slicer used to assume 0.4 mm in that case, so a printer with any other nozzle got
// a mismatch warning on every send and a blocked calibration, and "0.0" filament lookups came back
// empty. A diameter of 0 (or anything not positive) is now unknown everywhere, and every check
// against it passes.

#include <cmath>

namespace Slic3r { namespace GUI { namespace NozzleInfo {

// True when the printer has reported a usable diameter.
inline bool diameter_known(float diameter) { return std::isfinite(diameter) && diameter > 0.f; }

}}} // namespace Slic3r::GUI::NozzleInfo
