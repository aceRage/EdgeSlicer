#include "SliceBakeDialog.hpp"

#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"

#include "libslic3r/AppConfig.hpp"

#include <wx/sizer.h>
#include <wx/valtext.h>

#include <algorithm>
#include <cmath>

namespace Slic3r { namespace GUI {

static const char *CFG_SECTION    = "slice_bake";
static const char *CFG_RESULT     = "result";
static const char *CFG_CLOSE_ON   = "close_gaps";
static const char *CFG_CLOSE_R    = "close_gaps_radius";
static const char *CFG_SOURCE     = "contour_source";
static const char *CFG_RESOLUTION = "resolution";
static const char *CFG_SMOOTH     = "smooth_steps";

static const double DEFAULT_CLOSE_RADIUS = 0.05; // mm, the value offered when the box is ticked

static wxString num_str(double v) { return wxString::Format("%.3g", v); }

SliceBakeSettings SliceBakeDialog::load_from_config(double default_resolution)
{
    SliceBakeSettings s;
    // The seed is the PRINT's own resolution, not a fixed number: baking at the tolerance the
    // G-code was simplified at is the setting that surprises nobody. A stored value overrides it,
    // because a user who typed one meant it.
    s.options.resolution = std::clamp(default_resolution, SLICE_BAKE_RESOLUTION_MIN, SLICE_BAKE_RESOLUTION_MAX);
    AppConfig *cfg = wxGetApp().app_config;
    if (cfg != nullptr) {
        if (cfg->has(CFG_SECTION, CFG_RESULT)) {
            const std::string r = cfg->get(CFG_SECTION, CFG_RESULT);
            if (r == "add")         s.result = SliceBakeResultMode::AddNew;
            else if (r == "export") s.result = SliceBakeResultMode::ExportSTL;
            else                    s.result = SliceBakeResultMode::Replace;
        }
        bool close_on = cfg->has(CFG_SECTION, CFG_CLOSE_ON) && cfg->get(CFG_SECTION, CFG_CLOSE_ON) == "true";
        double radius = DEFAULT_CLOSE_RADIUS;
        if (cfg->has(CFG_SECTION, CFG_CLOSE_R)) {
            try {
                radius = std::stod(cfg->get(CFG_SECTION, CFG_CLOSE_R));
            } catch (...) {
                // A hand-edited config must not stop the bake.
            }
        }
        s.options.close_gaps_radius = close_on ? std::clamp(radius, 0., SLICE_BAKE_CLOSE_GAPS_MAX) : 0.;

        if (cfg->has(CFG_SECTION, CFG_SOURCE))
            // Owner (2026-09-13): the slice-contour source is the original model minus fuzzy skin,
            // so it is of little use; the extrusion path is always used and the choice is hidden.
            (void) CFG_SOURCE;
            s.options.contour_source = SliceBakeContourSource::Extrusion;
        if (cfg->has(CFG_SECTION, CFG_RESOLUTION)) {
            try {
                s.options.resolution = std::clamp(std::stod(cfg->get(CFG_SECTION, CFG_RESOLUTION)),
                                                  SLICE_BAKE_RESOLUTION_MIN, SLICE_BAKE_RESOLUTION_MAX);
            } catch (...) {
                // A hand-edited config must not stop the bake.
            }
        }
        s.options.smooth_vertical_steps =
            cfg->has(CFG_SECTION, CFG_SMOOTH) && cfg->get(CFG_SECTION, CFG_SMOOTH) == "true";
    }
    return s;
}

SliceBakeDialog::SliceBakeDialog(wxWindow              *parent,
                                 const wxString        &object_name,
                                 size_t                 layers,
                                 double                 default_resolution,
                                 const TriangleCounter &counter)
    : DPIDialog(parent ? parent : static_cast<wxWindow *>(wxGetApp().mainframe),
                wxID_ANY,
                _L("Bake slice to mesh"),
                wxDefaultPosition,
                wxDefaultSize,
                wxCAPTION | wxCLOSE_BOX)
    , m_object_name(object_name)
    , m_layers(layers)
    , m_default_res(std::clamp(default_resolution, SLICE_BAKE_RESOLUTION_MIN, SLICE_BAKE_RESOLUTION_MAX))
    , m_counter(counter)
{
    SetBackgroundColour(*wxWHITE);
    SetFont(Label::Body_14);

    m_settings = load_from_config(m_default_res);

    auto v_sizer = new wxBoxSizer(wxVERTICAL);
    auto f_sizer = new wxFlexGridSizer(2, 2, FromDIP(4), FromDIP(20));

    const wxSize input_size(FromDIP(140), -1);

    // Every label is a ::Label rather than a bare wxStaticText, for the dark-mode reason spelled
    // out in FillBedDialog.cpp: a wxStaticText reports the system button face as its background
    // and UpdateDlgDarkUI() then paints a lighter grey band behind it.

    // ---- what to do with the result -----------------------------------------------------------
    auto result_label = new ::Label(this, Label::Body_14, _L("Result") + ":");
    m_result_choice = new ::ComboBox(this, wxID_ANY, wxEmptyString, wxDefaultPosition, input_size, 0, nullptr, wxCB_READONLY);
    m_result_choice->Append(_L("Replace object"));
    m_result_choice->Append(_L("Add as new object"));
    m_result_choice->Append(_L("Export STL..."));
    m_result_choice->SetSelection(int(m_settings.result));
    m_result_choice->SetToolTip(_L("Replace object: the part becomes its own baked surface, keeping its position "
                                   "and its print settings.\n\n"
                                   "Add as new object: the bake arrives beside the original, which is left alone.\n\n"
                                   "Export STL: nothing in the scene changes; the mesh is written to a file."));
    f_sizer->Add(result_label, 0, wxEXPAND | wxALIGN_CENTER_VERTICAL);
    f_sizer->Add(m_result_choice, 0, wxALIGN_CENTER_VERTICAL);

    // ---- where the boundary comes from ---------------------------------------------------------
    // This is the control the owner's "the bake looks more angular than the preview" report is
    // about. By the time an outer wall reaches LayerRegion::perimeters it has been simplified at
    // the print's `resolution`; the slice contour it was derived from has not, and the printed
    // boundary is recoverable from it exactly. So the slice contour is the default, and the
    // extrusion is there for the case it cannot reproduce - fuzzy skin, which is displacement
    // applied to the path after the slice.
    const wxString source_tip = _L("Slice contours: the boundary is taken from the shape the slicer cut from the "
                                   "model, which is smooth and exact.\n\n"
                                   "Extrusion paths: the boundary is taken from the wall the nozzle will actually "
                                   "follow. This is the only source that carries fuzzy skin, but it has been "
                                   "simplified at the print's resolution, so it is more angular.");
    auto source_label = new ::Label(this, Label::Body_14, _L("Boundary from") + ":");
    source_label->SetToolTip(source_tip);
    m_source_choice = new ::ComboBox(this, wxID_ANY, wxEmptyString, wxDefaultPosition, input_size, 0, nullptr, wxCB_READONLY);
    m_source_choice->Append(_L("Slice contours (smooth)"));
    m_source_choice->Append(_L("Extrusion paths (fuzzy skin)"));
    m_source_choice->SetSelection(1); // extrusion paths, always (see the settings loader)
    m_source_choice->SetToolTip(source_tip);
    f_sizer->Add(source_label, 0, wxEXPAND | wxALIGN_CENTER_VERTICAL);
    f_sizer->Add(m_source_choice, 0, wxALIGN_CENTER_VERTICAL);
    // Hidden rather than removed: the slice-contour source stays available to tests and can be
    // re-offered later; the row takes no space while hidden.
    source_label->Hide();
    m_source_choice->Hide();
    f_sizer->Hide(source_label);
    f_sizer->Hide(m_source_choice);

    // ---- resolution ------------------------------------------------------------------------------
    const wxString res_tip = _L("How far a straight edge of the baked mesh may depart from the curve it stands in "
                                "for. A smaller value adds points where the shape curves - and only there, so a "
                                "flat wall costs nothing however fine this is set.\n\n"
                                "The default is the print's own Resolution setting.");
    auto res_label = new ::Label(this, Label::Body_14, _L("Resolution") + ":");
    res_label->SetToolTip(res_tip);
    m_res_input = new ::TextInput(this, num_str(m_settings.options.resolution), _L("mm"), wxEmptyString,
                                  wxDefaultPosition, input_size, wxTE_PROCESS_ENTER);
    m_res_input->GetTextCtrl()->SetFont(Label::Body_14);
    m_res_input->GetTextCtrl()->SetValidator(wxTextValidator(wxFILTER_NUMERIC));
    m_res_input->SetToolTip(res_tip);
    f_sizer->Add(res_label, 0, wxEXPAND | wxALIGN_CENTER_VERTICAL);
    f_sizer->Add(m_res_input, 0, wxALIGN_CENTER_VERTICAL);

    // ---- smooth the vertical steps --------------------------------------------------------------
    const wxString smooth_tip = _L("Off, every layer is a band with vertical walls - a staircase, which is what the "
                                   "printer actually makes.\n\n"
                                   "On, consecutive layers are joined by a sloped surface instead, so a curved part "
                                   "bakes to a curve. Layers whose outlines do not correspond (where a hole opens, "
                                   "or two islands merge) keep their step.");
    auto smooth_label = new ::Label(this, Label::Body_14, _L("Smooth vertical steps") + ":");
    smooth_label->Wrap(FromDIP(300));
    smooth_label->SetToolTip(smooth_tip);
    m_smooth_cb = new ::CheckBox(this);
    m_smooth_cb->SetValue(m_settings.options.smooth_vertical_steps);
    m_smooth_cb->SetToolTip(smooth_tip);
    f_sizer->Add(smooth_label, 0, wxEXPAND | wxALIGN_CENTER_VERTICAL);
    f_sizer->Add(m_smooth_cb, 0, wxALIGN_CENTER_VERTICAL | wxTOP | wxBOTTOM, FromDIP(5));

    // ---- close small gaps ---------------------------------------------------------------------
    const wxString close_tip = _L("A fuzzed or steeply curved wall can leave hairline gaps between the printed "
                                  "widths of neighbouring loops. With this on, each layer's outline is grown by "
                                  "the radius and shrunk back again, which bridges those gaps - at the cost of "
                                  "rounding off any detail finer than the radius.");

    auto close_label = new ::Label(this, Label::Body_14, _L("Close small gaps") + ":");
    close_label->Wrap(FromDIP(300));
    close_label->SetToolTip(close_tip);
    m_close_cb = new ::CheckBox(this);
    m_close_cb->SetValue(m_settings.options.close_gaps_radius > 0.);
    m_close_cb->SetToolTip(close_tip);
    f_sizer->Add(close_label, 0, wxEXPAND | wxALIGN_CENTER_VERTICAL);
    f_sizer->Add(m_close_cb, 0, wxALIGN_CENTER_VERTICAL | wxTOP | wxBOTTOM, FromDIP(5));

    auto radius_label = new ::Label(this, Label::Body_14, _L("Gap radius") + ":");
    radius_label->Wrap(FromDIP(300));
    m_close_input = new ::TextInput(this,
                                    num_str(m_settings.options.close_gaps_radius > 0. ? m_settings.options.close_gaps_radius
                                                                                      : DEFAULT_CLOSE_RADIUS),
                                    _L("mm"), wxEmptyString, wxDefaultPosition, input_size, wxTE_PROCESS_ENTER);
    m_close_input->GetTextCtrl()->SetFont(Label::Body_14);
    m_close_input->GetTextCtrl()->SetValidator(wxTextValidator(wxFILTER_NUMERIC));
    m_close_input->Enable(m_close_cb->GetValue());
    f_sizer->Add(radius_label, 0, wxEXPAND | wxALIGN_CENTER_VERTICAL);
    f_sizer->Add(m_close_input, 0, wxALIGN_CENTER_VERTICAL);

    m_close_cb->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent &e) {
        e.Skip();
        m_close_input->Enable(m_close_cb->GetValue());
        update_preview();
    });

