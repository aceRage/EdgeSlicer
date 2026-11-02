#ifndef slic3r_GUI_FileArchiveDialog_hpp_
#define slic3r_GUI_FileArchiveDialog_hpp_

#include "GUI_Utils.hpp"
#include "libslic3r/ZipOpen.hpp"
#include "libslic3r/miniz_extension.hpp"

#include <boost/filesystem/path.hpp>
#include <wx/wx.h>
#include <wx/dataview.h>
#include <slic3r/GUI/Widgets/Button.hpp>
#include "wxExtensions.hpp"

class CheckBox;

namespace Slic3r {
namespace GUI {

class ArchiveViewCtrl;

class ArchiveViewNode
{
public:
    ArchiveViewNode(const wxString& name) : m_name(name) {}

    std::vector<std::shared_ptr<ArchiveViewNode>>&  get_children()                                      { return m_children; }
    void                                            set_parent(std::shared_ptr<ArchiveViewNode> parent) { m_parent = parent; }
    // On Linux, get_parent cannot just return size of m_children. ItemAdded would than crash.
    std::shared_ptr<ArchiveViewNode>                get_parent() const                                  { return m_parent; }
    bool                                            is_container() const                                { return m_container; }
    void                                            set_container(bool is_container)                    { m_container = is_container; }
    wxString                                        get_name() const                                    { return m_name; }
    void                                            set_name(const wxString& name)                      { m_name = name; }
    bool                                            get_toggle() const                                  { return  m_toggle; }
    void                                            set_toggle(bool toggle)                             { m_toggle = toggle; }
    bool                                            get_is_folder() const                               { return m_folder; }
    void                                            set_is_folder(bool is_folder)                       { m_folder = is_folder; }
    void                                            set_fullpath(boost::filesystem::path path)          { m_fullpath = path; }
    boost::filesystem::path                         get_fullpath() const                                { return m_fullpath; }
    void                                            set_size(size_t size)                               { m_size = size; }
    size_t                                          get_size() const                                    { return m_size; }
    void                                            set_index(mz_uint index)                            { m_index = index; }
    mz_uint                                         get_index() const                                   { return m_index; }
    void                                            set_selectable(bool selectable)                     { m_selectable = selectable; }
    bool                                            is_selectable() const                               { return m_selectable; }
    void                                            set_encrypted(bool encrypted)                       { m_encrypted = encrypted; }
    bool                                            is_encrypted() const                                { return m_encrypted; }
    void                                            set_kind(ZipEntryKind kind)                         { m_kind = kind; }
    ZipEntryKind                                    get_kind() const                                    { return m_kind; }

private:
    wxString m_name;
    std::shared_ptr<ArchiveViewNode> m_parent { nullptr };
    std::vector<std::shared_ptr<ArchiveViewNode>> m_children;

    bool        m_toggle { false };
    bool        m_folder { false };
    boost::filesystem::path m_fullpath;
    bool        m_container { false };
    size_t      m_size { 0 };
    mz_uint     m_index { 0 };
    bool        m_selectable { false };
    bool        m_encrypted { false };
    ZipEntryKind m_kind { ZipEntryKind::Unsupported };
};

class ArchiveViewModel : public wxDataViewModel
{
public:
    ArchiveViewModel(wxWindow* parent);
    ~ArchiveViewModel();

    std::shared_ptr<ArchiveViewNode>  AddFile(std::shared_ptr<ArchiveViewNode> parent,const wxString& name, bool container);

    wxString        GetColumnType(unsigned int col) const override;
    unsigned int    GetColumnCount() const override { return 3; }

    void            Rescale();
    void            Delete(const wxDataViewItem& item);
    void            Clear();

    wxDataViewItem  GetParent(const wxDataViewItem& item) const override;
    unsigned int    GetChildren(const wxDataViewItem& parent, wxDataViewItemArray& array) const override;

    void            SetAssociatedControl(ArchiveViewCtrl* ctrl) { m_ctrl = ctrl; }

    void GetValue(wxVariant& variant, const wxDataViewItem& item, unsigned int col) const override;
    bool SetValue(const wxVariant& variant, const wxDataViewItem& item, unsigned int col) override;

    void untoggle_folders(const wxDataViewItem& item);

    bool IsEnabled(const wxDataViewItem& item, unsigned int col) const override;
    bool IsContainer(const wxDataViewItem& item) const override;
    // Is the container just a header or an item with all columns
    // In our case it is an item with all columns
    bool HasContainerColumns(const wxDataViewItem& WXUNUSED(item)) const override { return true; }

protected:
    wxWindow* m_parent { nullptr };
    ArchiveViewCtrl* m_ctrl { nullptr };
    std::vector<std::shared_ptr<ArchiveViewNode>> m_top_children;
};

class ArchiveViewCtrl : public wxDataViewCtrl
{
 public:
    ArchiveViewCtrl(wxWindow* parent, wxSize size);
    ~ArchiveViewCtrl();

     ArchiveViewModel* get_model() const {return m_model; }
protected:
    ArchiveViewModel* m_model;
};


class FileArchiveDialog : public DPIDialog
{
public:
    FileArchiveDialog(wxWindow* parent_window, const std::vector<ZipEntryInfo>& entries, std::vector<mz_uint>& selected_indices);

    void update_selected_size();

protected:
    void on_dpi_changed(const wxRect& suggested_rect) override;

    void on_open_button();
    void on_all_button();
    void on_none_button();
    void on_item_activated(wxDataViewEvent& evt);

    void populate_tree();
    wxBoxSizer* create_btn_sizer();

    std::vector<ZipEntryInfo> m_entries;
    std::vector<mz_uint>& m_selected_indices;
    ArchiveViewCtrl* m_avc { nullptr };
    std::vector<Button*> m_button_list;
    CheckBox* m_show_unsupported { nullptr };
    wxStaticText* m_size_label { nullptr };
    wxStaticText* m_encrypted_msg { nullptr };
};

} // namespace GUI
} // namespace Slic3r
#endif //  slic3r_GUI_FileArchiveDialog_hpp_
