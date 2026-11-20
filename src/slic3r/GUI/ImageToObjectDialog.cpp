#include "ImageToObjectDialog.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "GUI_ObjectList.hpp"
#include "I18N.hpp"
#include "ImageTraceDialog.hpp"
#include "MainFrame.hpp"
#include "MsgDialog.hpp"
#include "Plater.hpp"
#include "format.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/LocalesUtils.hpp"
#include "libslic3r/NSVGUtils.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include "nanosvg/nanosvg.h"
#include "nanosvg/nanosvgrast.h"

#include <wx/bitmap.h>
#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/image.h>
#include <wx/sizer.h>
#include <wx/slider.h>
#include <wx/spinctrl.h>
#include <wx/statbmp.h>
#include <wx/stattext.h>
#include <wx/utils.h>

#include <algorithm>
#include <cmath>
#include <sstream>

namespace Slic3r { namespace GUI {

namespace {

const char *I2O_CFG_SECTION  = "image_to_object";
const int   I2O_PREVIEW_PX   = 240;
// Longest side of the image traced for the live preview / for the object
const int   I2O_PREVIEW_SIDE = 480;
const int   I2O_FINAL_SIDE   = 1536;

unsigned char i2o_checker(int x, int y) { return ((x / 8 + y / 8) % 2) ? 205 : 235; }

wxImage i2o_blank(int px)
{
    wxImage image(px, px);
    for (int y = 0; y < px; ++y)
        for (int x = 0; x < px; ++x) {
            unsigned char v = i2o_checker(x, y);
            image.SetRGB(x, y, v, v, v);
        }
    return image;
}

// RGBA buffer blended over the checkerboard into a square image
wxBitmap i2o_bitmap(const unsigned char *rgba, int w, int h, int px)
{
    wxImage image = i2o_blank(px);
    int     ox = (px - w) / 2, oy = (px - h) / 2;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const unsigned char *c = &rgba[(size_t(y) * size_t(w) + size_t(x)) * 4];
            int                  X = ox + x, Y = oy + y;
            if (X < 0 || Y < 0 || X >= px || Y >= px)
                continue;
            unsigned char bg    = i2o_checker(X, Y);
            auto          blend = [a = c[3], bg](unsigned char v) { return (unsigned char) ((v * a + bg * (255 - a)) / 255); };
            image.SetRGB(X, Y, blend(c[0]), blend(c[1]), blend(c[2]));
        }
    return wxBitmap(image);
}

// Cross-sections of the cutter seen from above: handle, blade and what the clean-up removed
wxBitmap i2o_render(const Silhouette &silhouette, const CookieCutterSections &sections, int max_px)
{
    if (sections.handle.empty())
        return wxBitmap(i2o_blank(max_px));
    BoundingBox bb = get_extents(sections.handle);
    if (!silhouette.removed.empty())
        bb.merge(get_extents(silhouette.removed));
    auto to_um = [](coord_t c) { return std::llround(double(c) * SCALING_FACTOR * 1000.); };
    long long w = std::max(1ll, (long long) to_um(bb.size().x()));
    long long h = std::max(1ll, (long long) to_um(bb.size().y()));

    std::stringstream svg;
    svg << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << w << "\" height=\"" << h << "\" viewBox=\"0 0 " << w << " " << h << "\">";
    auto write = [&svg, &bb, &to_um](const ExPolygons &shape, const char *fill) {
        if (shape.empty())
            return;
        svg << "<path fill=\"" << fill << "\" fill-rule=\"evenodd\" d=\"";
        auto write_polygon = [&svg, &bb, &to_um](const Slic3r::Polygon &polygon) {
            for (size_t k = 0; k < polygon.points.size(); ++k)
                // the shape has Y up, the picture has Y down
                svg << (k == 0 ? "M" : (k == 1 ? "L" : " ")) << to_um(polygon.points[k].x() - bb.min.x()) << " "
                    << to_um(bb.max.y() - polygon.points[k].y());
            svg << "Z";
        };
        for (const ExPolygon &e : shape) {
            write_polygon(e.contour);
            for (const Slic3r::Polygon &hole : e.holes)
                write_polygon(hole);
        }
        svg << "\"/>";
    };
    write(sections.handle, "#8FB3E8");
    write(sections.blade, "#1F3F7A");
    write(silhouette.removed, "#E03030");
    svg << "</svg>";

    NSVGimage_ptr nsvg = Slic3r::nsvgParse(svg.str(), "px");
    if (nsvg == nullptr || nsvg->width <= 0.f || nsvg->height <= 0.f)
        return wxBitmap(i2o_blank(max_px));
    float scale = float(max_px) / std::max(nsvg->width, nsvg->height);
    int   iw    = std::max(1, int(nsvg->width * scale));
    int   ih    = std::max(1, int(nsvg->height * scale));
    std::vector<unsigned char> rgba(size_t(iw) * size_t(ih) * 4, 0);
    NSVGrasterizer            *rast = nsvgCreateRasterizer();
    if (rast == nullptr)
        return wxBitmap(i2o_blank(max_px));
    nsvgRasterize(rast, nsvg.get(), 0, 0, scale, rgba.data(), iw, ih, iw * 4);
    nsvgDeleteRasterizer(rast);
    return i2o_bitmap(rgba.data(), iw, ih, max_px);
}

wxString i2o_messages(const std::vector<std::string> &messages)
{
    wxString out;
    for (const std::string &m : messages)
        out += (out.empty() ? "" : "\n") + _(from_u8(m));
    return out;
}

} // namespace

ImageToObjectSettings ImageToObjectDialog::load_from_config()
{
    ImageToObjectSettings s;
    AppConfig            *cfg = wxGetApp().app_config;
    if (cfg == nullptr)
        return s;
    auto read_double = [cfg](const char *key, double &out, double min, double max) {
        if (!cfg->has(I2O_CFG_SECTION, key))
            return;
        try {
            out = std::clamp(std::stod(cfg->get(I2O_CFG_SECTION, key)), min, max);
        } catch (...) {}
    };
    auto read_bool = [cfg](const char *key, bool &out) {
        if (cfg->has(I2O_CFG_SECTION, key))
            out = cfg->get(I2O_CFG_SECTION, key) == "1";
    };
    read_double("blur", s.blur, 0., 5.);
    read_double("size", s.silhouette.longest_side, 10., 1000.);
    read_double("close_gaps", s.silhouette.close_gaps, 0., 5.);
    read_double("min_feature", s.silhouette.min_feature, 0., 10.);
    read_bool("fill_holes", s.silhouette.fill_holes);
    read_bool("largest_only", s.silhouette.largest_only);
    read_bool("mirror", s.silhouette.mirror);
    read_double("blade_height", s.cutter.blade_height, 3., 100.);
    read_double("blade_thickness", s.cutter.blade_thickness, 0.3, 5.);
    read_double("handle_width", s.cutter.handle_width, 0.3, 30.);
    read_double("handle_height", s.cutter.handle_height, 0.4, 20.);
    read_bool("sharp_edge", s.cutter.sharp_edge);
    double braces = double(int(s.cutter.braces));
    read_double("braces", braces, 0., 2.);
    s.cutter.braces = CutterBraces(int(braces));
    return s;
}

void ImageToObjectDialog::save_to_config() const
{
    AppConfig *cfg = wxGetApp().app_config;
    if (cfg == nullptr)
        return;
    const ImageToObjectSettings &s = m_settings;
    auto set_double = [cfg](const char *key, double v) { cfg->set(I2O_CFG_SECTION, key, float_to_string_decimal_point(v)); };
    auto set_bool   = [cfg](const char *key, bool v) { cfg->set(I2O_CFG_SECTION, key, v ? "1" : "0"); };
    set_double("blur", s.blur);
    set_double("size", s.silhouette.longest_side);
    set_double("close_gaps", s.silhouette.close_gaps);
    set_double("min_feature", s.silhouette.min_feature);
    set_bool("fill_holes", s.silhouette.fill_holes);
    set_bool("largest_only", s.silhouette.largest_only);
    set_bool("mirror", s.silhouette.mirror);
    set_double("blade_height", s.cutter.blade_height);
    set_double("blade_thickness", s.cutter.blade_thickness);
    set_double("handle_width", s.cutter.handle_width);
    set_double("handle_height", s.cutter.handle_height);
    set_bool("sharp_edge", s.cutter.sharp_edge);
    cfg->set(I2O_CFG_SECTION, "braces", std::to_string(int(s.cutter.braces)));
}

ImageToObjectDialog::ImageToObjectDialog(wxWindow *parent, const TraceImage &image, const std::string &image_name,
                                         const ImageToObjectSettings &settings)
    : DPIDialog(parent ? parent : static_cast<wxWindow *>(wxGetApp().mainframe),
                wxID_ANY,
                _L("Image to object"),
                wxDefaultPosition,
                wxDefaultSize,
                wxDEFAULT_DIALOG_STYLE)
    , m_image(image)
    , m_preview_image(downscale_trace_image(image, I2O_PREVIEW_SIDE))
    , m_image_name(image_name)
    , m_settings(settings)
    , m_timer(this)
{
    wxBoxSizer      *root = new wxBoxSizer(wxVERTICAL);
    wxBoxSizer      *cols = new wxBoxSizer(wxHORIZONTAL);
    wxFlexGridSizer *grid = new wxFlexGridSizer(2, FromDIP(6), FromDIP(10));
    grid->AddGrowableCol(1);

    auto label = [this](const wxString &text) { return new wxStaticText(this, wxID_ANY, text); };
    auto row   = [&grid, &label](const wxString &text, wxWindow *ctrl, std::vector<wxWindow *> *group = nullptr) {
        wxStaticText *l = label(text);
        grid->Add(l, 0, wxALIGN_CENTER_VERTICAL);
        grid->Add(ctrl, 1, wxEXPAND | wxALIGN_CENTER_VERTICAL);
        if (group != nullptr) {
            group->push_back(l);
            group->push_back(ctrl);
        }
    };
    auto check_row = [this, &grid](wxCheckBox *check) {
        grid->Add(new wxStaticText(this, wxID_ANY, wxEmptyString));
        grid->Add(check, 0, wxALIGN_CENTER_VERTICAL);
    };
    auto heading = [this, &grid](const wxString &text) {
        wxStaticText *h = new wxStaticText(this, wxID_ANY, text);
        h->SetFont(h->GetFont().Bold());
        grid->Add(h, 0, wxALIGN_CENTER_VERTICAL | wxTOP, FromDIP(6));
        grid->Add(new wxStaticText(this, wxID_ANY, wxEmptyString));
    };
    auto spin_double = [this](double value, double min, double max, double inc, int digits = 2) {
        auto *s = new wxSpinCtrlDouble(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(110), -1), wxSP_ARROW_KEYS, min, max,
                                       value, inc);
        s->SetDigits(digits);
        return s;
    };

