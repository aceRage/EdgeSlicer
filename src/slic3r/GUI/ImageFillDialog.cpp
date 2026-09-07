#include "ImageFillDialog.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "format.hpp"

#include "libslic3r/TriangleMesh.hpp"

#include <wx/bitmap.h>
#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/checklst.h>
#include <wx/choice.h>
#include <wx/filedlg.h>
#include <wx/image.h>
#include <wx/mstream.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/statbmp.h>
#include <wx/stattext.h>
#include <wx/wfstream.h>

#include <algorithm>
#include <cmath>

namespace Slic3r { namespace GUI {

static const int PREVIEW_PX = 128;

// Every image the store holds is a PNG, because that is the only thing PNGReadWrite can decode
// and because a fixed encoding is what makes the content hash - and therefore the 3MF round trip
// - reproducible. wxImage reads whatever the user picked and writes the PNG this returns.
static bool load_image_as_png(const wxString &path, std::vector<uint8_t> &out, wxImage &decoded)
{
    wxLogNull no_log;   // a bad file is reported by us, not by wx's own modal
    if (!decoded.LoadFile(path))
        return false;
    if (!decoded.IsOk() || decoded.GetWidth() <= 0 || decoded.GetHeight() <= 0)
        return false;
    // An alpha channel would be dropped by the RGB re-encode anyway; do it here so what is stored
    // and what is sampled cannot disagree.
    if (decoded.HasAlpha())
        decoded.ClearAlpha();
    wxMemoryOutputStream mem;
    if (!decoded.SaveFile(mem, wxBITMAP_TYPE_PNG))
        return false;
    out.resize(mem.GetSize());
    mem.CopyTo(out.data(), out.size());
    return !out.empty();
}

ImageFillDialog::ImageFillDialog(wxWindow                                      *parent,
                                 const std::vector<ImageFillFilament>          &filaments,
                                 const ImageFillParams                         &initial,
                                 ImageAssetStore                               *assets,
                                 const indexed_triangle_set                    &mesh,
                                 const TriangleSelector::TriangleSplittingData &existing,
                                 const std::vector<int>                        &painted_states)
    : DPIDialog(parent, wxID_ANY, _L("Apply image fill"), wxDefaultPosition, wxDefaultSize,
                wxDEFAULT_DIALOG_STYLE)
    , m_params(initial)
    , m_assets(assets)
    , m_mesh(mesh)
    , m_existing(existing)
    , m_filaments(filaments)
    , m_painted_states(painted_states)
{
    wxBoxSizer *root = new wxBoxSizer(wxVERTICAL);
    wxBoxSizer *cols = new wxBoxSizer(wxHORIZONTAL);
    wxBoxSizer *left = new wxBoxSizer(wxVERTICAL);

    auto row = [this, left](const wxString &label, wxWindow *ctrl) {
        wxBoxSizer *s = new wxBoxSizer(wxHORIZONTAL);
        s->Add(new wxStaticText(this, wxID_ANY, label), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
        s->Add(ctrl, 1, wxALIGN_CENTER_VERTICAL);
        left->Add(s, 0, wxEXPAND | wxTOP, FromDIP(6));
    };

    // ---- the source ---------------------------------------------------------------------------
    wxArrayString sources;
    sources.Add(_L("Image file"));
    sources.Add(_L("Gradient, two colours"));
    sources.Add(_L("Gradient, three colours"));
    m_source_choice = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, sources);
    m_source_choice->SetSelection(m_params.gradient.enabled ? (m_params.gradient.three_stop ? 2 : 1) : 0);
    row(_L("Source") + ":", m_source_choice);

    wxBoxSizer *pick = new wxBoxSizer(wxHORIZONTAL);
    wxButton   *pick_btn = new wxButton(this, wxID_ANY, _L("Choose image..."));
    m_image_text = new wxStaticText(this, wxID_ANY, _L("(none)"));
    pick->Add(pick_btn, 0, wxRIGHT, FromDIP(8));
    pick->Add(m_image_text, 1, wxALIGN_CENTER_VERTICAL);
    left->Add(pick, 0, wxEXPAND | wxTOP, FromDIP(6));

    // ---- the projection -----------------------------------------------------------------------
    wxArrayString projections;
    projections.Add(_L("Flat, along an axis"));
    projections.Add(_L("Wrapped around an axis"));
    projections.Add(_L("The model's own texture coordinates"));
    m_projection = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, projections);
    m_projection->SetSelection(int(m_params.projection));
    row(_L("Projection") + ":", m_projection);

    wxArrayString axes;
    axes.Add("X");
    axes.Add("Y");
    axes.Add("Z");
    m_axis = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, axes);
    m_axis->SetSelection(int(m_params.axis));
    row(_L("Axis") + ":", m_axis);

