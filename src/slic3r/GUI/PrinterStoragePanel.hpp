#ifndef slic3r_GUI_PrinterStoragePanel_hpp_
#define slic3r_GUI_PrinterStoragePanel_hpp_

// The Device page's Storage tab for printers reached over Moonraker (Klipper hosts, a Snapmaker U1
// on the LAN): the G-code files on the printer with their preview pictures and slicer details,
// delete with a confirmation, start a print with the usual checks, refresh. Adapted from
// OrcaSlicer PR #16156's StoragePanel / OrcaFilesPanel (merge 029bc7928f), drawn as a list (a
// wxDataViewListCtrl: keyboard, screen readers, light and dark themes and DPI come with it) on the
// shared file-grid model (Printer/IFileGridModel.h) rather than on Bambu's ImageGrid, which stays
// with the Bambu storage page.
//
// PrinterWebView hosts it beside the printer's own page and tells it which printer to show through
// a Source: the print host device the page is on, or the Snapmaker LAN list for a U1.

#include "Printer/RemoteFileGridModel.h"

#include <wx/bitmap.h>
#include <wx/colour.h>
#include <wx/image.h>
#include <wx/panel.h>
#include <wx/timer.h>

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

class wxChoice;
class wxDataViewListCtrl;
class wxDataViewEvent;
class wxStaticBitmap;
class wxStaticText;
class Button;

namespace Slic3r {
namespace GUI {

class PrinterStoragePanel : public wxPanel
{
public:
    // The printers this tab may show, and which one to start on. More than one shows a picker.
    struct Targets
    {
        std::vector<PrinterStorage::Target> list;
        size_t                              select { 0 };
    };
    // Asked whenever the tab is opened or refreshed, on the GUI thread; cheap.
    using Source = std::function<Targets()>;

    explicit PrinterStoragePanel(wxWindow* parent);
    ~PrinterStoragePanel() override;

    // The printer may have changed (another preset, another device picked). An empty source means
    // the page has no printer the tab could show.
    void set_source(Source source);
    // The tab was opened / left. Opening loads the list when it was never loaded or the printer
    // changed; while open, the rows on screen get their details.
    void activate();
    void deactivate();

    void sys_color_changed();
    void msw_rescale();

    // One fixed printer.
    static Source fixed_source(const PrinterStorage::Target& target);
    // The Snapmaker printers this PC knows on the LAN (SnapmakerLan), the one the Device page is
    // connected to first.
    static Source snapmaker_source();

private:
    void apply_targets(const Targets& targets);
    void use_target(const PrinterStorage::Target& target);
    void on_model_change(FileGridChange change, const std::string& id);
    void rebuild_rows();
    void update_row(const std::string& id);
    void update_status();
    void update_details();
    void update_focus();
    void refresh();
    void load(); // ask the printer for the list

    std::string selected_id() const;
    void        on_delete();
    void        on_print();
    void        set_busy(bool busy);
    void        show_note(const wxString& text);

    const wxImage* image_for(const FileGridCard& card);
    wxBitmap       row_bitmap(const FileGridCard& card);
    wxBitmap       fitted(const wxImage* image, int side, const wxColour& background);
    wxColour       list_background() const;

    Source                                m_source;
    std::vector<PrinterStorage::Target>   m_targets;
    PrinterStorage::Target                m_target;
    std::shared_ptr<FileGridExecutor>     m_executor;
    std::shared_ptr<RemoteFileGridModel>  m_model;
    bool                                  m_active { false };
    bool                                  m_loaded { false }; // the list was asked for this target
    long long                             m_loaded_ms { 0 };  // when (steady clock)
    unsigned                              m_loads { 0 };      // how many times the list was asked for
    bool                                  m_busy { false };   // a delete or print is under way

    std::map<std::string, wxImage> m_images; // decoded pictures by file id (cleared with the model)

    wxPanel*            m_printer_row { nullptr };
    wxChoice*           m_printer_choice { nullptr };
    wxStaticText*       m_title { nullptr };
    wxChoice*           m_sort { nullptr };
    ::Button*           m_refresh { nullptr };
    wxStaticText*       m_message { nullptr };
    wxDataViewListCtrl* m_list { nullptr };
    wxPanel*            m_details { nullptr };
    wxStaticBitmap*     m_preview { nullptr };
    wxStaticText*       m_detail_name { nullptr };
    wxStaticText*       m_detail_text { nullptr };
    ::Button*           m_print { nullptr };
    ::Button*           m_delete { nullptr };
    wxStaticText*       m_note { nullptr };
    wxTimer             m_focus_timer;
    std::vector<std::string> m_row_ids; // the list's rows, in order
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_PrinterStoragePanel_hpp_