    // Every control that moves the triangle count re-runs the counter. wxEVT_TEXT rather than
    // wxEVT_TEXT_ENTER on the two number boxes, so the figure tracks what is being typed instead
    // of waiting for a Return the user has no reason to press.
    m_source_choice->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent &e) { e.Skip(); update_preview(); });
    m_smooth_cb->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent &e) { e.Skip(); update_preview(); });
    m_res_input->GetTextCtrl()->Bind(wxEVT_TEXT, [this](wxCommandEvent &e) { e.Skip(); update_preview(); });
    m_close_input->GetTextCtrl()->Bind(wxEVT_TEXT, [this](wxCommandEvent &e) { e.Skip(); update_preview(); });

    v_sizer->Add(f_sizer, 0, wxEXPAND | wxALL, FromDIP(10));

    // ---- the size line --------------------------------------------------------------------------
    // This is the reason the action has a dialog at all: a lofted bake is one stair-stepped band
    // per layer, so a 100 mm part at 0.2 mm is 500 bands of tens of thousands of triangles each.
    // The user gets the number BEFORE the job starts, not after it has eaten the memory.
    m_size_text = new ::Label(this, Label::Head_14, wxEmptyString);
    m_size_text->Wrap(FromDIP(340));
    v_sizer->Add(m_size_text, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));

    auto note_text = new ::Label(this, Label::Body_14,
                                 _L("The bake is the outer wall as it will be printed - fuzzy skin included. "
                                    "Supports, brim, skirt, the wipe tower and the infill are not baked. "
                                    "Replacing the object clears any painted supports, seams and colours, "
                                    "and the plate has to be sliced again afterwards."));
    note_text->Wrap(FromDIP(340));
    // Light-mode tone only; UpdateDlgDarkUI() maps it for dark mode, as in FillBedDialog.
    note_text->SetForegroundColour(wxColour("#6B6B6B"));
    v_sizer->Add(note_text, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));

    // ---- buttons --------------------------------------------------------------------------------
    auto dlg_btns = new DialogButtons(this, {"OK", "Cancel"});
    dlg_btns->GetOK()->SetLabel(_L("Bake"));
    dlg_btns->GetOK()->Bind(wxEVT_BUTTON, [this](wxCommandEvent &e) {
        m_settings = current_settings();
        save_to_config();
        EndModal(wxID_OK);
    });
    dlg_btns->GetCANCEL()->Bind(wxEVT_BUTTON, [this](wxCommandEvent &e) { EndModal(wxID_CANCEL); });
    v_sizer->Add(dlg_btns, 0, wxEXPAND);

    update_preview();

    this->SetSizer(v_sizer);
    this->Layout();
    v_sizer->Fit(this);
    CenterOnParent();
    wxGetApp().UpdateDlgDarkUI(this);
}