    wxBoxSizer *flips = new wxBoxSizer(wxHORIZONTAL);
    m_flip_u = new wxCheckBox(this, wxID_ANY, _L("Mirror horizontally"));
    m_flip_v = new wxCheckBox(this, wxID_ANY, _L("Mirror vertically"));
    m_flip_u->SetValue(m_params.flip_u);
    m_flip_v->SetValue(m_params.flip_v);
    flips->Add(m_flip_u, 0, wxRIGHT, FromDIP(12));
    flips->Add(m_flip_v, 0);
    left->Add(flips, 0, wxEXPAND | wxTOP, FromDIP(6));

    // ---- where --------------------------------------------------------------------------------
    wxArrayString where;
    where.Add(_L("The whole part"));
    for (int s : m_painted_states)
        where.Add(format_wxstr(_L("Only where filament %1% is painted"), s));
    m_selection = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, where);
    m_selection->SetSelection(0);
    for (size_t i = 0; i < m_painted_states.size(); ++i)
        if (m_painted_states[i] == m_params.selection_state)
            m_selection->SetSelection(int(i) + 1);
    m_selection->Enable(!m_painted_states.empty());
    m_selection->SetToolTip(_L("Paint a face selection with the colour-painting tool first, then apply the "
                               "image only to those faces."));
    row(_L("Apply to") + ":", m_selection);

    // ---- the filaments it may use ---------------------------------------------------------------
    left->Add(new wxStaticText(this, wxID_ANY, _L("Filaments the image may use") + ":"), 0,
              wxEXPAND | wxTOP, FromDIP(10));
    wxArrayString names;
    for (const ImageFillFilament &f : m_filaments)
        names.Add(wxString::Format("%d", f.id) + (f.name.empty() ? wxString() : wxString(" - ") + from_u8(f.name)));
    m_filament_list = new wxCheckListBox(this, wxID_ANY, wxDefaultPosition, wxSize(-1, FromDIP(120)), names);
    for (size_t i = 0; i < m_filaments.size(); ++i) {
        const bool on = m_params.allowed.empty() ||
                        std::find(m_params.allowed.begin(), m_params.allowed.end(), m_filaments[i].id) !=
                            m_params.allowed.end();
        m_filament_list->Check((unsigned) i, on);
    }
    left->Add(m_filament_list, 0, wxEXPAND | wxTOP, FromDIP(4));

    // ---- resolution -----------------------------------------------------------------------------
    m_detail = new wxSpinCtrlDouble(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(100), -1),
                                    wxSP_ARROW_KEYS, 0.0, 50.0, m_params.detail_mm > 0.f ? m_params.detail_mm : 1.0,
                                    0.1);
    m_detail->SetToolTip(_L("How small a facet the image is allowed to decide, in millimetres. Smaller means "
                            "more detail and more triangles. 0 keeps the model's own triangles."));
    row(_L("Detail (mm)") + ":", m_detail);

    cols->Add(left, 1, wxEXPAND | wxALL, FromDIP(10));

    // ---- the preview ------------------------------------------------------------------------------
    wxBoxSizer *right = new wxBoxSizer(wxVERTICAL);
    right->Add(new wxStaticText(this, wxID_ANY, _L("Preview") + ":"), 0, wxBOTTOM, FromDIP(4));
    wxImage blank(PREVIEW_PX, PREVIEW_PX);
    blank.SetRGB(wxRect(0, 0, PREVIEW_PX, PREVIEW_PX), 200, 200, 200);
    m_preview = new wxStaticBitmap(this, wxID_ANY, wxBitmap(blank));
    right->Add(m_preview, 0);
    m_summary = new wxStaticText(this, wxID_ANY, wxEmptyString, wxDefaultPosition,
                                 wxSize(FromDIP(200), FromDIP(60)));
    right->Add(m_summary, 0, wxTOP, FromDIP(6));
    cols->Add(right, 0, wxEXPAND | wxALL, FromDIP(10));
    root->Add(cols, 1, wxEXPAND);

    wxStdDialogButtonSizer *buttons = new wxStdDialogButtonSizer();
    m_ok = new wxButton(this, wxID_OK, _L("Apply"));
    buttons->AddButton(m_ok);
    buttons->AddButton(new wxButton(this, wxID_CANCEL));
    buttons->Realize();
    root->Add(buttons, 0, wxEXPAND | wxALL, FromDIP(10));

    // Every control refreshes the preview, which is also what keeps m_params current: there is one
    // collect() and one place that reads the widgets, so Apply cannot disagree with the picture.
    auto changed = [this](wxCommandEvent &evt) { refresh_preview(); evt.Skip(); };
    m_source_choice->Bind(wxEVT_CHOICE, changed);
    m_projection->Bind(wxEVT_CHOICE, changed);
    m_axis->Bind(wxEVT_CHOICE, changed);
    m_selection->Bind(wxEVT_CHOICE, changed);
    m_flip_u->Bind(wxEVT_CHECKBOX, changed);
    m_flip_v->Bind(wxEVT_CHECKBOX, changed);
    m_filament_list->Bind(wxEVT_CHECKLISTBOX, changed);
    m_detail->Bind(wxEVT_SPINCTRLDOUBLE, [this](wxSpinDoubleEvent &evt) { refresh_preview(); evt.Skip(); });
    pick_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent &evt) { on_pick_image(); evt.Skip(); });

    if (!m_params.asset.empty() && m_assets != nullptr && m_assets->contains(m_params.asset))
        m_image_label = m_params.asset.substr(0, 12) + "...";
    refresh_preview();

    SetSizerAndFit(root);
    wxGetApp().UpdateDlgDarkUI(this);
    CenterOnParent();
}

