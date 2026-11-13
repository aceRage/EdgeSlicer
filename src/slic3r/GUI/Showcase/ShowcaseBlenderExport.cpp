// Showcase realism phase 4 (tests/design_showcase_realism.md 5): the Blender package from the Showcase
// tab. "Blender..." next to the image export offers:
//   - Export for Blender...      the package as one zip (decision D-R15: a zip for Export);
//   - Open in Blender            the package as a folder in the temp folder, then Blender started with
//                                resources/blender/edgeslicer_launch.py, which loads the add-on for that
//                                session only (nothing is installed);
//   - Install Blender add-on...  only on click: Blender's own extension install of the add-on zip.
// The package itself is libslic3r's (Showcase/ShowcaseBlender.hpp, unit tested); here the plate's parts,
// toolpaths, filaments, HDRI and pictures are gathered for it.

#include "ShowcaseView.hpp"

#include "ShowcaseRenderer.hpp"
#include "ShowcaseTextures.hpp"
#include "ShowcaseWatermark.hpp"

#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Showcase/GlowFilament.hpp"
#include "libslic3r/Showcase/ShowcaseBlender.hpp"
#include "libslic3r/Showcase/ShowcaseHdr.hpp"
#include "libslic3r/Showcase/ShowcaseLens.hpp"
#include "libslic3r/Showcase/ShowcaseSurfaceColours.hpp"
#include "libslic3r/Showcase/ShowcaseTubes.hpp"
#include "libslic3r/Slicing.hpp"
#include "libslic3r/Utils.hpp"

#include "slic3r/Utils/BlenderLauncher.hpp"

#include "../3DScene.hpp"
#include "../GLCanvas3D.hpp"
#include "../GUI.hpp"
#include "../GUI_App.hpp"
#include "../I18N.hpp"
#include "../MsgDialog.hpp"
#include "../PartPlate.hpp"
#include "../Plater.hpp"

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/dialog.h>
#include <wx/filedlg.h>
#include <wx/image.h>
#include <wx/menu.h>
#include <wx/mstream.h>
#include <wx/radiobut.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/utils.h>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <set>

#ifndef Snapmaker_VERSION
#define Snapmaker_VERSION "unknown"
#endif

