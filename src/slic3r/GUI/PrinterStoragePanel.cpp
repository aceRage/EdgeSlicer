#include "PrinterStoragePanel.hpp"

#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MsgDialog.hpp"
#include "SnapmakerLan.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/StateColor.hpp"
#include "slic3r/Utils/PrintHost.hpp"

#include <wx/choice.h>
#include <wx/datetime.h>
#include <wx/dataview.h>
#include <wx/dcmemory.h>
#include <wx/log.h>
#include <wx/mstream.h>
#include <wx/sizer.h>
#include <wx/statbmp.h>
#include <wx/stattext.h>
#include <wx/variant.h>

#include <boost/algorithm/string/replace.hpp>
#include <boost/log/trivial.hpp>

#include <algorithm>
#include <chrono>
#include <set>
#include <thread>

namespace Slic3r {
namespace GUI {

// No using-directive: in a unity build it would leak into the neighbouring sources.
namespace pstore = Slic3r::PrinterStorage;

namespace {

// The real executor: the blocking request on a thread of its own, the answer back on the GUI
// thread through the app's event queue. The model drops answers that arrive after it is gone.
class WxFileGridExecutor : public FileGridExecutor
{
public:
    void run(std::function<void()> work, std::function<void()> then) override
    {
        std::thread([work = std::move(work), then = std::move(then)]() mutable {
            try {
                work();
            } catch (const std::exception& e) {
                BOOST_LOG_TRIVIAL(error) << "[Storage] request failed: " << e.what();
            } catch (...) {
                BOOST_LOG_TRIVIAL(error) << "[Storage] request failed";
            }
            if (wxTheApp)
                wxTheApp->CallAfter([then = std::move(then)]() { then(); });
        }).detach();
    }
};

enum StorageColumn { scPicture = 0, scName, scTime, scFilament, scSize, scModified };

// Rows the details of which are fetched beyond the visible ones, so a short scroll finds them ready.
constexpr int STORAGE_FOCUS_MARGIN = 6;
// Opening the tab again lists the files again once the list is this old.
constexpr long long STORAGE_RELIST_AFTER_MS = 60000;

long long storage_now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

wxString storage_modified_text(std::int64_t t)
{
    if (t <= 0)
        return wxString();
    return wxDateTime((time_t) t).Format("%Y-%m-%d %H:%M");
}

// "PLA;PLA;PETG" -> "PLA, PETG"
std::string storage_types_text(const std::string& types)
{
    std::vector<std::string> seen;
    size_t                   start = 0;
    while (start <= types.size()) {
        size_t end = types.find(';', start);
        if (end == std::string::npos)
            end = types.size();
        std::string t     = types.substr(start, end - start);
        const size_t first = t.find_first_not_of(" \"");
        t                  = first == std::string::npos ? std::string() : t.substr(first, t.find_last_not_of(" \"") - first + 1);
        if (!t.empty() && std::find(seen.begin(), seen.end(), t) == seen.end())
            seen.push_back(t);
        start = end + 1;
    }
    std::string out;
    for (size_t i = 0; i < seen.size(); ++i)
        out += (i ? ", " : "") + seen[i];
    return out;
}

} // namespace

// ---- sources -------------------------------------------------------------------------------------

PrinterStoragePanel::Source PrinterStoragePanel::fixed_source(const pstore::Target& target)
{
    return [target]() {
        Targets t;
        t.list.push_back(target);
        return t;
    };
}

PrinterStoragePanel::Source PrinterStoragePanel::snapmaker_source()
{
    return []() {
        Targets out;
        std::vector<SnapmakerLan::Device> list;
        try {
            SnapmakerLan::merge_app_devices();
            list = SnapmakerLan::devices();
        } catch (...) {}
        // The printer the Device page is connected to, when that connection is a LAN one.
        SnapmakerLan::Device       connected;
        bool                       has_connected = false;
        std::shared_ptr<PrintHost> host;
        wxGetApp().get_connect_host(host);
        if (host)
            has_connected = SnapmakerLan::match_host(list, host->get_host(), connected);
        for (const SnapmakerLan::Device& d : list) {
            if (!SnapmakerLan::is_lan_host(SnapmakerLan::host_of(d.ip)))
                continue;
            const std::string name = d.name.empty() ? d.ip : d.name;
            pstore::Target            t    = pstore::target_for_snapmaker(d.ip, d.port, name, "sm:" + d.id);
            SnapmakerLan::Status st;
            if (SnapmakerLan::cached_status(d, st) && st.login_required)
                t = pstore::unsupported_target(name, "This printer asks for a login on its local network API, so its files cannot be read from here.");
            if (t.kind == pstore::Target::Kind::None)
                continue;
            if (has_connected && d.id == connected.id)
                out.select = out.list.size();
            out.list.push_back(t);
        }
        if (out.list.empty())
            out.list.push_back(pstore::unsupported_target(
                "Snapmaker", "No Snapmaker printer on the local network is known yet. Connect to it on the printer page over the "
                             "LAN (a printer reached only through the Snapmaker cloud has no file access from here), then refresh."));
        return out;
    };
}

// ---- construction ------------------------------------------------------------------------------

PrinterStoragePanel::PrinterStoragePanel(wxWindow* parent)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL)
    , m_executor(std::make_shared<WxFileGridExecutor>())
{
    SetName("Printer storage");
    SetBackgroundColour(*wxWHITE);
    const int gap = FromDIP(8);
    auto*     top = new wxBoxSizer(wxVERTICAL);

    // A printer picker, only when there is more than one (the Snapmaker LAN list).
    m_printer_row   = new wxPanel(this);
    auto* prow      = new wxBoxSizer(wxHORIZONTAL);
    prow->Add(new wxStaticText(m_printer_row, wxID_ANY, _L("Printer") + ":"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, gap);
    m_printer_choice = new wxChoice(m_printer_row, wxID_ANY);
    m_printer_choice->SetName(_L("Printer"));
    prow->Add(m_printer_choice, 0, wxALIGN_CENTER_VERTICAL);
    m_printer_row->SetSizer(prow);
    m_printer_row->Hide();
    m_printer_choice->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
        const int sel = m_printer_choice->GetSelection();
        if (sel >= 0 && sel < (int) m_targets.size())
            use_target(m_targets[sel]);
    });
    top->Add(m_printer_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, gap);