void ImageFillDialog::on_pick_image()
{
    wxFileDialog dlg(this, _L("Choose an image"), wxEmptyString, wxEmptyString,
                     _L("Images") + " (*.png;*.jpg;*.jpeg;*.bmp)|*.png;*.PNG;*.jpg;*.JPG;*.jpeg;*.JPEG;*.bmp;*.BMP",
                     wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dlg.ShowModal() != wxID_OK)
        return;
    std::vector<uint8_t> png;
    wxImage              decoded;
    if (!load_image_as_png(dlg.GetPath(), png, decoded) || m_assets == nullptr) {
        show_error(this, _L("This image could not be read."));
        return;
    }
    m_params.asset = m_assets->add(std::move(png));
    m_image_label  = into_u8(wxFileName(dlg.GetPath()).GetFullName());
    m_source_choice->SetSelection(0);
    refresh_preview();
}

void ImageFillDialog::collect()
{
    const int source = m_source_choice->GetSelection();
    m_params.gradient.enabled    = source > 0;
    m_params.gradient.three_stop = source == 2;
    if (m_params.gradient.enabled) {
        // Phase 2 ships one gradient: the first allowed filament's colour to the last one's, which
        // is the useful default and needs no colour pickers. Editing the stops is Phase 3's job,
        // when a gradient becomes a mixed-filament row rather than a source of samples.
        m_params.gradient.direction = 1;
    }
    m_params.projection = ImageFillProjection(std::max(0, m_projection->GetSelection()));
    m_params.axis       = ImageFillAxis(std::max(0, m_axis->GetSelection()));
    m_params.flip_u     = m_flip_u->GetValue();
    m_params.flip_v     = m_flip_v->GetValue();
    const int sel       = m_selection->GetSelection();
    m_params.selection_state = (sel > 0 && size_t(sel - 1) < m_painted_states.size()) ? m_painted_states[sel - 1] : 0;
    m_params.detail_mm  = float(m_detail->GetValue());
    m_params.subdivision = IMAGE_FILL_MAX_SUBDIVISION;   // the cap; detail_mm decides the depth

    m_params.allowed.clear();
    for (size_t i = 0; i < m_filaments.size(); ++i)
        if (m_filament_list->IsChecked((unsigned) i))
            m_params.allowed.push_back(m_filaments[i].id);

    if (m_params.gradient.enabled && m_params.allowed.size() >= 2) {
        m_params.gradient.stop_a = m_filaments.front().color;
        m_params.gradient.stop_b = m_filaments.back().color;
        for (const ImageFillFilament &f : m_filaments) {
            if (f.id == m_params.allowed.front()) m_params.gradient.stop_a = f.color;
            if (f.id == m_params.allowed.back())  m_params.gradient.stop_b = f.color;
            if (m_params.gradient.three_stop && m_params.allowed.size() >= 3 &&
                f.id == m_params.allowed[m_params.allowed.size() / 2])
                m_params.gradient.stop_c = m_params.gradient.stop_b, m_params.gradient.stop_b = f.color;
        }
    }
    m_params.axis = ImageFillAxis(std::max(0, m_axis->GetSelection()));
}

