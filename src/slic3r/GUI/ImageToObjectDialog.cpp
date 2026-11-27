#include "ImageToObjectDialog.hpp"

#include "GLCanvas3D.hpp"
#include "GUI.hpp"
#include "GUI_App.hpp"
#include "GUI_ObjectList.hpp"
#include "I18N.hpp"
#include "ImageTraceDialog.hpp"
#include "MainFrame.hpp"
#include "MsgDialog.hpp"
#include "Plater.hpp"
#include "PresetComboBoxes.hpp"
#include "format.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/LocalesUtils.hpp"
#include "libslic3r/MeshPreview.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/NSVGUtils.hpp"
#include "libslic3r/FilamentColorLibrary.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Utils.hpp"

#include "nanosvg/nanosvg.h"
#include "nanosvg/nanosvgrast.h"

#include <wx/bitmap.h>
#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/image.h>
#include <wx/menu.h>
#include <wx/sizer.h>
#include <wx/slider.h>
#include <wx/spinctrl.h>
#include <wx/statbmp.h>
#include <wx/stattext.h>
#include <wx/utils.h>

#include <boost/log/trivial.hpp>

#include <algorithm>
#include <cmath>
#include <exception>
#include <functional>
#include <limits>
#include <map>
#include <sstream>

namespace Slic3r { namespace GUI {

#define I2O_LOG(message) BOOST_LOG_TRIVIAL(debug) << "Image to object: " << message

namespace {

const char *I2O_CFG_SECTION  = "image_to_object";
const int   I2O_PREVIEW_PX   = 240;
// Longest side of the image traced for the live preview / for the object
const int   I2O_PREVIEW_SIDE = 480;
const int   I2O_FINAL_SIDE   = 1536;
// Gap between objects added to the plate together [in mm]
const double I2O_OBJECT_GAP  = 8.;

// One filled shape of the preview picture
struct I2OLayer
{
    ExPolygons  shape;
    std::string fill; // "#RRGGBB"
    double      opacity = 1.;
};

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

// The layers seen from above, painted in the given order. Shapes have Y up.
wxBitmap i2o_render(const std::vector<I2OLayer> &layers, int max_px)
{
    BoundingBox bb;
    for (const I2OLayer &layer : layers)
        if (!layer.shape.empty())
            bb.merge(get_extents(layer.shape));
    if (!bb.defined)
        return wxBitmap(i2o_blank(max_px));
    auto to_um = [](coord_t c) { return std::llround(double(c) * SCALING_FACTOR * 1000.); };
    long long w = std::max(1ll, (long long) to_um(bb.size().x()));
    long long h = std::max(1ll, (long long) to_um(bb.size().y()));

    std::stringstream svg;
    svg << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << w << "\" height=\"" << h << "\" viewBox=\"0 0 " << w << " " << h << "\">";
    for (const I2OLayer &layer : layers) {
        if (layer.shape.empty())
            continue;
        svg << "<path fill=\"" << layer.fill << "\" fill-opacity=\"" << float_to_string_decimal_point(layer.opacity)
            << "\" fill-rule=\"evenodd\" d=\"";
        auto write_polygon = [&svg, &bb, &to_um](const Slic3r::Polygon &polygon) {
            for (size_t k = 0; k < polygon.points.size(); ++k)
                // the shape has Y up, the picture has Y down
                svg << (k == 0 ? "M" : (k == 1 ? "L" : " ")) << to_um(polygon.points[k].x() - bb.min.x()) << " "
                    << to_um(bb.max.y() - polygon.points[k].y());
            svg << "Z";
        };
        for (const ExPolygon &e : layer.shape) {
            write_polygon(e.contour);
            for (const Slic3r::Polygon &hole : e.holes)
                write_polygon(hole);
        }
        svg << "\"/>";
    }
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

wxString i2o_filament_label(int index) { return index == 0 ? _L("Default") : wxString::Format(_L("Filament %d"), index); }

// Where an object which is replaced was, and what it was set to
struct I2OPlacement
{
    std::vector<Geometry::Transformation> instances;
    DynamicPrintConfig                    config;
    bool                                  printable = true;
};

// "#RRGGBB" (or "#RRGGBBAA") -> colour, false for anything else
bool i2o_parse_color(const std::string &hex, std::array<uint8_t, 3> &out)
{
    if ((hex.size() != 7 && hex.size() != 9) || hex[0] != '#')
        return false;
    auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < 3; ++i) {
        int hi = digit(hex[1 + 2 * i]), lo = digit(hex[2 + 2 * i]);
        if (hi < 0 || lo < 0)
            return false;
        out[i] = uint8_t(hi * 16 + lo);
    }
    return true;
}

// Add the objects to the plate, side by side. The caller takes the undo snapshot.
// placements .. the n-th object takes the place, the copies and the settings of the n-th entry
void i2o_add_objects(Plater *plater, const std::vector<OutlineObject> &objects, const ImageToObjectDialog &dialog,
                     const std::vector<I2OPlacement> &placements = {})
{
    Model               &model = plater->model();
    std::vector<size_t>  object_idxs;

    // The first object goes to an empty place of the plate, the next ones beside it
    auto   start  = plater->build_volume().bounding_volume2d().center();
    auto   cell   = plater->canvas3D()->get_nearest_empty_cell({start(0), start(1)});
    double next_x = double(cell(0));
    double y      = double(cell(1));
    bool   first  = true;

    // The objects made together are edited together
    const std::string group    = new_image_object_group();
    const std::string settings = image_object_settings_to_json(dialog.settings());

    for (size_t n = 0; n < objects.size(); ++n) {
        const OutlineObject &source = objects[n];
        if (source.parts.empty())
            continue;
        const I2OPlacement *placement = n < placements.size() && !placements[n].instances.empty() ? &placements[n] : nullptr;
        I2O_LOG("adding \"" << source.name << "\", " << source.parts.size() << " parts");
        ModelObject *object = model.add_object();
        object->name        = source.name;
        object->add_instance(); // each object should have at least one instance
        if (placement != nullptr) {
            object->config.assign_config(placement->config);
            object->printable = placement->printable;
        }
        BoundingBoxf3 bb;
        for (const OutlinePart &part : source.parts) {
            TriangleMesh mesh{indexed_triangle_set(part.mesh)};
            bb.merge(mesh.bounding_box());
            // the copying overload keeps the mesh where it is, so the parts stay aligned
            ModelVolume *volume = object->add_volume(mesh);
            volume->name        = part.name;
            // the body follows the filament of the object
            if (int extruder = dialog.extruder(part); extruder > 0 && part.level >= 0)
                volume->config.set_key_value("extruder", new ConfigOptionInt(extruder));
        }
        int body = 0;
        for (const OutlinePart &part : source.parts)
            if (part.level < 0)
                body = dialog.extruder(part);
        // the dialog has no filament for a cutter: a replaced one keeps the filament it had
        if (placement == nullptr || dialog.settings().type != ImageObjectType::CookieCutter)
            object->config.set_key_value("extruder", new ConfigOptionInt(body));
        if (!dialog.is_fit_test()) {
            object->image_object_recipe.group      = group;
            object->image_object_recipe.index      = int(n);
            object->image_object_recipe.image_name = dialog.image_name();
            object->image_object_recipe.settings   = settings;
            object->image_object_recipe.source_png = dialog.source_png();
        }
        object->invalidate_bounding_box();
        object->translate(-bb.center());

        double half = bb.size().x() / 2.;
        if (placement != nullptr) {
            // the middle of the object stays where it was, with every copy of it
            object->instances[0]->set_transformation(placement->instances.front());
            for (size_t k = 1; k < placement->instances.size(); ++k)
                object->add_instance()->set_transformation(placement->instances[k]);
            object->ensure_on_bed();
            const Vec3d offset = placement->instances.front().get_offset();
            y      = offset.y();
            next_x = offset.x() + half + I2O_OBJECT_GAP;
        } else {
            double x = first ? next_x : next_x + half;
            object->instances[0]->set_offset(Vec3d(x, y, -object->origin_translation.z()));
            object->ensure_on_bed();
            next_x = x + half + I2O_OBJECT_GAP;
        }
        first = false;
        I2O_LOG("object placed");

        model.InitializeAssemblyPositions({object});
        Slic3r::save_object_mesh(*object);
        object_idxs.push_back(model.objects.size() - 1);
    }
    I2O_LOG(object_idxs.size() << " objects in the model, updating the object list");
    if (!object_idxs.empty())
        wxGetApp().obj_list()->paste_objects_into_list(object_idxs);
    wxGetApp().mainframe->update_title();
}

// Index of the selected object when it was made by "Image to object", otherwise -1
int i2o_selected_object()
{
    Plater *plater = wxGetApp().plater();
    if (plater == nullptr || wxGetApp().obj_list() == nullptr)
        return -1;
    int          idx   = wxGetApp().obj_list()->get_selected_obj_idx();
    const Model &model = plater->model();
    if (idx < 0 || size_t(idx) >= model.objects.size() || model.objects[size_t(idx)]->image_object_recipe.empty())
        return -1;
    return idx;
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
    auto read_int = [cfg](const char *key, int &out, int min, int max) {
        if (!cfg->has(I2O_CFG_SECTION, key))
            return;
        try {
            out = std::clamp(std::stoi(cfg->get(I2O_CFG_SECTION, key)), min, max);
        } catch (...) {}
    };
    auto read_bool = [cfg](const char *key, bool &out) {
        if (cfg->has(I2O_CFG_SECTION, key))
            out = cfg->get(I2O_CFG_SECTION, key) == "1";
    };
    int type = int(s.type);
    read_int("type", type, 0, 2);
    s.type = ImageObjectType(type);
    read_double("blur", s.blur, 0., 5.);
    read_double("size", s.silhouette.longest_side, 10., 1000.);
    read_double("close_gaps", s.silhouette.close_gaps, 0., 5.);
    read_double("min_feature", s.silhouette.min_feature, 0., 10.);
    read_bool("fill_holes", s.silhouette.fill_holes);
    read_bool("largest_only", s.silhouette.largest_only);
    read_bool("mirror", s.cutter_mirror);
    read_double("blade_height", s.cutter.blade_height, 3., 100.);
    read_double("blade_thickness", s.cutter.blade_thickness, 0.3, 5.);
    read_double("handle_width", s.cutter.handle_width, 0.3, 30.);
    read_double("handle_height", s.cutter.handle_height, 0.4, 20.);
    read_bool("sharp_edge", s.cutter.sharp_edge);
    int braces = int(s.cutter.braces);
    read_int("braces", braces, 0, 2);
    s.cutter.braces = CutterBraces(braces);
    read_double("box_height", s.box.height, 3., 300.);
    read_double("box_wall", s.box.wall, 0.4, 10.);
    read_double("lid_overhang", s.box.lid_overhang, 0., 10.);
    read_double("lid_clearance", s.box.clearance, 0., 1.);
    read_double("clicker_height", s.clicker.body_height, 9., 60.);
    read_double("socket_fit", s.clicker.socket_fit, -0.2, 0.5);
    read_double("stem_fit", s.clicker.stem_fit, -0.1, 0.3);
    read_double("cap_overhang", s.clicker.cap_overhang, 0., 10.);
    read_bool("keyring", s.clicker.keyring);
    read_double("keyring_angle", s.clicker.keyring_angle, -360., 360.);
    read_int("picture_colors", s.picture_colors, 0, IMAGE_OBJECT_MAX_COLORS);
    if (s.picture_colors == 1)
        s.picture_colors = 0;
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
    auto set_int    = [cfg](const char *key, int v) { cfg->set(I2O_CFG_SECTION, key, std::to_string(v)); };
    set_int("type", int(s.type));
    set_double("blur", s.blur);
    set_double("size", s.silhouette.longest_side);
    set_double("close_gaps", s.silhouette.close_gaps);
    set_double("min_feature", s.silhouette.min_feature);
    set_bool("fill_holes", s.silhouette.fill_holes);
    set_bool("largest_only", s.silhouette.largest_only);
    set_bool("mirror", s.cutter_mirror);
    set_double("blade_height", s.cutter.blade_height);
    set_double("blade_thickness", s.cutter.blade_thickness);
    set_double("handle_width", s.cutter.handle_width);
    set_double("handle_height", s.cutter.handle_height);
    set_bool("sharp_edge", s.cutter.sharp_edge);
    set_int("braces", int(s.cutter.braces));
    set_double("box_height", s.box.height);
    set_double("box_wall", s.box.wall);
    set_double("lid_overhang", s.box.lid_overhang);
    set_double("lid_clearance", s.box.clearance);
    set_double("clicker_height", s.clicker.body_height);
    set_double("socket_fit", s.clicker.socket_fit);
    set_double("stem_fit", s.clicker.stem_fit);
    set_double("cap_overhang", s.clicker.cap_overhang);
    set_bool("keyring", s.clicker.keyring);
    set_double("keyring_angle", s.clicker.keyring_angle);
    set_int("picture_colors", s.picture_colors);
    set_bool("view_3d", view_3d());
}

ImageToObjectDialog::ImageToObjectDialog(wxWindow                    *parent,
                                         const TraceImage            &image,
                                         const std::string           &source_png,
                                         const std::string           &image_name,
                                         const ImageToObjectSettings &settings,
                                         bool                         is_edit)
    : DPIDialog(parent ? parent : static_cast<wxWindow *>(wxGetApp().mainframe),
                wxID_ANY,
                is_edit ? _L("Edit image object") : _L("Image to object"),
                wxDefaultPosition,
                wxDefaultSize,
                wxDEFAULT_DIALOG_STYLE)
    , m_image(image)
    , m_source_png(source_png)
    , m_is_edit(is_edit)
    , m_preview_image(downscale_trace_image(image, I2O_PREVIEW_SIDE))
    , m_image_name(image_name)
    , m_settings(settings)
    , m_timer(this)
{
    if (Plater *plater = wxGetApp().plater(); plater != nullptr)
        m_filament_colors = plater->get_extruder_colors_from_plater_config(nullptr, false);
    const int filaments = std::max<int>(1, int(m_filament_colors.size()));
    // the filaments of an edited object are the ones it has, not the nearest ones
    if (m_is_edit)
        m_user_filament.fill(true);

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
    auto check_row = [this, &grid](wxCheckBox *check, std::vector<wxWindow *> *group = nullptr) {
        wxStaticText *empty = new wxStaticText(this, wxID_ANY, wxEmptyString);
        grid->Add(empty);
        grid->Add(check, 0, wxALIGN_CENTER_VERTICAL);
        if (group != nullptr) {
            group->push_back(empty);
            group->push_back(check);
        }
    };
    auto heading = [this, &grid](const wxString &text, std::vector<wxWindow *> *group = nullptr) {
        wxStaticText *h = new wxStaticText(this, wxID_ANY, text);
        h->SetFont(h->GetFont().Bold());
        wxStaticText *empty = new wxStaticText(this, wxID_ANY, wxEmptyString);
        grid->Add(h, 0, wxALIGN_CENTER_VERTICAL | wxTOP, FromDIP(6));
        grid->Add(empty);
        if (group != nullptr) {
            group->push_back(h);
            group->push_back(empty);
        }
    };
    auto spin_double = [this](double value, double min, double max, double inc, int digits = 2) {
        auto *s = new wxSpinCtrlDouble(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(110), -1), wxSP_ARROW_KEYS, min, max,
                                       value, inc);
        s->SetDigits(digits);
        return s;
    };
    auto filament_choice = [this, filaments](int selected) {
        auto *c = new wxChoice(this, wxID_ANY);
        for (int f = 0; f <= filaments; ++f)
            c->Append(i2o_filament_label(f));
        c->SetSelection(std::clamp(selected, 0, filaments));
        return c;
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
    types.Add(_L("Box with lid"));
    types.Add(_L("Clicker"));
    m_type = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, types);
    m_type->SetSelection(std::clamp(int(s.type), 0, 2));
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
    row(_L("Shape from") + ":", m_shape_from, &m_one_color);

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
    check_row(m_invert, &m_one_color);