    // Title, sort, refresh.
    auto* header = new wxBoxSizer(wxHORIZONTAL);
    m_title      = new wxStaticText(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
    m_title->SetFont(Label::Head_14);
    header->Add(m_title, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, gap);
    header->Add(new wxStaticText(this, wxID_ANY, _L("Sort by") + ":"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
    m_sort = new wxChoice(this, wxID_ANY);
    m_sort->Append(_L("Newest first"));
    m_sort->Append(_L("Oldest first"));
    m_sort->Append(_L("Name"));
    m_sort->Append(_L("Size"));
    m_sort->SetSelection(0);
    m_sort->SetName(_L("Sort by"));
    m_sort->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
        if (m_model)
            m_model->SetSort((FileGridSort) std::max(0, m_sort->GetSelection()));
    });
    header->Add(m_sort, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, gap);
    m_refresh = new ::Button(this, _L("Refresh"));
    m_refresh->SetStyle(ButtonStyle::Regular, ButtonType::Window);
    m_refresh->SetToolTip(_L("List the printer's files again (F5)"));
    m_refresh->SetName(_L("Refresh"));
    m_refresh->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { refresh(); });
    header->Add(m_refresh, 0, wxALIGN_CENTER_VERTICAL);
    top->Add(header, 0, wxEXPAND | wxALL, gap);

    // Loading / empty / failed / not supported.
    m_message = new wxStaticText(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxALIGN_CENTRE_HORIZONTAL);
    top->Add(m_message, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, gap);