    const ImageToObjectSettings &s = m_settings;

    // ---- image and type ----------------------------------------------------------------------
    wxBoxSizer *image_row = new wxBoxSizer(wxHORIZONTAL);
    m_image_label         = new wxStaticText(this, wxID_ANY, wxEmptyString);
    m_change_image        = new wxButton(this, wxID_ANY, _L("Change image") + dots);
    image_row->Add(m_image_label, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    image_row->Add(m_change_image, 0);
    grid->Add(label(_L("Image") + ":"), 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(image_row, 1, wxEXPAND);

    wxArrayString types;
    types.Add(_L("Cookie cutter"));
    m_type = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, types);
    m_type->SetSelection(0);
    m_type->SetToolTip(_L("What is made from the outline of the image."));
    row(_L("Object") + ":", m_type);

    // ---- shape -------------------------------------------------------------------------------
    heading(_L("Shape"));
    wxArrayString froms;
    froms.Add(_L("Automatic"));
    froms.Add(_L("Transparency"));
    froms.Add(_L("Brightness"));
    m_shape_from = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, froms);
    m_shape_from->SetSelection(int(s.shape_from));
    m_shape_from->SetToolTip(_L("Automatic uses the transparency of an image with transparent background, "
                                "otherwise the brightness."));
    row(_L("Shape from") + ":", m_shape_from);

