// SPLIT TO FIT, phase 5 (tests/design_split_to_build_volume.md, 7.5 and "P5 as built"): the shapes
// and the meshes of the labels recessed into the cut faces.
//
// The text uses a small built-in stroke font (digits, A-Z, '.', '-') rather than the Emboss tool's
// TrueType path: that one needs a font file read from the resources at run time, and its strokes
// grow with the text, while a cut-face label must keep every stroke at least two 0.4 mm lines wide
// at any size and give the same shapes on every machine (and in the tests). The 2D shapes are made
// solid with the Emboss tool's own mesher (Emboss::polygons2model), which closes them into a
// watertight prism.

#include "SplitToVolume.hpp"

#include "ClipperUtils.hpp"
#include "Emboss.hpp"
#include "Polyline.hpp"
#include "TriangleMesh.hpp"

#include <algorithm>
#include <cmath>
#include <memory>

namespace Slic3r {

namespace split_labels_detail {

// A glyph's skeleton on a 4 x 6 grid (x right, y up), as polylines.
struct GlyphPoint { float x, y; };
using GlyphStroke = std::vector<GlyphPoint>;
using Glyph       = std::vector<GlyphStroke>;

static const Glyph *glyph(char c)
{
    static const Glyph G0 = { { { 1, 0 }, { 3, 0 }, { 4, 1 }, { 4, 5 }, { 3, 6 }, { 1, 6 }, { 0, 5 }, { 0, 1 }, { 1, 0 } } };
    static const Glyph G1 = { { { 1, 5 }, { 2, 6 }, { 2, 0 } }, { { 1, 0 }, { 3, 0 } } };
    static const Glyph G2 = { { { 0, 5 }, { 1, 6 }, { 3, 6 }, { 4, 5 }, { 4, 4 }, { 0, 0 }, { 4, 0 } } };
    static const Glyph G3 = { { { 0, 5 }, { 1, 6 }, { 3, 6 }, { 4, 5 }, { 4, 4 }, { 3, 3 }, { 1, 3 } },
                              { { 3, 3 }, { 4, 2 }, { 4, 1 }, { 3, 0 }, { 1, 0 }, { 0, 1 } } };
    static const Glyph G4 = { { { 3, 0 }, { 3, 6 }, { 0, 2 }, { 4, 2 } } };
    static const Glyph G5 = { { { 4, 6 }, { 0, 6 }, { 0, 3 }, { 3, 3 }, { 4, 2 }, { 4, 1 }, { 3, 0 }, { 0, 0 } } };
    static const Glyph G6 = { { { 4, 6 }, { 1, 6 }, { 0, 5 }, { 0, 1 }, { 1, 0 }, { 3, 0 }, { 4, 1 }, { 4, 2 }, { 3, 3 }, { 0, 3 } } };
    static const Glyph G7 = { { { 0, 6 }, { 4, 6 }, { 1, 0 } } };
    static const Glyph G8 = { { { 1, 3 }, { 0, 4 }, { 0, 5 }, { 1, 6 }, { 3, 6 }, { 4, 5 }, { 4, 4 }, { 3, 3 }, { 1, 3 }, { 0, 2 }, { 0, 1 }, { 1, 0 },
                                { 3, 0 }, { 4, 1 }, { 4, 2 }, { 3, 3 } } };
    static const Glyph G9 = { { { 0, 0 }, { 3, 0 }, { 4, 1 }, { 4, 5 }, { 3, 6 }, { 1, 6 }, { 0, 5 }, { 0, 4 }, { 1, 3 }, { 4, 3 } } };
    static const Glyph GDash = { { { 0.5f, 3 }, { 3.5f, 3 } } };
    static const Glyph GA = { { { 0, 0 }, { 0, 4 }, { 2, 6 }, { 4, 4 }, { 4, 0 } }, { { 0, 3 }, { 4, 3 } } };
    static const Glyph GB = { { { 0, 0 }, { 0, 6 }, { 3, 6 }, { 4, 5 }, { 4, 4 }, { 3, 3 }, { 0, 3 } },
                              { { 3, 3 }, { 4, 2 }, { 4, 1 }, { 3, 0 }, { 0, 0 } } };
    static const Glyph GC = { { { 4, 6 }, { 1, 6 }, { 0, 5 }, { 0, 1 }, { 1, 0 }, { 4, 0 } } };
    static const Glyph GD = { { { 0, 0 }, { 0, 6 }, { 2, 6 }, { 4, 4 }, { 4, 2 }, { 2, 0 }, { 0, 0 } } };
    static const Glyph GE = { { { 4, 6 }, { 0, 6 }, { 0, 0 }, { 4, 0 } }, { { 0, 3 }, { 3, 3 } } };
    static const Glyph GF = { { { 4, 6 }, { 0, 6 }, { 0, 0 } }, { { 0, 3 }, { 3, 3 } } };
    static const Glyph GG = { { { 4, 5 }, { 3, 6 }, { 1, 6 }, { 0, 5 }, { 0, 1 }, { 1, 0 }, { 3, 0 }, { 4, 1 }, { 4, 3 }, { 2, 3 } } };
    static const Glyph GH = { { { 0, 0 }, { 0, 6 } }, { { 4, 0 }, { 4, 6 } }, { { 0, 3 }, { 4, 3 } } };
    static const Glyph GI = { { { 1, 0 }, { 3, 0 } }, { { 2, 0 }, { 2, 6 } }, { { 1, 6 }, { 3, 6 } } };
    static const Glyph GJ = { { { 4, 6 }, { 4, 1 }, { 3, 0 }, { 1, 0 }, { 0, 1 } } };
    static const Glyph GK = { { { 0, 0 }, { 0, 6 } }, { { 4, 6 }, { 0, 2 } }, { { 1.4f, 3.4f }, { 4, 0 } } };
    static const Glyph GL = { { { 0, 6 }, { 0, 0 }, { 4, 0 } } };
    static const Glyph GM = { { { 0, 0 }, { 0, 6 }, { 2, 3 }, { 4, 6 }, { 4, 0 } } };
    static const Glyph GN = { { { 0, 0 }, { 0, 6 }, { 4, 0 }, { 4, 6 } } };
    static const Glyph GO = G0;
    static const Glyph GP = { { { 0, 0 }, { 0, 6 }, { 3, 6 }, { 4, 5 }, { 4, 4 }, { 3, 3 }, { 0, 3 } } };
    static const Glyph GQ = { { { 1, 0 }, { 3, 0 }, { 4, 1 }, { 4, 5 }, { 3, 6 }, { 1, 6 }, { 0, 5 }, { 0, 1 }, { 1, 0 } }, { { 2.5f, 1.5f }, { 4, 0 } } };
    static const Glyph GR = { { { 0, 0 }, { 0, 6 }, { 3, 6 }, { 4, 5 }, { 4, 4 }, { 3, 3 }, { 0, 3 } }, { { 2, 3 }, { 4, 0 } } };
    static const Glyph GS = { { { 4, 5 }, { 3, 6 }, { 1, 6 }, { 0, 5 }, { 0, 4 }, { 1, 3 }, { 3, 3 }, { 4, 2 }, { 4, 1 }, { 3, 0 }, { 1, 0 }, { 0, 1 } } };
    static const Glyph GT = { { { 0, 6 }, { 4, 6 } }, { { 2, 6 }, { 2, 0 } } };
    static const Glyph GU = { { { 0, 6 }, { 0, 1 }, { 1, 0 }, { 3, 0 }, { 4, 1 }, { 4, 6 } } };
    static const Glyph GV = { { { 0, 6 }, { 2, 0 }, { 4, 6 } } };
    static const Glyph GW = { { { 0, 6 }, { 1, 0 }, { 2, 3 }, { 3, 0 }, { 4, 6 } } };
    static const Glyph GX = { { { 0, 0 }, { 4, 6 } }, { { 0, 6 }, { 4, 0 } } };
    static const Glyph GY = { { { 0, 6 }, { 2, 3 }, { 4, 6 } }, { { 2, 3 }, { 2, 0 } } };
    static const Glyph GZ = { { { 0, 6 }, { 4, 6 }, { 0, 0 }, { 4, 0 } } };

    static const Glyph *const digits[10]  = { &G0, &G1, &G2, &G3, &G4, &G5, &G6, &G7, &G8, &G9 };
    static const Glyph *const letters[26] = { &GA, &GB, &GC, &GD, &GE, &GF, &GG, &GH, &GI, &GJ, &GK, &GL, &GM,
                                              &GN, &GO, &GP, &GQ, &GR, &GS, &GT, &GU, &GV, &GW, &GX, &GY, &GZ };
    if (c >= '0' && c <= '9')
        return digits[c - '0'];
    if (c >= 'a' && c <= 'z')
        c = char(c - 'a' + 'A');
    if (c >= 'A' && c <= 'Z')
        return letters[c - 'A'];
    if (c == '-')
        return &GDash;
    return nullptr;
}

// Glyph skeleton width per unit of skeleton height.
static constexpr double GLYPH_ASPECT = 0.6;

static Polygon scaled_square(double cx, double cy, double side)
{
    const double h = 0.5 * side;
    Polygon      p;
    p.points = { Point::new_scale(cx - h, cy - h), Point::new_scale(cx + h, cy - h), Point::new_scale(cx + h, cy + h),
                 Point::new_scale(cx - h, cy + h) };
    return p;
}

} // namespace split_labels_detail

ExPolygons split_label_text_shapes(const std::string &text, double height, double stroke, double *width)
{
    using namespace split_labels_detail;
    if (width != nullptr)
        *width = 0.;
    if (text.empty() || height <= 0. || stroke <= 0. || stroke >= height)
        return {};
    // The skeleton fits inside the glyph's height less its stroke, so the stroked glyph is `height` tall.
    const double sh  = height - stroke;
    const double sw  = GLYPH_ASPECT * sh;
    const double gap = std::max(stroke, 0.15 * height);
    const double ux  = sw / 4.;
    const double uy  = sh / 6.;

    Polylines lines;
    Polygons  dots;
    double    x = 0.;   // left edge of the next glyph's stroked box
    bool      first = true;
    for (char c : text) {
        if (!first)
            x += gap;
        first = false;
        if (c == '.') {
            // A square dot on the baseline, as wide as a stroke.
            dots.push_back(scaled_square(x + 0.5 * stroke, 0.5 * stroke, stroke));
            x += stroke;
            continue;
        }
        if (const Glyph *g = glyph(c)) {
            const double ox = x + 0.5 * stroke, oy = 0.5 * stroke;
            for (const GlyphStroke &s : *g) {
                Polyline pl;
                for (const GlyphPoint &p : s)
                    pl.points.push_back(Point::new_scale(ox + ux * double(p.x), oy + uy * double(p.y)));
                lines.push_back(std::move(pl));
            }
        }
        // An unknown character leaves a gap as wide as a glyph.
        x += sw + stroke;
    }
    const double total = x;

    // Round ends and joins, within 0.01 mm of a true arc.
    Polygons shapes = offset(lines, float(scale_(0.5 * stroke)), ClipperLib::jtRound, scale_(0.01), ClipperLib::etOpenRound);
    append(shapes, std::move(dots));
    ExPolygons out = union_ex(shapes);
    // Centred on the origin.
    const Point shift = Point::new_scale(-0.5 * total, -0.5 * height);
    for (ExPolygon &ex : out)
        ex.translate(shift);
    if (width != nullptr)
        *width = total;
    return out;
}

ExPolygons split_label_pattern_shapes(int number, double stroke, double *size)
{
    using namespace split_labels_detail;
    if (size != nullptr)
        *size = 0.;
    if (stroke <= 0.)
        return {};
    const double pitch = 2. * stroke;
    const double total = 2. * pitch + stroke;
    Polygons     dots;
    int          bit = 0;
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col) {
            // Row 0 is the top; the top-left dot is the marker.
            const bool marker = row == 0 && col == 0;
            const bool on     = marker || ((number >> bit) & 1) != 0;
            if (!marker)
                ++bit;
            if (!on)
                continue;
            const double cx = -0.5 * total + 0.5 * stroke + double(col) * pitch;
            const double cy = 0.5 * total - 0.5 * stroke - double(row) * pitch;
            dots.push_back(scaled_square(cx, cy, stroke));
        }
    if (size != nullptr)
        *size = total;
    return union_ex(dots);
}

indexed_triangle_set split_label_mesh(const SplitLabel &label)
{
    // How far the recess reaches out of the face: Apply may move a plane up to about 0.07 mm off the
    // mesh's vertices, and the recess must still open at the face.
    static constexpr double OUTSIDE = 0.2;
    ExPolygons shapes = label.pattern ? split_label_pattern_shapes(label.number, label.stroke) :
                                        split_label_text_shapes(label.text, label.height, label.stroke);
    if (shapes.empty() || label.depth <= 0.)
        return {};
    const double total = label.depth + OUTSIDE;
    Emboss::ProjectScale projection(std::make_unique<Emboss::ProjectZ>(total / SCALING_FACTOR), SCALING_FACTOR);
    indexed_triangle_set its = Emboss::polygons2model(shapes, projection);
    // Local (x right, y up, z from the outside in) to the grid frame.
    Transform3d m = Transform3d::Identity();
    m.linear().col(0) = label.right.normalized();
    m.linear().col(1) = label.up.normalized();
    m.linear().col(2) = -label.normal.normalized();
    m.translation()   = label.center + OUTSIDE * label.normal.normalized();
    its_transform(its, m);
    if (its_volume(its) < 0.)
        its_flip_triangles(its);
    return its;
}

} // namespace Slic3r