SliceBakeSettings SliceBakeDialog::current_settings() const
{
    SliceBakeSettings s = m_settings;
    if (m_result_choice != nullptr) {
        const int sel = m_result_choice->GetSelection();
        s.result = sel == 1 ? SliceBakeResultMode::AddNew
                            : (sel == 2 ? SliceBakeResultMode::ExportSTL : SliceBakeResultMode::Replace);
    }
    const bool close_on = m_close_cb != nullptr && m_close_cb->GetValue();
    s.options.close_gaps_radius =
        close_on ? std::clamp(read_mm(m_close_input, DEFAULT_CLOSE_RADIUS), 0., SLICE_BAKE_CLOSE_GAPS_MAX) : 0.;

    s.options.contour_source = SliceBakeContourSource::Extrusion; // the choice is hidden; extrusion paths always
    s.options.resolution = std::clamp(read_mm(m_res_input, m_default_res),
                                      SLICE_BAKE_RESOLUTION_MIN, SLICE_BAKE_RESOLUTION_MAX);
    s.options.smooth_vertical_steps = m_smooth_cb != nullptr && m_smooth_cb->GetValue();
    // Phase 1 bakes every layer; the range picker is deferred (spec section 4).
    s.options.layer_begin = 0;
    s.options.layer_end   = std::numeric_limits<size_t>::max();
    return s;
}

