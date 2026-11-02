#include "FileArchiveDialog.hpp"

#include "I18N.hpp"
#include "GUI_App.hpp"
#include "GUI.hpp"
#include "MainFrame.hpp"
#include "ExtraRenderers.hpp"
#include "MsgDialog.hpp"
#include "format.hpp"
#include "Widgets/CheckBox.hpp"
#include <algorithm>
#include <functional>
#include <boost/log/trivial.hpp>
#include <boost/nowide/convert.hpp>

namespace Slic3r {
namespace GUI {

#define BTN_SIZE                wxSize(FromDIP(58), FromDIP(24))
#define BTN_GAP                 FromDIP(20)

ArchiveViewModel::ArchiveViewModel(wxWindow* parent)
    :m_parent(parent)
{}
ArchiveViewModel::~ArchiveViewModel()
{}

std::shared_ptr<ArchiveViewNode> ArchiveViewModel::AddFile(std::shared_ptr<ArchiveViewNode> parent, const wxString& name,  bool container)
{
    std::shared_ptr<ArchiveViewNode> node = std::make_shared<ArchiveViewNode>(ArchiveViewNode(name));
    node->set_container(container);

    if (parent.get() != nullptr) {
        parent->get_children().push_back(node);
        node->set_parent(parent);
        parent->set_is_folder(true);
    } else {
        m_top_children.emplace_back(node);
    }

    wxDataViewItem child = wxDataViewItem((void*)node.get());
    wxDataViewItem parent_item= wxDataViewItem((void*)parent.get());
    ItemAdded(parent_item, child);

    if (parent)
        m_ctrl->Expand(parent_item);
    return node;
}

wxString ArchiveViewModel::GetColumnType(unsigned int col) const
{
    if (col == 0)
        return "bool";
    if (col == 1)
        return "DataViewBitmapText";
    return "string";
}

void ArchiveViewModel::Rescale()
{
    // There should be no pictures rendered
}

void  ArchiveViewModel::Delete(const wxDataViewItem& item)
{
    assert(item.IsOk());
    ArchiveViewNode* node = static_cast<ArchiveViewNode*>(item.GetID());
    assert(node->get_parent() != nullptr);
    for (std::shared_ptr<ArchiveViewNode> child : node->get_children())
    {
        Delete(wxDataViewItem((void*)child.get()));
    }
    delete [] node;
}
void  ArchiveViewModel::Clear()
{
    m_top_children.clear();
    Cleared();
}

wxDataViewItem ArchiveViewModel::GetParent(const wxDataViewItem& item) const
{
    assert(item.IsOk());
    ArchiveViewNode* node = static_cast<ArchiveViewNode*>(item.GetID());
    return wxDataViewItem((void*)node->get_parent().get());
}
unsigned int ArchiveViewModel::GetChildren(const wxDataViewItem& parent, wxDataViewItemArray& array) const
{
    if (!parent.IsOk()) {
        for (std::shared_ptr<ArchiveViewNode>child : m_top_children) {
            array.push_back(wxDataViewItem((void*)child.get()));
        }
        return m_top_children.size();
    }

    ArchiveViewNode* node = static_cast<ArchiveViewNode*>(parent.GetID());
    for (std::shared_ptr<ArchiveViewNode> child : node->get_children()) {
        array.push_back(wxDataViewItem((void*)child.get()));
    }
    return node->get_children().size();
}

void ArchiveViewModel::GetValue(wxVariant& variant, const wxDataViewItem& item, unsigned int col) const
{
    assert(item.IsOk());
    ArchiveViewNode* node = static_cast<ArchiveViewNode*>(item.GetID());
    if (col == 0) {
        variant = node->get_toggle();
    } else if (col == 1) {
        wxBitmap bmp;
        if (node->is_encrypted())
            bmp = create_scaled_bitmap("lock_normal", m_parent);
        variant << DataViewBitmapText(node->get_name(), bmp);
    } else {
        if (node->get_is_folder())
            variant = wxString();
        else
            variant = wxString::FromUTF8(format_zip_size(node->get_size()).c_str());
    }
}

void ArchiveViewModel::untoggle_folders(const wxDataViewItem& item)
{
    assert(item.IsOk());
    ArchiveViewNode* node = static_cast<ArchiveViewNode*>(item.GetID());
    node->set_toggle(false);
    if (node->get_parent().get() != nullptr)
        untoggle_folders(wxDataViewItem((void*)node->get_parent().get()));
}

bool ArchiveViewModel::SetValue(const wxVariant& variant, const wxDataViewItem& item, unsigned int col)
{
    assert(item.IsOk());
    ArchiveViewNode* node = static_cast<ArchiveViewNode*>(item.GetID());
    if (col == 0) {
        const bool on = variant.GetBool();
        if (node->get_is_folder() || node->is_selectable())
            node->set_toggle(on);
        else
            node->set_toggle(false);
        for (std::shared_ptr<ArchiveViewNode> child : node->get_children()) {
            SetValue(variant, wxDataViewItem((void*)child.get()), col);
        }
        if(!on && node->get_parent())
            untoggle_folders(wxDataViewItem((void*)node->get_parent().get()));
    } else if (col == 1) {
        DataViewBitmapText data;
        data << variant;
        node->set_name(data.GetText());
    }
    m_parent->Refresh();
    if (auto* dlg = dynamic_cast<FileArchiveDialog*>(m_parent))
        dlg->update_selected_size();
    return true;
}
bool ArchiveViewModel::IsEnabled(const wxDataViewItem& item, unsigned int col) const
{
    ArchiveViewNode* node = static_cast<ArchiveViewNode*>(item.GetID());
    if (node->get_is_folder())
        return true;
    if (!node->is_selectable())
        return false;
    return true;
}

bool ArchiveViewModel::IsContainer(const wxDataViewItem& item) const
{
    if(!item.IsOk())
        return true;
    ArchiveViewNode* node = static_cast<ArchiveViewNode*>(item.GetID());
    return node->is_container();
}

ArchiveViewCtrl::ArchiveViewCtrl(wxWindow* parent, wxSize size)
    : wxDataViewCtrl(parent, wxID_ANY, wxDefaultPosition, size, wxDV_VARIABLE_LINE_HEIGHT | wxDV_ROW_LINES
#ifdef _WIN32
        | wxBORDER_SIMPLE
#endif
    )
    //, m_em_unit(em_unit(parent))
{
    wxGetApp().UpdateDVCDarkUI(this);

    m_model = new ArchiveViewModel(parent);
    this->AssociateModel(m_model);
    m_model->SetAssociatedControl(this);
}

ArchiveViewCtrl::~ArchiveViewCtrl()
{
    if (m_model) {
        m_model->Clear();
        m_model->DecRef();
    }
}

FileArchiveDialog::FileArchiveDialog(wxWindow* parent_window, const std::vector<ZipEntryInfo>& entries, std::vector<mz_uint>& selected_indices)
    : DPIDialog(parent_window, wxID_ANY, _(L("Archive preview")), wxDefaultPosition,
        wxSize(45 * wxGetApp().em_unit(), 40 * wxGetApp().em_unit()),
        wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER | wxMAXIMIZE_BOX)
    , m_entries(entries)
    , m_selected_indices(selected_indices)
{
#ifdef _WIN32
    SetBackgroundColour(*wxWHITE);
    wxGetApp().UpdateDarkUI(this);
    wxGetApp().UpdateDlgDarkUI(this);
#else
    SetBackgroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW));
#endif

