// Showcase tab, phase S4: the videographer half of ShowcaseView (design tests/design_showcase_tab.md,
// section 4, D5, D16), and the user's saved callout styles.
//
// The path maths, the timing, the encoders and the fade of the callouts are libslic3r's
// (Showcase/ShowcaseVideo.hpp, Showcase/ShowcaseEncoder.hpp, unit tested). Here: the Video panel,
// the preview in the view, and the export - one frame per timer tick on the GUI thread (the GL
// context is single-threaded), each through the still export's renderer (world-fixed light, SSAO
// scaled to the size, supersampling), the callouts drawn on it with the occlusion fade, the
// watermark over everything, then handed to the encoder thread through a queue of three frames.

#include "ShowcaseView.hpp"

#include "ShowcaseCallouts.hpp"
#include "ShowcaseRenderer.hpp"
#include "ShowcaseVideoSinks.hpp"
#include "ShowcaseWatermark.hpp"

#include "libslic3r/Showcase/ShowcaseEncoder.hpp"
#include "libslic3r/Utils.hpp"

#include "../GUI.hpp"
#include "../GUI_App.hpp"
#include "../I18N.hpp"
#include "../MsgDialog.hpp"
#include "../Plater.hpp"

#include <wx/bitmap.h>
#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/dialog.h>
#include <wx/dirdlg.h>
#include <wx/filedlg.h>
#include <wx/filename.h>
#include <wx/gauge.h>
#include <wx/image.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/slider.h>
#include <wx/spinctrl.h>
#include <wx/statbmp.h>
#include <wx/statline.h>
#include <wx/stattext.h>
#include <wx/textdlg.h>

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>

#include <algorithm>
#include <cmath>
#include <functional>
#include <initializer_list>

namespace Slic3r {
namespace GUI {

namespace {

// The shorter side of the video, as the Resolution choice lists it.
const int VIDEO_RESOLUTIONS[] = { 720, 1080, 1440, 2160 };
const int VIDEO_FPS[]         = { 24, 25, 30, 60 };

wxString video_format_name(Showcase::VideoFormat f)
{
    switch (f) {
    case Showcase::VideoFormat::MP4: return _L("MP4 (H.264)");
    case Showcase::VideoFormat::WebM: return _L("WebM (VP9)");
    case Showcase::VideoFormat::WebMAlpha: return _L("WebM (VP9) with transparency");
    case Showcase::VideoFormat::GIF: return _L("Animated GIF");
    case Showcase::VideoFormat::WebP: return _L("Animated WebP");
    default: return _L("PNG sequence (a folder of frames)");
    }
}

std::vector<Showcase::VideoFormat> all_video_formats()
{
    return { Showcase::VideoFormat::MP4, Showcase::VideoFormat::WebM, Showcase::VideoFormat::WebMAlpha,
             Showcase::VideoFormat::GIF, Showcase::VideoFormat::WebP, Showcase::VideoFormat::PNGSequence };
}

wxString seconds_text(double s) { return wxString::Format("%.1f s", s); }

// Turns the scene's lights and environment the other way round from a turntable's spin (they stay put
// relative to the model; the camera goes round instead, which looks the same).
void spin_lights(Showcase::Scene &scene, double spin_deg)
{
    if (spin_deg == 0.)
        return;
    scene.lighting.azimuth_deg      = float(std::fmod(double(scene.lighting.azimuth_deg) - spin_deg, 360.));
    scene.lighting.env_rotation_deg = float(std::fmod(double(scene.lighting.env_rotation_deg) - spin_deg, 360.));
}

} // namespace

// ------------------------------------------------------------------------------------------------
// The progress window
// ------------------------------------------------------------------------------------------------

// Not modal: frame n of N, the time left, the frame being written and Cancel (design 4.2).
class ShowcaseVideoProgressDialog : public wxDialog
{
public:
    ShowcaseVideoProgressDialog(wxWindow *parent, const wxString &what, std::function<void()> on_cancel)
        : wxDialog(parent, wxID_ANY, _L("Exporting video"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE)
        , m_on_cancel(std::move(on_cancel))
    {
        const int em  = wxGetApp().em_unit();
        auto     *col = new wxBoxSizer(wxVERTICAL);
        auto     *head = new wxStaticText(this, wxID_ANY, what);
        col->Add(head, 0, wxALL, em);
        m_thumb = new wxStaticBitmap(this, wxID_ANY, wxBitmap(24 * em, 24 * em * 9 / 16));
        col->Add(m_thumb, 0, wxLEFT | wxRIGHT | wxALIGN_CENTER_HORIZONTAL, em);
        m_gauge = new wxGauge(this, wxID_ANY, 1000, wxDefaultPosition, wxSize(24 * em, -1));
        col->Add(m_gauge, 0, wxEXPAND | wxALL, em);
        m_text = new wxStaticText(this, wxID_ANY, _L("Starting..."));
        col->Add(m_text, 0, wxLEFT | wxRIGHT, em);
        auto *cancel = new wxButton(this, wxID_CANCEL, _L("Cancel"));
        col->Add(cancel, 0, wxALIGN_RIGHT | wxALL, em);
        SetSizerAndFit(col);
        cancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { request_cancel(); });
        Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent &evt) {
            if (evt.CanVeto())
                evt.Veto();
            request_cancel();
        });
        wxGetApp().UpdateDlgDarkUI(this);
        CenterOnParent();
    }

    void update(int rendered, int written, int total, double progress, double eta)
    {
        m_gauge->SetValue(std::clamp(int(std::lround(progress * 1000.)), 0, 1000));
        wxString text = wxString::Format(_L("Frame %d of %d rendered, %d written"), rendered, total, written);
        if (eta >= 0.)
            text += "\n" + wxString::Format(_L("About %d s left"), int(std::ceil(eta)));
        if (text != m_text->GetLabel()) {
            m_text->SetLabel(text);
            Layout();
        }
    }

    void set_thumbnail(const Showcase::VideoFrame &frame)
    {
        if (frame.width <= 0 || frame.height <= 0 || frame.rgba.empty())
            return;
        wxImage img(frame.width, frame.height, false);
        unsigned char *rgb = img.GetData();
        for (size_t i = 0, n = size_t(frame.width) * size_t(frame.height); i < n; ++i) {
            rgb[i * 3 + 0] = frame.rgba[i * 4 + 0];
            rgb[i * 3 + 1] = frame.rgba[i * 4 + 1];
            rgb[i * 3 + 2] = frame.rgba[i * 4 + 2];
        }
        const wxSize box = m_thumb->GetSize();
        const double s   = std::min(double(box.x) / frame.width, double(box.y) / frame.height);
        img.Rescale(std::max(1, int(frame.width * s)), std::max(1, int(frame.height * s)), wxIMAGE_QUALITY_BILINEAR);
        m_thumb->SetBitmap(wxBitmap(img));
    }

private:
    void request_cancel()
    {
        if (m_cancelled)
            return;
        m_cancelled = true;
        m_text->SetLabel(_L("Cancelling..."));
        // Not from inside this window's own handler: the export closes it.
        if (m_on_cancel)
            CallAfter(m_on_cancel);
    }

    wxGauge              *m_gauge = nullptr;
    wxStaticText         *m_text  = nullptr;
    wxStaticBitmap       *m_thumb = nullptr;
    std::function<void()> m_on_cancel;
    bool                  m_cancelled = false;
};

#ifndef __APPLE__
bool showcase_has_native_mp4() { return false; }
std::unique_ptr<Showcase::FrameSink> showcase_make_native_mp4_sink(const std::string &, int, int, int, int) { return nullptr; }
#endif

