#pragma once

#include <algorithm>
#include <cmath>

namespace Slic3r { namespace GUI { namespace Emboss {

// Size of one preview image in the Text tool's style list (CreateFontStyleImagesJob).
// OrcaSlicer #14612: the preview used to be scaled by the style's font size and the screen DPI, so a small
// style rendered as a few unreadable pixels and a large one was cropped to its top-left corner. The text
// shape is now fitted to the row height instead (then the width is cropped), with a margin for the
// anti-aliasing. Edge: the glyphs fit inside the row height including the margin (upstream let the image
// grow 2 * padding taller than the row), and a degenerate shape (no glyphs: empty text, only spaces or only
// inline shape placeholders the font lacks) gives no image instead of a zero scale.
struct StyleImageSize
{
    bool   valid  = false; // false: nothing to draw, leave the image empty
    double scale  = 1.;    // pixels per shape unit
    int    width  = 0;     // pixels, including the padding
    int    height = 0;     // pixels, including the padding
};

// bb_width, bb_height: size of the text shape's bounding box in shape units.
// max_width, max_height: largest image in pixels (the style list's image column and row height).
inline StyleImageSize style_image_size(double bb_width, double bb_height, int max_width, int max_height, int padding)
{
    StyleImageSize out;
    const int glyph_height = max_height - 2 * padding;
    if (bb_width < 1. || bb_height < 1. || glyph_height < 1 || max_width < 1)
        return out;
    out.valid  = true;
    out.scale  = glyph_height / bb_height;
    out.height = max_height;
    out.width  = std::min(max_width, int(std::ceil(bb_width * out.scale)) + 2 * padding);
    return out;
}

}}} // namespace Slic3r::GUI::Emboss