    int em = em_unit();

    wxBoxSizer* topSizer = new wxBoxSizer(wxVERTICAL);

    m_encrypted_msg = new wxStaticText(this, wxID_ANY,
        _L("This ZIP is password-protected. Edge can't open encrypted archives; extract it with your system tool first."));
    m_encrypted_msg->Wrap(40 * em);
    std::vector<ZipListedEntry> listed;
    listed.reserve(m_entries.size());
    for (const ZipEntryInfo& e : m_entries)
        listed.push_back({e.index, e.decoded_name, e.is_directory, e.is_encrypted, e.is_symlink});
    if (!zip_archive_is_all_encrypted(listed))
        m_encrypted_msg->Hide();
    topSizer->Add(m_encrypted_msg, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);

    m_avc = new ArchiveViewCtrl(this, wxSize(45 * em, 30 * em));
    wxDataViewColumn*  toggle_column = m_avc->AppendToggleColumn(L"\u2714", 0, wxDATAVIEW_CELL_ACTIVATABLE, 6 * em);
    m_avc->AppendColumn(new wxDataViewColumn("filename", new BitmapTextRenderer(false, wxDATAVIEW_CELL_INERT), 1, 24 * em, wxALIGN_LEFT, wxDATAVIEW_COL_RESIZABLE));
    m_avc->AppendTextColumn(_L("size"), 2, wxDATAVIEW_CELL_INERT, 8 * em);

