// Showcase tab, phase S5: programmatic sequencing in the Showcase view (design tests/design_showcase_tab.md,
// section 4; docs/showcase_paths.md). The path format, its compiler, the timeline and the build-up
// schedule are libslic3r's (Showcase/ShowcasePath.hpp, unit tested). Here:
//   - importing a path file into the scene's video (its moves become editable keyframes, its events
//     the video's timeline) and exporting the video as one,
//   - what the moves need to know about the plate (PathContext),
//   - the --showcase-render job: the hidden instance renders a path (or the scene's own video, or a
//     still) through the same renderer, callouts and watermark as the tab's export.

#include "ShowcaseView.hpp"

#include "ShowcaseCallouts.hpp"
#include "ShowcaseRenderer.hpp"
#include "ShowcaseVideoSinks.hpp"
#include "ShowcaseWatermark.hpp"

#include "libslic3r/Model.hpp"
#include "libslic3r/Showcase/ShowcaseEncoder.hpp"
#include "libslic3r/Showcase/ShowcaseWebP.hpp"
#include "libslic3r/Utils.hpp"

#include "../GLCanvas3D.hpp"
#include "../GUI.hpp"
#include "../GUI_App.hpp"
#include "../I18N.hpp"
#include "../MsgDialog.hpp"
#include "../PartPlate.hpp"
#include "../Plater.hpp"

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/filedlg.h>
#include <wx/filename.h>
#include <wx/image.h>
#include <wx/stattext.h>

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/nowide/iostream.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <sstream>
#include <thread>

