#include "FinBakeDialog.hpp"

#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"
#include "format.hpp"

#include "libslic3r/BoundingBox.hpp"

#include <wx/dcbuffer.h>
#include <wx/sizer.h>

#include <algorithm>
#include <vector>

namespace Slic3r { namespace GUI {

static wxString fin_bake_mm(double v) { return wxString::Format("%.2f ", v) + _L("mm"); }

// ---- the preview ------------------------------------------------------------------------------------

FinBakePreview::FinBakePreview(wxWindow *parent, const FinBakePreviewData &data, const wxSize &size)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, size, wxBORDER_NONE), m_data(data)
{
    SetBackgroundStyle(wxBG_STYLE_PAINT); // drawn whole by on_paint, through wxAutoBufferedPaintDC
    SetMinSize(size);
    Bind(wxEVT_PAINT, &FinBakePreview::on_paint, this);
}

void FinBakePreview::on_paint(wxPaintEvent &)
{
    wxAutoBufferedPaintDC dc(this);
    const bool     dark   = wxGetApp().dark_mode();
    const wxColour bg     = dark ? wxColour("#2D2D31") : wxColour("#F4F4F4");
    const wxColour part   = dark ? wxColour("#6E6E73") : wxColour("#B9B9B9");
    const wxColour fin    = wxColour("#00AE42");
    const wxColour tine   = dark ? wxColour("#7CF0A8") : wxColour("#00632A");
    const wxSize   sz     = GetClientSize();
    dc.SetBackground(wxBrush(bg));
    dc.Clear();

    BoundingBox bb;
    for (const ExPolygons *set : { &m_data.part, &m_data.fins })
        for (const ExPolygon &e : *set)
            bb.merge(get_extents(e.contour));
    if (! bb.defined || sz.x < 10 || sz.y < 10)
        return;
    const double margin = FromDIP(8);
    const double w      = std::max(1., unscaled(double(bb.max.x() - bb.min.x())));
    const double h      = std::max(1., unscaled(double(bb.max.y() - bb.min.y())));
    const double s      = std::min((sz.x - 2. * margin) / w, (sz.y - 2. * margin) / h);
    const double ox     = 0.5 * (sz.x - s * w), oy = 0.5 * (sz.y - s * h);
    // Bed coordinates, Y up, into the panel.
    auto to_px = [&](const Point &p) {
        return wxPoint(int(ox + s * unscaled(double(p.x() - bb.min.x()))), int(sz.y - oy - s * unscaled(double(p.y() - bb.min.y()))));
    };
    auto fill = [&](const ExPolygons &expolys, const wxColour &colour) {
        for (const ExPolygon &e : expolys) {
            std::vector<wxPoint> pts;
            pts.reserve(e.contour.size());
            for (const Point &p : e.contour.points)
                pts.push_back(to_px(p));
            if (pts.size() < 3)
                continue;
            dc.SetPen(wxPen(colour));
            dc.SetBrush(wxBrush(colour));
            dc.DrawPolygon(int(pts.size()), pts.data());
            // Holes in the background colour.
            for (const Polygon &hole : e.holes) {
                std::vector<wxPoint> hp;
                for (const Point &p : hole.points)
                    hp.push_back(to_px(p));
                if (hp.size() < 3)
                    continue;
                dc.SetPen(wxPen(bg));
                dc.SetBrush(wxBrush(bg));
                dc.DrawPolygon(int(hp.size()), hp.data());
            }
        }
    };
    fill(m_data.part, part);
    fill(m_data.fins, fin);
    dc.SetPen(wxPen(tine, std::max(1, FromDIP(2))));
    for (const Polyline &pl : m_data.tines)
        if (pl.size() >= 2)
            dc.DrawLine(to_px(pl.first_point()), to_px(pl.last_point()));
}

// ---- the dialog -------------------------------------------------------------------------------------

FinBakeDialog::FinBakeDialog(wxWindow                 *parent,
                             const wxString           &object_name,
                             const wxString           &mode_label,
                             const FinBakePreviewData &preview,
                             double                    sliced_gap,
                             double                    layer_height,
                             int                       support_filament,
                             bool                      by_object,
                             bool                      part_allowed)
    : DPIDialog(parent ? parent : static_cast<wxWindow *>(wxGetApp().mainframe),
                wxID_ANY,
                _L("Bake fins"),
                wxDefaultPosition,
                wxDefaultSize,
                wxCAPTION | wxCLOSE_BOX)
    , m_sliced_gap(sliced_gap)
    , m_part_allowed(part_allowed)
    , m_by_object(by_object)
    , m_has_support_filament(support_filament > 0)
{
    // The same frame, fonts and widgets as StabilizerBakeDialog: white in light mode, mapped for dark mode
    // and the active theme by UpdateDlgDarkUI() at the end. Every text is a ::Label.
    SetBackgroundColour(*wxWHITE);
    SetFont(Label::Body_14);

    const int wrap = FromDIP(400);
    auto v_sizer = new wxBoxSizer(wxVERTICAL);

    auto intro = new ::Label(this, Label::Body_14,
                             format_wxstr(_L("Turn the support fins of \"%1%\" (%2%) into real geometry that prints in any slicer."),
                                          object_name, mode_label));
    intro->Wrap(wrap);
    v_sizer->Add(intro, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(10));

    // ---- the preview --------------------------------------------------------------------------------
    auto preview_panel = new FinBakePreview(this, preview, wxSize(wrap, FromDIP(220)));
    v_sizer->Add(preview_panel, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(10));
    auto preview_caption = new ::Label(this, Label::Body_12,
                                       format_wxstr(_L("Top view from the current slice: the part in grey, %1% fin(s) in green, "
                                                       "%2% tine(s) as dark lines; the fins stand up to %3%."),
                                                    preview.fin_count, preview.tine_count, fin_bake_mm(preview.fin_top)));
    preview_caption->Wrap(wrap);
    preview_caption->SetForegroundColour(wxColour("#6B6B6B"));
    v_sizer->Add(preview_caption, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(4));

    auto f_sizer = new wxFlexGridSizer(2, FromDIP(4), FromDIP(20));
    const wxSize input_size(FromDIP(240), -1);

    // ---- placement ----------------------------------------------------------------------------------
    const wxString placement_tip =
        _L("Part of this object: a new part that moves with the object. Parts of one object fuse where they touch, "
           "so the tines fuse into the part as the live tines do, and the walls keep their gap.\n\n"
           "Separate object: a new object beside the part with its own settings (no supports, no brim, in the "
           "support filament). Separate objects never fuse, so the tines stop at the part and only catch it. It "
           "does not move with the part.");
    auto placement_label = new ::Label(this, Label::Body_14, _L("Placement") + ":");
    placement_label->SetToolTip(placement_tip);
    m_placement_choice = new ::ComboBox(this, wxID_ANY, wxEmptyString, wxDefaultPosition, input_size, 0, nullptr, wxCB_READONLY);
    if (part_allowed)
        m_placement_choice->Append(_L("Part of this object (recommended)"));
    m_placement_choice->Append(_L("Separate object"));
    m_placement_choice->SetSelection(0);
    m_placement_choice->SetToolTip(placement_tip);
    f_sizer->Add(placement_label, 0, wxEXPAND | wxALIGN_CENTER_VERTICAL);
    f_sizer->Add(m_placement_choice, 0, wxALIGN_CENTER_VERTICAL);

    // ---- tines --------------------------------------------------------------------------------------
    const wxString tines_tip =
        format_wxstr(_L("Wide: %1% lines wide, so any wall generator and any slicer prints them.\n\n"
                        "One line: exactly the live tine, a single bead that snaps off most easily. Only Arachne walls "
                        "print a strip one line wide; the classic wall generator (Bambu Studio's default) drops it."),
                     wxString::Format("%.1f", FIN_BAKE_WIDE_TINE_LINES));
    auto tines_label = new ::Label(this, Label::Body_14, _L("Tines") + ":");
    tines_label->SetToolTip(tines_tip);
    m_tines_choice = new ::ComboBox(this, wxID_ANY, wxEmptyString, wxDefaultPosition, input_size, 0, nullptr, wxCB_READONLY);
    m_tines_choice->Append(_L("Wide (any slicer)"));
    m_tines_choice->Append(_L("One line (Arachne walls)"));
    m_tines_choice->SetSelection(0);
    m_tines_choice->SetToolTip(tines_tip);
    f_sizer->Add(tines_label, 0, wxEXPAND | wxALIGN_CENTER_VERTICAL);
    f_sizer->Add(m_tines_choice, 0, wxALIGN_CENTER_VERTICAL);

    // ---- the sliced settings, read only -------------------------------------------------------------
    auto gap_label = new ::Label(this, Label::Body_14, _L("Fin gap") + ":");
    m_gap_value    = new ::Label(this, Label::Body_14, fin_bake_mm(sliced_gap));
    f_sizer->Add(gap_label, 0, wxEXPAND | wxALIGN_CENTER_VERTICAL);
    f_sizer->Add(m_gap_value, 0, wxALIGN_CENTER_VERTICAL);
    auto lh_label = new ::Label(this, Label::Body_14, _L("Layer height") + ":");
    auto lh_value = new ::Label(this, Label::Body_14, fin_bake_mm(layer_height));
    f_sizer->Add(lh_label, 0, wxEXPAND | wxALIGN_CENTER_VERTICAL);
    f_sizer->Add(lh_value, 0, wxALIGN_CENTER_VERTICAL);

    v_sizer->Add(f_sizer, 0, wxEXPAND | wxALL, FromDIP(10));

    // A secondary note, in the light-mode grey StabilizerBakeDialog uses for its notes.
    auto add_note = [this, v_sizer, wrap](const wxString &text, const wxColour &colour) {
        auto note = new ::Label(this, Label::Body_14, text);
        note->Wrap(wrap);
        note->SetForegroundColour(colour);
        v_sizer->Add(note, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));
        return note;
    };
    const wxColour grey("#6B6B6B");
    // #FF6F00 is the light-mode warning colour; UpdateDlgDarkUI() maps it for dark mode and the theme.
    const wxColour orange("#FF6F00");
    add_note(_L("The fins are planned from the current slice; change them in the object's settings and re-slice."), grey);
    if (! part_allowed)
        add_note(_L("Part of this object is not offered: the object's copies are rotated or scaled differently."), grey);
    m_gap_warning = add_note(format_wxstr(_L("The fin walls will keep a %1% mm gap from the part, which a part of the same object "
                                             "needs so the walls do not fuse to it. The slice used %2% mm."),
                                          FIN_BAKE_PART_MIN_GAP, wxString::Format("%.2f", sliced_gap)),
                             orange);
    m_separate_note = add_note(_L("As a separate object the tines do not fuse into the part: they stop at its outline and only "
                                  "catch it."),
                               orange);
    m_one_line_note = add_note(_L("One-line tines print only with Arachne walls. In Bambu Studio, or with classic walls, "
                                  "choose Wide."),
                               orange);
    m_filament_note = add_note(_L("As a part, the fins print in the object's own filament, not the support filament, so the "
                                  "tines bond to the part."),
                               grey);
    m_by_object_note = add_note(_L("This plate prints by object: a separate fin object is printed apart from the part, which "
                                   "defeats it. Choose \"Part of this object\" or print by layer."),
                                orange);

