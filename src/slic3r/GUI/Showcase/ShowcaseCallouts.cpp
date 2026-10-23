#include "ShowcaseCallouts.hpp"
#include "ShowcaseTextures.hpp"   // showcase_filaments, showcase_finish_name

#include "libslic3r/CostOverrides.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/PresetBundle.hpp"

#include "../CostsDialog.hpp"   // currency_symbol
#include "../GUI.hpp"
#include "../GUI_App.hpp"
#include "../I18N.hpp"
#include "../PartPlate.hpp"
#include "../Plater.hpp"

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/font.h>
#include <wx/graphics.h>
#include <wx/image.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#include <boost/log/trivial.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <memory>

namespace Slic3r {
namespace GUI {

namespace {

wxColour callout_colour(const std::string &hex, unsigned char alpha = 255)
{
    std::array<float, 4> c{ 0.f, 0.f, 0.f, 1.f };
    Showcase::parse_hex_color(hex, c);
    return wxColour((unsigned char) std::lround(c[0] * 255.f), (unsigned char) std::lround(c[1] * 255.f),
                    (unsigned char) std::lround(c[2] * 255.f), alpha);
}

// S5: the accent a path's "highlight" draws round a callout.
wxColour showcase_highlight_colour() { return wxColour(255, 138, 0); }

wxColour callout_with_alpha(const wxColour &c, double a)
{
    return wxColour(c.Red(), c.Green(), c.Blue(), (unsigned char) std::lround(std::clamp(a, 0., 1.) * 255.));
}

wxFont callout_font(const Showcase::CalloutStyle &st, double px, bool bold)
{
    const int  size = std::max(1, int(std::lround(px)));
    wxFontInfo info(wxSize(0, size));
    if (!st.font_face.empty())
        info.FaceName(wxString::FromUTF8(st.font_face));
    else if (st.shape == Showcase::CalloutShape::Technical)
        info.Family(wxFONTFAMILY_TELETYPE);
    info.Bold(bold);
    wxFont font(info);
    if (!font.IsOk())
        font = wxFont(wxFontInfo(wxSize(0, size)).Bold(bold));
    return font;
}

// The label's pieces and sizes, in image pixels.
struct MeasuredLabel
{
    int         draw = -1;   // index into the callouts given
    std::string line1, hex, details, note;
    bool        swatch = false;
    double      w1 = 0., hex_w = 0., details_w = 0., note_w = 0.;
    double      w = 0., h = 0.;
};

} // namespace

// ------------------------------------------------------------------------------------------------
// The rasteriser (D6)
// ------------------------------------------------------------------------------------------------

void showcase_draw_callouts(ShowcaseImage &image, bool has_alpha, const Showcase::Scene &scene,
                            const std::vector<ShowcaseCalloutDraw> &callouts, const ShowcaseCalloutOptions &opt, ShowcaseCalloutHits *hits)
{
    using namespace Showcase;
    if (hits != nullptr)
        *hits = ShowcaseCalloutHits();
    if (image.empty() || callouts.empty())
        return;
    const int W = image.width, H = image.height;
    if (hits != nullptr) {
        hits->width  = W;
        hits->height = H;
    }
    const CalloutStyle &st = scene.callouts.style;
    // Everything is sized from the text height, a share of the image height: the same picture at any size.
    const double F  = std::max(6., double(st.size_pct) / 100. * double(H));
    const double Fs = 0.8 * F;

    wxImage canvas(W, H, false);
    {
        unsigned char *rgb = canvas.GetData();
        for (size_t i = 0; i < size_t(W) * size_t(H); ++i) {
            rgb[i * 3 + 0] = image.rgba[i * 4 + 0];
            rgb[i * 3 + 1] = image.rgba[i * 4 + 1];
            rgb[i * 3 + 2] = image.rgba[i * 4 + 2];
        }
        if (has_alpha) {
            canvas.InitAlpha();
            unsigned char *a = canvas.GetAlpha();
            for (size_t i = 0; i < size_t(W) * size_t(H); ++i)
                a[i] = image.rgba[i * 4 + 3];
        }
    }

    {
        std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(canvas));
        if (!gc) {
            BOOST_LOG_TRIVIAL(warning) << "Showcase: no graphics context to draw the callouts";
            return;
        }
        gc->SetAntialiasMode(wxANTIALIAS_DEFAULT);
        gc->SetInterpolationQuality(wxINTERPOLATION_GOOD);
        const wxFont font_main  = callout_font(st, F, st.bold);
        const wxFont font_small = callout_font(st, Fs, false);
        auto extent = [&gc](const wxFont &font, const std::string &text, double &w, double &h) {
            w = h = 0.;
            if (text.empty())
                return;
            gc->SetFont(font, *wxBLACK);
            wxDouble tw = 0, th = 0, descent = 0, leading = 0;
            gc->GetTextExtent(wxString::FromUTF8(text), &tw, &th, &descent, &leading);
            w = tw;
            h = th;
        };
        double dummy = 0., h_main = 0., h_small = 0.;
        extent(font_main, "Ag", dummy, h_main);
        extent(font_small, "Ag", dummy, h_small);
        h_main  = std::max(h_main, F);
        h_small = std::max(h_small, Fs);

        const bool   boxed     = st.shape == CalloutShape::Card || st.shape == CalloutShape::Pill;
        const double pad_x     = st.shape == CalloutShape::Card ? 0.6 * F : st.shape == CalloutShape::Pill ? 0.9 * F : 0.2 * F;
        const double pad_y     = st.shape == CalloutShape::Card ? 0.4 * F : st.shape == CalloutShape::Pill ? 0.3 * F : 0.1 * F;
        const double line_gap  = 0.15 * F;
        const double sw        = 0.9 * h_main;           // swatch
        const double sw_gap    = 0.4 * F;
        const double underline = st.shape == CalloutShape::Technical ? 0.3 * F : 0.;
        const double lw        = std::max(1., 0.085 * F);   // leader width
        const double dot_r     = std::max(2., 0.22 * F);
        const double badge_r   = 0.62 * F;

        // --- measure ---------------------------------------------------------------------------
        std::vector<MeasuredLabel> measured;
        for (size_t i = 0; i < callouts.size(); ++i) {
            const CalloutLabel &l = callouts[i].label;
            if (l.empty())
                continue;
            MeasuredLabel m;
            m.draw    = int(i);
            m.line1   = callout_first_line(l);
            m.swatch  = !l.swatches.empty();
            m.hex     = l.hex;   // S5b: the hex code and the swatch are separate fields
            m.details = l.details;
            m.note    = l.note;
            double h  = 0.;
            extent(font_main, m.line1, m.w1, h);
            extent(font_small, m.hex, m.hex_w, h);
            extent(font_small, m.details, m.details_w, h);
            extent(font_small, m.note, m.note_w, h);
            const double first = (m.swatch ? sw + (m.line1.empty() && m.hex.empty() ? 0. : sw_gap) : 0.) + m.w1 +
                                 (m.hex.empty() ? 0. : (m.line1.empty() ? 0. : 0.5 * F) + m.hex_w);
            const double content_w = std::max({ first, m.details_w, m.note_w });
            double       content_h = h_main;
            if (!m.details.empty())
                content_h += line_gap + h_small;
            if (!m.note.empty())
                content_h += line_gap + h_small;
            m.w = std::ceil(content_w + 2. * pad_x);
            m.h = std::ceil(content_h + 2. * pad_y + underline);
            measured.push_back(std::move(m));
        }
        if (measured.empty())
            return;

        // --- lay out ---------------------------------------------------------------------------
        LayoutParams p;
        p.frame_w        = W;
        p.frame_h        = H;
        p.margin         = std::round(0.035 * double(std::min(W, H)));
        p.gap            = 0.45 * F;
        p.landing        = st.leader == LeaderStyle::Elbow ? 1.1 * F : 0.;
        p.leader         = st.leader;
        p.mode           = scene.callouts.layout;
        p.max_per_column = scene.callouts.max_per_column;
        p.legend_badge   = 2. * badge_r + 0.5 * F;
        p.model_center_x = opt.model_center_x >= 0. ? opt.model_center_x * W : -1.;
        if (!opt.reserved.empty())
            p.reserved.push_back({ opt.reserved.x - p.gap, opt.reserved.y - p.gap, opt.reserved.w + 2. * p.gap, opt.reserved.h + 2. * p.gap });
        if (!opt.model_box.empty())
            p.model_box = { opt.model_box.x * W, opt.model_box.y * H, opt.model_box.w * W, opt.model_box.h * H };
        std::vector<LayoutLabel> in;
        for (const MeasuredLabel &m : measured) {
            const ShowcaseCalloutDraw &d = callouts[size_t(m.draw)];
            LayoutLabel l;
            l.has_anchor = !d.anchors.empty();
            if (l.has_anchor)
                l.anchor = { d.anchors.front().x * W, d.anchors.front().y * H };
            for (size_t a = 1; a < d.anchors.size(); ++a)
                l.extra_anchors.push_back({ d.anchors[a].x * W, d.anchors[a].y * H });
            l.w          = m.w;
            l.h          = m.h;
            l.pinned     = d.pinned;
            l.pinned_pos = { d.pinned_pos.x * W, d.pinned_pos.y * H };
            in.push_back(l);
        }
        if (opt.previous_sides != nullptr) {
            // S4, video: the sides of the previous frame, so labels do not jump across.
            p.side_hysteresis = opt.side_hysteresis * W;
            for (const MeasuredLabel &m : measured) {
                auto it = opt.previous_sides->find(callouts[size_t(m.draw)].item);
                p.previous_side.push_back(it == opt.previous_sides->end() ? 0 : it->second);
            }
        }
        const CalloutLayoutResult layout = layout_callouts(in, p);
        if (hits != nullptr)
            hits->legend = layout.legend;
        if (opt.sides != nullptr) {
            opt.sides->clear();
            for (size_t k = 0; k < measured.size(); ++k)
                (*opt.sides)[callouts[size_t(measured[k].draw)].item] = layout.bands ? layout.labels[k].band : layout.labels[k].side;
        }
        // S4: fades. Every part that is not fully there is drawn with its colours' alpha scaled by its
        // opacity. (S5: not with wxGraphicsContext::BeginLayer(), which GDI+ - the Windows renderer
        // for a wxImage - leaves unimplemented, so nothing faded there.)
        double layer_opacity = 1.;
        auto   layered = [&layer_opacity](const wxColour &c) {
            return wxColour(c.Red(), c.Green(), c.Blue(), (unsigned char) std::lround(double(c.Alpha()) * std::clamp(layer_opacity, 0., 1.)));
        };
        auto set_pen = [&gc, &layered](wxGraphicsPenInfo info) {
            info.Colour(layered(info.GetColour()));
            gc->SetPen(gc->CreatePen(info));
        };
        auto set_brush = [&gc, &layered](const wxColour &c) { gc->SetBrush(gc->CreateBrush(wxBrush(layered(c)))); };
        auto set_font  = [&gc, &layered](const wxFont &font, const wxColour &c) { gc->SetFont(font, layered(c)); };
        auto with_layer = [&layer_opacity](double opacity, const std::function<void()> &draw) {
            opacity = std::clamp(opacity, 0., 1.);
            if (opacity <= 0.004)
                return;
            const double before = layer_opacity;
            layer_opacity       = before * opacity;
            draw();
            layer_opacity = before;
        };

        // --- colours ---------------------------------------------------------------------------
        const wxColour box_bg = callout_colour(st.background, (unsigned char) std::lround(std::clamp(st.background_opacity, 0.f, 1.f) * 255.f));
        auto ink_at = [&](const RectD &box) {
            if (!st.auto_colors)
                return callout_colour(st.line_color);
            return callout_colour(auto_ink(backdrop_luminance(scene.background, (box.x + 0.5 * box.w) / W, (box.y + 0.5 * box.h) / H)));
        };
        auto text_ink = [&](const RectD &box, bool on_box) {
            if (!st.auto_colors)
                return callout_colour(st.text_color);
            const double lum = on_box ? hex_luminance(st.background) : backdrop_luminance(scene.background, (box.x + 0.5 * box.w) / W,
                                                                                         (box.y + 0.5 * box.h) / H);
            return callout_colour(auto_ink(lum));
        };
        auto halo_of = [](const wxColour &ink) {
            const bool dark = 0.2126 * ink.Red() + 0.7152 * ink.Green() + 0.0722 * ink.Blue() < 128.;
            return dark ? wxColour(255, 255, 255, 170) : wxColour(0, 0, 0, 170);
        };

        auto draw_text = [&](const wxFont &font, const std::string &text, const wxColour &colour, double x, double y, bool halo) {
            if (text.empty())
                return;
            const wxString s = wxString::FromUTF8(text);
            if (halo) {
                const double h = std::max(1., 0.07 * F);
                set_font(font, halo_of(colour));
                const double offsets[8][2] = { { -h, 0. }, { h, 0. }, { 0., -h }, { 0., h }, { -h, -h }, { h, h }, { -h, h }, { h, -h } };
                for (const auto &o : offsets)
                    gc->DrawText(s, x + o[0], y + o[1]);
            }
            set_font(font, colour);
            gc->DrawText(s, x, y);
        };

        // --- leaders and dots, under the labels ------------------------------------------------
        for (size_t k = 0; k < measured.size(); ++k) {
            const ShowcaseCalloutDraw &d  = callouts[size_t(measured[k].draw)];
            const PlacedLabel         &pl = layout.labels[k];
            const wxColour             ink = ink_at(pl.box);
            const wxColour             halo = halo_of(ink);
            auto stroke = [&](const std::vector<PointD> &pts, bool dashed) {
                if (pts.size() < 2)
                    return;
                wxGraphicsPath path = gc->CreatePath();
                path.MoveToPoint(pts[0].x, pts[0].y);
                for (size_t i = 1; i < pts.size(); ++i)
                    path.AddLineToPoint(pts[i].x, pts[i].y);
                // A thin halo first, so the leader reads over the model too.
                set_pen((wxGraphicsPenInfo(callout_with_alpha(halo, 0.45), lw + 2. * std::max(1., 0.5 * lw)).Join(wxJOIN_ROUND).Cap(wxCAP_ROUND)));
                gc->StrokePath(path);
                set_pen((wxGraphicsPenInfo(ink, lw, dashed ? wxPENSTYLE_SHORT_DASH : wxPENSTYLE_SOLID).Join(wxJOIN_ROUND).Cap(wxCAP_ROUND)));
                gc->StrokePath(path);
            };
            std::vector<PointD> dots;
            for (size_t a = 0; a < d.anchors.size(); ++a)
                dots.push_back({ d.anchors[a].x * W, d.anchors[a].y * H });
            // Video (S4): leaders fade instead of turning dashed.
            const bool video = !d.fade.empty();
            // S5: a path's timeline dims or hides a callout (visual) on top of the occlusion fade.
            auto fade_of = [&d, video](size_t a) { return (video ? (a < d.fade.size() ? d.fade[a] : 1.) : 1.) * std::clamp(d.visual, 0., 1.); };
            auto dashed  = [&d, video](size_t a) { return !video && a < d.occluded.size() && d.occluded[a]; };
            // Which anchors have no line to their label (S4: left out so nothing crosses).
            auto unlinked = [&pl, &layout](size_t a) {
                if (layout.legend)
                    return false;
                return a == 0 ? pl.leader_dropped : (a - 1 >= pl.legs.size() || pl.legs[a - 1].empty());
            };
            if (!layout.legend) {
                if (!pl.leader.empty())
                    with_layer(fade_of(0), [&]() { stroke(pl.leader, dashed(0)); });
                // The other regions of the same filament: each its own leg to the label (S4).
                for (size_t a = 1; a < dots.size(); ++a)
                    if (a - 1 < pl.legs.size() && !pl.legs[a - 1].empty())
                        with_layer(fade_of(a), [&]() { stroke(pl.legs[a - 1], dashed(a)); });
            }
            for (size_t a = 0; a < dots.size(); ++a) {
                const bool occluded = !video && a < d.occluded.size() && d.occluded[a];
                if (pl.badge > 0 && unlinked(a)) {
                    // S4: the label's number instead of a line.
                    with_layer(fade_of(a), [&]() {
                        set_pen((wxGraphicsPenInfo(halo, std::max(1., 0.08 * F))));
                        set_brush((ink));
                        const double r = 0.8 * badge_r;
                        gc->DrawEllipse(dots[a].x - r, dots[a].y - r, 2. * r, 2. * r);
                        const std::string num = std::to_string(pl.badge);
                        double nw = 0., nh = 0.;
                        extent(font_small, num, nw, nh);
                        set_font(font_small, wxColour(halo.Red(), halo.Green(), halo.Blue()));
                        gc->DrawText(wxString::FromUTF8(num), dots[a].x - 0.5 * nw, dots[a].y - 0.5 * nh);
                    });
                    continue;
                }
                if (video && !layout.legend) {
                    with_layer(fade_of(a), [&]() {
                        // S5: a highlighted callout's dots grow a little and get the accent ring.
                        const double r = dot_r * (1. + 0.5 * std::clamp(d.highlight, 0., 1.));
                        if (d.highlight > 0.004) {
                            set_pen((wxGraphicsPenInfo(callout_with_alpha(showcase_highlight_colour(), d.highlight), std::max(1., 0.9 * lw))));
                            gc->SetBrush(*wxTRANSPARENT_BRUSH);
                            gc->DrawEllipse(dots[a].x - 1.8 * r, dots[a].y - 1.8 * r, 3.6 * r, 3.6 * r);
                        }
                        set_pen((wxGraphicsPenInfo(halo, std::max(1., 0.45 * lw))));
                        set_brush((ink));
                        gc->DrawEllipse(dots[a].x - r, dots[a].y - r, 2. * r, 2. * r);
                    });
                    continue;
                }
                if (layout.legend) {
                    // The legend's number on the model.
                    set_pen((wxGraphicsPenInfo(halo, std::max(1., 0.08 * F))));
                    set_brush((occluded ? callout_with_alpha(ink, 0.55) : ink));
                    gc->DrawEllipse(dots[a].x - badge_r, dots[a].y - badge_r, 2. * badge_r, 2. * badge_r);
                    const std::string num = std::to_string(pl.number);
                    double nw = 0., nh = 0.;
                    extent(font_small, num, nw, nh);
                    set_font(font_small, wxColour(halo.Red(), halo.Green(), halo.Blue()));
                    gc->DrawText(wxString::FromUTF8(num), dots[a].x - 0.5 * nw, dots[a].y - 0.5 * nh);
                    continue;
                }
                with_layer(fade_of(a), [&]() {
                    const double r = dot_r * (1. + 0.5 * std::clamp(d.highlight, 0., 1.));
                    if (d.highlight > 0.004) {
                        set_pen((wxGraphicsPenInfo(callout_with_alpha(showcase_highlight_colour(), d.highlight), std::max(1., 0.9 * lw))));
                        gc->SetBrush(*wxTRANSPARENT_BRUSH);
                        gc->DrawEllipse(dots[a].x - 1.8 * r, dots[a].y - 1.8 * r, 3.6 * r, 3.6 * r);
                    }
                    set_pen((wxGraphicsPenInfo(occluded ? ink : halo, std::max(1., 0.45 * lw))));
                    set_brush(occluded ? callout_with_alpha(halo, 0.6) : ink);
                    gc->DrawEllipse(dots[a].x - r, dots[a].y - r, 2. * r, 2. * r);
                });
            }
            if (hits != nullptr) {
                ShowcaseCalloutHits::Item hit;
                hit.item = d.item;
                hit.box  = pl.box;
                hit.dots = dots;
                hits->items.push_back(std::move(hit));
            }
        }

        // --- the legend's key ------------------------------------------------------------------
        if (layout.legend && !layout.legend_box.empty()) {
            const RectD &b = layout.legend_box;
            set_pen((wxGraphicsPenInfo(callout_with_alpha(callout_colour(auto_ink(hex_luminance(st.background))), 0.15), 1.)));
            set_brush((box_bg));
            gc->DrawRoundedRectangle(b.x, b.y, b.w, b.h, 0.4 * F);
        }

        // --- labels ----------------------------------------------------------------------------
        for (size_t k = 0; k < measured.size(); ++k) {
            const MeasuredLabel       &m  = measured[k];
            const ShowcaseCalloutDraw &d  = callouts[size_t(m.draw)];
            const PlacedLabel         &pl = layout.labels[k];
            const RectD               &b  = pl.box;
            const bool                 on_box = boxed || layout.legend;
            const wxColour             ink    = text_ink(b, on_box);
            const wxColour             second = callout_with_alpha(ink, 0.78);
            const bool                 halo   = !on_box;
            // S4, video: a label fades with the last of its anchors (never in the legend's key).
            const double label_alpha = layout.legend ? 1. : std::clamp(d.alpha * d.visual, 0., 1.);
            if (label_alpha <= 0.004)
                continue;
            const double before_label = layer_opacity;
            layer_opacity             = before_label * label_alpha;

            if (boxed && !layout.legend) {
                const double radius = st.shape == CalloutShape::Pill ? 0.5 * b.h : 0.3 * F;
                // A soft two-step shadow under the card.
                gc->SetPen(*wxTRANSPARENT_PEN);
                for (int s = 1; s <= 2; ++s) {
                    set_brush((wxColour(0, 0, 0, 22)));
                    gc->DrawRoundedRectangle(b.x, b.y + 0.07 * F * s, b.w, b.h, radius);
                }
                set_pen((wxGraphicsPenInfo(callout_with_alpha(callout_colour(auto_ink(hex_luminance(st.background))), 0.15), 1.)));
                set_brush((box_bg));
                gc->DrawRoundedRectangle(b.x, b.y, b.w, b.h, radius);
            }
            if (layout.legend) {
                // The number in front of the row.
                const double cx = b.x - 0.5 * p.legend_badge, cy = b.y + pad_y + 0.5 * h_main;
                const wxColour badge = ink_at(b);
                gc->SetPen(*wxTRANSPARENT_PEN);
                set_brush((badge));
                gc->DrawEllipse(cx - badge_r, cy - badge_r, 2. * badge_r, 2. * badge_r);
                const std::string num = std::to_string(pl.number);
                double nw = 0., nh = 0.;
                extent(font_small, num, nw, nh);
                const wxColour on_badge = halo_of(badge);
                set_font(font_small, wxColour(on_badge.Red(), on_badge.Green(), on_badge.Blue()));
                gc->DrawText(wxString::FromUTF8(num), cx - 0.5 * nw, cy - 0.5 * nh);
            }

            double x = b.x + pad_x, y = b.y + pad_y;
            if (m.swatch) {
                const std::vector<std::string> &colors = d.label.swatches;
                const double sy = y + 0.5 * (h_main - sw);
                gc->SetPen(*wxTRANSPARENT_PEN);
                for (size_t c = 0; c < colors.size(); ++c) {
                    set_brush((callout_colour(colors[c])));
                    const double x0 = x + sw * double(c) / double(colors.size()), x1 = x + sw * double(c + 1) / double(colors.size());
                    gc->DrawRectangle(x0, sy, x1 - x0, sw);
                }
                set_pen((wxGraphicsPenInfo(callout_with_alpha(halo ? halo_of(ink) : ink, 0.35), std::max(1., 0.05 * F))));
                gc->SetBrush(*wxTRANSPARENT_BRUSH);
                gc->DrawRectangle(x, sy, sw, sw);
                x += sw + sw_gap;
            }
            draw_text(font_main, m.line1, ink, x, y, halo);
            if (!m.hex.empty())
                draw_text(font_small, m.hex, second, x + m.w1 + (m.line1.empty() ? 0. : 0.5 * F), y + 0.5 * (h_main - h_small), halo);
            y += h_main;
            if (st.shape == CalloutShape::Technical && !layout.legend) {
                // AutoCAD's multileader: the text sits on a line the leader lands on.
                const double ly = y + 0.12 * F;
                set_pen((wxGraphicsPenInfo(ink_at(b), lw)));
                gc->StrokeLine(b.x, ly, b.right(), ly);
                y += underline;
            }
            if (!m.details.empty()) {
                y += line_gap;
                draw_text(font_small, m.details, second, b.x + pad_x, y, halo);
                y += h_small;
            }
            if (!m.note.empty()) {
                y += line_gap;
                draw_text(font_small, m.note, second, b.x + pad_x, y, halo);
            }
            if (d.highlight > 0.004 && !layout.legend) {
                // S5: a path's highlight - an accent ring round the label.
                const double g = 0.25 * F;
                set_pen((wxGraphicsPenInfo(callout_with_alpha(showcase_highlight_colour(), std::clamp(d.highlight, 0., 1.)),
                                                           std::max(1.5, 0.16 * F))
                                             .Join(wxJOIN_ROUND)));
                gc->SetBrush(*wxTRANSPARENT_BRUSH);
                gc->DrawRoundedRectangle(b.x - g, b.y - g, b.w + 2. * g, b.h + 2. * g, 0.4 * F);
            }
            if (opt.overlay && d.item == opt.selected) {
                // The live view's selection; never in an export.
                set_pen((wxGraphicsPenInfo(wxColour(0, 150, 136), std::max(1., 0.1 * F), wxPENSTYLE_SHORT_DASH)));
                gc->SetBrush(*wxTRANSPARENT_BRUSH);
                const double g = 0.2 * F;
                gc->DrawRoundedRectangle(b.x - g, b.y - g, b.w + 2. * g, b.h + 2. * g, 0.3 * F);
            }
            if (pl.badge > 0 && !layout.legend) {
                // S4: the number its unlinked dots carry, on the label's corner.
                const wxColour badge_ink = ink_at(b);
                const wxColour on_badge  = halo_of(badge_ink);
                const double   r         = 0.8 * badge_r;
                set_pen((wxGraphicsPenInfo(on_badge, std::max(1., 0.08 * F))));
                set_brush((badge_ink));
                gc->DrawEllipse(b.x - r, b.y - r, 2. * r, 2. * r);
                const std::string num = std::to_string(pl.badge);
                double nw = 0., nh = 0.;
                extent(font_small, num, nw, nh);
                set_font(font_small, wxColour(on_badge.Red(), on_badge.Green(), on_badge.Blue()));
                gc->DrawText(wxString::FromUTF8(num), b.x - 0.5 * nw, b.y - 0.5 * nh);
            }
            layer_opacity = before_label;
        }
        // The context writes its pixels back into `canvas` when it is destroyed, here.
    }