    wxBoxSizer *threshold_row = new wxBoxSizer(wxHORIZONTAL);
    m_threshold = new wxSlider(this, wxID_ANY, s.threshold, 0, 255, wxDefaultPosition, wxSize(FromDIP(160), -1), wxSL_HORIZONTAL | wxSL_LABELS);
    m_threshold->SetToolTip(_L("Pixels with brightness up to this value become the shape."));
    m_auto_threshold = new wxCheckBox(this, wxID_ANY, _L("Auto"));
    m_auto_threshold->SetValue(s.auto_threshold);
    m_auto_threshold->SetToolTip(_L("Find the threshold which separates the dark and the light part best (Otsu)."));
    threshold_row->Add(m_threshold, 1, wxALIGN_CENTER_VERTICAL);
    threshold_row->Add(m_auto_threshold, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(6));
    wxStaticText *threshold_label = label(_L("Threshold") + ":");
    grid->Add(threshold_label, 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(threshold_row, 1, wxEXPAND);
    m_brightness.insert(m_brightness.end(), {threshold_label, m_threshold, m_auto_threshold});

    m_invert = new wxCheckBox(this, wxID_ANY, _L("Invert (use the light part)"));
    m_invert->SetValue(s.invert);
    check_row(m_invert);

    m_blur = spin_double(s.blur, 0., 5., 0.25);
    m_blur->SetToolTip(_L("Blur the image before the outline is traced. Smooths jagged and noisy edges."));
    row(_L("Smoothing") + " (px):", m_blur);

    m_size = spin_double(s.silhouette.longest_side, 10., 1000., 5., 1);
    m_size->SetToolTip(_L("Longest side of the shape. For a cookie cutter it is the size of the cookie."));
    row(_L("Size") + " (mm):", m_size);

    m_close_gaps = spin_double(s.silhouette.close_gaps, 0., 5., 0.1);
    m_close_gaps->SetToolTip(_L("Gaps narrower than this are closed and parts closer than this are joined. 0 = off."));
    row(_L("Close gaps") + " (mm):", m_close_gaps);

    m_min_feature = spin_double(s.silhouette.min_feature, 0., 10., 0.1);
    m_min_feature->SetToolTip(_L("Parts of the shape narrower than this are removed: dough does not come out of thin slots. "
                                 "Removed parts are red in the preview. 0 = off."));
    row(_L("Remove thin parts") + " (mm):", m_min_feature);

    m_fill_holes = new wxCheckBox(this, wxID_ANY, _L("Fill holes"));
    m_fill_holes->SetValue(s.silhouette.fill_holes);
    m_fill_holes->SetToolTip(_L("Use only the outer outline. Untick to get a blade around every hole of the shape too."));
    check_row(m_fill_holes);

    m_largest_only = new wxCheckBox(this, wxID_ANY, _L("Largest part only"));
    m_largest_only->SetValue(s.silhouette.largest_only);
    m_largest_only->SetToolTip(_L("An image with separate parts: use only the largest one. Untick to keep all of them."));
    check_row(m_largest_only);

    m_mirror = new wxCheckBox(this, wxID_ANY, _L("Mirror"));
    m_mirror->SetValue(s.silhouette.mirror);
    m_mirror->SetToolTip(_L("A cookie cutter is turned over when it is used. Mirrored, the cookie looks like the image."));
    check_row(m_mirror);

    // ---- cookie cutter -----------------------------------------------------------------------
    heading(_L("Cookie cutter"));
    m_blade_height = spin_double(s.cutter.blade_height, 3., 100., 0.5, 1);
    m_blade_height->SetToolTip(_L("Height of the whole cutter, from the bed to the cutting edge."));
    row(_L("Height") + " (mm):", m_blade_height);

    m_blade_thickness = spin_double(s.cutter.blade_thickness, 0.3, 5., 0.1);
    m_blade_thickness->SetToolTip(_L("Thickness of the blade. Two lines of the nozzle (0.8 mm for a 0.4 mm nozzle) print well."));
    row(_L("Blade thickness") + " (mm):", m_blade_thickness);

    m_handle_width = spin_double(s.cutter.handle_width, 0.3, 30., 0.5, 1);
    m_handle_width->SetToolTip(_L("Width of the flange around the blade, which is pressed by hand. It lies on the bed."));
    row(_L("Handle width") + " (mm):", m_handle_width);

    m_handle_height = spin_double(s.cutter.handle_height, 0.4, 20., 0.5, 1);
    row(_L("Handle height") + " (mm):", m_handle_height);

    m_sharp_edge = new wxCheckBox(this, wxID_ANY, _L("Sharp cutting edge"));
    m_sharp_edge->SetValue(s.cutter.sharp_edge);
    m_sharp_edge->SetToolTip(_L("The last 2 mm of the blade get thinner, down to one line of the nozzle (0.4 mm)."));
    check_row(m_sharp_edge);

    wxArrayString braces;
    braces.Add(_L("Automatic"));
    braces.Add(_L("Always"));
    braces.Add(_L("Never"));
    m_braces = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, braces);
    m_braces->SetSelection(std::clamp(int(s.cutter.braces), 0, 2));
    m_braces->SetToolTip(_L("Bars across the opening, at the handle, keep a large cutter in shape. "
                            "Automatic adds them to cutters over 90 mm."));
    row(_L("Braces") + ":", m_braces);