    populate_tree();

    size_t selectable = 0;
    for (const ZipEntryInfo& e : m_entries) {
        const ZipEntryKind kind = classify_zip_entry_name(e.decoded_name, e.is_directory);
        if (zip_entry_is_selectable(kind, e.is_encrypted, e.is_symlink))
            ++selectable;
    }
    if (selectable == 1)
        on_all_button();

    toggle_column->SetWidth(6 * em);

    m_avc->Bind(wxEVT_DATAVIEW_ITEM_ACTIVATED, &FileArchiveDialog::on_item_activated, this);

    auto* unsupported_row = new wxBoxSizer(wxHORIZONTAL);
    m_show_unsupported = new CheckBox(this);
    m_show_unsupported->SetValue(false);
    auto* unsupported_lbl = new wxStaticText(this, wxID_ANY, _L("Show unsupported files"));
    unsupported_row->Add(m_show_unsupported, 0, wxALIGN_CENTER_VERTICAL);
    unsupported_row->Add(unsupported_lbl, 0, wxLEFT | wxALIGN_CENTER_VERTICAL, FromDIP(6));
    m_show_unsupported->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent&) {
        m_avc->get_model()->Clear();
        populate_tree();
        update_selected_size();
        Layout();
    });

    m_size_label = new wxStaticText(this, wxID_ANY, wxEmptyString);
    update_selected_size();

    wxBoxSizer* btn_sizer = create_btn_sizer();

    topSizer->Add(m_avc, 1, wxEXPAND | wxALL, 10);
    topSizer->Add(unsupported_row, 0, wxLEFT | wxRIGHT, 10);
    topSizer->Add(m_size_label, 0, wxLEFT | wxRIGHT | wxTOP, 10);
    topSizer->Add(btn_sizer, 0, wxEXPAND | wxALL, 10);
    this->SetSizer(topSizer);
    SetMinSize(wxSize(40 * em, 30 * em));

    for (auto btn : m_button_list)
        wxGetApp().UpdateDarkUI(btn);
    wxGetApp().UpdateDarkUI(m_encrypted_msg);
    wxGetApp().UpdateDarkUI(unsupported_lbl);
    wxGetApp().UpdateDarkUI(m_size_label);
}