    // The files, and the selected one's details beside them.
    auto* body = new wxBoxSizer(wxHORIZONTAL);
    m_list     = new wxDataViewListCtrl(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxDV_SINGLE | wxDV_ROW_LINES);
    m_list->SetName(_L("Files on the printer"));
    const int em = GetTextExtent("m").x;
    // The list store needs the column's value type, which only AppendColumn tells it.
    m_list->AppendColumn(new wxDataViewColumn(wxEmptyString, new wxDataViewBitmapRenderer(), scPicture, FromDIP(56), wxALIGN_CENTER, 0),
                         "wxBitmap");
    m_list->AppendTextColumn(_L("Name"), wxDATAVIEW_CELL_INERT, 26 * em, wxALIGN_LEFT, wxDATAVIEW_COL_RESIZABLE);
    m_list->AppendTextColumn(_L("Print time"), wxDATAVIEW_CELL_INERT, 9 * em, wxALIGN_RIGHT, wxDATAVIEW_COL_RESIZABLE);
    m_list->AppendTextColumn(_L("Filament"), wxDATAVIEW_CELL_INERT, 11 * em, wxALIGN_RIGHT, wxDATAVIEW_COL_RESIZABLE);
    m_list->AppendTextColumn(_L("Size"), wxDATAVIEW_CELL_INERT, 8 * em, wxALIGN_RIGHT, wxDATAVIEW_COL_RESIZABLE);
    m_list->AppendTextColumn(_L("Modified"), wxDATAVIEW_CELL_INERT, 12 * em, wxALIGN_LEFT, wxDATAVIEW_COL_RESIZABLE);
    m_list->SetRowHeight(FromDIP(52));
    m_list->Bind(wxEVT_DATAVIEW_SELECTION_CHANGED, [this](wxDataViewEvent&) {
        update_details();
        update_focus();
    });
    // Enter or a double click: print (with its confirmation).
    m_list->Bind(wxEVT_DATAVIEW_ITEM_ACTIVATED, [this](wxDataViewEvent&) { on_print(); });
    body->Add(m_list, 1, wxEXPAND | wxRIGHT, gap);

    m_details        = new wxPanel(this);
    auto* dsizer     = new wxBoxSizer(wxVERTICAL);
    m_preview        = new wxStaticBitmap(m_details, wxID_ANY, wxNullBitmap, wxDefaultPosition, wxSize(FromDIP(220), FromDIP(220)));
    m_detail_name    = new wxStaticText(m_details, wxID_ANY, wxEmptyString);
    m_detail_name->SetFont(Label::Head_13);
    m_detail_text    = new wxStaticText(m_details, wxID_ANY, wxEmptyString);
    m_detail_text->SetFont(Label::Body_13);
    m_print          = new ::Button(m_details, _L("Print"));
    m_print->SetStyle(ButtonStyle::Confirm, ButtonType::Choice);
    m_print->SetToolTip(_L("Start printing this file on the printer (Enter)"));
    m_print->SetName(_L("Print"));
    m_print->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { on_print(); });
    m_delete = new ::Button(m_details, _L("Delete"));
    m_delete->SetStyle(ButtonStyle::Alert, ButtonType::Choice);
    m_delete->SetToolTip(_L("Delete this file from the printer (Delete)"));
    m_delete->SetName(_L("Delete"));
    m_delete->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { on_delete(); });
    auto* buttons = new wxBoxSizer(wxHORIZONTAL);
    buttons->Add(m_print, 0, wxRIGHT, FromDIP(ButtonProps::ChoiceButtonGap()));
    buttons->Add(m_delete, 0);
    dsizer->Add(m_preview, 0, wxALIGN_CENTER_HORIZONTAL | wxBOTTOM, gap);
    dsizer->Add(m_detail_name, 0, wxEXPAND | wxBOTTOM, FromDIP(4));
    dsizer->Add(m_detail_text, 0, wxEXPAND | wxBOTTOM, gap);
    dsizer->Add(buttons, 0, wxALIGN_LEFT);
    m_details->SetSizer(dsizer);
    m_details->SetMinSize(wxSize(FromDIP(240), -1));
    body->Add(m_details, 0, wxEXPAND);
    top->Add(body, 1, wxEXPAND | wxLEFT | wxRIGHT, gap);

    // What the last action did.
    m_note = new wxStaticText(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
    top->Add(m_note, 0, wxEXPAND | wxALL, gap);

    SetSizer(top);

    // F5 anywhere in the tab; Delete while the list has the focus.
    Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& e) {
        if (e.GetKeyCode() == WXK_F5) {
            refresh();
            return;
        }
        if (e.GetKeyCode() == WXK_DELETE && !e.HasAnyModifiers() && m_list && FindFocus() &&
            (FindFocus() == m_list || m_list->IsDescendant(FindFocus()))) {
            on_delete();
            return;
        }
        e.Skip();
    });

    m_focus_timer.SetOwner(this);
    Bind(wxEVT_TIMER, [this](wxTimerEvent&) { update_focus(); }, m_focus_timer.GetId());

    wxGetApp().UpdateDarkUIWin(this);
    wxGetApp().UpdateDVCDarkUI(m_list);
    update_status();
    update_details();
}