    m_blur = spin_double(s.blur, 0., 5., 0.25);
    m_blur->SetToolTip(_L("Blur the image before the outline is traced. Smooths jagged and noisy edges."));
    row(_L("Smoothing") + " (px):", m_blur);

    m_size = spin_double(s.silhouette.longest_side, 10., 1000., 5., 1);
    m_size->SetToolTip(_L("Longest side of the shape: the size of the cookie, or of the box or the clicker without the overhang "
                          "of the lid or the cap."));
    row(_L("Size") + " (mm):", m_size);

    m_close_gaps = spin_double(s.silhouette.close_gaps, 0., 5., 0.1);
    m_close_gaps->SetToolTip(_L("Gaps narrower than this are closed and parts closer than this are joined. 0 = off."));
    row(_L("Close gaps") + " (mm):", m_close_gaps);

    m_min_feature = spin_double(s.silhouette.min_feature, 0., 10., 0.1);
    m_min_feature->SetToolTip(_L("Parts of the shape narrower than this are removed: dough does not come out of thin slots, "
                                 "and a thin arm of a box has no room inside. Removed parts are red in the preview. 0 = off."));
    row(_L("Remove thin parts") + " (mm):", m_min_feature);

    m_fill_holes = new wxCheckBox(this, wxID_ANY, _L("Fill holes"));
    m_fill_holes->SetValue(s.silhouette.fill_holes);
    m_fill_holes->SetToolTip(_L("Use only the outer outline. Untick to keep the holes of the shape: "
                                "a blade around every hole of a cutter, a pillar in a box."));
    check_row(m_fill_holes);