namespace Slic3r {
namespace GUI {

namespace {

bool showcase_read_text(const std::string &path, std::string &out)
{
    boost::nowide::ifstream f(path, std::ios::binary);
    if (!f)
        return false;
    std::stringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

Showcase::PathContext::Box showcase_box(const BoundingBoxf3 &b)
{
    Showcase::PathContext::Box out;
    if (!b.defined)
        return out;
    out.min = { b.min.x(), b.min.y(), b.min.z() };
    out.max = { b.max.x(), b.max.y(), b.max.z() };
    return out;
}

// The first lines of a list of problems, for a dialog.
wxString showcase_issue_lines(const Showcase::PathIssues &issues, size_t max_lines = 14)
{
    wxString out;
    size_t   n = 0;
    for (const Showcase::PathIssue &i : issues.items()) {
        if (n++ == max_lines) {
            out += wxString::Format(_L("... and %d more"), int(issues.items().size() - max_lines));
            break;
        }
        out += (i.error ? _L("Error") : _L("Warning")) + ": " + wxString::FromUTF8((i.where.empty() ? "" : i.where + ": ") + i.message) + "\n";
    }
    return out;
}

} // namespace

// ------------------------------------------------------------------------------------------------
// The context, the events, the frame height
// ------------------------------------------------------------------------------------------------

void ShowcaseView::note_frame_height(Showcase::Scene &s)
{
    if (m_renderer == nullptr || read_only())
        return;
    const Showcase::RectI f = m_renderer->live_frame();
    if (f.h > 0)
        s.view_frame_height = f.h;
}

const std::vector<Showcase::PathEvent> &ShowcaseView::video_events(const Showcase::VideoSettings &v)
{
    if (v.events != m_video.events_text) {
        m_video.events_text = v.events;
        m_video.events_cache.clear();
        if (!v.events.empty()) {
            Showcase::PathIssues issues;
            if (!Showcase::parse_path_events(v.events, m_video.events_cache, issues))
                BOOST_LOG_TRIVIAL(warning) << "Showcase: the video's timeline does not read and is ignored:\n" << issues.text();
        }
    }
    return m_video.events_cache;
}

Showcase::PathContext ShowcaseView::path_context(double frame_height_px)
{
    Showcase::PathContext ctx;
    const Showcase::Scene &s = scene();
    ctx.frame_height_px      = std::max(1., frame_height_px);
    ctx.start                = s.camera;
    ctx.projection           = s.camera.valid ? s.camera.projection :
                                   (wxGetApp().plater()->get_camera().get_type() == Camera::EType::Ortho ? Showcase::Projection::Ortho :
                                                                                                         Showcase::Projection::Perspective);
    ctx.views                 = s.views;
    ctx.has_watermark_content = s.export_settings.watermark.has_content();
    for (const Showcase::Callout &c : s.callouts.items)
        ctx.callouts.push_back({ c.key, c.filament, c.title });

    // The plate's objects, each with the box of its instances on the plate, and where each filament
    // is (the volumes using it, painted ones included).
    BoundingBoxf3                plate_box;
    std::map<int, BoundingBoxf3> filament_boxes;
    PartPlate                   *plate = wxGetApp().plater()->get_partplate_list().get_curr_plate();
    if (m_model != nullptr)
        for (size_t o = 0; o < m_model->objects.size(); ++o) {
            const ModelObject *mo = m_model->objects[o];
            BoundingBoxf3      obox;
            for (size_t i = 0; i < mo->instances.size(); ++i) {
                if (plate != nullptr && !plate->contain_instance(int(o), int(i)))
                    continue;
                const ModelInstance *inst = mo->instances[i];
                for (const ModelVolume *mv : mo->volumes) {
                    if (!mv->is_model_part())
                        continue;
                    const BoundingBoxf3 vb = mv->mesh().transformed_bounding_box(inst->get_matrix() * mv->get_matrix());
                    obox.merge(vb);
                    for (int f : mv->get_extruders())
                        if (f >= 1)
                            filament_boxes[f].merge(vb);
                }
            }
            if (!obox.defined)
                continue;
            Showcase::PathContext::Object ob;
            ob.index = int(o);
            ob.name  = mo->name;
            ob.box   = showcase_box(obox);
            ctx.objects.push_back(ob);
            plate_box.merge(obox);
        }
    for (const auto &[f, b] : filament_boxes)
        ctx.filaments.push_back({ f, showcase_box(b) });
    ctx.plate = showcase_box(plate_box);

    // The default view: front right and a little above, the plate's objects filling the frame.
    if (plate_box.defined) {
        const Vec3d  c   = plate_box.center();
        const double r   = std::max(1., 0.5 * plate_box.size().norm());
        const Vec3d  dir = Vec3d(0.55, -0.75, 0.45).normalized();
        const double d   = std::max(Camera::DefaultDistance, 4. * r);
        Showcase::CameraView v;
        v.valid      = true;
        v.projection = ctx.projection;
        v.target     = { c.x(), c.y(), c.z() };
        v.eye        = { c.x() + dir.x() * d, c.y() + dir.y() * d, c.z() + dir.z() * d };
        v.up         = { 0., 0., 1. };
        v.zoom       = Showcase::path_zoom(2. * r * 1.15, ctx.frame_height_px);
        ctx.default_view = v;
    } else
        ctx.default_view = s.camera.valid ? s.camera : live_camera_view();
    if (!ctx.start.valid)
        ctx.start = ctx.default_view;
    return ctx;
}

// ------------------------------------------------------------------------------------------------
// Import and export
// ------------------------------------------------------------------------------------------------

void ShowcaseView::refresh_path_info()
{
    if (m_vid_path_info == nullptr)
        return;
    const Showcase::VideoSettings &v      = scene().video;
    const size_t                   events = video_events(v).size();
    wxString info;
    if (!v.path_name.empty() || events > 0) {
        info = v.path_name.empty() ? _L("Imported path") : wxString::FromUTF8(v.path_name);
        info += ": " + wxString::Format(_L("%d keyframes, %d timed changes"), int(v.keyframes.size()), int(events));
    } else
        info = _L("None: the path set here. Import one to script the camera and timed changes.");
    if (m_vid_path_info->GetLabel() != info) {
        m_vid_path_info->SetLabel(info);
        m_vid_path_info->Wrap(22 * wxGetApp().em_unit());
    }
}

void ShowcaseView::on_import_path()
{
    if (read_only() || video_busy() || m_renderer == nullptr)
        return;
    video_restore_view();
    // The shipped examples are where a first look goes.
    wxString dir = wxString::FromUTF8(wxGetApp().app_config->get_last_dir());
    const boost::filesystem::path examples = boost::filesystem::path(resources_dir()) / "showcase" / "paths";
    if (wxGetApp().app_config->get("showcase_path_dir").empty() && boost::filesystem::is_directory(examples))
        dir = wxString::FromUTF8(examples.string());
    else if (!wxGetApp().app_config->get("showcase_path_dir").empty())
        dir = wxString::FromUTF8(wxGetApp().app_config->get("showcase_path_dir"));
    wxFileDialog dlg(this, _L("Import a Showcase path"), dir, "", _L("Showcase path") + " (*.json)|*.json", wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dlg.ShowModal() != wxID_OK)
        return;
    const std::string path = into_u8(dlg.GetPath());
    wxGetApp().app_config->set("showcase_path_dir", into_u8(wxFileName(dlg.GetPath()).GetPath()));

    std::string text;
    if (!showcase_read_text(path, text)) {
        show_error(this, _L("The path file could not be read."));
        return;
    }
    Showcase::PathDocument doc;
    Showcase::PathIssues   issues;
    Showcase::CompiledPath compiled;
    const Showcase::RectI  frame = m_renderer->live_frame();
    bool ok = Showcase::parse_path(text, doc, issues);
    if (ok)
        ok = Showcase::compile_path(doc, path_context(frame.h > 0 ? frame.h : 540), compiled, issues);
    if (!ok) {
        MessageDialog err(this, _L("The path was not imported.") + "\n\n" + showcase_issue_lines(issues), _L("Import a Showcase path"),
                          wxOK | wxICON_ERROR);
        err.ShowModal();
        return;
    }
    bool callouts_on = false;
    commit();
    {
        Showcase::Scene &s = scene();
        Showcase::apply_compiled_path(compiled, s.video);
        if (compiled.projection) {
            s.camera.projection = *compiled.projection;
            wxGetApp().plater()->get_camera().set_type(*compiled.projection == Showcase::Projection::Ortho ? Camera::EType::Ortho :
                                                                                                            Camera::EType::Perspective);
        }
        Showcase::sanitize(s.video);
        // A path that shows callouts needs them: switched on (its events say when they are seen).
        for (const Showcase::PathEvent &e : compiled.events)
            if (e.type == Showcase::PathEventType::Callouts && e.action != Showcase::CalloutAction::Hide && !s.callouts.enabled) {
                s.callouts.enabled = true;
                callouts_on        = true;
            }
    }
    m_video.selected_key = -1;
    after_change(true);
    if (callouts_on)
        callouts_sync(false);
    sync_video_controls();
    update_video_enabled();
    BOOST_LOG_TRIVIAL(info) << "Showcase: imported the path " << path << ": " << compiled.keys.size() << " keyframes, " << compiled.events.size()
                            << " events, " << compiled.duration << " s";
    if (issues.warnings() > 0 || callouts_on) {
        wxString msg = wxString::Format(_L("Imported \"%s\": %d keyframes, %d timed changes."),
                                        wxString::FromUTF8(compiled.name.empty() ? boost::filesystem::path(path).stem().string() : compiled.name),
                                        int(compiled.keys.size()), int(compiled.events.size()));
        if (callouts_on)
            msg += "\n" + _L("The path shows callouts, so they were switched on.");
        if (issues.warnings() > 0)
            msg += "\n\n" + showcase_issue_lines(issues);
        MessageDialog info(this, msg, _L("Import a Showcase path"), wxOK | wxICON_INFORMATION);
        info.ShowModal();
    }
}

void ShowcaseView::on_export_path()
{
    if (m_renderer == nullptr)
        return;
    video_restore_view();
    store_scene_camera(true);
    wxString name = wxGetApp().plater()->get_project_name();
    if (name.empty())
        name = "showcase";
    name += wxString::Format("_plate%d_path.json", current_plate() + 1);
    wxString dir = wxString::FromUTF8(wxGetApp().app_config->get("showcase_path_dir"));
    if (dir.empty())
        dir = wxString::FromUTF8(wxGetApp().app_config->get_last_dir());
    wxFileDialog dlg(this, _L("Export a Showcase path"), dir, name, _L("Showcase path") + " (*.json)|*.json", wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
    if (dlg.ShowModal() != wxID_OK)
        return;
    const std::string     path  = into_u8(dlg.GetPath());
    const Showcase::RectI frame = m_renderer->live_frame();
    const Showcase::PathDocument doc = Showcase::export_path(scene().video, path_context(frame.h > 0 ? frame.h : 540),
                                                             scene().video.path_name.empty() ? into_u8(wxFileName(dlg.GetPath()).GetName()) :
                                                                                               scene().video.path_name);
    boost::nowide::ofstream f(path, std::ios::binary | std::ios::trunc);
    const std::string       text = Showcase::path_to_json(doc) + "\n";
    if (f)
        f.write(text.data(), std::streamsize(text.size()));
    if (!f || !f.good()) {
        show_error(this, _L("The path file could not be written."));
        return;
    }
    wxGetApp().app_config->set("showcase_path_dir", into_u8(wxFileName(dlg.GetPath()).GetPath()));
    m_vid_info->SetLabel(wxString::Format(_L("Saved the path %s."), wxFileName(dlg.GetPath()).GetFullName()));
    m_vid_info->Wrap(30 * wxGetApp().em_unit());
    BOOST_LOG_TRIVIAL(info) << "Showcase: exported the path to " << path;
}

// ------------------------------------------------------------------------------------------------
// --showcase-render
// ------------------------------------------------------------------------------------------------

int ShowcaseView::run_render_job(const Showcase::RenderRequest &req, Showcase::RenderReport &rep)
{
    using namespace Showcase;
    const auto started = std::chrono::steady_clock::now();
    auto fail = [&](int code, const std::string &msg) {
        rep.errors.push_back(msg);
        rep.exit_code = code;
        rep.ok        = false;
        rep.seconds   = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        return code;
    };
    auto say = [](const std::string &line) {
        boost::nowide::cout << line << std::endl;
        BOOST_LOG_TRIVIAL(info) << "showcase-render: " << line;
    };

    if (m_canvas == nullptr || m_renderer == nullptr)
        return fail(RENDER_FAILED, "the Showcase view could not be created");
    if (!m_canvas->ensure_gl_ready())
        return fail(RENDER_FAILED, "OpenGL could not be set up for off-screen rendering");
    if (!ShowcaseRenderer::is_supported())
        return fail(RENDER_FAILED, "this OpenGL context cannot render off-screen (OpenGL 3.2 with framebuffer objects is needed)");
    m_canvas->reload_scene(true, true);
    if (!m_active)
        on_activate();
    commit();   // the plate's scene becomes the project's (the project is never saved)

    Scene         &s = scene();
    VideoSettings &v = s.video;
    // The output asked for over the scene's.
    if (req.size > 0)
        v.resolution = req.size;
    if (req.aspect_w > 0 && req.aspect_h > 0) {
        const int k = std::max(1, 1920 / std::max(req.aspect_w, req.aspect_h));
        s.export_settings.width  = clamp_export_side(req.aspect_w * k);
        s.export_settings.height = clamp_export_side(req.aspect_h * k);
    }
    if (req.quality)
        v.quality = *req.quality;
    if (req.bitrate >= 0)
        v.bitrate = req.bitrate;
    v.format = req.still ? VideoFormat::PNGSequence : req.format;

    // The frame is the output itself: the camera's zoom is pixels per mm of it.
    int w = 0, h = 0;
    video_size(w, h);
    if (req.still) {
        // A still is the export's own size at the requested shorter side.
        Showcase::video_size(v.resolution, s.export_settings.width, s.export_settings.height, w, h);
    }
    m_renderer->set_offscreen_view(w, h);
    // Zooms were set at the tab's frame height; ours is h.
    const double ratio = s.view_frame_height > 0 ? double(h) / double(s.view_frame_height) : 1.;
    if (s.view_frame_height <= 0 && s.camera.valid)
        rep.warnings.push_back("the scene's camera was saved before this version: its framing may differ from the Showcase tab "
                               "(open the project, look at the Showcase tab and save it once to fix that)");
    s.camera.zoom *= ratio;
    for (CameraView &cv : s.views)
        cv.zoom *= ratio;
    for (Keyframe &k : v.keyframes)
        k.zoom *= ratio;
    s.view_frame_height = h;
    apply_scene_camera();
    m_renderer->set_scene(&scene(), &project());
    m_renderer->prepare_live_camera();
    if (!s.camera.valid) {
        // place_default_camera() framed the plate: that is the scene's camera from now on.
        s.camera = live_camera_view();
    }

    // The path file over the scene's own video.
    if (!req.path.empty()) {
        std::string text;
        if (!showcase_read_text(req.path, text))
            return fail(RENDER_INVALID_PATH, "the path file could not be read: " + req.path);
        PathDocument doc;
        PathIssues   issues;
        CompiledPath compiled;
        bool         ok = parse_path(text, doc, issues);
        if (ok)
            ok = compile_path(doc, path_context(h), compiled, issues);
        for (const PathIssue &i : issues.items())
            (i.error ? rep.errors : rep.warnings).push_back((i.where.empty() ? "" : i.where + ": ") + i.message);
        if (!ok) {
            rep.exit_code = RENDER_INVALID_PATH;
            rep.seconds   = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            return RENDER_INVALID_PATH;
        }
        apply_compiled_path(compiled, v);
        if (compiled.projection) {
            s.camera.projection = *compiled.projection;
            wxGetApp().plater()->get_camera().set_type(*compiled.projection == Projection::Ortho ? Camera::EType::Ortho : Camera::EType::Perspective);
        }
        rep.path_name = compiled.name;
    }
    if (req.fps > 0)
        v.fps = req.fps;
    if (!req.still && req.format == VideoFormat::GIF)
        v.fps = std::min(v.fps, GIF_MAX_FPS);
    sanitize(v);
    if (v.path == VideoPath::Keyframes && v.keyframes.empty())
        return fail(RENDER_INVALID_PATH, "the scene's video is a keyframe path without keyframes: give --path");
    const std::vector<PathEvent> &events = video_events(v);
    rep.keyframes = int(v.keyframes.size());
    rep.events    = int(events.size());

    // Callouts the timeline shows must exist, anchored from the starting view.
    bool callout_events = false;
    for (const PathEvent &e : events)
        callout_events |= e.type == PathEventType::Callouts && e.action != CalloutAction::Hide;
    if ((s.callouts.enabled || callout_events) && v.callouts) {
        const bool was_on  = s.callouts.enabled;
        s.callouts.enabled = true;
        m_renderer->set_scene(&scene(), &project());
        m_renderer->prepare_live_camera();
        callouts_find_anchors(s, false);
        s.callouts.enabled = was_on || callout_events;
        if (s.callouts.items.empty() && callout_events)
            rep.warnings.push_back("the path shows callouts, but the plate gave none");
    }
    m_renderer->set_scene(&scene(), &project());

    rep.width    = w;
    rep.height   = h;
    rep.fps      = v.fps;
    rep.duration = total_seconds(v);
    rep.source   = ShowcaseRenderer::toolpaths_available() && s.source == "toolpaths" ? "toolpaths" : "model";
    if (s.source == "toolpaths" && !ShowcaseRenderer::toolpaths_available())
        rep.warnings.push_back("the scene shows toolpaths, but the plate is not sliced here: the model is shown");

    const bool transparent = s.background.type == BackgroundType::Transparent;
    if (req.dry_run) {
        rep.frames  = req.still ? 1 : total_frames(v);
        rep.format  = req.still ? Showcase::to_string(req.still_format) : Showcase::to_string(req.format);
        rep.encoder = "none (dry run)";
        rep.ok      = rep.errors.empty();
        rep.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        say("dry run: " + std::to_string(rep.frames) + " frames of " + std::to_string(w) + "x" + std::to_string(h) + ", " +
            std::to_string(rep.keyframes) + " keyframes, " + std::to_string(rep.events) + " events");
        return RENDER_OK;
    }

    // The frames' common state, as video_start_export() sets it.
    m_video.settings = v;
    m_video.events   = events;
    m_video.base     = s.camera;
    m_video.fade     = CalloutFade(std::max(2, v.fps / 3), std::max(2, v.fps / 4));
    m_video.sides.clear();
    m_video.width  = w;
    m_video.height = h;

    if (req.still) {
        const bool keep_alpha = transparent && req.still_format != ImageFormat::JPEG;
        m_video.alpha         = keep_alpha;
        const double t        = std::clamp(req.still_time, 0., total_seconds(v));
        if (req.still_time > total_seconds(v) + 1e-9)
            rep.warnings.push_back("--still is after the end of the video: its last moment is shown");
        VideoFrame frame;
        m_renderer->begin_batch();
        const bool ok = video_render_frame_at(t, 0, frame);
        m_renderer->end_batch();
        rep.build = m_renderer->build_used();
        if (!ok)
            return fail(RENDER_FAILED, "the frame could not be rendered (see the log)");
        rep.still      = true;
        rep.still_time = t;
        rep.format     = Showcase::to_string(req.still_format);
        rep.encoder    = req.still_format == ImageFormat::WebP ? "webp (lossless)" : req.still_format == ImageFormat::JPEG ? "jpeg" : "png";
        bool saved     = false;
        if (req.still_format == ImageFormat::WebP) {
            const std::vector<uint8_t> bytes = Showcase::encode_webp_lossless(frame.rgba, frame.width, frame.height);
            boost::nowide::ofstream    f(req.out, std::ios::binary | std::ios::trunc);
            if (f && !bytes.empty()) {
                f.write(reinterpret_cast<const char *>(bytes.data()), std::streamsize(bytes.size()));
                saved = f.good();
            }
        } else {
            wxImage img(frame.width, frame.height, false);
            if (keep_alpha)
                img.InitAlpha();
            unsigned char *rgb   = img.GetData();
            unsigned char *alpha = keep_alpha ? img.GetAlpha() : nullptr;
            for (size_t i = 0; i < size_t(frame.width) * size_t(frame.height); ++i) {
                rgb[i * 3 + 0] = frame.rgba[i * 4 + 0];
                rgb[i * 3 + 1] = frame.rgba[i * 4 + 1];
                rgb[i * 3 + 2] = frame.rgba[i * 4 + 2];
                if (alpha)
                    alpha[i] = frame.rgba[i * 4 + 3];
            }
            if (req.still_format == ImageFormat::JPEG)
                img.SetOption(wxIMAGE_OPTION_QUALITY, s.export_settings.jpeg_quality);
            saved = img.SaveFile(wxString::FromUTF8(req.out), req.still_format == ImageFormat::JPEG ? wxBITMAP_TYPE_JPEG : wxBITMAP_TYPE_PNG);
        }
        if (!saved)
            return fail(RENDER_ENCODER_ERROR, "the image could not be written to " + req.out);
        rep.frames  = 1;
        rep.ok      = true;
        rep.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        say("wrote " + req.out + " (" + std::to_string(w) + "x" + std::to_string(h) + ", t = " + std::to_string(t) + " s)");
        return RENDER_OK;
    }

    // A video: the encoder (D5), then every frame through the queue.
    const VideoFormat format = req.format;
    const bool        alpha  = transparent && (format == VideoFormat::WebMAlpha || format == VideoFormat::PNGSequence || format == VideoFormat::WebP);
    m_video.alpha            = alpha;
    m_video.format           = format;
    m_video.path             = req.out;
    const int bitrate        = video_bitrate_kbps(w, h, v.fps, v.bitrate);
    std::unique_ptr<FrameSink> sink;
    if (format == VideoFormat::PNGSequence)
        sink = std::make_unique<PngSequenceSink>(req.out, alpha);
    else if (format == VideoFormat::MP4 && showcase_has_native_mp4())
        sink = showcase_make_native_mp4_sink(req.out, w, h, v.fps, bitrate);
    else {
        const EncoderCaps caps = Showcase::probe_ffmpeg(Showcase::find_ffmpeg());
        const std::vector<VideoFormat> formats = Showcase::available_formats(caps, showcase_has_native_mp4());
        if (std::find(formats.begin(), formats.end(), format) == formats.end())
            return fail(RENDER_ENCODER_ERROR, std::string(Showcase::to_string(format)) + " cannot be written here: " +
                                                  (caps.ffmpeg.empty() ? "no ffmpeg was found" : "the ffmpeg found cannot write it") +
                                                  " (use --format png for a PNG sequence)");
        FfmpegRequest r;
        r.ffmpeg  = caps.ffmpeg;
        r.output  = req.out;
        r.width   = w;
        r.height  = h;
        r.fps     = v.fps;
        r.format  = format;
        r.quality = v.bitrate;
        r.alpha   = alpha;
        if (format == VideoFormat::MP4) {
            if (caps.h264.empty())
                return fail(RENDER_ENCODER_ERROR, "no working H.264 encoder");
            r.encoder = caps.h264.front();
        }
        sink = std::make_unique<FfmpegSink>(r);
    }
    if (!sink)
        return fail(RENDER_ENCODER_ERROR, "no encoder for this format");
    EncoderWorker worker(std::move(sink));
    std::string   error;
    if (!worker.start(error))
        return fail(RENDER_ENCODER_ERROR, "the encoder could not be started: " + error);
    rep.encoder = worker.description();
    rep.format  = Showcase::to_string(format);

    const int n = total_frames(v);
    say("rendering " + std::to_string(n) + " frames of " + std::to_string(w) + "x" + std::to_string(h) + " at " + std::to_string(v.fps) +
        " fps with " + rep.encoder);
    m_renderer->begin_batch();
    int  last_tenth = -1;
    bool frame_ok   = true;
    for (int i = 0; i < n && frame_ok; ++i) {
        VideoFrame frame;
        if (!video_render_frame(i, frame)) {
            frame_ok = false;
            break;
        }
        if (rep.build.empty())
            rep.build = m_renderer->build_used();
        while (!worker.try_submit(frame)) {
            if (worker.failed() || worker.done())
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (worker.failed())
            break;
        const int tenth = (i + 1) * 10 / n;
        if (tenth != last_tenth) {
            last_tenth = tenth;
            say("frame " + std::to_string(i + 1) + " of " + std::to_string(n));
        }
    }
    m_renderer->end_batch();
    if (!frame_ok) {
        worker.cancel();
        worker.wait(60000);
        return fail(RENDER_FAILED, "a frame could not be rendered (see the log)");
    }
    worker.finish();
    worker.wait(10 * 60 * 1000);
    if (worker.failed() || !worker.done()) {
        const std::string err = worker.error();
        if (!worker.done())
            worker.cancel();
        return fail(RENDER_ENCODER_ERROR, "the encoder failed: " + (err.empty() ? std::string("it did not finish") : err));
    }
    rep.frames  = worker.written();
    rep.ok      = rep.frames == n;
    rep.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    if (!rep.ok)
        return fail(RENDER_ENCODER_ERROR, "only " + std::to_string(rep.frames) + " of " + std::to_string(n) + " frames were written");
    say("wrote " + req.out + " (" + std::to_string(rep.frames) + " frames, " + rep.encoder + ")");
    return RENDER_OK;
}

} // namespace GUI
} // namespace Slic3r