PrinterStoragePanel::~PrinterStoragePanel()
{
    m_focus_timer.Stop();
    m_model.reset();
}

// ---- which printer -------------------------------------------------------------------------------

void PrinterStoragePanel::set_source(Source source)
{
    m_source = std::move(source);
    if (!m_source) {
        m_targets.clear();
        use_target(pstore::Target());
        return;
    }
    // A fixed printer is cheap to settle now; the Snapmaker list is read when the tab is opened.
    if (m_active)
        apply_targets(m_source());
}

void PrinterStoragePanel::apply_targets(const Targets& targets)
{
    m_targets = targets.list;
    m_printer_choice->Clear();
    for (const pstore::Target& t : m_targets)
        m_printer_choice->Append(from_u8(t.name.empty() ? t.base_url : t.name));
    // Stay on the printer picked here, if it is still listed.
    size_t pick = std::min(targets.select, m_targets.empty() ? size_t(0) : m_targets.size() - 1);
    for (size_t i = 0; i < m_targets.size(); ++i)
        if (!m_target.key.empty() && m_targets[i].key == m_target.key)
            pick = i;
    if (!m_targets.empty())
        m_printer_choice->SetSelection((int) pick);
    const bool many = m_targets.size() > 1;
    if (m_printer_row->IsShown() != many) {
        m_printer_row->Show(many);
        Layout();
    }
    use_target(m_targets.empty() ? pstore::Target() : m_targets[pick]);
}

void PrinterStoragePanel::use_target(const pstore::Target& target)
{
    if (m_model && target == m_target)
        return;
    m_target = target;
    m_images.clear();
    m_row_ids.clear();
    m_list->DeleteAllItems();
    m_model.reset(); // answers to the old model's requests are dropped with it
    m_loaded = false;
    m_busy   = false;
    if (target.kind != pstore::Target::Kind::None) {
        m_model = std::make_shared<RemoteFileGridModel>(pstore::make_files(target), m_executor, target.reason);
        m_model->SetSort((FileGridSort) std::max(0, m_sort->GetSelection()));
        m_model->SetChangeHandler([this](FileGridChange c, const std::string& id) { on_model_change(c, id); });
    }
    m_title->SetLabel(target.kind == pstore::Target::Kind::None ? wxString() :
                                                          wxString::Format(_L("Files on %s"), from_u8(target.name)));
    show_note(wxEmptyString);
    update_status();
    update_details();
    if (m_active && m_model)
        load();
}

void PrinterStoragePanel::load()
{
    if (!m_model)
        return;
    m_loaded    = true;
    m_loaded_ms = storage_now_ms();
    ++m_loads;
    m_model->Refresh();
}

void PrinterStoragePanel::activate()
{
    m_active = true;
    if (m_source)
        apply_targets(m_source());
    if (m_model && !m_busy && (!m_loaded || storage_now_ms() - m_loaded_ms > STORAGE_RELIST_AFTER_MS))
        load();
    m_focus_timer.Start(500);
}

