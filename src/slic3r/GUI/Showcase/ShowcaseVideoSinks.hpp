#ifndef slic3r_GUI_ShowcaseVideoSinks_hpp_
#define slic3r_GUI_ShowcaseVideoSinks_hpp_

// Showcase tab, S4 (D5): the platform's own video encoder. On macOS an MP4 is written with
// AVFoundation (AVAssetWriter, hardware H.264 through VideoToolbox), since EdgeSlicer ships no ffmpeg
// there; Windows and Linux pipe to ffmpeg (libslic3r/Showcase/ShowcaseEncoder.hpp) instead.

#include "libslic3r/Showcase/ShowcaseEncoder.hpp"

#include <memory>
#include <string>

namespace Slic3r {
namespace GUI {

// True where showcase_make_native_mp4_sink() gives a sink (macOS).
bool showcase_has_native_mp4();

// An H.264 MP4 writer at `bitrate_kbps`; null where there is none.
std::unique_ptr<Showcase::FrameSink> showcase_make_native_mp4_sink(const std::string &path, int width, int height, int fps, int bitrate_kbps);

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_ShowcaseVideoSinks_hpp_