// ------------------------------------------------------------------------------------------------
// The Video panel
// ------------------------------------------------------------------------------------------------

void ShowcaseView::build_video_section(wxBoxSizer *col)
{
    const int em = wxGetApp().em_unit();
    {
        auto *t = new wxStaticText(m_side, wxID_ANY, _L("Video"));
        t->SetFont(wxGetApp().bold_font());
        col->Add(t, 0, wxLEFT | wxRIGHT | wxTOP, em);
        col->Add(new wxStaticLine(m_side), 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, em / 2);
    }
    auto *grid = new wxFlexGridSizer(2, em / 2, em);
    grid->AddGrowableCol(1, 1);
    col->Add(grid, 0, wxEXPAND | wxALL, em);
    // A row whose label and control can be hidden together.
    auto row = [this, grid](const wxString &label, wxWindow *ctrl, std::vector<wxWindow *> *group = nullptr) {
        auto *l = new wxStaticText(m_side, wxID_ANY, label);
        grid->Add(l, 0, wxALIGN_CENTER_VERTICAL);
        grid->Add(ctrl, 1, wxEXPAND);
        if (group != nullptr) {
            group->push_back(l);
            group->push_back(ctrl);
        }
    };
    auto check_row = [this, grid](wxCheckBox *cb, std::vector<wxWindow *> *group = nullptr) {
        auto *spacer = new wxStaticText(m_side, wxID_ANY, "");
        grid->Add(spacer);
        grid->Add(cb);
        if (group != nullptr) {
            group->push_back(spacer);
            group->push_back(cb);
        }
    };
    auto spin_double = [this](double v, double lo, double hi, double step) {
        auto *c = new wxSpinCtrlDouble(m_side, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, lo, hi, v, step);
        c->SetDigits(1);
        return c;
    };

    wxArrayString paths;
    paths.Add(_L("Orbit"));
    paths.Add(_L("Turntable"));
    paths.Add(_L("Orbit with bob"));
    paths.Add(_L("Keyframes"));
    m_vid_path = new wxChoice(m_side, wxID_ANY, wxDefaultPosition, wxDefaultSize, paths);
    m_vid_path->SetToolTip(_L("Orbit: the camera circles the model, the light stays. Turntable: the model turns in front of the camera. "
                              "Orbit with bob: the camera also rises and dips. Keyframes: a path through views you add."));
    row(_L("Camera path"), m_vid_path);
    m_vid_duration = spin_double(8., Showcase::MIN_VIDEO_SECONDS, Showcase::MAX_VIDEO_SECONDS, 0.5);
    row(_L("Length (s)"), m_vid_duration);
    wxArrayString fps;
    for (int f : VIDEO_FPS)
        fps.Add(wxString::Format("%d", f));
    m_vid_fps = new wxChoice(m_side, wxID_ANY, wxDefaultPosition, wxDefaultSize, fps);
    row(_L("Frames per second"), m_vid_fps);
    wxArrayString res;
    res.Add(_L("720 (HD)"));
    res.Add(_L("1080 (Full HD)"));
    res.Add(_L("1440 (QHD)"));
    res.Add(_L("2160 (4K)"));
    m_vid_resolution = new wxChoice(m_side, wxID_ANY, wxDefaultPosition, wxDefaultSize, res);
    m_vid_resolution->SetToolTip(_L("The shorter side of the video in pixels. The shape is the export's (the frame on the view)."));
    row(_L("Resolution"), m_vid_resolution);
    wxArrayString qualities;
    qualities.Add(_L("Draft"));
    qualities.Add(_L("High"));
    qualities.Add(_L("Best"));
    m_vid_quality = new wxChoice(m_side, wxID_ANY, wxDefaultPosition, wxDefaultSize, qualities);
    m_vid_quality->SetToolTip(_L("How each frame is rendered, as for images. High and Best take longer per frame."));
    row(_L("Render quality"), m_vid_quality);
    m_vid_bitrate = new wxSlider(m_side, wxID_ANY, 60, 0, 100);
    m_vid_bitrate->SetToolTip(_L("Smaller files to the left, better pictures to the right."));
    row(_L("Encoder quality"), m_vid_bitrate);
    wxArrayString formats;
    for (Showcase::VideoFormat f : all_video_formats())
        formats.Add(video_format_name(f));
    m_vid_format = new wxChoice(m_side, wxID_ANY, wxDefaultPosition, wxDefaultSize, formats);
    m_vid_format->SetToolTip(_L("MP4 plays everywhere. WebM with transparency keeps a transparent background. GIF is at most 800 pixels "
                                "wide and 20 frames a second. The PNG sequence always works."));
    row(_L("Format"), m_vid_format);

    wxArrayString easings;
    easings.Add(_L("Even speed"));
    easings.Add(_L("Ease in and out"));
    easings.Add(_L("Ease in"));
    easings.Add(_L("Ease out"));
    m_vid_easing = new wxChoice(m_side, wxID_ANY, wxDefaultPosition, wxDefaultSize, easings);
    row(_L("Speed"), m_vid_easing, &m_vid_orbit_rows);
    m_vid_sweep = new wxSpinCtrl(m_side, wxID_ANY, "360", wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 10, 1080, 360);
    m_vid_sweep->SetToolTip(_L("How far round, in degrees. A whole number of turns loops seamlessly."));
    row(_L("Turn (degrees)"), m_vid_sweep, &m_vid_orbit_rows);
    m_vid_clockwise = new wxCheckBox(m_side, wxID_ANY, _L("Clockwise"));
    check_row(m_vid_clockwise, &m_vid_orbit_rows);
    m_vid_bob = new wxSpinCtrl(m_side, wxID_ANY, "10", wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 0, 60, 10);
    row(_L("Bob (degrees)"), m_vid_bob, &m_vid_bob_rows);
    m_vid_bob_cycles = new wxSpinCtrl(m_side, wxID_ANY, "1", wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 1, 8, 1);
    row(_L("Bobs per turn"), m_vid_bob_cycles, &m_vid_bob_rows);
    m_vid_hold_start = spin_double(0., 0., Showcase::MAX_HOLD_SECONDS, 0.5);
    m_vid_hold_start->SetToolTip(_L("Seconds of standing still before the camera moves."));
    row(_L("Hold at start (s)"), m_vid_hold_start);
    m_vid_hold_end = spin_double(0., 0., Showcase::MAX_HOLD_SECONDS, 0.5);
    row(_L("Hold at end (s)"), m_vid_hold_end);
    m_vid_loop = new wxCheckBox(m_side, wxID_ANY, _L("Seamless loop"));
    m_vid_loop->SetToolTip(_L("For whole turns without holds: the last frame leads straight into the first, so the video can repeat."));
    check_row(m_vid_loop, &m_vid_orbit_rows);
    m_vid_callouts = new wxCheckBox(m_side, wxID_ANY, _L("Show the callouts"));
    m_vid_callouts->SetToolTip(_L("The filament callouts (when they are on) in every frame. A label whose spot goes behind the model fades out."));
    check_row(m_vid_callouts);

    // Keyframes.
    m_vid_keys = new wxChoice(m_side, wxID_ANY, wxDefaultPosition, wxSize(12 * em, -1));
    row(_L("Keyframe"), m_vid_keys, &m_vid_key_rows);
    m_vid_key_time = spin_double(0., 0., Showcase::MAX_VIDEO_SECONDS, 0.5);
    m_vid_key_time->SetToolTip(_L("When the keyframe is reached, in seconds from the start of the motion. Also where a new keyframe goes."));
    row(_L("At (s)"), m_vid_key_time, &m_vid_key_rows);
    m_vid_key_ease = new wxChoice(m_side, wxID_ANY, wxDefaultPosition, wxDefaultSize, easings);
    m_vid_key_ease->SetToolTip(_L("How the camera speeds up and slows down on its way to this keyframe."));
    row(_L("Arrive"), m_vid_key_ease, &m_vid_key_rows);
    {
        auto *buttons = new wxBoxSizer(wxHORIZONTAL);
        m_vid_key_add    = new wxButton(m_side, wxID_ANY, _L("Add view"));
        m_vid_key_add->SetToolTip(_L("Adds the view as it is now as a keyframe at the time above."));
        m_vid_key_update = new wxButton(m_side, wxID_ANY, _L("Update"));
        m_vid_key_update->SetToolTip(_L("Sets the selected keyframe to the view as it is now."));
        m_vid_key_goto   = new wxButton(m_side, wxID_ANY, _L("Go to"));
        m_vid_key_delete = new wxButton(m_side, wxID_ANY, _L("Delete"));
        buttons->Add(m_vid_key_add, 0, wxRIGHT, em / 2);
        buttons->Add(m_vid_key_update, 0, wxRIGHT, em / 2);
        buttons->Add(m_vid_key_goto, 0, wxRIGHT, em / 2);
        buttons->Add(m_vid_key_delete, 0);
        auto *spacer = new wxStaticText(m_side, wxID_ANY, "");
        grid->Add(spacer);
        grid->Add(buttons);
        m_vid_key_rows.push_back(spacer);
        for (wxWindow *w : std::initializer_list<wxWindow *>{ m_vid_key_add, m_vid_key_update, m_vid_key_goto, m_vid_key_delete })
            m_vid_key_rows.push_back(w);
    }

    // The timeline: a scrubber that shows a moment of the path, the preview, the export.
    m_vid_time = new wxSlider(m_side, wxID_ANY, 0, 0, 1000);
    m_vid_time->SetToolTip(_L("Shows the camera at that moment. The view goes back when you click into it."));
    row(_L("Time"), m_vid_time);
    m_vid_time_label = new wxStaticText(m_side, wxID_ANY, "");
    grid->AddSpacer(0);
    grid->Add(m_vid_time_label);
    {
        auto *buttons = new wxBoxSizer(wxHORIZONTAL);
        m_vid_preview = new wxButton(m_side, wxID_ANY, _L("Preview"));
        m_vid_preview->SetToolTip(_L("Plays the path in the view, in real time and at draft quality."));
        m_vid_export  = new wxButton(m_side, wxID_ANY, _L("Export video..."));
        buttons->Add(m_vid_preview, 0, wxRIGHT, em / 2);
        buttons->Add(m_vid_export, 0);
        col->Add(buttons, 0, wxLEFT | wxRIGHT, em);
        m_vid_info = new wxStaticText(m_side, wxID_ANY, "");
        col->Add(m_vid_info, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, em);
    }

    // --- handlers ---------------------------------------------------------------------------------
    auto video_edit = [this](auto &&change, bool resync) {
        if (m_syncing || read_only() || video_busy())
            return;
        video_restore_view();
        commit();
        change(scene().video);
        Showcase::sanitize(scene().video);
        after_change(false);
        if (resync)
            sync_video_controls();
        update_video_enabled();
    };
    m_vid_path->Bind(wxEVT_CHOICE, [this, video_edit](wxCommandEvent &) {
        const int sel = m_vid_path->GetSelection();
        if (sel >= 0)
            video_edit([sel](Showcase::VideoSettings &v) { v.path = Showcase::VideoPath(sel); }, true);
    });
    m_vid_duration->Bind(wxEVT_SPINCTRLDOUBLE, [this, video_edit](wxSpinDoubleEvent &) {
        const double d = m_vid_duration->GetValue();
        video_edit([d](Showcase::VideoSettings &v) { v.duration = d; }, false);
    });
    m_vid_fps->Bind(wxEVT_CHOICE, [this, video_edit](wxCommandEvent &) {
        const int sel = m_vid_fps->GetSelection();
        if (sel >= 0 && size_t(sel) < sizeof(VIDEO_FPS) / sizeof(VIDEO_FPS[0]))
            video_edit([sel](Showcase::VideoSettings &v) { v.fps = VIDEO_FPS[sel]; }, false);
    });
    m_vid_resolution->Bind(wxEVT_CHOICE, [this, video_edit](wxCommandEvent &) {
        const int sel = m_vid_resolution->GetSelection();
        if (sel >= 0 && size_t(sel) < sizeof(VIDEO_RESOLUTIONS) / sizeof(VIDEO_RESOLUTIONS[0]))
            video_edit([sel](Showcase::VideoSettings &v) { v.resolution = VIDEO_RESOLUTIONS[sel]; }, false);
    });
    m_vid_quality->Bind(wxEVT_CHOICE, [this, video_edit](wxCommandEvent &) {
        const int sel = m_vid_quality->GetSelection();
        if (sel >= 0)
            video_edit([sel](Showcase::VideoSettings &v) { v.quality = Showcase::Quality(sel); }, false);
    });
    m_vid_bitrate->Bind(wxEVT_SLIDER, [this, video_edit](wxCommandEvent &) {
        const int q = m_vid_bitrate->GetValue();
        video_edit([q](Showcase::VideoSettings &v) { v.bitrate = q; }, false);
    });
    m_vid_format->Bind(wxEVT_CHOICE, [this, video_edit](wxCommandEvent &) {
        const int sel = m_vid_format->GetSelection();
        if (sel >= 0)
            video_edit([sel](Showcase::VideoSettings &v) { v.format = Showcase::VideoFormat(sel); }, false);
    });
    m_vid_easing->Bind(wxEVT_CHOICE, [this, video_edit](wxCommandEvent &) {
        const int sel = m_vid_easing->GetSelection();
        if (sel >= 0)
            video_edit([sel](Showcase::VideoSettings &v) { v.easing = Showcase::Easing(sel); }, false);
    });
    m_vid_sweep->Bind(wxEVT_SPINCTRL, [this, video_edit](wxSpinEvent &) {
        const int d = m_vid_sweep->GetValue();
        video_edit([d](Showcase::VideoSettings &v) { v.sweep_deg = d; }, false);
    });
    m_vid_clockwise->Bind(wxEVT_CHECKBOX, [this, video_edit](wxCommandEvent &) {
        const bool on = m_vid_clockwise->GetValue();
        video_edit([on](Showcase::VideoSettings &v) { v.clockwise = on; }, false);
    });
    m_vid_bob->Bind(wxEVT_SPINCTRL, [this, video_edit](wxSpinEvent &) {
        const int d = m_vid_bob->GetValue();
        video_edit([d](Showcase::VideoSettings &v) { v.bob_deg = d; }, false);
    });
    m_vid_bob_cycles->Bind(wxEVT_SPINCTRL, [this, video_edit](wxSpinEvent &) {
        const int n = m_vid_bob_cycles->GetValue();
        video_edit([n](Showcase::VideoSettings &v) { v.bob_cycles = n; }, false);
    });
    m_vid_hold_start->Bind(wxEVT_SPINCTRLDOUBLE, [this, video_edit](wxSpinDoubleEvent &) {
        const double d = m_vid_hold_start->GetValue();
        video_edit([d](Showcase::VideoSettings &v) { v.hold_start = d; }, false);
    });
    m_vid_hold_end->Bind(wxEVT_SPINCTRLDOUBLE, [this, video_edit](wxSpinDoubleEvent &) {
        const double d = m_vid_hold_end->GetValue();
        video_edit([d](Showcase::VideoSettings &v) { v.hold_end = d; }, false);
    });
    m_vid_loop->Bind(wxEVT_CHECKBOX, [this, video_edit](wxCommandEvent &) {
        const bool on = m_vid_loop->GetValue();
        video_edit([on](Showcase::VideoSettings &v) { v.loop = on; }, false);
    });
    m_vid_callouts->Bind(wxEVT_CHECKBOX, [this, video_edit](wxCommandEvent &) {
        const bool on = m_vid_callouts->GetValue();
        video_edit([on](Showcase::VideoSettings &v) { v.callouts = on; }, false);
    });

    m_vid_keys->Bind(wxEVT_CHOICE, [this](wxCommandEvent &) {
        if (m_syncing)
            return;
        m_video.selected_key = m_vid_keys->GetSelection();
        refresh_keyframes();
        update_video_enabled();
    });
    m_vid_key_time->Bind(wxEVT_SPINCTRLDOUBLE, [this, video_edit](wxSpinDoubleEvent &) {
        const int    sel = m_video.selected_key;
        const double t   = m_vid_key_time->GetValue();
        if (sel < 0 || size_t(sel) >= scene().video.keyframes.size())
            return;   // only the time the next key is added at
        video_edit([this, sel, t](Showcase::VideoSettings &v) {
            Showcase::Keyframe k = v.keyframes[size_t(sel)];
            v.keyframes.erase(v.keyframes.begin() + sel);
            k.time               = t;
            m_video.selected_key = Showcase::insert_keyframe(v.keyframes, k);
        }, false);
        refresh_keyframes();
    });
    m_vid_key_ease->Bind(wxEVT_CHOICE, [this, video_edit](wxCommandEvent &) {
        const int sel = m_video.selected_key, e = m_vid_key_ease->GetSelection();
        if (sel < 0 || size_t(sel) >= scene().video.keyframes.size() || e < 0)
            return;
        video_edit([sel, e](Showcase::VideoSettings &v) { v.keyframes[size_t(sel)].ease = Showcase::Easing(e); }, false);
    });
    m_vid_key_add->Bind(wxEVT_BUTTON, [this, video_edit](wxCommandEvent &) {
        const Showcase::CameraView view = live_camera_view();
        const double               t    = m_vid_key_time->GetValue();
        const int                  e    = std::max(0, m_vid_key_ease->GetSelection());
        if (int(scene().video.keyframes.size()) >= Showcase::MAX_KEYFRAMES) {
            show_info(this, wxString::Format(_L("A path has at most %d keyframes."), Showcase::MAX_KEYFRAMES), _L("Keyframes"));
            return;
        }
        video_edit([this, &view, t, e](Showcase::VideoSettings &v) {
            m_video.selected_key = Showcase::insert_keyframe(v.keyframes, Showcase::make_keyframe(view, t, Showcase::Easing(e)));
            if (v.path != Showcase::VideoPath::Keyframes)
                v.path = Showcase::VideoPath::Keyframes;
            // The next one goes a little later by default.
            if (t + 2. > v.duration && t + 2. <= Showcase::MAX_VIDEO_SECONDS && v.keyframes.size() >= 2)
                v.duration = std::max(v.duration, t);
        }, true);
        m_vid_key_time->SetValue(std::min(Showcase::MAX_VIDEO_SECONDS, t + 2.));
    });
    m_vid_key_update->Bind(wxEVT_BUTTON, [this, video_edit](wxCommandEvent &) {
        const int sel = m_video.selected_key;
        if (sel < 0 || size_t(sel) >= scene().video.keyframes.size())
            return;
        const Showcase::CameraView view = live_camera_view();
        video_edit([sel, &view](Showcase::VideoSettings &v) {
            const Showcase::Keyframe old = v.keyframes[size_t(sel)];
            v.keyframes[size_t(sel)]     = Showcase::make_keyframe(view, old.time, old.ease);
        }, true);
    });
    m_vid_key_delete->Bind(wxEVT_BUTTON, [this, video_edit](wxCommandEvent &) {
        const int sel = m_video.selected_key;
        if (sel < 0 || size_t(sel) >= scene().video.keyframes.size())
            return;
        video_edit([this, sel](Showcase::VideoSettings &v) {
            v.keyframes.erase(v.keyframes.begin() + sel);
            m_video.selected_key = std::min(sel, int(v.keyframes.size()) - 1);
        }, true);
    });
    m_vid_key_goto->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        const int sel = m_video.selected_key;
        const std::vector<Showcase::Keyframe> &keys = scene().video.keyframes;
        if (sel < 0 || size_t(sel) >= keys.size())
            return;
        // Like choosing a saved view: the camera moves there for good, to adjust and Update.
        video_restore_view();
        const Showcase::Keyframe &k = keys[size_t(sel)];
        Camera &camera = wxGetApp().plater()->get_camera();
        camera.look_at(Vec3d(k.eye[0], k.eye[1], k.eye[2]), Vec3d(k.target[0], k.target[1], k.target[2]), Vec3d(k.up[0], k.up[1], k.up[2]));
        camera.set_zoom(k.zoom);
        store_scene_camera(true);
        set_as_dirty();
    });

    m_vid_time->Bind(wxEVT_SLIDER, [this](wxCommandEvent &) {
        if (m_syncing || video_busy())
            return;
        const double t = double(m_vid_time->GetValue()) / 1000. * Showcase::total_seconds(scene().video);
        m_vid_time_label->SetLabel(seconds_text(t) + " / " + seconds_text(Showcase::total_seconds(scene().video)));
        if (!m_video.previewing) {
            video_show_pose(t);
            // A peek: the view goes back by itself a moment after the slider rests.
            m_video.peek_ticks = 90;
            if (!m_video_timer.IsRunning())
                m_video_timer.Start(16);
        } else {
            m_video.preview_offset = t;
            m_video.preview_start  = std::chrono::steady_clock::now();
        }
        // Where the next keyframe goes.
        if (scene().video.path == Showcase::VideoPath::Keyframes && m_video.selected_key < 0)
            m_vid_key_time->SetValue(std::max(0., t - scene().video.hold_start));
    });
    m_vid_preview->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { video_toggle_preview(); });
    m_vid_export->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { on_export_video(); });
}