    cols->Add(grid, 1, wxEXPAND | wxALL, FromDIP(10));

    // ---- preview -----------------------------------------------------------------------------
    wxBoxSizer *right = new wxBoxSizer(wxVERTICAL);
    int         px    = FromDIP(I2O_PREVIEW_PX);
    right->Add(label(_L("Image") + ":"), 0, wxBOTTOM, FromDIP(4));
    m_source_preview = new wxStaticBitmap(this, wxID_ANY, wxBitmap(i2o_blank(px)));
    right->Add(m_source_preview, 0, wxBOTTOM, FromDIP(8));
    right->Add(label(_L("Object from above") + ":"), 0, wxBOTTOM, FromDIP(4));
    m_preview_bitmap = new wxStaticBitmap(this, wxID_ANY, wxBitmap(i2o_blank(px)));
    right->Add(m_preview_bitmap, 0);
    m_info = new wxStaticText(this, wxID_ANY, wxEmptyString);
    right->Add(m_info, 0, wxTOP, FromDIP(6));
    m_warning = new wxStaticText(this, wxID_ANY, wxEmptyString);
    m_warning->SetForegroundColour(wxColour("#D9534F"));
    right->Add(m_warning, 0, wxTOP, FromDIP(6));
    cols->Add(right, 0, wxEXPAND | wxALL, FromDIP(10));
    root->Add(cols, 1, wxEXPAND);