namespace Slic3r {
namespace GUI {

namespace {

std::vector<uint8_t> sbx_read_file(const std::string &path)
{
    boost::nowide::ifstream f(path, std::ios::binary);
    if (!f)
        return {};
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

// RGBA (top row first) -> PNG bytes.
std::vector<uint8_t> sbx_encode_png(const std::vector<uint8_t> &rgba, int w, int h)
{
    if (w <= 0 || h <= 0 || rgba.size() < size_t(w) * size_t(h) * 4)
        return {};
    wxImage img(w, h, false);
    img.InitAlpha();
    unsigned char *rgb = img.GetData(), *alpha = img.GetAlpha();
    for (size_t i = 0; i < size_t(w) * size_t(h); ++i) {
        rgb[i * 3 + 0] = rgba[i * 4 + 0];
        rgb[i * 3 + 1] = rgba[i * 4 + 1];
        rgb[i * 3 + 2] = rgba[i * 4 + 2];
        alpha[i]       = rgba[i * 4 + 3];
    }
    wxMemoryOutputStream out;
    if (!img.SaveFile(out, wxBITMAP_TYPE_PNG))
        return {};
    std::vector<uint8_t> bytes(out.GetSize());
    out.CopyTo(bytes.data(), bytes.size());
    return bytes;
}

Showcase::TubeRole sbx_role(ExtrusionRole r)
{
    switch (r) {
    case erPerimeter:
    case erExternalPerimeter:
    case erOverhangPerimeter:
    case erOverSupportPerimeter:
    case erGapFill: return Showcase::TubeRole::Wall;
    case erTopSolidInfill:
    case erBottomSurface:
    case erBottomSurfaceOverSupport:
    case erIroning:
    case erBridgeInfill: return Showcase::TubeRole::Skin;
    case erInternalInfill: return Showcase::TubeRole::SparseInfill;
    case erSolidInfill:
    case erInternalBridgeInfill: return Showcase::TubeRole::InternalSolid;
    case erSupportMaterial:
    case erSupportMaterialInterface:
    case erSupportTransition: return Showcase::TubeRole::Support;
    case erSkirt:
    case erBrim: return Showcase::TubeRole::Skirt;
    default: return Showcase::TubeRole::Other;
    }
}

// What the export dialog asks.
struct BlenderExportChoice
{
    bool toolpaths  = false;
    int  detail     = 0;       // 0 auto, else the tube's sides
    bool internals  = false;
    bool video      = true;
};

bool sbx_ask(wxWindow *parent, bool can_toolpaths, bool has_video, bool open_in_blender, BlenderExportChoice &choice)
{
    wxDialog dlg(parent, wxID_ANY, open_in_blender ? _L("Open in Blender") : _L("Export for Blender"), wxDefaultPosition, wxDefaultSize,
                 wxDEFAULT_DIALOG_STYLE);
    auto *col = new wxBoxSizer(wxVERTICAL);
    const int em = dlg.GetCharWidth();
    col->Add(new wxStaticText(&dlg, wxID_ANY,
                              _L("A package for Blender's Cycles renderer: the plate, its finishes, the camera, lights, environment and floor.")),
             0, wxALL, em);
    auto *model = new wxRadioButton(&dlg, wxID_ANY, _L("Model (the parts as meshes)"), wxDefaultPosition, wxDefaultSize, wxRB_GROUP);
    auto *paths = new wxRadioButton(&dlg, wxID_ANY, _L("Toolpaths (every extrusion as a bead; the plate must be sliced)"));
    model->SetValue(!choice.toolpaths || !can_toolpaths);
    paths->SetValue(choice.toolpaths && can_toolpaths);
    paths->Enable(can_toolpaths);
    col->Add(model, 0, wxLEFT | wxRIGHT | wxTOP, em);
    col->Add(paths, 0, wxLEFT | wxRIGHT | wxTOP, em);
    auto *row = new wxBoxSizer(wxHORIZONTAL);
    row->Add(new wxStaticText(&dlg, wxID_ANY, _L("Bead detail")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, em);
    wxArrayString details;
    details.Add(_L("Automatic"));
    details.Add(_L("High (8 sides)"));
    details.Add(_L("Medium (6 sides)"));
    details.Add(_L("Low (4 sides)"));
    auto *detail = new wxChoice(&dlg, wxID_ANY, wxDefaultPosition, wxDefaultSize, details);
    detail->SetSelection(0);
    row->Add(detail, 0);
    col->Add(row, 0, wxLEFT | wxRIGHT | wxTOP, em * 3);
    auto *internals = new wxCheckBox(&dlg, wxID_ANY, _L("Include the internal infill"));
    internals->SetToolTip(_L("Off: only what can be seen from outside is exported, which keeps the file small."));
    col->Add(internals, 0, wxLEFT | wxRIGHT | wxTOP, em * 3);
    auto *video = new wxCheckBox(&dlg, wxID_ANY, _L("Include the video's camera path"));
    video->SetValue(has_video && choice.video);
    video->Enable(has_video);
    col->Add(video, 0, wxALL, em);
    auto update = [&]() {
        detail->Enable(paths->GetValue());
        internals->Enable(paths->GetValue());
    };
    model->Bind(wxEVT_RADIOBUTTON, [&](wxCommandEvent &) { update(); });
    paths->Bind(wxEVT_RADIOBUTTON, [&](wxCommandEvent &) { update(); });
    update();
    col->Add(dlg.CreateStdDialogButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxALL, em);
    dlg.SetSizerAndFit(col);
    dlg.CentreOnParent();
    if (dlg.ShowModal() != wxID_OK)
        return false;
    choice.toolpaths = paths->GetValue();
    const int sides[] = { 0, 8, 6, 4 };
    choice.detail    = sides[std::clamp(detail->GetSelection(), 0, 3)];
    choice.internals = internals->GetValue();
    choice.video     = video->GetValue();
    return true;
}

// The Blender command and its version; an error message when there is none or it is too old.
bool sbx_find_blender(std::vector<std::string> &command, int &major, int &minor, wxString &error)
{
    command = BlenderLauncher::find_blender(wxGetApp().app_config->get("blender_path"));
    if (command.empty()) {
        error = _L("Blender was not found. Install Blender 4.2 or newer, or set its path in Preferences.");
        return false;
    }
    major = minor = 0;
    if (!BlenderLauncher::blender_version(command, major, minor)) {
        error = _L("The Blender found does not say which version it is.") + "\n" + wxString::FromUTF8(command.front());
        return false;
    }
    if (!BlenderLauncher::showcase_version_supported(major, minor)) {
        error = wxString::Format(_L("Blender %d.%d was found; the Showcase add-on needs Blender 4.2 or newer."), major, minor);
        return false;
    }
    return true;
}

} // namespace

void ShowcaseView::on_blender_menu(wxWindow *anchor)
{
    enum { ID_EXPORT = 1, ID_OPEN, ID_INSTALL };
    wxMenu menu;
    menu.Append(ID_EXPORT, _L("Export for Blender..."));
    menu.Append(ID_OPEN, _L("Open in Blender"));
    menu.AppendSeparator();
    menu.Append(ID_INSTALL, _L("Install the Blender add-on..."));
    const int picked = anchor->GetPopupMenuSelectionFromUser(menu, wxPoint(0, anchor->GetSize().GetHeight()));
    if (picked == ID_EXPORT)
        on_export_blender(false);
    else if (picked == ID_OPEN)
        on_export_blender(true);
    else if (picked == ID_INSTALL)
        on_install_blender_addon();
}

bool ShowcaseView::gather_blender_package(Showcase::PackageInput &in, bool toolpaths, int detail, bool internals, bool video,
                                          std::string &error)
{
    const Showcase::Scene &s = scene();
    in.scene              = s;
    in.edgeslicer_version = Snapmaker_VERSION;
    in.width              = s.export_settings.width;
    in.height             = s.export_settings.height;
    in.frame_height_px    = s.view_frame_height > 0 ? double(s.view_frame_height) : double(m_renderer->live_frame().h);
    in.show_internals     = internals;

    // Filaments: colour, finish, glow.
    const std::vector<ShowcaseFilament> filaments = showcase_filaments();
    PresetBundle *bundle = wxGetApp().preset_bundle;
    std::vector<Glow::Slot> glow;
    float glow_intensity = 1.f;
    if (bundle != nullptr) {
        glow           = Glow::slots_from_bundle(*bundle);
        glow_intensity = Glow::intensity_from_config(bundle->project_config);
    }
    for (const ShowcaseFilament &f : filaments) {
        Showcase::PackageFilament pf;
        pf.slot        = f.slot;
        pf.color       = f.color;
        pf.preset_name = f.preset_name;
        pf.type        = f.type;
        pf.finish      = s.materials.finish_for(f.slot, f.preset_name, f.type);
        if (size_t(f.slot - 1) < glow.size() && glow[size_t(f.slot - 1)].on) {
            std::array<float, 4> rgba;
            if (Showcase::parse_hex_color(f.color, rgba)) {
                const MultiColour::RGB g = glow[size_t(f.slot - 1)].glow_srgb({ rgba[0], rgba[1], rgba[2] });
                pf.glow.on        = true;
                pf.glow.srgb      = { g[0], g[1], g[2] };
                pf.glow.intensity = glow_intensity;
            }
        }
        in.filaments.push_back(pf);
    }

    // The plate's objects as the view shows them (active, not modifiers, no prime tower).
    const Model &model = wxGetApp().model();
    BoundingBoxf3 box;
    DynamicPrintConfig full;
    bool have_full = false;
    if (bundle != nullptr) {
        try {
            full      = bundle->full_config();
            have_full = true;
        } catch (const std::exception &) {}
    }
    for (const GLVolume *v : m_canvas->get_volumes().volumes) {
        if (v == nullptr || !v->is_active || v->is_modifier || v->is_wipe_tower || v->object_idx() < 0 ||
            size_t(v->object_idx()) >= model.objects.size())
            continue;
        const ModelObject *mo = model.objects[size_t(v->object_idx())];
        if (v->volume_idx() < 0 || size_t(v->volume_idx()) >= mo->volumes.size())
            continue;
        const ModelVolume *mv = mo->volumes[size_t(v->volume_idx())];
        if (!mv->is_model_part())
            continue;
        const Transform3d world = v->world_matrix();
        box.merge(v->transformed_convex_hull_bounding_box());

        Showcase::PackageLayers layers;
        if (have_full) {
            try {
                const SlicingParameters sp = PrintObject::slicing_parameters(full, *mo, float(mo->max_z()), Vec3d::Ones());
                std::vector<coordf_t> profile;
                PrintObject::update_layer_height_profile(*mo, sp, profile);
                const Showcase::LayerTable table(generate_object_layers(sp, profile, false));
                if (!table.empty()) {
                    layers.valid   = true;
                    layers.uniform = table.uniform();
                    layers.first   = table.first_layer_height();
                    layers.height  = table.layer_height();
                    if (!table.uniform())
                        layers.tops = table.tops();
                }
            } catch (const std::exception &) {}
        }
        // A painted part splits by colour: each painted state is its own part of that filament.
        int base_extruder = mv->extruder_id();
        if (base_extruder <= 0)
            base_extruder = 1;
        // Edge: so does a modifier on a filament of its own, where it covers the part (as the view draws it).
        const Transform3d instance_matrix = v->instance_idx() >= 0 && size_t(v->instance_idx()) < mo->instances.size() ?
                                                mo->instances[size_t(v->instance_idx())]->get_matrix() : Transform3d::Identity();
        const std::vector<Showcase::ColourRegion> regions = Showcase::colour_regions_for(*mv, instance_matrix, filaments.size());
        std::vector<std::pair<int, indexed_triangle_set>> pieces;
        if (mv->mmu_segmentation_facets.empty() && regions.empty())
            pieces.emplace_back(base_extruder, mv->mesh().its);
        else {
            std::vector<indexed_triangle_set> per_state;
            if (regions.empty())
                mv->mmu_segmentation_facets.get_facets(*mv, per_state);
            else
                Showcase::surface_colour_facets(mv->mesh(), mv->mmu_segmentation_facets.empty() ? nullptr : &mv->mmu_segmentation_facets.get_data(),
                                                regions, Showcase::surface_colour_edge_limit(instance_matrix * mv->get_matrix()), per_state);
            for (size_t st = 0; st < per_state.size(); ++st)
                if (!per_state[st].indices.empty())
                    pieces.emplace_back(st == 0 ? base_extruder : int(st), std::move(per_state[st]));
        }
        for (auto &[extruder, its] : pieces) {
            Showcase::PackagePart part;
            part.name         = mo->name + (mo->volumes.size() > 1 ? " - " + mv->name : std::string()) +
                                (pieces.size() > 1 ? " F" + std::to_string(extruder) : std::string());
            part.object_idx   = v->object_idx();
            part.volume_idx   = v->volume_idx();
            part.instance_idx = v->instance_idx();
            part.filament     = extruder;
            part.layers       = layers;
            part.vertices.reserve(its.vertices.size() * 3);
            for (const stl_vertex &p : its.vertices) {
                const Vec3d w = world * p.cast<double>();
                part.vertices.insert(part.vertices.end(), { float(w.x()), float(w.y()), float(w.z()) });
            }
            // A mirrored instance turns its winding over.
            const bool flip = world.matrix().block<3, 3>(0, 0).determinant() < 0.;
            part.indices.reserve(its.indices.size() * 3);
            for (const stl_triangle_vertex_indices &f : its.indices) {
                if (flip)
                    part.indices.insert(part.indices.end(), { uint32_t(f[0]), uint32_t(f[2]), uint32_t(f[1]) });
                else
                    part.indices.insert(part.indices.end(), { uint32_t(f[0]), uint32_t(f[1]), uint32_t(f[2]) });
            }
            in.parts.push_back(std::move(part));
        }
    }
    if (box.defined) {
        in.model_min = { box.min.x(), box.min.y(), box.min.z() };
        in.model_max = { box.max.x(), box.max.y(), box.max.z() };
    }

    // Toolpaths: the slice's moves as tubes.
    if (toolpaths) {
        PartPlate *plate = wxGetApp().plater()->get_partplate_list().get_curr_plate();
        const GCodeProcessorResult *result = plate != nullptr ? plate->get_slice_result() : nullptr;
        if (result == nullptr || result->moves.empty()) {
            error = _u8L("The plate is not sliced: slice it first to export its toolpaths.");
            return false;
        }
        const Vec3d origin = plate->get_origin();
        std::vector<Showcase::TubeMove> moves;
        moves.reserve(result->moves.size());
        BoundingBoxf3 tbox;
        for (const GCodeProcessorResult::MoveVertex &m : result->moves) {
            Showcase::TubeMove t;
            t.x        = m.position.x() + float(origin.x());
            t.y        = m.position.y() + float(origin.y());
            t.z        = m.position.z() + float(origin.z());
            t.width    = m.width;
            t.height   = m.height;
            t.extruder = int(m.extruder_id);
            t.kind     = m.type == EMoveType::Extrude ? Showcase::TubeKind::Extrude :
                         m.type == EMoveType::Travel  ? Showcase::TubeKind::Travel : Showcase::TubeKind::Other;
            t.role     = sbx_role(m.extrusion_role);
            if (t.kind == Showcase::TubeKind::Extrude && t.role != Showcase::TubeRole::Other)
                tbox.merge(Vec3d(t.x, t.y, t.z));
            moves.push_back(t);
        }
        Showcase::TubeOptions opt;
        opt.sides     = detail;
        opt.internals = internals;
        opt.supports  = s.materials.toolpath_supports;
        opt.skirt     = s.materials.toolpath_skirt;
        in.tubes      = Showcase::build_tubes(moves, opt);
        in.toolpaths  = true;
        if (tbox.defined && !box.defined) {
            in.model_min = { tbox.min.x(), tbox.min.y(), 0. };
            in.model_max = { tbox.max.x(), tbox.max.y(), tbox.max.z() };
        }
        BOOST_LOG_TRIVIAL(info) << "Showcase: Blender toolpaths: " << in.tubes.runs << " runs, " << in.tubes.sides << " sides, "
                                << in.tubes.triangles << " triangles, " << in.tubes.culled << " culled, " << in.tubes.dropped
                                << " dropped by the budget";
    }

    // The environment: an HDRI (copied as it is; a procedural one written out), or a bundled
    // pre-filtered one unfolded into a panorama.
    const std::string env_id = s.lighting.environment;
    if (!env_id.empty()) {
        if (const Showcase::HdriInfo *hdri = showcase_hdris().find(env_id); hdri != nullptr) {
            Showcase::HdrImage image;
            std::string        err;
            bool               loaded = false;
            in.hdri.id               = hdri->id;
            in.hdri.name             = hdri->name;
            in.hdri.source           = hdri->source;
            in.hdri.licence          = hdri->licence;
            in.hdri.ground_height_mm = hdri->ground_height_mm;
            in.hdri.ground_radius_mm = hdri->ground_radius_mm;
            if (hdri->procedural) {
                image  = Showcase::make_procedural_hdr(hdri->kind, 2048);
                loaded = !image.empty();
                if (loaded) {
                    const std::string text = Showcase::encode_rgbe(image);
                    in.hdri.bytes.assign(text.begin(), text.end());
                    in.hdri.ext = "hdr";
                }
            } else {
                const std::string path = showcase_hdri_path(*hdri);
                in.hdri.bytes = sbx_read_file(path);
                in.hdri.ext   = boost::algorithm::iends_with(path, ".exr") ? "exr" : "hdr";
                loaded        = !in.hdri.bytes.empty() && Showcase::load_hdri_any(path, image, &err);
            }
            if (loaded) {
                const float peak       = Showcase::sh_peak_luminance(Showcase::project_hdr_sh(image));
                in.hdri.radiance_scale = peak > 0.f ? 1.f / peak : 1.f;
                in.hdri.sun_found      = Showcase::find_hdr_sun(image).found;
                in.has_hdri            = true;
            } else
                BOOST_LOG_TRIVIAL(warning) << "Showcase: the HDRI " << env_id << " could not be read for Blender (" << err << ")";
        } else if (const Showcase::EnvironmentInfo *env = showcase_environments().find(env_id); env != nullptr) {
            ShowcaseImage atlas;
            const Showcase::EnvironmentManifest &mf = showcase_environments();
            Showcase::HdrImage pano;
            if (showcase_load_rgba_file(showcase_environment_path(*env), atlas) &&
                Showcase::atlas_to_equirect(atlas.rgba, atlas.width, atlas.height, mf.face_size, mf.levels, float(mf.rgbm_range), 1024, pano)) {
                const std::string text = Showcase::encode_rgbe(pano);
                in.hdri.id       = env->id;
                in.hdri.name     = env->name;
                in.hdri.source   = env->source;
                in.hdri.licence  = env->licence;
                in.hdri.ext      = "hdr";
                in.hdri.bytes.assign(text.begin(), text.end());
                in.hdri.radiance_scale = 1.f;   // the bundled environments are scaled already
                in.has_hdri      = true;
            }
        }
    }

    // The backdrop picture or photo backplate, turned as Showcase shows it.
    const Showcase::Background &bg = s.background;
    if ((bg.type == Showcase::BackgroundType::Image || bg.type == Showcase::BackgroundType::Backplate) && !bg.image.empty()) {
        if (const Showcase::ImageBlob *blob = project().find_image(bg.image); blob != nullptr) {
            ShowcaseImage decoded;
            if (showcase_decode_image(blob->bytes, decoded)) {
                int w = decoded.width, h = decoded.height;
                if (bg.type == Showcase::BackgroundType::Backplate) {
                    if (bg.plate_flip)
                        Showcase::orient_rgba(decoded.rgba, w, h, 2);
                    Showcase::orient_rgba(decoded.rgba, w, h, Showcase::rotation_orientation(Showcase::floor_rotation_snap(double(bg.plate_rotation))));
                }
                in.background_image.bytes  = sbx_encode_png(decoded.rgba, w, h);
                in.background_image.ext    = "png";
                in.background_image.width  = w;
                in.background_image.height = h;
            }
        }
    }

    // The floor picture.
    const Showcase::Floor &fl = s.floor;
    if (fl.type == Showcase::FloorType::Texture) {
        if (const Showcase::FloorTextureInfo *ft = showcase_floors().find(fl.texture); ft != nullptr) {
            const std::string path = showcase_floor_path(*ft);
            in.floor_image.bytes   = sbx_read_file(path);
            const std::string ext  = boost::filesystem::path(path).extension().string();
            in.floor_image.ext     = ext.size() > 1 ? ext.substr(1) : std::string("jpg");
            in.floor_tile_mm       = ft->tile_mm;
            in.floor_roughness     = ft->roughness;
        }
    } else if (fl.type == Showcase::FloorType::Image && !fl.image.empty()) {
        if (const Showcase::ImageBlob *blob = project().find_image(fl.image); blob != nullptr) {
            in.floor_image.bytes = blob->bytes;
            in.floor_image.ext   = blob->ext;
        }
    }

    // The callouts and the watermark over a still (D-R18), as one transparent picture.
    {
        ShowcaseImage overlay;
        overlay.width  = in.width;
        overlay.height = in.height;
        overlay.rgba.assign(size_t(in.width) * size_t(in.height) * 4, 0);
        callouts_apply_to_export(overlay, true);
        showcase_apply_watermark(overlay, s.export_settings.watermark, project());
        bool any = false;
        for (size_t i = 3; i < overlay.rgba.size() && !any; i += 4)
            any = overlay.rgba[i] != 0;
        if (any)
            in.overlay_png = sbx_encode_png(overlay.rgba, overlay.width, overlay.height);
    }

    // The video's camera, one entry per frame.
    const Showcase::VideoSettings &v = s.video;
    if (video && (!Showcase::is_static_path(v.path) || !v.keyframes.empty() || v.build.enabled)) {
        const std::vector<Showcase::PathEvent> &events = video_events(v);
        const Showcase::CameraView base = s.camera.valid ? s.camera : live_camera_view();
        const int count = std::min(Showcase::total_frames(v), 7200);
        const std::vector<double> tops = m_renderer->model_layer_tops();
        in.fps = v.fps;
        for (int i = 0; i < count; ++i) {
            const double t = Showcase::frame_time(v, i);
            Showcase::PackageFrame fr;
            fr.pose = video_pose(v, base, t, events);
            const double f = Showcase::build_fraction(v, t);
            if (f >= 0. && f < 1. && !tops.empty()) {
                const size_t n = tops.size();
                const size_t k = size_t(std::clamp(std::ceil(f * double(n)), 0., double(n)));
                fr.build_height_mm = k == 0 ? 0. : tops[k - 1];
            }
            in.frames.push_back(fr);
        }
    }
    return true;
}

void ShowcaseView::on_export_blender(bool open_in_blender)
{
    if (m_renderer == nullptr || m_canvas == nullptr)
        return;
    std::vector<std::string> blender;
    int major = 0, minor = 0;
    if (open_in_blender) {
        wxString why;
        if (!sbx_find_blender(blender, major, minor, why)) {
            show_error(this, why);
            return;
        }
    }
    commit();
    store_scene_camera(true);
    const Showcase::Scene &s = scene();
    const bool can_toolpaths  = ShowcaseRenderer::toolpaths_available();
    const bool has_video      = !Showcase::is_static_path(s.video.path) || !s.video.keyframes.empty() || s.video.build.enabled;
    BlenderExportChoice choice;
    choice.toolpaths = can_toolpaths && s.source == "toolpaths";
    if (!sbx_ask(this, can_toolpaths, has_video, open_in_blender, choice))
        return;

    wxString zip_path;
    if (!open_in_blender) {
        wxString name = wxGetApp().plater()->get_project_name();
        if (name.empty())
            name = "showcase";
        name += wxString::Format("_plate%d_blender.zip", current_plate() + 1);
        wxFileDialog dlg(this, _L("Export for Blender"), wxString::FromUTF8(wxGetApp().app_config->get_last_dir()), name,
                         _L("EdgeSlicer Blender package") + " (*.zip)|*.zip", wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
        if (dlg.ShowModal() != wxID_OK)
            return;
        zip_path = dlg.GetPath();
    }

    Showcase::PackageInput  in;
    Showcase::PackageResult result;
    std::string             error;
    {
        wxBusyCursor busy;
        m_status->SetLabel(_L("Writing the Blender package..."));
        m_status->Update();
        if (!gather_blender_package(in, choice.toolpaths, choice.detail, choice.internals, choice.video, error) ||
            !Showcase::build_blender_package(in, result, &error)) {
            m_status->SetLabel("");
            show_error(this, _L("The Blender package could not be made.") + "\n" + wxString::FromUTF8(error));
            return;
        }
    }
    set_as_dirty();
    BOOST_LOG_TRIVIAL(info) << "Showcase: Blender package: " << result.summary;

    if (!open_in_blender) {
        if (!Showcase::write_package_zip(result.files, into_u8(zip_path), &error)) {
            m_status->SetLabel("");
            show_error(this, _L("The Blender package could not be saved.") + "\n" + wxString::FromUTF8(error));
            return;
        }
        m_status->SetLabel(_L("Blender package saved:") + " " + wxString::FromUTF8(result.summary));
        return;
    }

    // Open in Blender: a folder (D-R15) under the temp folder, replaced each time.
    namespace fs = boost::filesystem;
    const std::string stem = "plate" + std::to_string(current_plate() + 1);
    const fs::path dir = fs::temp_directory_path() / "edgeslicer_blender" / stem;
    boost::system::error_code ec;
    fs::remove_all(dir, ec);
    if (!Showcase::write_package_folder(result.files, dir.string(), &error)) {
        m_status->SetLabel("");
        show_error(this, _L("The Blender package could not be written.") + "\n" + wxString::FromUTF8(error));
        return;
    }
    const std::string script = resources_dir() + "/blender/edgeslicer_launch.py";
    if (!fs::exists(fs::path(script))) {
        show_error(this, _L("The Blender add-on is missing from this installation."));
        return;
    }
    if (!BlenderLauncher::launch(BlenderLauncher::showcase_session_args(blender, script, dir.string()))) {
        show_error(this, _L("Blender could not be started."));
        return;
    }
    m_status->SetLabel(wxString::Format(_L("Opening in Blender %d.%d:"), major, minor) + " " + wxString::FromUTF8(result.summary));
}

void ShowcaseView::on_install_blender_addon()
{
    std::vector<std::string> blender;
    int major = 0, minor = 0;
    wxString why;
    if (!sbx_find_blender(blender, major, minor, why)) {
        show_error(this, why);
        return;
    }
    // Never without asking: Blender's own install writes into the user's Blender settings.
    MessageDialog ask(this,
                      wxString::Format(_L("Install the EdgeSlicer Render add-on into Blender %d.%d?\n\n"
                                          "Blender installs it as an extension for your user, so it is there whenever you start "
                                          "Blender. Open in Blender works without it."),
                                       major, minor),
                      _L("Install the Blender add-on"), wxYES_NO | wxICON_QUESTION);
    if (ask.ShowModal() != wxID_YES)
        return;
    namespace fs = boost::filesystem;
    const fs::path zip = fs::temp_directory_path() / "edgeslicer_render.zip";
    std::string error;
    if (!Showcase::zip_folder(resources_dir() + "/blender/" + Showcase::BLENDER_ADDON_ID, Showcase::BLENDER_ADDON_ID, zip.string(), &error)) {
        show_error(this, _L("The add-on could not be packed.") + "\n" + wxString::FromUTF8(error));
        return;
    }
    std::string output;
    int         code = -1;
    {
        wxBusyCursor busy;
        if (!BlenderLauncher::run_and_wait(BlenderLauncher::install_addon_args(blender, zip.string()), output, code))
            code = -1;
    }
    BOOST_LOG_TRIVIAL(info) << "Showcase: Blender add-on install exited with " << code << ": " << output;
    if (code != 0) {
        show_error(this, _L("Blender could not install the add-on.") + "\n" + wxString::FromUTF8(output));
        return;
    }
    m_status->SetLabel(wxString::Format(_L("The EdgeSlicer add-on is installed in Blender %d.%d."), major, minor));
}

} // namespace GUI
} // namespace Slic3r