void ShowcaseView::sync_video_controls()
{
    if (m_vid_path == nullptr)
        return;
    const bool syncing = m_syncing;
    m_syncing          = true;
    const Showcase::VideoSettings &v = scene().video;
    m_vid_path->SetSelection(int(v.path));
    m_vid_duration->SetValue(v.duration);
    {
        int sel = 2;
        for (size_t i = 0; i < sizeof(VIDEO_FPS) / sizeof(VIDEO_FPS[0]); ++i)
            if (VIDEO_FPS[i] == v.fps)
                sel = int(i);
        m_vid_fps->SetSelection(sel);
        sel = 1;
        for (size_t i = 0; i < sizeof(VIDEO_RESOLUTIONS) / sizeof(VIDEO_RESOLUTIONS[0]); ++i)
            if (VIDEO_RESOLUTIONS[i] == v.resolution)
                sel = int(i);
        m_vid_resolution->SetSelection(sel);
    }
    m_vid_quality->SetSelection(int(v.quality));
    m_vid_bitrate->SetValue(v.bitrate);
    m_vid_format->SetSelection(int(v.format));
    m_vid_easing->SetSelection(int(v.easing));
    m_vid_sweep->SetValue(int(std::lround(v.sweep_deg)));
    m_vid_clockwise->SetValue(v.clockwise);
    m_vid_bob->SetValue(int(std::lround(v.bob_deg)));
    m_vid_bob_cycles->SetValue(v.bob_cycles);
    m_vid_hold_start->SetValue(v.hold_start);
    m_vid_hold_end->SetValue(v.hold_end);
    m_vid_loop->SetValue(v.loop);
    m_vid_callouts->SetValue(v.callouts);
    if (m_video.selected_key >= int(v.keyframes.size()))
        m_video.selected_key = int(v.keyframes.size()) - 1;
    refresh_keyframes();

    // Only the rows the path uses.
    const bool orbit = v.path != Showcase::VideoPath::Keyframes;
    for (wxWindow *w : m_vid_orbit_rows)
        w->Show(orbit);
    for (wxWindow *w : m_vid_bob_rows)
        w->Show(v.path == Showcase::VideoPath::OrbitBob);
    for (wxWindow *w : m_vid_key_rows)
        w->Show(!orbit);
    m_vid_time_label->SetLabel(seconds_text(double(m_vid_time->GetValue()) / 1000. * Showcase::total_seconds(v)) + " / " +
                               seconds_text(Showcase::total_seconds(v)));
    m_syncing = syncing;
    // Mac lesson: the panel follows its rows.
    m_side->Layout();
    fit_side_width();
}