void FileArchiveDialog::populate_tree()
{
    const bool show_unsupported = m_show_unsupported && m_show_unsupported->GetValue();

    std::vector<std::shared_ptr<ArchiveViewNode>> stack;

    std::function<void(std::vector<std::shared_ptr<ArchiveViewNode> >&, size_t)> reduce_stack = [] (std::vector<std::shared_ptr<ArchiveViewNode>>& stack, size_t size) {
        if (size == 0) {
            stack.clear();
            return;
        }
        while (stack.size() > size)
            stack.pop_back();
    };
    std::function<size_t(const boost::filesystem::path&, std::vector<std::shared_ptr<ArchiveViewNode>>&)> adjust_stack = [&adjust_stack, &reduce_stack, &avc = m_avc](const boost::filesystem::path& const_file, std::vector<std::shared_ptr<ArchiveViewNode>>& stack)->size_t {
        boost::filesystem::path file(const_file);
        size_t struct_size = file.has_parent_path() ? adjust_stack(file.parent_path(), stack) : 0;

        if (stack.size() > struct_size && (file.has_extension() || file.filename().string() != stack[struct_size]->get_name()))
        {
            reduce_stack(stack, struct_size);
        }
        if (!file.has_extension() && stack.size() == struct_size)
            stack.push_back(avc->get_model()->AddFile((stack.empty() ? std::shared_ptr<ArchiveViewNode>(nullptr) : stack.back()), boost::nowide::widen(file.filename().string()), true));
        return struct_size + 1;
    };

    std::vector<const ZipEntryInfo*> filtered;
    filtered.reserve(m_entries.size());
    for (const ZipEntryInfo& e : m_entries) {
        const ZipEntryKind kind = classify_zip_entry_name(e.decoded_name, e.is_directory);
        const bool hidden_by_default = kind == ZipEntryKind::Ignored || kind == ZipEntryKind::Unsupported;
        if (hidden_by_default && !show_unsupported)
            continue;
        if (e.decoded_name.empty())
            continue;
        filtered.push_back(&e);
    }
    std::sort(filtered.begin(), filtered.end(), [](const ZipEntryInfo* a, const ZipEntryInfo* b) {
        return a->decoded_name < b->decoded_name;
    });

    for (const ZipEntryInfo* e : filtered)
    {
        const boost::filesystem::path path(e->decoded_name);
        std::shared_ptr<ArchiveViewNode> parent(nullptr);

        adjust_stack(path, stack);
        if (!stack.empty())
            parent = stack.back();

        const ZipEntryKind kind = classify_zip_entry_name(e->decoded_name, e->is_directory);
        wxString label = boost::nowide::widen(path.filename().string());
        if (kind == ZipEntryKind::NestedZip)
            label += " (" + _L("nested archive") + ")";
        else if (kind == ZipEntryKind::Unsupported)
            label += " (" + _L("unsupported") + ")";
        else if (e->is_encrypted)
            label += " (" + _L("encrypted") + ")";

        std::shared_ptr<ArchiveViewNode> new_node = m_avc->get_model()->AddFile(parent, label, false);
        new_node->set_fullpath(path);
        new_node->set_size(static_cast<size_t>(e->uncomp_size));
        new_node->set_index(e->index);
        new_node->set_kind(kind);
        new_node->set_encrypted(e->is_encrypted);
        new_node->set_selectable(zip_entry_is_selectable(kind, e->is_encrypted, e->is_symlink));
        new_node->set_is_folder(e->is_directory);
    }
}

void FileArchiveDialog::update_selected_size()
{
    if (!m_size_label || !m_avc)
        return;
    std::uint64_t total = 0;
    std::function<void(ArchiveViewNode*)> walk = [&](ArchiveViewNode* node) {
        if (node == nullptr)
            return;
        if (node->get_children().empty()) {
            if (node->get_toggle() && node->is_selectable())
                total += node->get_size();
        } else {
            for (std::shared_ptr<ArchiveViewNode> child : node->get_children())
                walk(child.get());
        }
    };
    wxDataViewItemArray top_items;
    m_avc->get_model()->GetChildren(wxDataViewItem(nullptr), top_items);
    for (const auto& item : top_items)
        walk(static_cast<ArchiveViewNode*>(item.GetID()));
    m_size_label->SetLabel(wxString::Format(_L("Selected: %s"), wxString::FromUTF8(format_zip_size(total).c_str())));
}

void FileArchiveDialog::on_dpi_changed(const wxRect& suggested_rect)
{
    int em = em_unit();
    BOOST_LOG_TRIVIAL(error) << "on_dpi_changed";

    for (auto btn : m_button_list) {
        btn->SetMinSize(BTN_SIZE);
        btn->SetCornerRadius(FromDIP(12));
    }

    const wxSize& size = wxSize(45 * em, 40 * em);
    SetSize(size);

    Fit();
    Refresh();
}

void FileArchiveDialog::on_item_activated(wxDataViewEvent& evt)
{
    ArchiveViewNode* node = static_cast<ArchiveViewNode*>(evt.GetItem().GetID());
    if (node == nullptr || node->get_is_folder() || !node->is_selectable())
        return;
    on_none_button();
    node->set_toggle(true);
    m_avc->get_model()->ValueChanged(evt.GetItem(), 0);
    update_selected_size();
    on_open_button();
}

void FileArchiveDialog::on_open_button()
{
    m_selected_indices.clear();
    wxDataViewItemArray top_items;
    m_avc->get_model()->GetChildren(wxDataViewItem(nullptr), top_items);

    bool has_model = false;
    bool has_gcode = false;
    std::function<void(ArchiveViewNode*)> deep_fill = [&](ArchiveViewNode* node){
        if (node == nullptr)
            return;
        if (node->get_children().empty()) {
            if (node->get_toggle() && node->is_selectable()) {
                m_selected_indices.push_back(node->get_index());
                if (node->get_kind() == ZipEntryKind::Model)
                    has_model = true;
                if (node->get_kind() == ZipEntryKind::GCode)
                    has_gcode = true;
            }
        } else {
            for (std::shared_ptr<ArchiveViewNode> child : node->get_children())
                deep_fill(child.get());
        }
    };

    for (const auto& item : top_items)
        deep_fill(static_cast<ArchiveViewNode*>(item.GetID()));

    if (has_model && has_gcode) {
        m_selected_indices.clear();
        MessageDialog dlg(this, _L("G-code files cannot be loaded with models together!"), _L("G-code loading"), wxOK | wxICON_INFORMATION);
        dlg.ShowModal();
        return;
    }
    this->EndModal(wxID_OK);
}

