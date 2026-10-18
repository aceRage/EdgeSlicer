#ifndef slic3r_GUI_ShowcaseWatermark_hpp_
#define slic3r_GUI_ShowcaseWatermark_hpp_

// Showcase tab, D12 (owner, 2026-10-18): the user's own branding - a logo and/or a line of text -
// drawn over stills (and, from S4, video frames) on the CPU, so the screen preview and every export
// size come out the same. The layout maths is libslic3r's (Showcase::layout_watermark, unit tested).
//
// Watermark presets are the user's, kept across projects in <data dir>/showcase/watermarks.json
// with their logos beside it. A scene carries a full copy of the watermark it uses (and its logo
// in the 3MF), so a project opened elsewhere still shows it.

#include "ShowcaseRenderer.hpp"   // ShowcaseImage

#include "libslic3r/Showcase/ShowcaseScene.hpp"

#include <string>
#include <vector>

namespace Slic3r {
namespace GUI {

// The mark for an image of image_w x image_h: its pixels and where they go (origin top-left).
// Returns false when there is nothing to draw (disabled, empty, or the logo cannot be decoded and
// there is no text).
bool showcase_rasterize_watermark(const Showcase::Watermark &wm, const Showcase::Project &project, int image_w, int image_h,
                                  ShowcaseImage &mark, Showcase::RectI &where);

// Draws the scene's watermark onto an exported image (no-op when it is off).
void showcase_apply_watermark(ShowcaseImage &image, const Showcase::Watermark &wm, const Showcase::Project &project);

// Decodes a PNG/JPEG (anything wxImage reads) into RGBA, top row first.
bool showcase_decode_image(const std::vector<uint8_t> &bytes, ShowcaseImage &out);

// Reads an image file for use as a backdrop or a logo: downscaled to at most `max_side` pixels and
// re-encoded - JPEG unless it has transparency (design section 5), or PNG always (`always_png`, for
// logos). `ext` gets "png" or "jpg".
bool showcase_load_image_file(const std::string &path, int max_side, bool always_png, std::vector<uint8_t> &bytes, std::string &ext,
                              std::string &error);

class WatermarkPresets
{
public:
    struct Preset
    {
        std::string          name;
        Showcase::Watermark  watermark;   // logo field: the logo's hash
        std::vector<uint8_t> logo;        // encoded logo, empty without one
        std::string          logo_ext;
    };

    // <data dir>/showcase/watermarks.json
    static std::string file_path();

    // Missing or unreadable file: no presets (never fatal).
    void load();
    bool save(std::string *error = nullptr) const;

    const std::vector<Preset> &presets() const { return m_presets; }
    const Preset              *find(const std::string &name) const;
    // Adds or replaces the preset of that name.
    void                       put(Preset preset);
    void                       remove(const std::string &name);

private:
    std::vector<Preset> m_presets;
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_ShowcaseWatermark_hpp_