    const unsigned char *rgb   = canvas.GetData();
    const unsigned char *alpha = has_alpha && canvas.HasAlpha() ? canvas.GetAlpha() : nullptr;
    for (size_t i = 0; i < size_t(W) * size_t(H); ++i) {
        image.rgba[i * 4 + 0] = rgb[i * 3 + 0];
        image.rgba[i * 4 + 1] = rgb[i * 3 + 1];
        image.rgba[i * 4 + 2] = rgb[i * 3 + 2];
        if (alpha != nullptr)
            image.rgba[i * 4 + 3] = alpha[i];
    }
}

// ------------------------------------------------------------------------------------------------
// The label context
// ------------------------------------------------------------------------------------------------

ShowcaseLabelContext ShowcaseLabelContext::current(int plate_index, bool with_usage)
{
    ShowcaseLabelContext c;
    c.m_words.mixed_title = _u8L("Mixed filament");
    c.m_words.from_height = _u8L("from %1% mm");
    c.m_words.currency    = currency_symbol();
    c.m_words.grams       = "g";
    c.m_lang              = into_u8(wxGetApp().current_language_code_safe());

    PresetBundle *bundle = wxGetApp().preset_bundle;
    Plater       *plater = wxGetApp().plater();
    if (bundle == nullptr || plater == nullptr)
        return c;

    FilamentColorLibrary &library = FilamentColorLibrary::Instance();
    if (library.EnsureLoaded())
        c.m_library = library.GetAllFilamentInfos();

    const ConfigOptionStrings *multi = bundle->project_config.option<ConfigOptionStrings>("filament_multi_colors");
    for (const ShowcaseFilament &f : showcase_filaments()) {
        Slot s;
        s.preset_name = f.preset_name;
        s.type        = f.type;
        s.colors      = { f.color };
        if (const Preset *preset = bundle->filaments.find_preset(f.preset_name, false); preset != nullptr && preset->config.has("filament_vendor"))
            s.vendor = preset->config.opt_string("filament_vendor", 0u);
        if (multi != nullptr && size_t(f.slot - 1) < multi->values.size()) {
            std::vector<std::string> split = SplitFilamentMultiColors(multi->values[size_t(f.slot - 1)]);
            if (split.size() > 1)
                s.colors = split;
        }
        const std::string match = GetFilamentMatchName(f.preset_name);
        for (size_t i = 0; i < c.m_library.size() && !match.empty(); ++i)
            if (GetFilamentMatchName(c.m_library[i].filamentName) == match) {
                s.family = int(i);
                break;
            }
        c.m_slots.push_back(std::move(s));
    }

    if (with_usage) {
        PartPlate *plate = plater->get_partplate_list().get_plate(plate_index);
        GCodeProcessorResult *result = plate != nullptr && plate->is_slice_result_valid() ? plate->get_slice_result() : nullptr;
        if (result != nullptr) {
            try {
                const DynamicPrintConfig full = bundle->full_config();
                const ConfigOptionFloats *density = full.option<ConfigOptionFloats>("filament_density");
                const std::vector<CostOverrides::Resolved> prices = CostOverrides::resolve_slots(full, *CostOverrides::global(), &bundle->filaments);
                for (const auto &kv : result->print_statistics.total_volumes_per_extruder) {
                    const int    slot = int(kv.first) + 1;
                    const double d    = density != nullptr && kv.first < density->values.size() ? density->values[kv.first] : 1.24;
                    const double g    = kv.second * d / 1000.;   // mm3 x g/cm3
                    c.m_weight_g[slot] = g;
                    if (kv.first < prices.size())
                        c.m_price[slot] = g / 1000. * prices[kv.first].price;
                }
            } catch (const std::exception &ex) {
                BOOST_LOG_TRIVIAL(warning) << "Showcase: no filament weights for the callouts: " << ex.what();
            }
        }
    }
    return c;
}

std::vector<std::string> ShowcaseLabelContext::colors_of(const Showcase::Callout &callout) const
{
    if (!callout.color.empty())
        return { callout.color };   // after a colour change
    if (callout.filament >= 1 && size_t(callout.filament) <= m_slots.size())
        return m_slots[size_t(callout.filament - 1)].colors;
    if (PresetBundle *bundle = wxGetApp().preset_bundle) {
        if (const MixedFilament *mf = bundle->mixed_filaments.mixed_filament_from_id(unsigned(callout.filament), m_slots.size());
            mf != nullptr && !mf->display_color.empty())
            return { mf->display_color };
    }
    return {};
}

std::string ShowcaseLabelContext::library_name(const Showcase::Callout &callout) const
{
    const std::vector<std::string> colors = colors_of(callout);
    if (colors.empty())
        return {};
    const FilamentColorInfo *family = nullptr;
    if (callout.filament >= 1 && size_t(callout.filament) <= m_slots.size() && m_slots[size_t(callout.filament - 1)].family >= 0)
        family = &m_library[size_t(m_slots[size_t(callout.filament - 1)].family)];
    return Showcase::library_color_name(family, m_library, colors, m_lang);
}

Showcase::LabelFilament ShowcaseLabelContext::filament(const Showcase::Scene &scene, const Showcase::Callout &callout) const
{
    Showcase::LabelFilament f;
    f.colors = colors_of(callout);
    auto translate = [](const char *s) { return into_u8(wxGetTranslation(wxString::FromUTF8(s))); };
    const std::string hex = f.colors.empty() ? std::string() : f.colors.front();
    if (callout.filament >= 1 && size_t(callout.filament) <= m_slots.size()) {
        const Slot &s = m_slots[size_t(callout.filament - 1)];
        f.vendor      = s.vendor;
        f.preset_name = s.preset_name;
        f.type        = s.type;
        const Showcase::Finish finish = scene.materials.finish_for(callout.filament, s.preset_name, s.type);
        f.finish          = into_u8(showcase_finish_name(finish));
        f.standard_finish = finish == Showcase::Finish::Standard;
    } else if (PresetBundle *bundle = wxGetApp().preset_bundle) {
        // A mixed filament: its parts and their shares.
        if (const MixedFilament *mf = bundle->mixed_filaments.mixed_filament_from_id(unsigned(callout.filament), m_slots.size())) {
            f.mixed = true;
            auto part_name = [this](unsigned id) {
                if (id >= 1 && size_t(id) <= m_slots.size()) {
                    const Slot &s = m_slots[size_t(id - 1)];
                    const std::string name = Showcase::product_name(s.preset_name, s.vendor);
                    return name.empty() ? std::to_string(id) : name;
                }
                return std::to_string(id);
            };
            std::vector<std::pair<unsigned, int>> parts;
            if (mf->gradient_component_ids.size() >= 3) {
                // "123" with weights "50/25/25".
                std::vector<int> weights;
                std::string      num;
                for (char ch : mf->gradient_component_weights + "/") {
                    if (ch == '/') {
                        if (!num.empty())
                            weights.push_back(std::atoi(num.c_str()));
                        num.clear();
                    } else if (std::isdigit((unsigned char) ch))
                        num += ch;
                }
                for (size_t i = 0; i < mf->gradient_component_ids.size(); ++i)
                    if (std::isdigit((unsigned char) mf->gradient_component_ids[i]))
                        parts.push_back({ unsigned(mf->gradient_component_ids[i] - '0'), i < weights.size() ? weights[i] : -1 });
            } else {
                const int b = std::clamp(mf->mix_b_percent, 0, 100);
                parts       = { { mf->component_a, 100 - b }, { mf->component_b, b } };
            }
            for (size_t i = 0; i < parts.size(); ++i) {
                f.mixed_parts += (i == 0 ? "" : " + ") + part_name(parts[i].first);
                if (parts[i].second >= 0)
                    f.mixed_parts += " " + std::to_string(parts[i].second) + "%";
            }
        }
    }
    f.color_name = Showcase::resolve_color_name(library_name(callout), callout.color_name, hex, translate);
    if (auto it = m_weight_g.find(callout.filament); it != m_weight_g.end())
        f.weight_g = it->second;
    if (auto it = m_price.find(callout.filament); it != m_price.end())
        f.price = it->second;
    return f;
}

std::string ShowcaseLabelContext::signature() const
{
    std::string s = m_lang + "|" + m_words.currency;
    for (const Slot &slot : m_slots) {
        s += "|" + slot.preset_name + "," + slot.vendor + "," + slot.type;
        for (const std::string &c : slot.colors)
            s += "," + c;
    }
    for (const auto &kv : m_weight_g)
        s += "|w" + std::to_string(kv.first) + "=" + std::to_string(kv.second);
    for (const auto &kv : m_price)
        s += "|p" + std::to_string(kv.first) + "=" + std::to_string(kv.second);
    return s;
}

// ------------------------------------------------------------------------------------------------
// The edit dialog
// ------------------------------------------------------------------------------------------------

ShowcaseCalloutDialog::ShowcaseCalloutDialog(wxWindow *parent, const Showcase::Callout &callout, const wxString &heading,
                                             const std::string &auto_title, const std::string &auto_color, const std::string &library_name)
    : wxDialog(parent, wxID_ANY, _L("Edit callout"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , m_callout(callout)
{
    const int em  = wxGetApp().em_unit();
    auto     *col = new wxBoxSizer(wxVERTICAL);
    auto     *top = new wxStaticText(this, wxID_ANY, heading);
    top->SetFont(wxGetApp().bold_font());
    col->Add(top, 0, wxALL, em);

    auto *grid = new wxFlexGridSizer(2, em / 2, em);
    grid->AddGrowableCol(1, 1);
    auto row = [this, grid](const wxString &label, wxWindow *ctrl) {
        grid->Add(new wxStaticText(this, wxID_ANY, label), 0, wxALIGN_CENTER_VERTICAL);
        grid->Add(ctrl, 1, wxEXPAND);
    };
    m_title = new wxTextCtrl(this, wxID_ANY, wxString::FromUTF8(callout.title), wxDefaultPosition, wxSize(30 * em, -1));
    m_title->SetHint(wxString::FromUTF8(auto_title));
    m_title->SetToolTip(_L("Replaces the brand and product name. Leave it empty for the automatic text."));
    row(_L("First line"), m_title);
    m_color = new wxTextCtrl(this, wxID_ANY, wxString::FromUTF8(library_name.empty() ? callout.color_name : library_name));
    m_color->SetHint(wxString::FromUTF8(auto_color));
    if (!library_name.empty()) {
        m_color->Enable(false);
        m_color->SetToolTip(_L("This colour's name comes from the Snapmaker colour library."));
    } else
        m_color->SetToolTip(_L("Your name for this colour. Leave it empty for the nearest known name."));
    row(_L("Colour name"), m_color);
    m_note = new wxTextCtrl(this, wxID_ANY, wxString::FromUTF8(callout.note));
    m_note->SetToolTip(_L("An extra line under the label."));
    row(_L("Note"), m_note);
    col->Add(grid, 0, wxEXPAND | wxLEFT | wxRIGHT, em);

    m_hidden = new wxCheckBox(this, wxID_ANY, _L("Hide this callout"));
    m_hidden->SetValue(callout.hidden);
    col->Add(m_hidden, 0, wxLEFT | wxRIGHT | wxTOP, em);
    m_pinned = new wxCheckBox(this, wxID_ANY, _L("Keep the label where I put it"));
    m_pinned->SetValue(callout.pinned);
    m_pinned->Enable(callout.pinned);
    m_pinned->SetToolTip(_L("Drag a label on the view to put it somewhere. Untick to let it follow the automatic layout again."));
    col->Add(m_pinned, 0, wxLEFT | wxRIGHT | wxTOP, em);

    auto *buttons = new wxBoxSizer(wxHORIZONTAL);
    auto *reset   = new wxButton(this, wxID_ANY, _L("Automatic text"));
    reset->SetToolTip(_L("Clears your first line, colour name and note."));
    reset->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        m_title->Clear();
        if (m_color->IsEnabled())
            m_color->Clear();
        m_note->Clear();
    });
    buttons->Add(reset, 0, wxALIGN_CENTER_VERTICAL);
    buttons->AddStretchSpacer(1);
    if (wxStdDialogButtonSizer *std_buttons = CreateStdDialogButtonSizer(wxOK | wxCANCEL))
        buttons->Add(std_buttons, 0, wxALIGN_CENTER_VERTICAL);
    col->Add(buttons, 0, wxEXPAND | wxALL, em);

    SetSizer(col);
    // Sized to what it holds (the Mac's controls are wider than Windows').
    col->SetSizeHints(this);
    wxGetApp().UpdateDlgDarkUI(this);
    CenterOnParent();
}

Showcase::Callout ShowcaseCalloutDialog::result() const
{
    Showcase::Callout c = m_callout;
    c.title             = into_u8(m_title->GetValue());
    if (m_color->IsEnabled())
        c.color_name = into_u8(m_color->GetValue());
    c.note   = into_u8(m_note->GetValue());
    c.hidden = m_hidden->GetValue();
    c.pinned = c.pinned && m_pinned->GetValue();
    return c;
}

} // namespace GUI
} // namespace Slic3r