void ShowcaseView::refresh_keyframes()
{
    if (m_vid_keys == nullptr)
        return;
    const bool syncing = m_syncing;
    m_syncing          = true;
    const std::vector<Showcase::Keyframe> &keys = scene().video.keyframes;
    m_vid_keys->Clear();
    for (size_t i = 0; i < keys.size(); ++i)
        m_vid_keys->Append(wxString::Format("%d   %s", int(i) + 1, seconds_text(keys[i].time)));
    if (m_video.selected_key >= int(keys.size()))
        m_video.selected_key = int(keys.size()) - 1;
    m_vid_keys->SetSelection(m_video.selected_key);
    if (m_video.selected_key >= 0) {
        const Showcase::Keyframe &k = keys[size_t(m_video.selected_key)];
        m_vid_key_time->SetValue(k.time);
        m_vid_key_ease->SetSelection(int(k.ease));
    } else if (m_vid_key_ease->GetSelection() < 0)
        m_vid_key_ease->SetSelection(int(Showcase::Easing::EaseInOut));
    m_syncing = syncing;
}

void ShowcaseView::update_video_enabled()
{
    if (m_vid_path == nullptr)
        return;
    const bool editable = !read_only() && !video_busy();
    const Showcase::VideoSettings &v = scene().video;
    for (wxWindow *w : std::initializer_list<wxWindow *>{ m_vid_path, m_vid_duration, m_vid_fps, m_vid_resolution, m_vid_quality, m_vid_bitrate,
                                                          m_vid_format, m_vid_easing, m_vid_sweep, m_vid_clockwise, m_vid_bob, m_vid_bob_cycles,
                                                          m_vid_hold_start, m_vid_hold_end, m_vid_callouts, m_vid_key_add, m_vid_key_time })
        w->Enable(editable);
    const bool selected = m_video.selected_key >= 0 && size_t(m_video.selected_key) < v.keyframes.size();
    m_vid_keys->Enable(!v.keyframes.empty());
    m_vid_key_ease->Enable(editable);
    m_vid_key_update->Enable(editable && selected);
    m_vid_key_delete->Enable(editable && selected);
    m_vid_key_goto->Enable(!video_busy() && selected);
    m_vid_loop->Enable(editable && v.path != Showcase::VideoPath::Keyframes);
    m_vid_callouts->Enable(editable && scene().callouts.enabled);
    m_vid_time->Enable(!video_busy());
    m_vid_preview->Enable(!video_busy());
    m_vid_preview->SetLabel(m_video.previewing ? _L("Stop") : _L("Preview"));
    m_vid_export->Enable(!video_busy() && ShowcaseRenderer::is_supported());
    int w = 0, h = 0;
    video_size(w, h);
    const wxString info = wxString::Format(_L("%d x %d, %d frames"), w, h, Showcase::total_frames(v)) +
                          (Showcase::seamless_loop(v) ? ", " + _L("loops seamlessly") : wxString());
    if (!video_busy() && m_vid_info->GetLabel() != info) {
        m_vid_info->SetLabel(info);
        m_vid_info->Wrap(30 * wxGetApp().em_unit());
    }
}

