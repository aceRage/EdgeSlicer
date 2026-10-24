#include <catch2/catch.hpp>

#include <cmath>
#include <utility>

#include "slic3r/GUI/Jobs/FontStyleImageSize.hpp"

using namespace Slic3r::GUI::Emboss;

// OrcaSlicer #14612 (5B-4): the Text tool's style list previews were scaled by each style's font size and the
// screen DPI, so small styles came out as a few pixels and large ones were cropped to a corner. The preview is
// now fitted to the row height. Edge adaptations: the glyphs plus the anti-aliasing margin fit inside the row
// (no taller image than the row), and a style whose text has no glyphs (empty, spaces only, or only inline
// shape placeholders the font lacks) gives no image rather than a zero scale and a division by zero.

namespace {
constexpr int row_width  = 120;
constexpr int row_height = 18;
constexpr int padding    = 2;
} // namespace

TEST_CASE("Style preview fits the row height whatever the font size", "[Emboss][StylePreview]")
{
    // The same text at a small and a large font size: same image, scaled the other way.
    // (powers of two, so the scaled sizes are exact)
    const StyleImageSize small = style_image_size(512., 128., row_width, row_height, padding);
    const StyleImageSize large = style_image_size(524288., 131072., row_width, row_height, padding);
    REQUIRE(small.valid);
    REQUIRE(large.valid);
    CHECK(small.height == row_height);
    CHECK(large.height == row_height);
    CHECK(small.width == large.width);
    CHECK(small.scale == Approx(large.scale * 1024.));
    // glyphs, then the margin on both sides: 4:1 text over 14 px is 56 px wide
    CHECK(small.width == 56 + 2 * padding);
}

TEST_CASE("Style preview glyphs and margin stay inside the row", "[Emboss][StylePreview]")
{
    for (double bb_height : { 1., 7., 99., 1234.5, 1e6 }) {
        const StyleImageSize s = style_image_size(bb_height * 3., bb_height, row_width, row_height, padding);
        REQUIRE(s.valid);
        CHECK(bb_height * s.scale + 2 * padding <= row_height + 1e-9);
        CHECK(s.height <= row_height);
        CHECK(s.width <= row_width);
    }
}

TEST_CASE("Wide style preview is cropped to the image column", "[Emboss][StylePreview]")
{
    const StyleImageSize s = style_image_size(10000., 100., row_width, row_height, padding);
    REQUIRE(s.valid);
    CHECK(s.width == row_width);
    CHECK(s.height == row_height);
    // Cropping does not shrink the text: it keeps the row height.
    CHECK(100. * s.scale == Approx(row_height - 2 * padding));
}

TEST_CASE("Style preview without glyphs gives no image and no zero scale", "[Emboss][StylePreview]")
{
    for (const auto &bb : { std::make_pair(0., 0.), std::make_pair(500., 0.), std::make_pair(0., 500.), std::make_pair(0.5, 0.5) }) {
        const StyleImageSize s = style_image_size(bb.first, bb.second, row_width, row_height, padding);
        CHECK_FALSE(s.valid);
        CHECK(s.width == 0);
        CHECK(s.height == 0);
        CHECK(s.scale > 0.);
        CHECK(std::isfinite(1. / s.scale));
    }
    // A row too small for the margin.
    CHECK_FALSE(style_image_size(400., 100., row_width, 2 * padding, padding).valid);
    CHECK_FALSE(style_image_size(400., 100., 0, row_height, padding).valid);
}