    wxStdDialogButtonSizer *buttons = new wxStdDialogButtonSizer();
    m_ok = new wxButton(this, wxID_OK, _L("Add"));
    buttons->AddButton(m_ok);
    buttons->AddButton(new wxButton(this, wxID_CANCEL));
    buttons->Realize();
    root->Add(buttons, 0, wxEXPAND | wxALL, FromDIP(10));

    // ---- events ------------------------------------------------------------------------------
    auto changed = [this](wxCommandEvent &evt) {
        update_enabled();
        schedule_preview();
        evt.Skip();
    };
    auto changed_double = [this](wxSpinDoubleEvent &evt) {
        schedule_preview();
        evt.Skip();
    };
    for (wxChoice *c : {m_shape_from, m_braces})
        c->Bind(wxEVT_CHOICE, changed);
    for (wxCheckBox *c : {m_auto_threshold, m_invert, m_fill_holes, m_largest_only, m_mirror, m_sharp_edge})
        c->Bind(wxEVT_CHECKBOX, changed);
    for (wxSpinCtrlDouble *sp : {m_blur, m_size, m_close_gaps, m_min_feature, m_blade_height, m_blade_thickness, m_handle_width, m_handle_height})
        sp->Bind(wxEVT_SPINCTRLDOUBLE, changed_double);
    m_threshold->Bind(wxEVT_SLIDER, [this](wxCommandEvent &evt) {
        // moving the slider means a manual threshold
        m_auto_threshold->SetValue(false);
        schedule_preview();
        evt.Skip();
    });
    m_change_image->Bind(wxEVT_BUTTON, [this](wxCommandEvent &evt) {
        change_image();
        evt.Skip();
    });
    Bind(wxEVT_TIMER, [this](wxTimerEvent &) { refresh_preview(); });
    m_ok->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        if (finish())
            EndModal(wxID_OK);
    });

    refresh_source_preview();
    update_enabled();
    refresh_preview();

    SetSizer(root);
    Layout();
    root->Fit(this);
    CenterOnParent();
    wxGetApp().UpdateDlgDarkUI(this);
}

ImageToObjectDialog::~ImageToObjectDialog() { m_timer.Stop(); }

std::string ImageToObjectDialog::object_name() const
{
    std::string base = m_image_name.empty() ? _u8L("Image") : m_image_name;
    return base + " - " + _u8L("Cookie cutter");
}