double SliceBakeDialog::read_mm(TextInput *input, double fallback) const
{
    if (input == nullptr)
        return fallback;
    double   v   = 0.;
    wxString txt = input->GetTextCtrl()->GetValue();
    if (txt.empty() || !txt.ToDouble(&v))
        return fallback;
    return v;
}

void SliceBakeDialog::update_preview()
{
    if (m_size_text == nullptr)
        return;

    // The count for the settings as they stand RIGHT NOW, which is why this runs on every control
    // change rather than being handed in once: the contour source alone can move it by a factor of
    // several (a slice contour is far denser than the simplified extrusion it produced), and the
    // resolution moves it continuously.
    const size_t estimate = m_counter ? m_counter(current_settings().options) : 0;

    // Rounded to two significant figures: the count depends on the tesselation of every layer's
    // free-top and overhang deltas, which is not knowable without running the bake, so anything
    // finer would be false precision.
    double est = double(estimate);
    if (est >= 10.) {
        const double mag = std::pow(10., std::floor(std::log10(est)) - 1.);
        est = std::round(est / mag) * mag;
    }
    m_size_text->SetLabel(wxString::Format(_L("%llu layers, about %llu triangles"),
                                           (unsigned long long) m_layers,
                                           (unsigned long long) std::llround(est)));
    m_size_text->Wrap(FromDIP(340));

    Layout();
    if (GetSizer() != nullptr)
        GetSizer()->Fit(this);
}

void SliceBakeDialog::save_to_config() const
{
    AppConfig *cfg = wxGetApp().app_config;
    if (cfg == nullptr)
        return;
    const char *r = m_settings.result == SliceBakeResultMode::AddNew ? "add"
                  : (m_settings.result == SliceBakeResultMode::ExportSTL ? "export" : "replace");
    cfg->set(CFG_SECTION, CFG_RESULT, std::string(r));
    cfg->set(CFG_SECTION, CFG_CLOSE_ON, m_settings.options.close_gaps_radius > 0.);
    if (m_settings.options.close_gaps_radius > 0.)
        cfg->set(CFG_SECTION, CFG_CLOSE_R, num_str(m_settings.options.close_gaps_radius).ToStdString());
    cfg->set(CFG_SECTION, CFG_SOURCE,
             std::string(m_settings.options.contour_source == SliceBakeContourSource::Extrusion ? "extrusion" : "slices"));
    cfg->set(CFG_SECTION, CFG_RESOLUTION, num_str(m_settings.options.resolution).ToStdString());
    cfg->set(CFG_SECTION, CFG_SMOOTH, m_settings.options.smooth_vertical_steps);
}

void SliceBakeDialog::on_dpi_changed(const wxRect &suggested_rect)
{
    Layout();
    if (GetSizer() != nullptr)
        GetSizer()->Fit(this);
    Refresh();
}

}} // namespace Slic3r::GUI