    m_largest_only = new wxCheckBox(this, wxID_ANY, _L("Largest part only"));
    m_largest_only->SetValue(s.silhouette.largest_only);
    m_largest_only->SetToolTip(_L("An image with separate parts: use only the largest one. Untick to keep all of them."));
    check_row(m_largest_only);

    // ---- cookie cutter -----------------------------------------------------------------------
    heading(_L("Cookie cutter"), &m_cutter_controls);
    m_mirror = new wxCheckBox(this, wxID_ANY, _L("Mirror"));
    m_mirror->SetValue(s.cutter_mirror);
    m_mirror->SetToolTip(_L("A cookie cutter is turned over when it is used. Mirrored, the cookie looks like the image."));
    check_row(m_mirror, &m_cutter_controls);

    m_blade_height = spin_double(s.cutter.blade_height, 3., 100., 0.5, 1);
    m_blade_height->SetToolTip(_L("Height of the whole cutter, from the bed to the cutting edge."));
    row(_L("Height") + " (mm):", m_blade_height, &m_cutter_controls);

    m_blade_thickness = spin_double(s.cutter.blade_thickness, 0.3, 5., 0.1);
    m_blade_thickness->SetToolTip(_L("Thickness of the blade. Two lines of the nozzle (0.8 mm for a 0.4 mm nozzle) print well."));
    row(_L("Blade thickness") + " (mm):", m_blade_thickness, &m_cutter_controls);

    m_handle_width = spin_double(s.cutter.handle_width, 0.3, 30., 0.5, 1);
    m_handle_width->SetToolTip(_L("Width of the flange around the blade, which is pressed by hand. It lies on the bed."));
    row(_L("Handle width") + " (mm):", m_handle_width, &m_cutter_controls);

    m_handle_height = spin_double(s.cutter.handle_height, 0.4, 20., 0.5, 1);
    row(_L("Handle height") + " (mm):", m_handle_height, &m_cutter_controls);

    m_sharp_edge = new wxCheckBox(this, wxID_ANY, _L("Sharp cutting edge"));
    m_sharp_edge->SetValue(s.cutter.sharp_edge);
    m_sharp_edge->SetToolTip(_L("The last 2 mm of the blade get thinner, down to one line of the nozzle (0.4 mm)."));
    check_row(m_sharp_edge, &m_cutter_controls);

    wxArrayString braces;
    braces.Add(_L("Automatic"));
    braces.Add(_L("Always"));
    braces.Add(_L("Never"));
    m_braces = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, braces);
    m_braces->SetSelection(std::clamp(int(s.cutter.braces), 0, 2));
    m_braces->SetToolTip(_L("Bars across the opening, at the handle, keep a large cutter in shape. "
                            "Automatic adds them to cutters over 90 mm."));
    row(_L("Braces") + ":", m_braces, &m_cutter_controls);

    // ---- box ---------------------------------------------------------------------------------
    heading(_L("Box with lid"), &m_box_controls);
    m_box_height = spin_double(s.box.height, 3., 300., 1., 1);
    m_box_height->SetToolTip(_L("Outer height of the base, without the lid."));
    row(_L("Height") + " (mm):", m_box_height, &m_box_controls);

    m_box_wall = spin_double(s.box.wall, 0.4, 10., 0.2);
    m_box_wall->SetToolTip(_L("Thickness of the wall. Four lines of the nozzle (1.6 mm for a 0.4 mm nozzle) make a stiff box."));
    row(_L("Wall") + " (mm):", m_box_wall, &m_box_controls);

    m_lid_overhang = spin_double(s.box.lid_overhang, 0., 10., 0.5, 1);
    m_lid_overhang->SetToolTip(_L("The lid is larger than the base by this distance on every side. 0 = flush with the base."));
    row(_L("Lid overhang") + " (mm):", m_lid_overhang, &m_box_controls);

    m_lid_clearance = spin_double(s.box.clearance, 0., 1., 0.05);
    m_lid_clearance->SetToolTip(_L("Gap between the lip of the lid and the wall, on every side. "
                                   "Raise it when the lid is too tight, lower it when it is loose."));
    row(_L("Lid fit gap") + " (mm):", m_lid_clearance, &m_box_controls);

    // ---- clicker -----------------------------------------------------------------------------
    heading(_L("Clicker"), &m_clicker_controls);
    m_body_height = spin_double(s.clicker.body_height, 9., 60., 0.5, 1);
    m_body_height->SetToolTip(_L("Height of the body. The switch needs 8.5 mm above the floor, so about 10 mm is the minimum."));
    row(_L("Body height") + " (mm):", m_body_height, &m_clicker_controls);

    m_socket_fit = spin_double(s.clicker.socket_fit, -0.2, 0.5, 0.05);
    m_socket_fit->SetToolTip(_L("Added to every side of the 14 mm opening of the switch. Raise it when the switch does not go in, "
                                "lower it when it is loose. The fit test piece has 0, 0.1 and 0.2 mm."));
    row(_L("Switch fit") + " (mm):", m_socket_fit, &m_clicker_controls);

    m_stem_fit = spin_double(s.clicker.stem_fit, -0.1, 0.3, 0.05);
    m_stem_fit->SetToolTip(_L("Added to every side of the cross in the cap, which is pushed onto the stem of the switch. "
                              "The fit test piece has 0, 0.05 and 0.1 mm."));
    row(_L("Cap fit") + " (mm):", m_stem_fit, &m_clicker_controls);