void PrinterStoragePanel::deactivate()
{
    m_active = false;
    m_focus_timer.Stop();
}

void PrinterStoragePanel::refresh()
{
    if (m_busy)
        return;
    const unsigned loads = m_loads;
    if (m_source)
        apply_targets(m_source()); // another printer than before is listed at once
    show_note(wxEmptyString);
    if (m_model && m_loads == loads)
        load();
}

// ---- the list ------------------------------------------------------------------------------------

void PrinterStoragePanel::on_model_change(FileGridChange change, const std::string& id)
{
    switch (change) {
    case FileGridChange::Files: rebuild_rows(); break;
    case FileGridChange::Card: update_row(id); break;
    case FileGridChange::Status: update_status(); break;
    }
}

void PrinterStoragePanel::rebuild_rows()
{
    const std::string keep = selected_id();
    m_list->Freeze();
    m_list->DeleteAllItems();
    m_row_ids.clear();
    // Pictures of files that are gone are dropped with them.
    std::set<std::string> listed;
    if (m_model) {
        for (size_t i = 0; i < m_model->GetCount(); ++i) {
            const FileGridCard* c = m_model->GetCard(i);
            if (!c)
                continue;
            listed.insert(c->id);
            wxVector<wxVariant> row;
            wxVariant           pic;
            pic << row_bitmap(*c);
            row.push_back(pic);
            row.push_back(wxVariant(from_u8(c->id)));
            row.push_back(wxVariant(from_u8(c->print_time_text())));
            row.push_back(wxVariant(from_u8(c->filament_text())));
            row.push_back(wxVariant(from_u8(c->size_text())));
            row.push_back(wxVariant(storage_modified_text(c->time)));
            m_list->AppendItem(row);
            m_row_ids.push_back(c->id);
        }
    }
    for (auto it = m_images.begin(); it != m_images.end();)
        it = listed.count(it->first) ? std::next(it) : m_images.erase(it);
    const auto pos = std::find(m_row_ids.begin(), m_row_ids.end(), keep);
    // Keep the selection on the same file; with none, the first one, so its details and the
    // Print / Delete buttons are there without a click.
    if (pos != m_row_ids.end())
        m_list->SelectRow((unsigned) (pos - m_row_ids.begin()));
    else if (!m_row_ids.empty())
        m_list->SelectRow(0);
    m_list->Thaw();
    update_details();
    update_focus();
}

void PrinterStoragePanel::update_row(const std::string& id)
{
    const auto pos = std::find(m_row_ids.begin(), m_row_ids.end(), id);
    if (!m_model || pos == m_row_ids.end())
        return;
    const FileGridCard* c = m_model->FindCard(id);
    if (!c)
        return;
    const unsigned row = (unsigned) (pos - m_row_ids.begin());
    m_images.erase(id); // a new picture may have arrived
    wxVariant pic;
    pic << row_bitmap(*c);
    m_list->SetValue(pic, row, scPicture);
    m_list->SetTextValue(from_u8(c->print_time_text()), row, scTime);
    m_list->SetTextValue(from_u8(c->filament_text()), row, scFilament);
    if (selected_id() == id)
        update_details();
}

void PrinterStoragePanel::update_focus()
{
    if (!m_model || !m_active || m_row_ids.empty())
        return;
    // The rows on screen, where the platform can say which those are; otherwise the first page.
    int top = 0, count = 30;
    const wxDataViewItem top_item = m_list->GetTopItem();
    if (top_item.IsOk()) {
        const int r = m_list->ItemToRow(top_item);
        if (r >= 0)
            top = r;
    }
    const int per_page = m_list->GetCountPerPage();
    if (per_page > 0)
        count = per_page;
    top = std::max(0, top - STORAGE_FOCUS_MARGIN);
    m_model->SetFocusRange((size_t) top, (size_t) (count + 2 * STORAGE_FOCUS_MARGIN));
    const int sel = m_list->GetSelectedRow();
    if (sel != wxNOT_FOUND)
        m_model->SetFocusRange((size_t) sel, 1);
}