Showcase::CameraView ShowcaseView::live_camera_view() const
{
    Camera              &camera = wxGetApp().plater()->get_camera();   // get_target() is not const
    Showcase::CameraView v;
    v.valid      = true;
    v.projection = camera.get_type() == Camera::EType::Ortho ? Showcase::Projection::Ortho : Showcase::Projection::Perspective;
    const Vec3d eye = camera.get_position(), target = camera.get_target(), up = camera.get_dir_up();
    v.eye    = { eye.x(), eye.y(), eye.z() };
    v.target = { target.x(), target.y(), target.z() };
    v.up     = { up.x(), up.y(), up.z() };
    v.zoom   = camera.get_zoom();
    return v;
}

void ShowcaseView::video_size(int &w, int &h)
{
    const Showcase::Scene &s = scene();
    Showcase::video_size(s.video.resolution, s.export_settings.width, s.export_settings.height, w, h);
    if (s.video.format == Showcase::VideoFormat::GIF && w > Showcase::GIF_MAX_WIDTH) {
        // A GIF is drawn no larger than it is written.
        const double k = double(Showcase::GIF_MAX_WIDTH) / double(w);
        w              = std::max(2, int(w * k) / 2 * 2);
        h              = std::max(2, int(h * k) / 2 * 2);
    }
}

// ------------------------------------------------------------------------------------------------
// Preview and peek
// ------------------------------------------------------------------------------------------------

void ShowcaseView::video_show_pose(double t)
{
    if (m_renderer == nullptr || !m_active)
        return;
    Camera &camera = wxGetApp().plater()->get_camera();
    if (!m_video.borrowed) {
        // The path starts from the view as it is now; it comes back when the preview ends.
        m_video.saved_camera = camera;
        m_video.base         = live_camera_view();
        m_video.borrowed     = true;
    }
    const Showcase::CameraPose pose = Showcase::evaluate_path(scene().video, m_video.base, t);
    camera.look_at(Vec3d(pose.eye[0], pose.eye[1], pose.eye[2]), Vec3d(pose.target[0], pose.target[1], pose.target[2]),
                   Vec3d(pose.up[0], pose.up[1], pose.up[2]));
    camera.set_zoom(pose.zoom);
    if (pose.spin_deg != 0.) {
        m_video.pose_scene = scene();
        spin_lights(m_video.pose_scene, pose.spin_deg);
        m_renderer->set_scene(&m_video.pose_scene, &project());
    } else
        m_renderer->set_scene(&scene(), &project());
    set_as_dirty();
}

void ShowcaseView::video_restore_view()
{
    m_video.peek_ticks = 0;
    if (m_video.previewing) {
        m_video.previewing = false;
        if (m_vid_preview != nullptr)
            m_vid_preview->SetLabel(_L("Preview"));
    }
    if (!video_busy() && m_video_timer.IsRunning())
        m_video_timer.Stop();
    if (!m_video.borrowed)
        return;
    wxGetApp().plater()->get_camera() = m_video.saved_camera;
    m_video.borrowed = false;
    if (m_renderer != nullptr)
        m_renderer->set_scene(&scene(), &project());
    set_as_dirty();
}