ImageToObjectSettings ImageToObjectDialog::collect() const
{
    ImageToObjectSettings s = m_settings;
    s.shape_from              = TraceShapeFrom(std::max(0, m_shape_from->GetSelection()));
    s.threshold               = m_threshold->GetValue();
    s.auto_threshold          = m_auto_threshold->GetValue();
    s.invert                  = m_invert->GetValue();
    s.blur                    = m_blur->GetValue();
    s.silhouette.longest_side = m_size->GetValue();
    s.silhouette.close_gaps   = m_close_gaps->GetValue();
    s.silhouette.min_feature  = m_min_feature->GetValue();
    s.silhouette.fill_holes   = m_fill_holes->GetValue();
    s.silhouette.largest_only = m_largest_only->GetValue();
    s.silhouette.mirror       = m_mirror->GetValue();
    s.cutter.blade_height     = m_blade_height->GetValue();
    s.cutter.blade_thickness  = m_blade_thickness->GetValue();
    s.cutter.handle_width     = m_handle_width->GetValue();
    s.cutter.handle_height    = m_handle_height->GetValue();
    s.cutter.sharp_edge       = m_sharp_edge->GetValue();
    s.cutter.braces           = CutterBraces(std::clamp(m_braces->GetSelection(), 0, 2));
    return s;
}

void ImageToObjectDialog::update_enabled()
{
    int  from       = m_shape_from->GetSelection();
    bool brightness = from == int(TraceShapeFrom::Brightness) || (from == int(TraceShapeFrom::Auto) && !m_image.has_alpha());
    for (wxWindow *w : m_brightness)
        w->Show(brightness);
    Layout();
    if (GetSizer() != nullptr)
        GetSizer()->Fit(this);
}

void ImageToObjectDialog::schedule_preview()
{
    // debounce: spin controls and the slider send many events
    m_timer.StartOnce(150);
}

void ImageToObjectDialog::refresh_source_preview()
{
    m_image_label->SetLabel(wxString::Format("%s  (%d x %d px)", from_u8(m_image_name.empty() ? _u8L("Image") : m_image_name), m_image.width,
                                             m_image.height));
    int        px    = FromDIP(I2O_PREVIEW_PX);
    TraceImage thumb = downscale_trace_image(m_preview_image, px);
    if (thumb.empty())
        m_source_preview->SetBitmap(wxBitmap(i2o_blank(px)));
    else
        m_source_preview->SetBitmap(i2o_bitmap(thumb.rgba.data(), thumb.width, thumb.height, px));
}

Silhouette ImageToObjectDialog::make_shape(const TraceImage &image, const ImageToObjectSettings &settings, int max_side, std::string &error,
                                           int *threshold) const
{
    ImageTraceParams tp;
    tp.colors         = 1;
    tp.shape_from     = settings.shape_from;
    tp.threshold      = settings.threshold;
    tp.auto_threshold = settings.auto_threshold;
    tp.invert         = settings.invert;
    tp.blur           = settings.blur;
    tp.max_side       = max_side;
    tp.keep_source    = false;
    // Near the final size, so the clean-up of the trace (specks) works in the right scale.
    // The silhouette is scaled exactly afterwards.
    double side = settings.silhouette.longest_side;
    tp.width    = image.width >= image.height ? side : side * double(image.width) / double(std::max(1, image.height));

    ImageTraceResult trace = trace_image(image, tp, {}, false);
    if (threshold != nullptr)
        *threshold = trace.threshold;
    if (!trace.is_valid()) {
        error = trace.error.empty() ? _u8L("No shape was found in the image.") : trace.error;
        return {};
    }
    std::vector<ExPolygons> levels;
    for (const ImageTraceLayer &layer : trace.layers)
        levels.push_back(layer.shape);
    return make_silhouette(levels, settings.silhouette);
}