void PrinterStoragePanel::update_status()
{
    wxString         text;
    const bool       has_model = m_model != nullptr;
    const FileGridStatus st    = has_model ? m_model->GetStatus() : FileGridStatus::Idle;
    switch (st) {
    case FileGridStatus::Idle: text = has_model ? wxString() : _L("This printer has no storage the Storage tab can show."); break;
    case FileGridStatus::Loading: text = _L("Reading the printer's files..."); break;
    case FileGridStatus::Ready: text = wxString(); break;
    case FileGridStatus::Empty: text = _L("There are no G-code files on the printer."); break;
    case FileGridStatus::Failed:
        text = _L("The printer's files could not be listed.") + "\n" + from_u8(m_model->GetStatusMessage());
        break;
    case FileGridStatus::NotSupported:
        text = _L("Storage is not available for this printer.") + "\n" + from_u8(m_model->GetStatusMessage());
        break;
    }
    m_message->SetLabel(text);
    m_message->Wrap(std::max(FromDIP(300), GetClientSize().x - FromDIP(32)));
    m_message->Show(!text.empty());
    const bool list_view = st == FileGridStatus::Ready || (st == FileGridStatus::Loading && !m_row_ids.empty());
    m_list->Show(list_view);
    m_details->Show(list_view);
    m_sort->Enable(has_model && st != FileGridStatus::NotSupported);
    m_refresh->Enable(has_model && !m_busy);
    Layout();
}

std::string PrinterStoragePanel::selected_id() const
{
    const int row = m_list ? m_list->GetSelectedRow() : wxNOT_FOUND;
    if (row == wxNOT_FOUND || row < 0 || row >= (int) m_row_ids.size())
        return {};
    return m_row_ids[row];
}

// ---- pictures ------------------------------------------------------------------------------------

const wxImage* PrinterStoragePanel::image_for(const FileGridCard& card)
{
    auto it = m_images.find(card.id);
    if (it != m_images.end())
        return it->second.IsOk() ? &it->second : nullptr;
    wxImage img;
    if (card.thumbnail_state == ThumbnailState::Ready && card.thumbnail && !card.thumbnail->empty()) {
        wxLogNull               quiet; // a broken picture is just no picture
        wxMemoryInputStream     stream(card.thumbnail->data(), card.thumbnail->size());
        if (!img.LoadFile(stream, wxBITMAP_TYPE_ANY))
            img = wxImage();
    }
    if (card.thumbnail_state == ThumbnailState::Ready || card.thumbnail_state == ThumbnailState::Missing)
        it = m_images.emplace(card.id, img).first; // settled: remember, even when there is none
    else
        return nullptr;
    return it->second.IsOk() ? &it->second : nullptr;
}

wxColour PrinterStoragePanel::list_background() const
{
    return m_list ? m_list->GetBackgroundColour() : GetBackgroundColour();
}

wxBitmap PrinterStoragePanel::fitted(const wxImage* image, int side, const wxColour& background)
{
    wxBitmap   canvas(side, side);
    wxMemoryDC dc(canvas);
    dc.SetBackground(wxBrush(background));
    dc.Clear();
    if (image && image->IsOk() && image->GetWidth() > 0 && image->GetHeight() > 0) {
        const double scale = std::min((double) side / image->GetWidth(), (double) side / image->GetHeight());
        const int    w     = std::max(1, (int) (image->GetWidth() * scale));
        const int    h     = std::max(1, (int) (image->GetHeight() * scale));
        wxBitmap     bmp(image->Scale(w, h, wxIMAGE_QUALITY_HIGH));
        dc.DrawBitmap(bmp, (side - w) / 2, (side - h) / 2, true);
    }
    dc.SelectObject(wxNullBitmap);
    return canvas;
}

wxBitmap PrinterStoragePanel::row_bitmap(const FileGridCard& card)
{
    return fitted(image_for(card), FromDIP(44), list_background());
}