    wxBoxSizer *switch_row = new wxBoxSizer(wxHORIZONTAL);
    m_switch_x = spin_double(s.clicker.switch_offset.x(), -200., 200., 1., 1);
    m_switch_y = spin_double(s.clicker.switch_offset.y(), -200., 200., 1., 1);
    for (wxSpinCtrlDouble *sp : {m_switch_x, m_switch_y})
        sp->SetToolTip(_L("The switch is placed in the widest part of the shape. Move it from there to the right (X) and up (Y)."));
    switch_row->Add(m_switch_x, 1, wxRIGHT, FromDIP(6));
    switch_row->Add(m_switch_y, 1);
    wxStaticText *switch_label = label(_L("Move switch X, Y") + " (mm):");
    grid->Add(switch_label, 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(switch_row, 1, wxEXPAND);
    m_clicker_controls.insert(m_clicker_controls.end(), {switch_label, m_switch_x, m_switch_y});

    m_cap_overhang = spin_double(s.clicker.cap_overhang, 0., 10., 0.5, 1);
    m_cap_overhang->SetToolTip(_L("The cap is larger than the body by this distance on every side. 0 = flush with the body."));
    row(_L("Cap overhang") + " (mm):", m_cap_overhang, &m_clicker_controls);

    wxBoxSizer *keyring_row = new wxBoxSizer(wxHORIZONTAL);
    m_keyring = new wxCheckBox(this, wxID_ANY, _L("at"));
    m_keyring->SetValue(s.clicker.keyring);
    m_keyring_angle = spin_double(s.clicker.keyring_angle, -360., 360., 15., 0);
    for (wxWindow *w : {static_cast<wxWindow *>(m_keyring), static_cast<wxWindow *>(m_keyring_angle)})
        w->SetToolTip(_L("A tab with a 5 mm hole for a key ring at the bottom of the body. "
                         "The angle is its direction from the middle of the shape: 0 = right, 90 = up."));
    keyring_row->Add(m_keyring, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
    keyring_row->Add(m_keyring_angle, 1);
    wxStaticText *keyring_label = label(_L("Key ring tab") + wxString::FromUTF8(" (\xC2\xB0):"));
    grid->Add(keyring_label, 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(keyring_row, 1, wxEXPAND);
    m_clicker_controls.insert(m_clicker_controls.end(), {keyring_label, m_keyring, m_keyring_angle});

    wxStaticText *fit_label = label(wxEmptyString);
    m_fit_test = new wxButton(this, wxID_ANY, _L("Add fit test piece"));
    m_fit_test->SetToolTip(_L("Adds a small test print instead of the clicker: a tile with three switch openings and three cap stubs, "
                              "each with another fit. The notches (1, 2, 3) tell them apart. Print it once per printer and filament, "
                              "then set Switch fit and Cap fit to the ones which fit best."));
    grid->Add(fit_label);
    grid->Add(m_fit_test, 0, wxALIGN_LEFT);
    m_clicker_controls.insert(m_clicker_controls.end(), {fit_label, m_fit_test});

    // ---- picture on the lid / cap ------------------------------------------------------------
    wxArrayString pictures;
    pictures.Add(_L("None"));
    for (int c = 2; c <= IMAGE_OBJECT_MAX_COLORS; ++c)
        pictures.Add(wxString::Format(_L("%d colours"), c));
    m_picture = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, pictures);
    m_picture->SetSelection(s.picture_colors < 2 ? 0 : std::min(s.picture_colors, IMAGE_OBJECT_MAX_COLORS) - 1);
    m_picture->SetToolTip(_L("The image is inlaid into the top face of the lid or the cap, one filament per colour. "
                             "That part is printed with its top face on the bed, so the picture is in the first layers."));
    row(_L("Picture on top") + ":", m_picture, &m_picture_controls);

    m_body_filament = filament_choice(s.body_extruder);
    m_body_filament->SetToolTip(_L("Filament of everything but the picture."));
    row(_L("Body filament") + ":", m_body_filament, &m_picture_controls);

    for (int i = 0; i < IMAGE_OBJECT_MAX_COLORS; ++i) {
        wxImage swatch(FromDIP(22), FromDIP(14));
        swatch.SetRGB(wxRect(0, 0, swatch.GetWidth(), swatch.GetHeight()), 128, 128, 128);
        wxBoxSizer *level = new wxBoxSizer(wxHORIZONTAL);
        m_level_swatch[size_t(i)] = new wxStaticBitmap(this, wxID_ANY, wxBitmap(swatch));
        m_level_label[size_t(i)]  = new wxStaticText(this, wxID_ANY, wxString::Format(_L("Colour %d"), i + 1) + ":");
        level->Add(m_level_swatch[size_t(i)], 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
        level->Add(m_level_label[size_t(i)], 0, wxALIGN_CENTER_VERTICAL);
        grid->Add(level, 0, wxALIGN_CENTER_VERTICAL);
        m_level_filament[size_t(i)] = filament_choice(s.level_extruders[size_t(i)]);
        m_level_filament[size_t(i)]->SetToolTip(_L("Filament of this colour of the picture. By default the loaded filament with the nearest colour."));
        grid->Add(m_level_filament[size_t(i)], 1, wxEXPAND | wxALIGN_CENTER_VERTICAL);
    }
    grid->Add(new wxStaticText(this, wxID_ANY, wxEmptyString));
    m_match_filaments = new wxButton(this, wxID_ANY, _L("Match filaments") + dots);
    m_match_filaments->SetToolTip(_L("Add filaments of the colours of the picture, give the project filaments these colours, "
                                     "or use the nearest existing filaments."));
    m_match_filaments->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { show_match_menu(); });
    grid->Add(m_match_filaments, 0, wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL);

    cols->Add(grid, 1, wxEXPAND | wxALL, FromDIP(10));

    // ---- preview -----------------------------------------------------------------------------
    wxBoxSizer *right = new wxBoxSizer(wxVERTICAL);
    int         px    = FromDIP(I2O_PREVIEW_PX);
    right->Add(label(_L("Image") + ":"), 0, wxBOTTOM, FromDIP(4));
    m_source_preview = new wxStaticBitmap(this, wxID_ANY, wxBitmap(i2o_blank(px)));
    right->Add(m_source_preview, 0, wxBOTTOM, FromDIP(8));
    wxArrayString views;
    views.Add(_L("Object from above"));
    views.Add(_L("Object in 3D"));
    m_view = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, views);
    {
        AppConfig *cfg = wxGetApp().app_config;
        m_view->SetSelection(cfg != nullptr && cfg->get(I2O_CFG_SECTION, "view_3d") == "1" ? 1 : 0);
    }
    m_view->SetToolTip(_L("From above shows the cross-sections and what is removed. 3D shows the objects as they are added to the plate; "
                          "it follows the settings a little slower."));
    right->Add(m_view, 0, wxBOTTOM, FromDIP(4));
    m_preview_bitmap = new wxStaticBitmap(this, wxID_ANY, wxBitmap(i2o_blank(px)));
    right->Add(m_preview_bitmap, 0);
    wxBoxSizer *turn_row = new wxBoxSizer(wxHORIZONTAL);
    m_turn_label         = new wxStaticText(this, wxID_ANY, _L("Turn") + ":");
    m_turn               = new wxSlider(this, wxID_ANY, int(m_yaw), -180, 180, wxDefaultPosition, wxSize(FromDIP(160), -1));
    m_turn->SetToolTip(_L("Turn the objects. The picture can also be dragged with the mouse."));
    turn_row->Add(m_turn_label, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
    turn_row->Add(m_turn, 1, wxALIGN_CENTER_VERTICAL);
    right->Add(turn_row, 0, wxEXPAND | wxTOP, FromDIP(4));
    m_info = new wxStaticText(this, wxID_ANY, wxEmptyString);
    right->Add(m_info, 0, wxTOP, FromDIP(6));
    m_warning = new wxStaticText(this, wxID_ANY, wxEmptyString);
    m_warning->SetForegroundColour(wxColour("#D9534F"));
    right->Add(m_warning, 0, wxTOP, FromDIP(6));
    cols->Add(right, 0, wxEXPAND | wxALL, FromDIP(10));
    root->Add(cols, 1, wxEXPAND);

    wxStdDialogButtonSizer *buttons = new wxStdDialogButtonSizer();
    m_ok = new wxButton(this, wxID_OK, m_is_edit ? _L("Apply") : _L("Add"));
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
    for (wxChoice *c : {m_type, m_shape_from, m_braces, m_picture, m_body_filament, m_view})
        c->Bind(wxEVT_CHOICE, changed);
    for (wxCheckBox *c : {m_auto_threshold, m_invert, m_fill_holes, m_largest_only, m_mirror, m_sharp_edge, m_keyring})
        c->Bind(wxEVT_CHECKBOX, changed);
    for (wxSpinCtrlDouble *sp : {m_blur, m_size, m_close_gaps, m_min_feature, m_blade_height, m_blade_thickness, m_handle_width,
                                 m_handle_height, m_box_height, m_box_wall, m_lid_overhang, m_lid_clearance, m_body_height,
                                 m_socket_fit, m_stem_fit, m_switch_x, m_switch_y, m_cap_overhang, m_keyring_angle})
        sp->Bind(wxEVT_SPINCTRLDOUBLE, changed_double);
    m_threshold->Bind(wxEVT_SLIDER, [this](wxCommandEvent &evt) {
        // moving the slider means a manual threshold
        m_auto_threshold->SetValue(false);
        schedule_preview();
        evt.Skip();
    });
    for (int i = 0; i < IMAGE_OBJECT_MAX_COLORS; ++i)
        m_level_filament[size_t(i)]->Bind(wxEVT_CHOICE, [this, i](wxCommandEvent &evt) {
            m_user_filament[size_t(i)] = true;
            schedule_preview();
            evt.Skip();
        });
    m_change_image->Bind(wxEVT_BUTTON, [this](wxCommandEvent &evt) {
        change_image();
        evt.Skip();
    });
    m_fit_test->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        I2O_LOG("Add fit test piece pressed");
        if (finish_fit_test()) {
            I2O_LOG("closing the dialog");
            EndModal(wxID_OK);
        }
    });
    Bind(wxEVT_TIMER, [this](wxTimerEvent &) { refresh_preview(); });
    // turning draws the same objects again, they are not made again
    m_turn->Bind(wxEVT_SLIDER, [this](wxCommandEvent &evt) {
        m_yaw = double(m_turn->GetValue());
        render_3d();
        evt.Skip();
    });
    m_preview_bitmap->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent &evt) {
        if (view_3d() && !m_preview_objects.empty()) {
            m_dragging  = true;
            m_drag_from = evt.GetPosition();
            if (!m_preview_bitmap->HasCapture())
                m_preview_bitmap->CaptureMouse();
        }
        evt.Skip();
    });
    m_preview_bitmap->Bind(wxEVT_MOTION, [this](wxMouseEvent &evt) {
        if (m_dragging && evt.LeftIsDown()) {
            const wxPoint delta = evt.GetPosition() - m_drag_from;
            m_drag_from         = evt.GetPosition();
            m_yaw              += 0.6 * double(delta.x);
            while (m_yaw > 180.)
                m_yaw -= 360.;
            while (m_yaw < -180.)
                m_yaw += 360.;
            m_pitch = std::clamp(m_pitch + 0.4 * double(delta.y), 5., 90.);
            m_turn->SetValue(int(m_yaw));
            render_3d();
        }
        evt.Skip();
    });
    auto end_drag = [this]() {
        m_dragging = false;
        if (m_preview_bitmap->HasCapture())
            m_preview_bitmap->ReleaseMouse();
    };
    m_preview_bitmap->Bind(wxEVT_LEFT_UP, [end_drag](wxMouseEvent &evt) {
        end_drag();
        evt.Skip();
    });
    m_preview_bitmap->Bind(wxEVT_MOUSE_CAPTURE_LOST, [this](wxMouseCaptureLostEvent &) { m_dragging = false; });
    m_ok->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        I2O_LOG("Add pressed");
        if (finish()) {
            I2O_LOG("closing the dialog");
            EndModal(wxID_OK);
        }
    });

    SetSizer(root);
    refresh_source_preview();
    update_enabled();
    refresh_preview();

    Layout();
    root->Fit(this);
    CenterOnParent();
    wxGetApp().UpdateDlgDarkUI(this);
}