    // ---- notes in the normal text colour -------------------------------------------------------------
    auto notes_text = new ::Label(this, Label::Body_14,
                                  format_wxstr(_L("Each tine is one layer tall at the layer heights the plate was sliced with "
                                                  "(%1%): slice the baked fins at the same layer height, here or in another "
                                                  "slicer. The object's live fins are switched off. The baked fins do not follow "
                                                  "later moves, rotations or scaling of the part, nor arrange: bake again after "
                                                  "changing it."),
                                               fin_bake_mm(layer_height)));
    notes_text->Wrap(wrap);
    v_sizer->Add(notes_text, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));

    for (::ComboBox *combo : { m_placement_choice, m_tines_choice })
        combo->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent &e) {
            e.Skip();
            update_choice();
        });

    // ---- buttons: the themed primary (Bake) and Cancel ----------------------------------------------
    auto dlg_btns = new DialogButtons(this, {"OK", "Cancel"});
    dlg_btns->GetOK()->SetLabel(_L("Bake"));
    dlg_btns->GetOK()->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        m_options.placement = placement();
        m_options.tines     = tines();
        EndModal(wxID_OK);
    });
    dlg_btns->GetCANCEL()->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { EndModal(wxID_CANCEL); });
    v_sizer->Add(dlg_btns, 0, wxEXPAND);

    SetSizer(v_sizer);
    update_choice();
    CenterOnParent();
    wxGetApp().UpdateDlgDarkUI(this);
}