void FileArchiveDialog::on_all_button()
{

    wxDataViewItemArray top_items;
    m_avc->get_model()->GetChildren(wxDataViewItem(nullptr), top_items);

    std::function<void(ArchiveViewNode*)> deep_fill = [&deep_fill](ArchiveViewNode* node) {
        if (node == nullptr)
            return;
        if (node->get_is_folder() || node->is_selectable())
            node->set_toggle(true);
        if (!node->get_children().empty()) {
            for (std::shared_ptr<ArchiveViewNode> child : node->get_children())
                deep_fill(child.get());
        }
    };

    for (const auto& item : top_items)
    {
        ArchiveViewNode* node = static_cast<ArchiveViewNode*>(item.GetID());
        deep_fill(node);
        m_avc->get_model()->ValueChanged(item, 0);
    }

    update_selected_size();
    Refresh();
}

void FileArchiveDialog::on_none_button()
{
    wxDataViewItemArray top_items;
    m_avc->get_model()->GetChildren(wxDataViewItem(nullptr), top_items);

    std::function<void(ArchiveViewNode*)> deep_fill = [&deep_fill](ArchiveViewNode* node) {
        if (node == nullptr)
            return;
        node->set_toggle(false);
        if (!node->get_children().empty()) {
            for (std::shared_ptr<ArchiveViewNode> child : node->get_children())
                deep_fill(child.get());
        }
    };

    for (const auto& item : top_items)
    {
        ArchiveViewNode* node = static_cast<ArchiveViewNode*>(item.GetID());
        deep_fill(node);
        m_avc->get_model()->ValueChanged(item, 0);
    }

    update_selected_size();
    this->Refresh();
}

//Orca: Apply buttons style
wxBoxSizer* FileArchiveDialog::create_btn_sizer()
{
    auto btn_sizer = new wxBoxSizer(wxHORIZONTAL);

    Button* all_btn = new Button(this, _L("All"));
    all_btn->SetStyle(ButtonStyle::Regular, ButtonType::Window);
    all_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent& evt) { on_all_button(); });
    btn_sizer->Add(all_btn, 0, wxALIGN_CENTER_VERTICAL);
    m_button_list.push_back(all_btn);

    Button* none_btn = new Button(this, _L("None"));
    none_btn->SetStyle(ButtonStyle::Regular, ButtonType::Window);
    none_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent& evt) { on_none_button(); });
    btn_sizer->Add(none_btn, 0, wxLEFT | wxALIGN_CENTER_VERTICAL, FromDIP(ButtonProps::WindowButtonGap()));
    m_button_list.push_back(none_btn);

    btn_sizer->AddStretchSpacer();

    Button* open_btn = new Button(this, _L("Open"));
    open_btn->SetStyle(ButtonStyle::Confirm, ButtonType::Choice);
    open_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent& evt) { on_open_button(); });
    open_btn->SetFocus();
    open_btn->SetId(wxID_OK);
    btn_sizer->Add(open_btn, 0, wxRIGHT | wxALIGN_CENTER_VERTICAL, FromDIP(ButtonProps::ChoiceButtonGap()));
    m_button_list.push_back(open_btn);

    Button* cancel_btn = new Button(this, _L("Cancel"));
    cancel_btn->SetStyle(ButtonStyle::Regular, ButtonType::Choice);
    cancel_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent& evt) { this->EndModal(wxID_CANCEL); });
    cancel_btn->SetId(wxID_CANCEL);
    btn_sizer->Add(cancel_btn, 0, wxALIGN_CENTER_VERTICAL);
    m_button_list.push_back(cancel_btn);

    return btn_sizer;
}

} // namespace GUI
} // namespace Slic3r