ImageToObjectDialog::~ImageToObjectDialog()
{
    m_timer.Stop();
    if (m_preview_bitmap != nullptr && m_preview_bitmap->HasCapture())
        m_preview_bitmap->ReleaseMouse();
}

std::string ImageToObjectDialog::name_of(const std::string &what) const
{
    return (m_image_name.empty() ? _u8L("Image") : m_image_name) + " - " + what;
}

std::string ImageToObjectDialog::filament_color(int extruder) const
{
    if (extruder >= 1 && size_t(extruder) <= m_filament_colors.size())
        return m_filament_colors[size_t(extruder - 1)];
    return {};
}

int ImageToObjectDialog::extruder(const OutlinePart &part) const
{
    if (m_settings.type == ImageObjectType::CookieCutter || m_is_fit_test)
        return 0;
    if (part.level < 0)
        return m_settings.body_extruder;
    return part.level < IMAGE_OBJECT_MAX_COLORS ? m_settings.level_extruders[size_t(part.level)] : 0;
}

bool ImageToObjectDialog::with_picture(const ImageToObjectSettings &settings) const
{
    return settings.type != ImageObjectType::CookieCutter && settings.picture_colors >= 2;
}

ImageToObjectSettings ImageToObjectDialog::collect() const
{
    ImageToObjectSettings s = m_settings;
    s.type                    = ImageObjectType(std::clamp(m_type->GetSelection(), 0, 2));
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
    s.cutter_mirror           = m_mirror->GetValue();
    // The box is seen from above as the image is; only the cutter is turned over
    s.silhouette.mirror       = s.type == ImageObjectType::CookieCutter && s.cutter_mirror;
    s.cutter.blade_height     = m_blade_height->GetValue();
    s.cutter.blade_thickness  = m_blade_thickness->GetValue();
    s.cutter.handle_width     = m_handle_width->GetValue();
    s.cutter.handle_height    = m_handle_height->GetValue();
    s.cutter.sharp_edge       = m_sharp_edge->GetValue();
    s.cutter.braces           = CutterBraces(std::clamp(m_braces->GetSelection(), 0, 2));
    s.box.height              = m_box_height->GetValue();
    s.box.wall                = m_box_wall->GetValue();
    s.box.lid_overhang        = m_lid_overhang->GetValue();
    s.box.clearance           = m_lid_clearance->GetValue();
    int picture               = m_picture->GetSelection();
    s.picture_colors          = picture <= 0 ? 0 : picture + 1;
    s.box.decoration          = s.picture_colors >= 2;
    s.clicker.decoration      = s.picture_colors >= 2;
    s.clicker.body_height     = m_body_height->GetValue();
    s.clicker.socket_fit      = m_socket_fit->GetValue();
    s.clicker.stem_fit        = m_stem_fit->GetValue();
    s.clicker.switch_offset   = Vec2d(m_switch_x->GetValue(), m_switch_y->GetValue());
    s.clicker.cap_overhang    = m_cap_overhang->GetValue();
    s.clicker.keyring         = m_keyring->GetValue();
    s.clicker.keyring_angle   = m_keyring_angle->GetValue();
    s.body_extruder           = std::max(0, m_body_filament->GetSelection());
    for (int i = 0; i < IMAGE_OBJECT_MAX_COLORS; ++i)
        s.level_extruders[size_t(i)] = std::max(0, m_level_filament[size_t(i)]->GetSelection());
    return s;
}

void ImageToObjectDialog::update_enabled()
{
    const ImageToObjectSettings s = collect();
    bool box        = s.type == ImageObjectType::Box;
    bool clicker    = s.type == ImageObjectType::Clicker;
    bool cutter     = s.type == ImageObjectType::CookieCutter;
    bool one        = !with_picture(s);
    int  from       = m_shape_from->GetSelection();
    bool brightness = one && (from == int(TraceShapeFrom::Brightness) || (from == int(TraceShapeFrom::Auto) && !m_image.has_alpha()));
    for (wxWindow *w : m_one_color)
        w->Show(one);
    for (wxWindow *w : m_brightness)
        w->Show(brightness);
    for (wxWindow *w : m_cutter_controls)
        w->Show(cutter);
    for (wxWindow *w : m_box_controls)
        w->Show(box);
    for (wxWindow *w : m_clicker_controls)
        w->Show(clicker);
    for (wxWindow *w : m_picture_controls)
        w->Show(!cutter);
    m_keyring_angle->Enable(m_keyring->GetValue());
    // the fit test piece is a new object, not a change of this one
    m_fit_test->Show(clicker && !m_is_edit);
    m_turn->Show(view_3d());
    m_turn_label->Show(view_3d());
    if (cutter || one)
        for (int i = 0; i < IMAGE_OBJECT_MAX_COLORS; ++i)
            for (wxWindow *w : {static_cast<wxWindow *>(m_level_swatch[size_t(i)]), static_cast<wxWindow *>(m_level_label[size_t(i)]),
                                static_cast<wxWindow *>(m_level_filament[size_t(i)])})
                w->Show(false);
    if (cutter || one)
        m_match_filaments->Show(false);
    Layout();
    if (GetSizer() != nullptr)
        GetSizer()->Fit(this);
}

void ImageToObjectDialog::update_level_rows(const Shape &shape)
{
    const ImageToObjectSettings s    = collect();
    size_t                      count = with_picture(s) ? shape.colors.size() : 0;

    // Levels the user did not touch get the loaded filament with the nearest colour
    if (count > 0 && !m_filament_colors.empty()) {
        TraceFilamentPlan plan = plan_trace_filaments(shape.colors, m_filament_colors, TraceFilamentMatch::Nearest);
        for (size_t i = 0; i < count && i < size_t(IMAGE_OBJECT_MAX_COLORS) && i < plan.filaments.size(); ++i)
            if (!m_user_filament[i])
                m_level_filament[i]->SetSelection(std::clamp(plan.filaments[i], 0, int(m_level_filament[i]->GetCount()) - 1));
    }
    for (size_t i = 0; i < size_t(IMAGE_OBJECT_MAX_COLORS); ++i) {
        bool show = i < count;
        m_level_swatch[i]->Show(show);
        m_level_label[i]->Show(show);
        m_level_filament[i]->Show(show);
        if (!show)
            continue;
        wxImage swatch(FromDIP(22), FromDIP(14));
        swatch.SetRGB(wxRect(0, 0, swatch.GetWidth(), swatch.GetHeight()), shape.colors[i][0], shape.colors[i][1], shape.colors[i][2]);
        m_level_swatch[i]->SetBitmap(wxBitmap(swatch));
    }
    m_level_colors.assign(shape.colors.begin(), shape.colors.begin() + std::min(count, shape.colors.size()));
    m_match_filaments->Show(count > 0);
}

void ImageToObjectDialog::refill_filament_choices()
{
    const int count = std::max<int>(1, int(m_filament_colors.size()));
    auto      refill = [count](wxChoice *c) {
        if (c == nullptr || int(c->GetCount()) == count + 1)
            return;
        int selection = c->GetSelection();
        c->Freeze();
        c->Clear();
        for (int f = 0; f <= count; ++f)
            c->Append(i2o_filament_label(f));
        c->SetSelection(std::clamp(selection, 0, count));
        c->Thaw();
    };
    refill(m_body_filament);
    for (wxChoice *c : m_level_filament)
        refill(c);
}

void ImageToObjectDialog::show_match_menu()
{
    if (m_level_colors.empty())
        return;
    wxMenu    menu;
    const int add_id = wxWindow::NewControlId(), replace_id = wxWindow::NewControlId(), nearest_id = wxWindow::NewControlId();
    menu.Append(add_id, _L("Add new filaments"),
                _L("Add a filament of every colour of the picture; a filament of a very close colour is used instead of a new one."));
    menu.Append(replace_id, _L("Replace filament colours"),
                _L("Give filaments 1, 2, ... the colours of the picture, adding filaments when there are fewer."));
    menu.Append(nearest_id, _L("Nearest existing filaments"), _L("Use the filament of the nearest colour, nothing changes."));
    menu.Bind(wxEVT_MENU, [this, add_id, replace_id](wxCommandEvent &evt) {
        TraceFilamentMatch mode = evt.GetId() == add_id     ? TraceFilamentMatch::AddNew :
                                  evt.GetId() == replace_id ? TraceFilamentMatch::ReplaceColours :
                                                              TraceFilamentMatch::Nearest;
        // after the menu is closed, the filament changes rebuild the sidebar
        CallAfter([this, mode]() { match_filaments(mode); });
    });
    PopupMenu(&menu, m_match_filaments->GetPosition() + wxPoint(0, m_match_filaments->GetSize().GetHeight()));
}