void ShowcaseView::video_toggle_preview()
{
    if (m_video.previewing) {
        video_stop_preview();
        return;
    }
    if (video_busy() || m_renderer == nullptr)
        return;
    const Showcase::VideoSettings &v = scene().video;
    if (v.path == Showcase::VideoPath::Keyframes && v.keyframes.size() < 2) {
        show_info(this, _L("Add at least two keyframes: set up a view, choose a time and click Add view."), _L("Keyframes"));
        return;
    }
    m_video.previewing     = true;
    m_video.peek_ticks     = 0;
    m_video.preview_start  = std::chrono::steady_clock::now();
    m_video.preview_offset = double(m_vid_time->GetValue()) / 1000. * Showcase::total_seconds(v);
    if (m_video.preview_offset >= Showcase::total_seconds(v) - 1e-3)
        m_video.preview_offset = 0.;
    m_vid_preview->SetLabel(_L("Stop"));
    video_show_pose(m_video.preview_offset);
    m_video_timer.Start(16);
}

void ShowcaseView::video_stop_preview()
{
    if (!m_video.previewing)
        return;
    video_restore_view();
}

void ShowcaseView::on_video_timer(wxTimerEvent &)
{
    if (video_busy()) {
        video_tick();
        return;
    }
    if (m_video.previewing) {
        const Showcase::VideoSettings &v     = scene().video;
        const double                   total = Showcase::total_seconds(v);
        double t = m_video.preview_offset + std::chrono::duration<double>(std::chrono::steady_clock::now() - m_video.preview_start).count();
        if (t >= total) {
            if (!v.loop || total <= 0.) {
                m_vid_time->SetValue(1000);
                video_stop_preview();
                return;
            }
            t = std::fmod(t, total);
        }
        video_show_pose(t);
        m_vid_time->SetValue(int(std::lround(t / std::max(1e-6, total) * 1000.)));
        m_vid_time_label->SetLabel(seconds_text(t) + " / " + seconds_text(total));
        return;
    }
    if (m_video.peek_ticks > 0 && --m_video.peek_ticks == 0) {
        video_restore_view();
        return;
    }
    if (m_video.peek_ticks <= 0)
        m_video_timer.Stop();
}

// ------------------------------------------------------------------------------------------------
// The export
// ------------------------------------------------------------------------------------------------