// ---- details -------------------------------------------------------------------------------------

void PrinterStoragePanel::update_details()
{
    const std::string   id   = selected_id();
    const FileGridCard* card = (m_model && !id.empty()) ? m_model->FindCard(id) : nullptr;
    m_preview->SetBitmap(fitted(card ? image_for(*card) : nullptr, FromDIP(220), m_details->GetBackgroundColour()));
    if (!card) {
        m_detail_name->SetLabel(m_row_ids.empty() ? wxString() : _L("Select a file to see its details."));
        m_detail_text->SetLabel(wxEmptyString);
        m_print->Enable(false);
        m_delete->Enable(false);
        m_details->Layout();
        return;
    }
    m_detail_name->SetLabel(from_u8(card->name));
    m_detail_name->Wrap(FromDIP(230));
    wxString t;
    auto     line = [&t](const wxString& label, const std::string& value) {
        if (!value.empty())
            t += label + ": " + from_u8(value) + "\n";
    };
    if (card->id != card->name)
        line(_L("Folder"), card->id.substr(0, card->id.size() - card->name.size() - 1));
    if (!card->details_loaded)
        t += _L("Reading the file's details...") + "\n";
    const pstore::FileMetadata& m = card->meta;
    line(_L("Print time"), card->print_time_text());
    line(_L("Filament"), card->filament_text());
    line(_L("Filament type"), storage_types_text(m.filament_type));
    if (m.layer_height > 0) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.2f mm", m.layer_height);
        line(_L("Layer height"), buf);
    }
    if (m.object_height > 0) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.1f mm", m.object_height);
        line(_L("Height"), buf);
    }
    if (m.nozzle_diameter > 0) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.2f mm", m.nozzle_diameter);
        line(_L("Nozzle"), buf);
    }
    line(_L("Size"), card->size_text());
    line(_L("Modified"), storage_modified_text(card->time).ToUTF8().data());
    line(_L("Sliced with"), m.slicer + (m.slicer_version.empty() ? std::string() : " " + m.slicer_version));
    if (!card->printable)
        t += _L("This file type cannot be started from here.") + "\n";
    if (t.EndsWith("\n"))
        t.RemoveLast();
    m_detail_text->SetLabel(t);
    m_detail_text->Wrap(FromDIP(230));
    m_print->Enable(!m_busy && card->printable && m_model->CanPrint());
    m_delete->Enable(!m_busy && m_model->CanDelete());
    m_details->Layout();
    Layout();
}

// ---- actions -------------------------------------------------------------------------------------

void PrinterStoragePanel::set_busy(bool busy)
{
    m_busy = busy;
    m_refresh->Enable(!busy && m_model);
    update_details();
}

void PrinterStoragePanel::show_note(const wxString& text)
{
    m_note->SetLabel(text);
    m_note->SetToolTip(text);
}

void PrinterStoragePanel::on_delete()
{
    const std::string id = selected_id();
    if (m_busy || !m_model || id.empty() || !m_model->CanDelete())
        return;
    const FileGridCard* card = m_model->FindCard(id);
    if (!card)
        return;
    MessageDialog confirm(this,
                          wxString::Format(_L("Delete \"%s\" from %s?\nThis cannot be undone."), from_u8(card->id), from_u8(m_target.name)),
                          _L("Delete file"), wxYES_NO | wxNO_DEFAULT | wxICON_WARNING);
    if (confirm.ShowModal() != wxID_YES)
        return;
    set_busy(true);
    show_note(wxString::Format(_L("Deleting %s..."), from_u8(id)));
    std::shared_ptr<RemoteFileGridModel> model = m_model;
    model->DeletePrinterFile(id, [this, id](const pstore::Result& r) {
        set_busy(false);
        if (r.ok()) {
            show_note(wxString::Format(_L("Deleted %s."), from_u8(id)));
            return;
        }
        if (r.status == pstore::Status::NotFound) {
            show_note(wxString::Format(_L("%s was already gone from the printer."), from_u8(id)));
            return;
        }
        show_note(wxEmptyString);
        MessageDialog(this, wxString::Format(_L("Could not delete \"%s\"."), from_u8(id)) + "\n\n" + from_u8(r.message), _L("Delete file"),
                      wxOK | wxICON_ERROR)
            .ShowModal();
    });
}