void ImageToObjectDialog::refresh_preview()
{
    m_timer.Stop();
    const ImageToObjectSettings s  = collect();
    int                         px = FromDIP(I2O_PREVIEW_PX);
    std::string                 error;
    int                         threshold = -1;
    Silhouette                  shape     = make_shape(m_preview_image, s, I2O_PREVIEW_SIDE, error, &threshold);
    if (s.auto_threshold && threshold >= 0)
        m_threshold->SetValue(threshold);

    if (!shape.empty()) {
        const CookieCutterParams   cutter   = sanitize(s.cutter);
        const CookieCutterSections sections = cookie_cutter_sections(shape.shape, cutter);
        m_preview_bitmap->SetBitmap(i2o_render(shape, sections, px));
        wxString info = wxString::Format(_L("Cookie: %.1f x %.1f mm"), shape.width, shape.height);
        info += "\n" + wxString::Format(_L("Cutter: %.1f x %.1f x %.1f mm"), shape.width + 2. * cutter.handle_width,
                                        shape.height + 2. * cutter.handle_width, cutter.blade_height);
        if (sections.braces)
            info += "\n" + _L("With braces across the opening.");
        info += "\n" + _L("Print it as it is: handle on the bed, no supports.");
        m_info->SetLabel(info);
        m_warning->SetLabel(i2o_messages(shape.warnings));
        m_ok->Enable(true);
    } else {
        m_preview_bitmap->SetBitmap(wxBitmap(i2o_blank(px)));
        m_info->SetLabel(wxEmptyString);
        m_warning->SetLabel(error.empty() ? i2o_messages(shape.warnings) : _(from_u8(error)));
        m_ok->Enable(false);
    }
    m_info->Wrap(px);
    m_warning->Wrap(px);
    Layout();
    if (GetSizer() != nullptr)
        GetSizer()->Fit(this);
}

void ImageToObjectDialog::change_image()
{
    wxString path = ImageTraceDialog::choose_image_file(this);
    if (path.empty())
        return;
    TraceImage  image;
    std::string source_png, name;
    if (!ImageTraceDialog::load_image_file(this, path, image, source_png, name))
        return;
    m_image         = std::move(image);
    m_preview_image = downscale_trace_image(m_image, I2O_PREVIEW_SIDE);
    m_image_name    = name;
    refresh_source_preview();
    update_enabled();
    refresh_preview();
}

bool ImageToObjectDialog::finish()
{
    m_timer.Stop();
    m_settings = collect();
    std::string error;
    {
        wxBusyCursor busy;
        Silhouette   shape = make_shape(m_image, m_settings, I2O_FINAL_SIDE, error);
        if (shape.empty()) {
            if (error.empty() && !shape.warnings.empty())
                error = shape.warnings.front();
        } else {
            m_result = make_cookie_cutter(shape.shape, m_settings.cutter);
            error    = m_result.error;
        }
    }
    if (!m_result.is_valid()) {
        m_warning->SetLabel(_(from_u8(error.empty() ? std::string("The object could not be created.") : error)));
        m_warning->Wrap(FromDIP(I2O_PREVIEW_PX));
        Layout();
        if (GetSizer() != nullptr)
            GetSizer()->Fit(this);
        return false;
    }
    save_to_config();
    return true;
}

void ImageToObjectDialog::on_dpi_changed(const wxRect &suggested_rect)
{
    Layout();
    if (GetSizer() != nullptr)
        GetSizer()->Fit(this);
    Refresh();
}

bool create_object_from_image(const wxString &image_path)
{
    Plater *plater = wxGetApp().plater();
    if (plater == nullptr || !plater->can_add_model())
        return false;

    wxString path = image_path;
    if (path.empty())
        path = ImageTraceDialog::choose_image_file(nullptr);
    if (path.empty())
        return false;
    TraceImage  image;
    std::string source_png, name;
    if (!ImageTraceDialog::load_image_file(nullptr, path, image, source_png, name))
        return false;

    ImageToObjectDialog dialog(nullptr, image, name, ImageToObjectDialog::load_from_config());
    if (dialog.ShowModal() != wxID_OK)
        return false;
    const OutlineObjectResult &result = dialog.result();
    if (!result.is_valid())
        return false;

    Plater::TakeSnapshot snapshot(plater, "Image to object");
    wxGetApp().obj_list()->load_mesh_object(TriangleMesh(indexed_triangle_set(result.mesh)), from_u8(dialog.object_name()));
    wxGetApp().mainframe->update_title();
    if (!result.warnings.empty())
        show_info(plater, i2o_messages(result.warnings), _L("Image to object"));
    return true;
}

}} // namespace Slic3r::GUI