void ShowcaseView::on_export_video()
{
    if (video_busy() || m_renderer == nullptr || !ShowcaseRenderer::is_supported())
        return;
    video_restore_view();
    // Exporting keeps the scene, as for images.
    commit();
    store_scene_camera(true);
    Showcase::VideoSettings v = scene().video;
    Showcase::sanitize(v);
    if (v.path == Showcase::VideoPath::Keyframes && v.keyframes.size() < 2) {
        show_info(this, _L("Add at least two keyframes: set up a view, choose a time and click Add view."), _L("Keyframes"));
        return;
    }

    Showcase::VideoFormat format = v.format;
    const bool            native = format == Showcase::VideoFormat::MP4 && showcase_has_native_mp4();
    if (format != Showcase::VideoFormat::PNGSequence && !native) {
        Showcase::EncoderCaps caps;
        {
            wxBusyCursor busy;
            m_vid_info->SetLabel(_L("Checking the video encoders..."));
            m_vid_info->Update();
            caps = Showcase::probe_ffmpeg(Showcase::find_ffmpeg());
        }
        const std::vector<Showcase::VideoFormat> formats = Showcase::available_formats(caps, showcase_has_native_mp4());
        if (std::find(formats.begin(), formats.end(), format) == formats.end()) {
            const wxString why = caps.ffmpeg.empty() ? _L("No ffmpeg was found to write it with.")
                                                     : _L("The ffmpeg found here cannot write it.");
            MessageDialog dlg(this, wxString::Format(_L("%s is not available on this computer. %s\n\nSave a PNG sequence instead?"),
                                                     video_format_name(format), why),
                              _L("Export video"), wxYES_NO | wxICON_QUESTION);
            if (dlg.ShowModal() != wxID_YES) {
                update_video_enabled();
                return;
            }
            format = Showcase::VideoFormat::PNGSequence;
        }
    }

    wxString name = wxGetApp().plater()->get_project_name();
    if (name.empty())
        name = "showcase";
    name += wxString::Format("_plate%d", current_plate() + 1);
    std::string path;
    const wxString last_dir = wxString::FromUTF8(wxGetApp().app_config->get_last_dir());
    if (format == Showcase::VideoFormat::PNGSequence) {
        wxDirDialog dlg(this, _L("Choose a folder for the frames"), last_dir, wxDD_DEFAULT_STYLE);
        if (dlg.ShowModal() != wxID_OK)
            return;
        // A folder of its own inside the one chosen, never mixed with other files.
        namespace fs = boost::filesystem;
        const fs::path parent = fs::path(into_u8(dlg.GetPath()));
        fs::path       folder = parent / (into_u8(name) + "_frames");
        boost::system::error_code ec;
        for (int i = 2; fs::exists(folder, ec) && i < 1000; ++i)
            folder = parent / (into_u8(name) + "_frames_" + std::to_string(i));
        path = folder.string();
        wxGetApp().app_config->update_skein_dir(into_u8(dlg.GetPath()));
    } else {
        const wxString ext = wxString::FromUTF8(Showcase::video_extension(format));
        wxFileDialog dlg(this, _L("Export Showcase video"), last_dir, name + "." + ext,
                         video_format_name(format) + " (*." + ext + ")|*." + ext, wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
        if (dlg.ShowModal() != wxID_OK)
            return;
        path = into_u8(dlg.GetPath());
        wxGetApp().app_config->update_skein_dir(into_u8(wxFileName(dlg.GetPath()).GetPath()));
    }

    std::string error;
    if (!video_start_export(format, path, error)) {
        show_error(this, _L("The video could not be started.") + "\n" + wxString::FromUTF8(error));
        update_video_enabled();
    }
}

bool ShowcaseView::video_start_export(Showcase::VideoFormat format, const std::string &path, std::string &error)
{
    const Showcase::Scene  &s = scene();
    Showcase::VideoSettings v = s.video;
    Showcase::sanitize(v);
    v.format = format;
    if (format == Showcase::VideoFormat::GIF)
        v.fps = std::min(v.fps, Showcase::GIF_MAX_FPS);   // no frames rendered only to be dropped

    int w = 0, h = 0;
    Showcase::video_size(v.resolution, s.export_settings.width, s.export_settings.height, w, h);
    if (format == Showcase::VideoFormat::GIF && w > Showcase::GIF_MAX_WIDTH) {
        const double k = double(Showcase::GIF_MAX_WIDTH) / double(w);
        w              = std::max(2, int(w * k) / 2 * 2);
        h              = std::max(2, int(h * k) / 2 * 2);
    }
    const bool transparent = s.background.type == Showcase::BackgroundType::Transparent;
    const bool alpha = transparent && (format == Showcase::VideoFormat::WebMAlpha || format == Showcase::VideoFormat::PNGSequence ||
                                       format == Showcase::VideoFormat::WebP);
    const int  bitrate = Showcase::video_bitrate_kbps(w, h, v.fps, v.bitrate);

    std::unique_ptr<Showcase::FrameSink> sink;
    if (format == Showcase::VideoFormat::PNGSequence)
        sink = std::make_unique<Showcase::PngSequenceSink>(path, alpha);
    else if (format == Showcase::VideoFormat::MP4 && showcase_has_native_mp4())
        sink = showcase_make_native_mp4_sink(path, w, h, v.fps, bitrate);
    else {
        const Showcase::EncoderCaps caps = Showcase::probe_ffmpeg(Showcase::find_ffmpeg());
        Showcase::FfmpegRequest     r;
        r.ffmpeg  = caps.ffmpeg;
        r.output  = path;
        r.width   = w;
        r.height  = h;
        r.fps     = v.fps;
        r.format  = format;
        r.quality = v.bitrate;
        r.alpha   = alpha;
        if (format == Showcase::VideoFormat::MP4) {
            // D5: Media Foundation first, then OpenH264, then the GPUs' own (as probed to work).
            if (caps.h264.empty()) {
                error = "no working H.264 encoder";
                return false;
            }
            r.encoder = caps.h264.front();
        }
        sink = std::make_unique<Showcase::FfmpegSink>(r);
    }
    if (!sink) {
        error = "no encoder for this format";
        return false;
    }

    auto worker = std::make_unique<Showcase::EncoderWorker>(std::move(sink));
    if (!worker->start(error))
        return false;

    m_video.worker   = std::move(worker);
    m_video.job      = std::make_unique<Showcase::VideoJob>(Showcase::total_frames(v));
    m_video.settings = v;
    m_video.base     = s.camera.valid ? s.camera : live_camera_view();
    m_video.fade     = Showcase::CalloutFade(std::max(2, v.fps / 3), std::max(2, v.fps / 4));
    m_video.sides.clear();
    m_video.pending.reset();
    m_video.path    = path;
    m_video.format  = format;
    m_video.alpha   = alpha;
    m_video.width   = w;
    m_video.height  = h;
    m_video.started = std::chrono::steady_clock::now();
    m_renderer->begin_batch();
    // Nothing in the scene changes while its frames are written.
    m_side->Disable();
    BOOST_LOG_TRIVIAL(info) << "Showcase video: " << w << "x" << h << " at " << v.fps << " fps, " << m_video.job->total() << " frames, "
                            << Showcase::to_string(v.path) << ", " << m_video.worker->description() << " -> " << path;

    const wxString what = wxString::Format(_L("%s, %d x %d, %d frames"), video_format_name(format), w, h, m_video.job->total()) + "\n" +
                          wxString::FromUTF8(m_video.worker->description());
    m_video.progress = new ShowcaseVideoProgressDialog(wxGetTopLevelParent(this), what, [this]() { video_cancel(); });
    m_video.progress->Show();
    update_enabled();
    m_video_timer.Start(1);
    return true;
}

bool ShowcaseView::video_render_frame(int index, Showcase::VideoFrame &out)
{
    const Showcase::Scene     &s    = scene();
    const Showcase::CameraPose pose = Showcase::pose_at_frame(m_video.settings, m_video.base, index);
    Camera                    &camera = wxGetApp().plater()->get_camera();
    const Camera               saved  = camera;
    camera.look_at(Vec3d(pose.eye[0], pose.eye[1], pose.eye[2]), Vec3d(pose.target[0], pose.target[1], pose.target[2]),
                   Vec3d(pose.up[0], pose.up[1], pose.up[2]));
    camera.set_zoom(pose.zoom);

    // The scene of this frame: the video's size and quality, a turntable's lights turned.
    m_video.frame_scene                         = s;
    m_video.frame_scene.export_settings.width   = m_video.width;
    m_video.frame_scene.export_settings.height  = m_video.height;
    m_video.frame_scene.export_settings.quality = m_video.settings.quality;
    spin_lights(m_video.frame_scene, pose.spin_deg);
    m_renderer->set_scene(&m_video.frame_scene, &project());
    m_renderer->prepare_live_camera();

    ShowcaseImage image;
    std::string   report;
    bool          ok = m_renderer->render_export(image, report) && !image.empty();
    if (ok && m_video.settings.callouts && s.callouts.enabled && !s.callouts.items.empty()) {
        // The callouts of this pose: which anchors are hidden now, their fade, the layout with the
        // sides of the last frame so labels do not jump (design 3.3 item 6, R13).
        const std::map<std::string, std::vector<bool>> occluded = callouts_compute_occlusion(s);
        for (const Showcase::Callout &c : s.callouts.items) {
            auto it = occluded.find(c.key);
            for (size_t a = 0; a < c.anchors.size(); ++a)
                m_video.fade.update(c.key, int(a), it != occluded.end() && a < it->second.size() && it->second[a]);
        }
        const std::vector<ShowcaseCalloutDraw> draws = callouts_draw_list(callouts_labels(), &occluded, &m_video.fade);
        std::map<int, int>     sides;
        ShowcaseCalloutOptions opt;
        opt.reserved        = showcase_watermark_rect(s.export_settings.watermark, project(), image.width, image.height);
        opt.model_center_x  = callouts_model_center_x();
        opt.model_box       = callouts_model_box();
        opt.previous_sides  = index > 0 ? &m_video.sides : nullptr;
        opt.sides           = &sides;
        opt.side_hysteresis = 0.06;
        showcase_draw_callouts(image, s.background.type == Showcase::BackgroundType::Transparent, s, draws, opt);
        m_video.sides = std::move(sides);
    }
    if (ok)
        showcase_apply_watermark(image, s.export_settings.watermark, project());

    camera = saved;
    m_renderer->set_scene(&scene(), &project());
    if (!ok) {
        BOOST_LOG_TRIVIAL(error) << "Showcase video: frame " << index << " could not be rendered. " << report;
        return false;
    }
    if (!m_video.alpha)
        Showcase::flatten_rgba(image.rgba, 255, 255, 255);
    out.index  = index;
    out.width  = image.width;
    out.height = image.height;
    out.rgba   = std::move(image.rgba);
    return true;
}

void ShowcaseView::video_tick()
{
    Showcase::VideoJob      *job    = m_video.job.get();
    Showcase::EncoderWorker *worker = m_video.worker.get();
    if (job == nullptr || worker == nullptr)
        return;
    while (job->encoded() < worker->written())
        job->frame_encoded();
    if (worker->failed()) {
        video_finish(false);
        return;
    }
    // A frame that found the queue full goes first.
    if (m_video.pending) {
        if (!worker->try_submit(*m_video.pending)) {
            if (m_video.progress != nullptr)
                m_video.progress->update(job->rendered(), worker->written(), job->total(), job->progress(), job->eta_seconds());
            return;
        }
        m_video.pending.reset();
    }
    if (!job->rendering_done()) {
        const int  index = job->next_frame();
        const auto t0    = std::chrono::steady_clock::now();
        auto       frame = std::make_unique<Showcase::VideoFrame>();
        if (!video_render_frame(index, *frame)) {
            job->cancel();
            worker->cancel();
            video_finish(true);
            show_error(this, wxString::Format(_L("Frame %d of the video could not be rendered."), index + 1));
            return;
        }
        job->frame_rendered(std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
        if (m_video.progress != nullptr && (index % 8 == 0 || index + 1 == job->total()))
            m_video.progress->set_thumbnail(*frame);
        if (!worker->try_submit(*frame))
            m_video.pending = std::move(frame);
    }
    if (job->rendering_done() && !m_video.pending) {
        worker->finish();
        if (worker->wait(0)) {
            while (job->encoded() < worker->written())
                job->frame_encoded();
            video_finish(false);
            return;
        }
    }
    if (m_video.progress != nullptr)
        m_video.progress->update(job->rendered(), worker->written(), job->total(), job->progress(), job->eta_seconds());
}

void ShowcaseView::video_cancel()
{
    if (!video_busy())
        return;
    m_video.job->cancel();
    if (m_video.worker)
        m_video.worker->cancel();
    video_finish(true);
}

void ShowcaseView::video_finish(bool cancelled)
{
    m_video_timer.Stop();
    if (m_renderer != nullptr)
        m_renderer->end_batch();
    if (m_video.progress != nullptr) {
        m_video.progress->Destroy();
        m_video.progress = nullptr;
    }
    std::string error;
    int         written = 0;
    bool        ok      = false;
    if (m_video.worker) {
        if (!m_video.worker->done())
            m_video.worker->cancel();
        m_video.worker->wait(60000);
        ok      = !cancelled && !m_video.worker->failed() && m_video.job && m_video.job->rendered() == m_video.job->total();
        error   = m_video.worker->error();
        written = m_video.worker->written();
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - m_video.started).count();
    const std::string           path   = m_video.path;
    const Showcase::VideoFormat format = m_video.format;
    m_video.worker.reset();
    m_video.job.reset();
    m_video.pending.reset();
    m_video.fade.reset();
    m_video.sides.clear();
    m_side->Enable();
    update_enabled();
    set_as_dirty();

    if (cancelled) {
        m_vid_info->SetLabel(_L("Video export cancelled."));
        BOOST_LOG_TRIVIAL(info) << "Showcase video: cancelled after " << written << " frames";
        return;
    }
    if (ok) {
        m_vid_info->SetLabel(wxString::Format(_L("Saved %s (%d frames in %d s)."), wxString::FromUTF8(boost::filesystem::path(path).filename().string()),
                                              written, int(std::lround(seconds))));
        m_vid_info->Wrap(30 * wxGetApp().em_unit());
        fit_side_width();
        BOOST_LOG_TRIVIAL(info) << "Showcase video: wrote " << written << " frames to " << path << " in " << seconds << " s";
        return;
    }
    BOOST_LOG_TRIVIAL(error) << "Showcase video: failed after " << written << " frames: " << error;
    m_vid_info->SetLabel(_L("The video could not be written."));
    if (format == Showcase::VideoFormat::PNGSequence) {
        show_error(this, _L("The frames could not be written.") + "\n" + wxString::FromUTF8(error));
        return;
    }
    // Design 4.2: the encoder's own words, and the way that always works.
    MessageDialog dlg(this, _L("The video could not be written.") + "\n\n" + wxString::FromUTF8(error) + "\n\n" + _L("Save a PNG sequence instead?"),
                      _L("Export video"), wxYES_NO | wxICON_WARNING);
    if (dlg.ShowModal() != wxID_YES)
        return;
    namespace fs = boost::filesystem;
    const fs::path file(path);
    fs::path       folder = file.parent_path() / (file.stem().string() + "_frames");
    boost::system::error_code ec;
    for (int i = 2; fs::exists(folder, ec) && i < 1000; ++i)
        folder = file.parent_path() / (file.stem().string() + "_frames_" + std::to_string(i));
    std::string err;
    if (!video_start_export(Showcase::VideoFormat::PNGSequence, folder.string(), err))
        show_error(this, _L("The video could not be started.") + "\n" + wxString::FromUTF8(err));
}

// ------------------------------------------------------------------------------------------------
// S4: the user's saved callout styles
// ------------------------------------------------------------------------------------------------

std::string ShowcaseView::callout_styles_path() { return (boost::filesystem::path(data_dir()) / "showcase" / "callout_styles.json").string(); }

void ShowcaseView::load_callout_styles()
{
    namespace fs = boost::filesystem;
    boost::system::error_code ec;
    const std::string file = callout_styles_path();
    if (!fs::exists(file, ec))
        return;
    boost::nowide::ifstream in(file, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (!m_callout_styles.from_json(text))
        BOOST_LOG_TRIVIAL(warning) << "Showcase: " << file << " is not a callout style file; ignored";
}

bool ShowcaseView::save_callout_styles()
{
    namespace fs = boost::filesystem;
    boost::system::error_code ec;
    const fs::path file = callout_styles_path();
    fs::create_directories(file.parent_path(), ec);
    const fs::path tmp = file.string() + ".tmp";
    {
        boost::nowide::ofstream out(tmp.string(), std::ios::binary | std::ios::trunc);
        out << m_callout_styles.to_json();
        if (!out)
            return false;
    }
    fs::rename(tmp, file, ec);
    return !ec;
}

void ShowcaseView::refresh_callout_styles()
{
    if (m_co_style == nullptr)
        return;
    const bool syncing = m_syncing;
    m_syncing          = true;
    const std::vector<Showcase::CalloutStylePreset> presets = Showcase::callout_style_presets();
    m_co_style->Clear();
    for (Showcase::CalloutStylePreset preset : presets) {
        switch (preset) {
        case Showcase::CalloutStylePreset::CardLight: m_co_style->Append(_L("Card, light")); break;
        case Showcase::CalloutStylePreset::CardDark: m_co_style->Append(_L("Card, dark")); break;
        case Showcase::CalloutStylePreset::Minimal: m_co_style->Append(_L("Minimal")); break;
        case Showcase::CalloutStylePreset::Pill: m_co_style->Append(_L("Pill")); break;
        case Showcase::CalloutStylePreset::Technical: m_co_style->Append(_L("Technical")); break;
        }
    }
    for (const Showcase::UserCalloutStyle &u : m_callout_styles.styles())
        m_co_style->Append(wxString::FromUTF8(u.name));
    // The preset the style still matches (a built-in one keeps the user's font), else a saved one.
    const Showcase::CalloutStyle &style = scene().callouts.style;
    int sel = -1;
    if (const Showcase::UserCalloutStyle *u = m_callout_styles.match(style)) {
        const auto &list = m_callout_styles.styles();
        sel = int(presets.size()) + int(u - list.data());
    } else
        for (size_t i = 0; i < presets.size(); ++i) {
            Showcase::CalloutStyle probe = style;
            Showcase::apply_callout_style_preset(probe, presets[i]);
            if (probe == style)
                sel = int(i);
        }
    m_co_style->SetSelection(sel);
    m_syncing = syncing;
}

void ShowcaseView::on_save_callout_style()
{
    const Showcase::CalloutStyle   style = scene().callouts.style;
    const Showcase::UserCalloutStyle *now = m_callout_styles.match(style);
    wxTextEntryDialog dlg(this, _L("Name of the style:"), _L("Save callout style"), now != nullptr ? wxString::FromUTF8(now->name) : _L("My style"));
    if (dlg.ShowModal() != wxID_OK)
        return;
    const std::string name = into_u8(dlg.GetValue().Trim().Trim(false));
    if (name.empty())
        return;
    if (m_callout_styles.find(name) != nullptr) {
        MessageDialog ask(this, wxString::Format(_L("Replace the style \"%s\"?"), wxString::FromUTF8(name)), _L("Save callout style"),
                          wxYES_NO | wxICON_QUESTION);
        if (ask.ShowModal() != wxID_YES)
            return;
    }
    m_callout_styles.put(name, style);
    if (!save_callout_styles())
        show_error(this, _L("The callout styles could not be saved."));
    refresh_callout_styles();
    update_enabled();
}

void ShowcaseView::on_delete_callout_style()
{
    const Showcase::UserCalloutStyle *u = m_callout_styles.match(scene().callouts.style);
    if (u == nullptr)
        return;
    const std::string name = u->name;
    MessageDialog ask(this, wxString::Format(_L("Delete the style \"%s\"? Scenes that use it keep their look."), wxString::FromUTF8(name)),
                      _L("Delete callout style"), wxYES_NO | wxICON_QUESTION);
    if (ask.ShowModal() != wxID_YES)
        return;
    m_callout_styles.remove(name);
    if (!save_callout_styles())
        show_error(this, _L("The callout styles could not be saved."));
    refresh_callout_styles();
    update_enabled();
}

} // namespace GUI
} // namespace Slic3r