void PrinterStoragePanel::on_print()
{
    const std::string id = selected_id();
    if (m_busy || !m_model || id.empty() || !m_model->CanPrint())
        return;
    const FileGridCard* card = m_model->FindCard(id);
    if (!card || !card->printable)
        return;
    set_busy(true);
    show_note(_L("Asking the printer whether it is free..."));
    // First what the printer is doing: a print never goes over a running or paused job, and the
    // person decides with that in front of them. StartPrint asks again just before it starts.
    m_model->QueryPrinter([this, id](const pstore::Result& r, const std::string& state, const std::string& current) {
        set_busy(false);
        show_note(wxEmptyString);
        if (!r.ok()) {
            MessageDialog(this, _L("The printer could not be asked whether it is free.") + "\n\n" + from_u8(r.message), _L("Print"),
                          wxOK | wxICON_ERROR)
                .ShowModal();
            return;
        }
        if (pstore::is_busy_state(state)) {
            MessageDialog(this,
                          (state == "paused" ? _L("The printer has a paused print.") : _L("The printer is printing.")) +
                              (current.empty() ? wxString() : " (" + from_u8(current) + ")") + "\n" +
                              _L("Finish or cancel that job before starting another one."),
                          _L("Print"), wxOK | wxICON_INFORMATION)
                .ShowModal();
            return;
        }
        const FileGridCard* c = m_model ? m_model->FindCard(id) : nullptr;
        if (!c)
            return; // the list changed meanwhile
        wxString details;
        if (!c->print_time_text().empty())
            details += _L("Print time") + ": " + from_u8(c->print_time_text()) + "\n";
        if (!c->filament_text().empty())
            details += _L("Filament") + ": " + from_u8(c->filament_text()) +
                       (c->meta.filament_type.empty() ? wxString() : " (" + from_u8(storage_types_text(c->meta.filament_type)) + ")") + "\n";
        MessageDialog confirm(this,
                              wxString::Format(_L("Start printing \"%s\" on %s?"), from_u8(c->id), from_u8(m_target.name)) + "\n\n" +
                                  details +
                                  _L("The file prints exactly as it was sliced. Make sure the build plate is clear and the "
                                     "filament the file was sliced for is loaded."),
                              _L("Print"), wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION);
        if (confirm.ShowModal() != wxID_YES)
            return;
        set_busy(true);
        show_note(wxString::Format(_L("Starting %s..."), from_u8(id)));
        m_model->StartPrint(id, [this, id](const pstore::Result& res) {
            set_busy(false);
            if (res.ok()) {
                show_note(wxString::Format(_L("Print started: %s. Follow it on the printer page."), from_u8(id)));
                return;
            }
            show_note(wxEmptyString);
            MessageDialog(this, wxString::Format(_L("The print of \"%s\" did not start."), from_u8(id)) + "\n\n" + from_u8(res.message),
                          _L("Print"), wxOK | wxICON_ERROR)
                .ShowModal();
        });
    });
}

// ---- theme and scale -----------------------------------------------------------------------------

void PrinterStoragePanel::sys_color_changed()
{
    wxGetApp().UpdateDarkUIWin(this);
    wxGetApp().UpdateDVCDarkUI(m_list);
    m_refresh->Rescale();
    m_print->Rescale();
    m_delete->Rescale();
    // The row pictures sit on the list's background colour: draw them again.
    rebuild_rows();
    Refresh();
}

void PrinterStoragePanel::msw_rescale()
{
    m_refresh->Rescale();
    m_print->Rescale();
    m_delete->Rescale();
    m_list->SetRowHeight(FromDIP(52));
    rebuild_rows();
    Layout();
}

} // namespace GUI
} // namespace Slic3r