FinBakePlacement FinBakeDialog::placement() const
{
    return m_part_allowed && m_placement_choice != nullptr && m_placement_choice->GetSelection() == 0 ?
               FinBakePlacement::PartOfObject : FinBakePlacement::SeparateObject;
}

FinBakeTines FinBakeDialog::tines() const
{
    return m_tines_choice != nullptr && m_tines_choice->GetSelection() == 1 ? FinBakeTines::OneLine : FinBakeTines::Wide;
}

void FinBakeDialog::update_choice()
{
    const FinBakePlacement p   = placement();
    const double           gap = fin_bake_gap(p, m_sliced_gap);
    // The gap the bake will use: the slice's, or the part's minimum.
    m_gap_value->SetLabel(fin_bake_mm(gap));
    m_gap_warning->Show(gap > m_sliced_gap + EPSILON);
    m_separate_note->Show(p == FinBakePlacement::SeparateObject);
    m_by_object_note->Show(m_by_object && p == FinBakePlacement::SeparateObject);
    m_filament_note->Show(m_has_support_filament && p == FinBakePlacement::PartOfObject);
    m_one_line_note->Show(tines() == FinBakeTines::OneLine);
    Layout();
    if (GetSizer() != nullptr)
        GetSizer()->Fit(this);
}

void FinBakeDialog::on_dpi_changed(const wxRect &suggested_rect)
{
    Layout();
    if (GetSizer() != nullptr)
        GetSizer()->Fit(this);
    Refresh();
}

}} // namespace Slic3r::GUI
