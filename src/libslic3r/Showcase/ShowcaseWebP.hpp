#ifndef slic3r_Showcase_ShowcaseWebP_hpp_
#define slic3r_Showcase_ShowcaseWebP_hpp_

// Showcase tab, phase S2: WebP export. The dependencies have no libwebp and wxWidgets has no WebP
// handler, so this is a small encoder of lossless WebP (VP8L, RFC 9649) of our own: the subtract
// green and predictor transforms, LZ77 back references and canonical prefix codes. No colour
// cache, no colour transform, one prefix code group: files come out near PNG size for rendered
// images, every WebP reader opens them, and nothing is lost (alpha included).

#include <cstdint>
#include <string>
#include <vector>

namespace Slic3r {
namespace Showcase {

// `rgba` is width x height RGBA8, top row first, straight alpha. Returns the bytes of a .webp file,
// or nothing for an empty image or one wider or taller than 16384 (the format's limit).
std::vector<uint8_t> encode_webp_lossless(const std::vector<uint8_t> &rgba, int width, int height);

} // namespace Showcase
} // namespace Slic3r

#endif // slic3r_Showcase_ShowcaseWebP_hpp_