void ImageFillDialog::refresh_preview()
{
    collect();

    // Which filaments are on the table, in id order.
    std::vector<std::array<float, 3>> colors;
    std::vector<int>                  ids;
    for (const ImageFillFilament &f : m_filaments)
        if (std::find(m_params.allowed.begin(), m_params.allowed.end(), f.id) != m_params.allowed.end()) {
            colors.push_back(f.color);
            ids.push_back(f.id);
        }

    m_axis->Enable(m_params.projection != ImageFillProjection::MeshUV);
    if (m_ok != nullptr)
        m_ok->Enable(!ids.empty() && (m_params.gradient.enabled || !m_params.asset.empty()));
    m_image_text->SetLabel(m_params.asset.empty() ? _L("(none)") : from_u8(m_image_label));

    // The preview is the SOURCE, quantised through the same palette and the same solver the fill
    // will use - so what the user sees is the colours the slicer will actually try to print, not
    // the picture's own. It is deliberately not a render of the model: a 3D preview would need
    // the gizmo machinery this phase does without, and the question the user is asking here is
    // "which of my filaments will this become".
    wxImage      out(PREVIEW_PX, PREVIEW_PX);
    const ImageAsset *img = (m_assets != nullptr && !m_params.asset.empty())
                                ? m_assets->pixels(m_params.asset) : nullptr;

    std::vector<std::array<float, 3>> samples;
    samples.reserve(PREVIEW_PX * PREVIEW_PX);
    for (int y = 0; y < PREVIEW_PX; ++y)
        for (int x = 0; x < PREVIEW_PX; ++x) {
            const float u = (float(x) + 0.5f) / float(PREVIEW_PX);
            const float v = 1.f - (float(y) + 0.5f) / float(PREVIEW_PX);
            if (img != nullptr) {
                int px = std::min(int(img->width) - 1, std::max(0, int(u * float(img->width))));
                int py = std::min(int(img->height) - 1, std::max(0, int((1.f - v) * float(img->height))));
                const size_t o = (size_t(py) * size_t(img->width) + size_t(px)) * 3;
                samples.push_back({img->rgb[o] / 255.f, img->rgb[o + 1] / 255.f, img->rgb[o + 2] / 255.f});
            } else if (m_params.gradient.enabled) {
                samples.push_back(m_params.gradient.sample(u, v));
            } else {
                samples.push_back({0.8f, 0.8f, 0.8f});
            }
        }

    size_t distinct = 0;
    if (!ids.empty() && (img != nullptr || m_params.gradient.enabled)) {
        ImageFillPalette palette = image_fill_quantise(samples, 256);
        image_fill_solve(palette, colors, ids);
        std::vector<int> used;
        for (int y = 0; y < PREVIEW_PX; ++y)
            for (int x = 0; x < PREVIEW_PX; ++x) {
                const std::array<float, 3> &c = samples[size_t(y) * PREVIEW_PX + x];
                size_t best = 0; float bestd = 1e30f;
                for (size_t i = 0; i < palette.colors.size(); ++i) {
                    const float dr = c[0] - palette.colors[i][0], dg = c[1] - palette.colors[i][1],
                                db = c[2] - palette.colors[i][2];
                    const float d = dr * dr + dg * dg + db * db;
                    if (d < bestd) { bestd = d; best = i; }
                }
                const int id = palette.filament[best];
                std::array<float, 3> shown{0.5f, 0.5f, 0.5f};
                for (size_t i = 0; i < ids.size(); ++i)
                    if (ids[i] == id) shown = colors[i];
                if (id > 0 && std::find(used.begin(), used.end(), id) == used.end())
                    used.push_back(id);
                out.SetRGB(x, y, (unsigned char) std::lround(shown[0] * 255.f),
                           (unsigned char) std::lround(shown[1] * 255.f),
                           (unsigned char) std::lround(shown[2] * 255.f));
            }
        distinct = used.size();
    } else {
        out.SetRGB(wxRect(0, 0, PREVIEW_PX, PREVIEW_PX), 200, 200, 200);
    }
    m_preview->SetBitmap(wxBitmap(out));

    // How deep the fill will actually go, worked out on the real mesh so the number is the one
    // that will be used - image_fill_compute makes the same call.
    float max_edge = 0.f;
    for (const Vec3i32 &t : m_mesh.indices) {
        const Vec3f &a = m_mesh.vertices[t(0)], &b = m_mesh.vertices[t(1)], &c = m_mesh.vertices[t(2)];
        max_edge = std::max({max_edge, (b - a).norm(), (c - b).norm(), (a - c).norm()});
    }
    const int depth = image_fill_depth_for_detail(max_edge, m_params.detail_mm, m_params.subdivision,
                                                  m_mesh.indices.size());
    size_t per = 1;
    for (int i = 0; i < depth; ++i) per *= 4;

    wxString text;
    if (ids.empty())
        text = _L("Tick at least one filament.");
    else if (img == nullptr && !m_params.gradient.enabled)
        text = _L("Choose an image, or pick a gradient.");
    else
        text = format_wxstr(_L("%1% filaments in use\n%2% painted facets (subdivision level %3%)"),
                            distinct, m_mesh.indices.size() * per, depth);
    m_summary->SetLabel(text);
    Layout();
}

}} // namespace Slic3r::GUI