void ImageToObjectDialog::match_filaments(TraceFilamentMatch mode)
{
    Plater       *plater = wxGetApp().plater();
    PresetBundle *bundle = wxGetApp().preset_bundle;
    if (plater == nullptr || bundle == nullptr || m_level_colors.empty())
        return;
    const std::vector<std::array<uint8_t, 3>> colors = m_level_colors;
    m_filament_colors      = plater->get_extruder_colors_from_plater_config(nullptr, false);
    TraceFilamentPlan plan = plan_trace_filaments(colors, m_filament_colors, mode);

    if (!plan.recolour.empty()) {
        // say all of it: with 4 filaments and 8 colours, "1 to 4" alone reads as if 4 colours were lost
        wxString text = GUI::format_wxstr(_L("The picture has %1% colours, which go to filaments 1 to %1%."), int(colors.size()));
        text += "\n" + GUI::format_wxstr(_L("%1% existing filaments (from %2% to %3%) get a new colour."), int(plan.recolour.size()),
                                         plan.recolour.front().first, plan.recolour.back().first);
        text += " " + _L("Other objects printed with these filaments change their colour too.");
        if (!plan.add.empty())
            text += "\n" + GUI::format_wxstr(_L("%1% new filaments are added."), int(plan.add.size()));
        text += "\n" + _L("Continue?");
        MessageDialog dialog(this, text, _L("Replace filament colours"), wxYES_NO | wxICON_QUESTION);
        if (dialog.ShowModal() != wxID_YES)
            return;
    }

    // the same paths as the sidebar's "+" button and colour picker
    Sidebar &sidebar = wxGetApp().sidebar();
    size_t   added   = 0;
    for (const std::string &hex : plan.add) {
        const size_t before = bundle->num_physical_filaments();
        sidebar.add_custom_filament(wxColour(from_u8(hex)));
        if (bundle->num_physical_filaments() <= before)
            break; // the slicer's own maximum of filaments
        ++added;
    }
    for (const auto &[filament, hex] : plan.recolour) {
        std::vector<PlaterPresetComboBox *> &combos = sidebar.combos_filament();
        if (filament >= 1 && size_t(filament) <= combos.size() && combos[size_t(filament - 1)] != nullptr)
            combos[size_t(filament - 1)]->ApplyFilamentColor(FilamentColor::FromColors({hex}, FilamentColorMode::Segment));
    }

    m_filament_colors = plater->get_extruder_colors_from_plater_config(nullptr, false);
    refill_filament_choices();
    const TraceFilamentPlan nearest = plan_trace_filaments(colors, m_filament_colors, TraceFilamentMatch::Nearest);
    for (size_t i = 0; i < plan.filaments.size() && i < m_level_filament.size(); ++i) {
        int filament = plan.filaments[i];
        if (filament > int(m_filament_colors.size()))
            filament = i < nearest.filaments.size() ? nearest.filaments[i] : 0; // not added (maximum of filaments)
        m_level_filament[i]->SetSelection(std::clamp(filament, 0, int(m_level_filament[i]->GetCount()) - 1));
        m_user_filament[i] = true;
    }
    refresh_preview();
    if (added < plan.add.size()) {
        m_warning->SetLabel(GUI::format_wxstr(_L("Only %1% of %2% filaments could be added, the slicer's maximum is reached. "
                                                 "The other colours use the nearest filament."),
                                              int(added), int(plan.add.size())));
        m_warning->Wrap(FromDIP(I2O_PREVIEW_PX));
        Layout();
    }
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

ImageToObjectDialog::Shape ImageToObjectDialog::make_shape(const TraceImage &image, const ImageToObjectSettings &settings, int max_side) const
{
    Shape            out;
    const bool       picture = with_picture(settings);
    ImageTraceParams tp;
    tp.colors         = picture ? std::clamp(settings.picture_colors, 2, IMAGE_OBJECT_MAX_COLORS) : 1;
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
    out.threshold          = trace.threshold;
    if (!trace.is_valid()) {
        out.error = trace.error.empty() ? std::string("No shape was found in the image.") : trace.error;
        return out;
    }
    std::vector<ExPolygons> levels;
    for (const ImageTraceLayer &layer : trace.layers)
        levels.push_back(layer.shape);
    out.silhouette = make_silhouette(levels, settings.silhouette);
    if (picture && !out.silhouette.empty())
        for (size_t i = 0; i < trace.layers.size() && i < size_t(IMAGE_OBJECT_MAX_COLORS); ++i) {
            out.detail.push_back(to_silhouette_frame(trace.layers[i].shape, out.silhouette));
            out.colors.push_back(trace.layers[i].color);
        }
    return out;
}

void ImageToObjectDialog::show_failure(const std::string &what)
{
    BOOST_LOG_TRIVIAL(error) << "Image to object failed: " << what;
    m_warning->SetLabel(_L("The object could not be created from this image with these settings.") + "\n" + from_u8(what));
    m_warning->Wrap(FromDIP(I2O_PREVIEW_PX));
    Layout();
    if (GetSizer() != nullptr)
        GetSizer()->Fit(this);
}

void ImageToObjectDialog::refresh_preview()
{
    m_timer.Stop();
    try {
        refresh_preview_unchecked();
    } catch (const std::exception &e) {
        m_preview_bitmap->SetBitmap(wxBitmap(i2o_blank(FromDIP(I2O_PREVIEW_PX))));
        m_info->SetLabel(wxEmptyString);
        m_ok->Enable(false);
        show_failure(e.what());
    }
}

void ImageToObjectDialog::refresh_preview_unchecked()
{
    ImageToObjectSettings s  = collect();
    I2O_LOG("preview of type " << int(s.type));
    int                   px = FromDIP(I2O_PREVIEW_PX);
    Shape                 shape = make_shape(m_preview_image, s, I2O_PREVIEW_SIDE);
    if (s.auto_threshold && shape.threshold >= 0)
        m_threshold->SetValue(shape.threshold);
    update_level_rows(shape);
    s = collect(); // filaments of the levels

    const Silhouette &sil = shape.silhouette;
    if (!sil.empty()) {
        std::vector<I2OLayer> layers;
        wxString              info, problem;
        bool                  fits = true;
        if (s.type == ImageObjectType::CookieCutter) {
            const CookieCutterParams   cutter   = sanitize(s.cutter);
            const CookieCutterSections sections = cookie_cutter_sections(sil.shape, cutter);
            layers.push_back({sections.handle, "#8FB3E8"});
            layers.push_back({sections.blade, "#1F3F7A"});
            info = wxString::Format(_L("Cookie: %.1f x %.1f mm"), sil.width, sil.height);
            info += "\n" + wxString::Format(_L("Cutter: %.1f x %.1f x %.1f mm"), sil.width + 2. * cutter.handle_width,
                                            sil.height + 2. * cutter.handle_width, cutter.blade_height);
            if (sections.braces)
                info += "\n" + _L("With braces across the opening.");
            info += "\n" + _L("Print it as it is: handle on the bed, no supports.");
        } else if (s.type == ImageObjectType::Clicker) {
            const ClickerParams   clicker  = sanitize(s.clicker);
            const ClickerSections sections = clicker_sections(sil.shape, shape.detail, clicker);
            std::string           body     = filament_color(s.body_extruder);
            layers.push_back({sections.cap, body.empty() ? std::string("#B8C4D6") : body});
            for (size_t i = 0; i < sections.decoration.size(); ++i) {
                std::string color = filament_color(i < size_t(IMAGE_OBJECT_MAX_COLORS) ? s.level_extruders[i] : 0);
                if (color.empty() && i < shape.colors.size())
                    color = image_trace_color_to_hex(shape.colors[i]);
                layers.push_back({sections.decoration[i], color.empty() ? std::string("#808080") : color});
            }
            // the body and the switch under the cap
            layers.push_back({sections.body, "#000000", 0.2});
            layers.push_back({sections.pocket, sections.fits ? std::string("#000000") : std::string("#E03030"), sections.fits ? 0.35 : 0.8});
            layers.push_back({sections.post, "#FFFFFF", 0.9});
            info = wxString::Format(_L("Body: %.1f x %.1f x %.1f mm"), sil.width, sil.height, clicker.body_height);
            info += "\n" + wxString::Format(_L("Cap: %.1f x %.1f mm"), sil.width + 2. * clicker.cap_overhang,
                                            sil.height + 2. * clicker.cap_overhang);
            info += "\n" + _L("The square is the switch. Two objects are added: body and cap. No supports.");
            fits = sections.fits;
            if (!fits) {
                // size at which the widest part of the shape takes the switch
                double needed = sections.inscribed_radius > 0. ?
                                    s.silhouette.longest_side * sections.needed_radius / sections.inscribed_radius * 1.02 : 0.;
                problem = sections.inscribed_radius >= sections.needed_radius ?
                              _L("The switch is too close to the edge here. Move it back towards the middle.") :
                              wxString::Format(_L("The shape is too small for the switch. Set Size to at least %.0f mm."), std::ceil(needed));
            }
        } else {
            const OutlineBoxParams   box      = sanitize(s.box);
            const OutlineBoxSections sections = outline_box_sections(sil.shape, shape.detail, box);
            std::string              body     = filament_color(s.body_extruder);
            layers.push_back({sections.lid, body.empty() ? std::string("#B8C4D6") : body});
            for (size_t i = 0; i < sections.decoration.size(); ++i) {
                std::string color = filament_color(i < size_t(IMAGE_OBJECT_MAX_COLORS) ? s.level_extruders[i] : 0);
                if (color.empty() && i < shape.colors.size())
                    color = image_trace_color_to_hex(shape.colors[i]);
                layers.push_back({sections.decoration[i], color.empty() ? std::string("#808080") : color});
            }
            // the wall of the base under the lid
            layers.push_back({sections.wall, "#000000", 0.3});
            info = wxString::Format(_L("Base: %.1f x %.1f x %.1f mm"), sil.width, sil.height, box.height);
            info += "\n" + wxString::Format(_L("Lid: %.1f x %.1f x %.1f mm"), sil.width + 2. * box.lid_overhang,
                                            sil.height + 2. * box.lid_overhang, box.lid_thickness + box.lip_height);
            if (sections.cavity.empty())
                info += "\n" + _L("The shape is too thin for a cavity at this wall thickness.");
            if (!sections.decoration.empty())
                info += "\n" + _L("The lid is printed with the picture on the bed.");
            info += "\n" + _L("Two objects are added: base and lid. No supports.");
        }
        layers.push_back({sil.removed, "#E03030"});

        // The objects themselves, when they are wanted and can be made
        bool drawn = false;
        m_preview_objects.clear();
        m_preview_colors.clear();
        if (view_3d() && fits) {
            try {
                std::vector<std::string> ignored;
                std::string              error;
                if (generate(shape, s, m_preview_objects, ignored, error)) {
                    for (const OutlineObject &object : m_preview_objects)
                        for (const OutlinePart &part : object.parts) {
                            std::array<uint8_t, 3> color{184, 196, 214};
                            if (s.type == ImageObjectType::CookieCutter)
                                color = {143, 179, 232};
                            else if (part.level < 0)
                                i2o_parse_color(filament_color(s.body_extruder), color);
                            else if (!i2o_parse_color(filament_color(part.level < IMAGE_OBJECT_MAX_COLORS ? s.level_extruders[size_t(part.level)] : 0),
                                                      color))
                                color = size_t(part.level) < shape.colors.size() ? shape.colors[size_t(part.level)] :
                                                                                    std::array<uint8_t, 3>{128, 128, 128};
                            m_preview_colors.push_back(color);
                        }
                    drawn = render_3d();
                }
            } catch (const std::exception &e) {
                // the picture from above is shown instead
                BOOST_LOG_TRIVIAL(warning) << "Image to object: no 3D preview, " << e.what();
                m_preview_objects.clear();
            }
        }
        if (!drawn)
            m_preview_bitmap->SetBitmap(i2o_render(layers, px));
        m_info->SetLabel(info);
        wxString warnings = i2o_messages(sil.warnings);
        if (!problem.empty())
            warnings = problem + (warnings.empty() ? "" : "\n") + warnings;
        m_warning->SetLabel(warnings);
        m_ok->Enable(fits);
    } else {
        m_preview_objects.clear();
        m_preview_bitmap->SetBitmap(wxBitmap(i2o_blank(px)));
        m_info->SetLabel(wxEmptyString);
        m_warning->SetLabel(shape.error.empty() ? i2o_messages(sil.warnings) : _(from_u8(shape.error)));
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
    m_source_png    = std::move(source_png);
    m_preview_image = downscale_trace_image(m_image, I2O_PREVIEW_SIDE);
    m_image_name    = name;
    m_user_filament.fill(false);
    refresh_source_preview();
    update_enabled();
    refresh_preview();
}

bool ImageToObjectDialog::finish()
{
    m_timer.Stop();
    try {
        return finish_unchecked();
    } catch (const std::exception &e) {
        m_objects.clear();
        show_failure(e.what());
        return false;
    }
}

bool ImageToObjectDialog::generate(const Shape &shape, const ImageToObjectSettings &settings, std::vector<OutlineObject> &objects,
                                   std::vector<std::string> &warnings, std::string &error) const
{
    objects.clear();
    warnings.clear();
    if (shape.silhouette.empty())
        return false;
    if (settings.type == ImageObjectType::CookieCutter) {
        OutlineObjectResult cutter = make_cookie_cutter(shape.silhouette.shape, settings.cutter);
        error                      = cutter.error;
        if (cutter.is_valid()) {
            OutlineObject object;
            object.name = name_of(_u8L("Cookie cutter"));
            object.parts.push_back({object.name, std::move(cutter.mesh), -1});
            objects.push_back(std::move(object));
            warnings = cutter.warnings;
        }
    } else if (settings.type == ImageObjectType::Clicker) {
        ClickerResult clicker = make_clicker(shape.silhouette.shape, shape.detail, settings.clicker);
        error                 = clicker.error;
        if (clicker.is_valid()) {
            clicker.body.name = name_of(_u8L("Clicker body"));
            clicker.cap.name  = name_of(_u8L("Clicker cap"));
            for (OutlineObject *object : {&clicker.body, &clicker.cap})
                for (OutlinePart &part : object->parts)
                    part.name = part.level < 0 ? object->name : GUI::format(_u8L("Colour %1%"), part.level + 1);
            objects.push_back(std::move(clicker.body));
            objects.push_back(std::move(clicker.cap));
            warnings = clicker.warnings;
        }
    } else {
        OutlineBoxResult box = make_outline_box(shape.silhouette.shape, shape.detail, settings.box);
        error                = box.error;
        if (box.is_valid()) {
            box.base.name = name_of(_u8L("Box"));
            box.lid.name  = name_of(_u8L("Lid"));
            for (OutlineObject *object : {&box.base, &box.lid})
                for (OutlinePart &part : object->parts)
                    part.name = part.level < 0 ? object->name : GUI::format(_u8L("Colour %1%"), part.level + 1);
            objects.push_back(std::move(box.base));
            objects.push_back(std::move(box.lid));
            warnings = box.warnings;
        }
    }
    return !objects.empty();
}

bool ImageToObjectDialog::view_3d() const { return m_view != nullptr && m_view->GetSelection() == 1; }

bool ImageToObjectDialog::render_3d()
{
    if (m_preview_objects.empty())
        return false;
    // The objects stand side by side on the bed, as they are added
    std::vector<MeshPreviewItem> items;
    float                        next_x = 0.f;
    size_t                       color  = 0;
    for (const OutlineObject &object : m_preview_objects) {
        Vec3f lo = Vec3f::Constant(std::numeric_limits<float>::max());
        Vec3f hi = Vec3f::Constant(std::numeric_limits<float>::lowest());
        for (const OutlinePart &part : object.parts)
            for (const stl_vertex &v : part.mesh.vertices) {
                lo = lo.cwiseMin(v);
                hi = hi.cwiseMax(v);
            }
        if (lo.x() > hi.x())
            continue;
        const Vec3f offset(next_x - lo.x(), -0.5f * (lo.y() + hi.y()), -lo.z());
        for (const OutlinePart &part : object.parts) {
            MeshPreviewItem item;
            item.mesh   = &part.mesh;
            item.offset = offset;
            if (color < m_preview_colors.size())
                item.color = m_preview_colors[color];
            ++color;
            items.push_back(item);
        }
        next_x += hi.x() - lo.x() + float(I2O_OBJECT_GAP);
    }
    const int        px    = FromDIP(I2O_PREVIEW_PX);
    MeshPreviewImage image = render_mesh_preview(items, px, px, m_yaw, m_pitch);
    if (image.empty())
        return false;
    m_preview_bitmap->SetBitmap(i2o_bitmap(image.rgba.data(), image.width, image.height, px));
    return true;
}

bool ImageToObjectDialog::finish_unchecked()
{
    m_settings = collect();
    I2O_LOG("generating type " << int(m_settings.type) << ", size " << m_settings.silhouette.longest_side << " mm, picture colours "
                               << m_settings.picture_colors);
    m_objects.clear();
    m_warnings.clear();
    m_is_fit_test = false;
    std::string error;
    {
        wxBusyCursor busy;
        Shape        shape = make_shape(m_image, m_settings, I2O_FINAL_SIDE);
        error              = shape.error;
        I2O_LOG("traced: " << shape.silhouette.shape.size() << " islands, " << shape.detail.size() << " colour levels, "
                           << shape.silhouette.width << " x " << shape.silhouette.height << " mm");
        // The colours of this trace are put into the rows of the dialog by their value: the
        // fine trace can leave out a colour of the rows (only specks of it), and by their order
        // every colour after it would get the filament of another one.
        const size_t            rows = m_level_colors.size();
        std::vector<bool>       found(rows, false);
        if (rows > 0 && !shape.detail.empty()) {
            const std::vector<int>  row_of = match_image_object_colors(shape.colors, m_level_colors);
            std::vector<ExPolygons> detail(rows);
            for (size_t i = 0; i < shape.detail.size() && i < row_of.size(); ++i) {
                const size_t row = size_t(std::clamp(row_of[i], 0, int(rows) - 1));
                append(detail[row], std::move(shape.detail[i]));
                found[row] = true;
            }
            shape.detail = std::move(detail);
            shape.colors = m_level_colors;
        }
        if (shape.silhouette.empty()) {
            if (error.empty() && !shape.silhouette.warnings.empty())
                error = shape.silhouette.warnings.front();
        } else if (generate(shape, m_settings, m_objects, m_warnings, error) && with_picture(m_settings)) {
            // say which colours of the rows are not in the object
            for (size_t row = 0; row < rows; ++row) {
                bool made = false;
                for (const OutlineObject &object : m_objects)
                    for (const OutlinePart &part : object.parts)
                        made = made || part.level == int(row);
                if (!made)
                    m_warnings.push_back(GUI::format(_u8L("Colour %1% of the picture is only in details too small to print and was left out."),
                                                     row + 1));
            }
        }
    }
    for (const OutlineObject &object : m_objects)
        for (const OutlinePart &part : object.parts)
            I2O_LOG("generated \"" << part.name << "\", " << part.mesh.indices.size() << " triangles");
    if (m_objects.empty()) {
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

bool ImageToObjectDialog::finish_fit_test()
{
    m_timer.Stop();
    m_settings = collect();
    m_warnings.clear();
    try {
        wxBusyCursor busy;
        m_objects = make_clicker_fit_test(m_settings.clicker);
        I2O_LOG("fit test generated, " << m_objects.size() << " objects");
    } catch (const std::exception &e) {
        m_objects.clear();
        show_failure(e.what());
        return false;
    }
    if (m_objects.size() == 2) {
        m_objects[0].name = _u8L("Clicker fit test - switch");
        m_objects[1].name = _u8L("Clicker fit test - cap");
        for (OutlineObject &object : m_objects)
            for (OutlinePart &part : object.parts)
                part.name = object.name;
    }
    m_is_fit_test = !m_objects.empty();
    if (m_is_fit_test)
        save_to_config();
    return m_is_fit_test;
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

    I2O_LOG("opening the dialog, image " << image.width << " x " << image.height << " px");
    ImageToObjectDialog dialog(nullptr, image, source_png, name, ImageToObjectDialog::load_from_config());
    int answer = dialog.ShowModal();
    I2O_LOG("dialog closed, answer " << answer << ", " << dialog.objects().size() << " objects");
    if (answer != wxID_OK || dialog.objects().empty())
        return false;

    {
        Plater::TakeSnapshot snapshot(plater, "Image to object");
        i2o_add_objects(plater, dialog.objects(), dialog);
    }
    I2O_LOG("objects are on the plate");
    if (!dialog.warnings().empty())
        show_info(plater, i2o_messages(dialog.warnings()), _L("Image to object"));
    return true;
}

bool can_edit_image_object() { return i2o_selected_object() >= 0; }

bool edit_image_object()
{
    Plater *plater   = wxGetApp().plater();
    int     selected = i2o_selected_object();
    if (plater == nullptr || selected < 0)
        return false;
    Model                  &model  = plater->model();
    const ImageObjectRecipe recipe = model.objects[size_t(selected)]->image_object_recipe;
    ImageToObjectSettings   settings;
    if (!image_object_settings_from_json(recipe.settings, settings)) {
        show_error(plater, _L("The settings this object was made with could not be read."));
        return false;
    }

    // The selected object and the other objects made with it (the lid of a box): one per place
    // in the group. Copies of them keep what they are.
    // A copy of an object has the group of the original. Copies are told apart by their order:
    // the n-th base belongs to the n-th lid, which holds for a box copied as a whole. A copy of
    // the lid alone has no base then, and gets a new one.
    auto find_targets = [&model, &recipe, selected]() {
        std::map<int, size_t> targets;
        targets[recipe.index] = size_t(selected);
        size_t rank = 0;
        for (size_t i = 0; i < size_t(selected); ++i) {
            const ImageObjectRecipe &other = model.objects[i]->image_object_recipe;
            if (other.group == recipe.group && other.index == recipe.index)
                ++rank;
        }
        std::map<int, size_t> seen;
        for (size_t i = 0; i < model.objects.size(); ++i) {
            const ImageObjectRecipe &other = model.objects[i]->image_object_recipe;
            if (other.group != recipe.group || other.index == recipe.index)
                continue;
            if (seen[other.index]++ == rank)
                targets[other.index] = i;
        }
        return targets;
    };
    std::map<int, size_t> targets = find_targets();

    // Filaments as they are set now, they could be changed after the object was made
    if (const ConfigOptionInt *body = model.objects[targets.begin()->second]->config.get().option<ConfigOptionInt>("extruder"); body != nullptr)
        settings.body_extruder = std::max(0, body->value);
    for (const auto &[index, object_idx] : targets)
        for (const ModelVolume *volume : model.objects[object_idx]->volumes)
            for (int level = 0; level < IMAGE_OBJECT_MAX_COLORS; ++level)
                if (volume->name == GUI::format(_u8L("Colour %1%"), level + 1))
                    if (const ConfigOptionInt *extruder = volume->config.get().option<ConfigOptionInt>("extruder"); extruder != nullptr)
                        settings.level_extruders[size_t(level)] = std::max(0, extruder->value);

    TraceImage  image;
    std::string source_png = recipe.source_png;
    if (source_png.empty() || !decode_trace_image(source_png, image) || image.empty()) {
        image = TraceImage{};
        MessageDialog question(nullptr,
                               _L("The image of this object is not stored in the project.") + "\n" +
                                   _L("Choose the image file again to change the object?"),
                               _L("Edit image object"), wxYES_NO | wxICON_QUESTION);
        if (question.ShowModal() != wxID_YES)
            return false;
        wxString path = ImageTraceDialog::choose_image_file(nullptr);
        if (path.empty())
            return false;
        std::string name;
        if (!ImageTraceDialog::load_image_file(nullptr, path, image, source_png, name))
            return false;
    }

    ImageToObjectDialog dialog(nullptr, image, source_png, recipe.image_name, settings, true);
    if (dialog.ShowModal() != wxID_OK || dialog.objects().empty())
        return false;

    // the dialog is modal, but be careful: the objects must still be there
    if (i2o_selected_object() != selected || model.objects[size_t(selected)]->image_object_recipe.group != recipe.group)
        return false;
    targets = find_targets();

    Plater::TakeSnapshot      snapshot(plater, "Edit image object");
    // the n-th new object takes the place of the old object which was the n-th of its group;
    // an entry without instances is an object which is not there any more and is added beside
    std::vector<I2OPlacement> placements(size_t(std::max(0, targets.rbegin()->first)) + 1);
    std::vector<size_t>       remove;
    for (const auto &[index, object_idx] : targets) {
        const ModelObject *object = model.objects[object_idx];
        I2OPlacement       placement;
        for (const ModelInstance *instance : object->instances)
            placement.instances.push_back(instance->get_transformation());
        placement.config    = object->config.get();
        placement.printable = object->printable;
        if (index >= 0 && size_t(index) < placements.size())
            placements[size_t(index)] = std::move(placement);
        remove.push_back(object_idx);
    }
    // from the end, so the indices of the other ones stay valid
    std::sort(remove.begin(), remove.end(), std::greater<size_t>());
    for (size_t object_idx : remove)
        plater->remove(object_idx);
    i2o_add_objects(plater, dialog.objects(), dialog, placements);
    if (!dialog.warnings().empty())
        show_info(plater, i2o_messages(dialog.warnings()), _L("Image to object"));
    return true;
}

}} // namespace Slic3r::GUI
