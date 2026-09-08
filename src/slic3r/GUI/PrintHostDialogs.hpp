#ifndef slic3r_PrintHostSendDialog_hpp_
#define slic3r_PrintHostSendDialog_hpp_

#include <set>
#include <string>
#include <boost/filesystem/path.hpp>

#include <wx/string.h>
#include <wx/event.h>
#include <wx/dialog.h>

#include "GUI_Utils.hpp"
#include "MsgDialog.hpp"
#include "../Utils/PrintHost.hpp"
#include "../Utils/PrintHostDeviceStatus.hpp"
#include "libslic3r/PrintConfig.hpp"
class wxButton;
class wxTextCtrl;
class wxChoice;
class wxComboBox;
class wxDataViewListCtrl;
class wxFlexGridSizer;
class wxStaticText;

namespace Slic3r {

namespace GUI {

// One filament of the plate being sent, as the mapping table shows it.
struct SendPlateFilament
{
    int         index { 0 };   // 0-based, the slicer's filament/extruder number
    std::string type;          // "PLA"
    std::string colour;        // "#RRGGBB" or "#RRGGBBAA"
    bool        used { false }; // this plate's slice result actually used it
};

class PrintHostSendDialog : public GUI::MsgDialog
{
public:
    PrintHostSendDialog(const boost::filesystem::path &path, PrintHostPostUploadActions post_actions, const wxArrayString& groups, const wxArrayString& storage_paths, const wxArrayString& storage_names, bool switch_to_device_tab);
    virtual ~PrintHostSendDialog() {}
    boost::filesystem::path filename() const;
    PrintHostPostUploadAction post_action() const;
    std::string group() const;
    std::string storage() const;
    bool switch_to_device_tab() const {return m_switch_to_device_tab;}

    // ---- the printer's devices (<datadir>/hub/print_host_devices.json) ----
    //
    // Call before init(). With a non-empty list the dialog grows a device dropdown at the top and,
    // once a device is picked, asks that device what is loaded in it (PrintHostDevices::probe) and
    // offers a plate-filament -> slot table when it said anything. Most print hosts say nothing -
    // Elegoo Link has no filament data in its protocol at all - and then the dialog says so and the
    // plate goes out exactly as it was sliced.
    void set_devices(const std::string& model_key, const std::vector<PrintHostDevices::Device>& devices, const std::string& preselect_id);
    // The plate's filaments, for that table. Call before init(); empty = no table.
    void set_plate_filaments(std::vector<SendPlateFilament> filaments);
    // The device the user chose, "" when there were none (the preset's own address is the target).
    std::string device_id() const;
    // "0:1,1:2" - the plate's filament 0 goes in slot 1, filament 1 in slot 2. Empty when nothing
    // was mapped, which is every host that reports no slots.
    std::string filament_mapping() const;

    const boost::filesystem::path origin_path() { return m_ori_file_path; }

    virtual void EndModal(int ret) override;
    virtual void init();
    virtual std::map<std::string, std::string> extendedInfo() const { return {}; }

protected:
    // Builds the device row and the mapping table into content_sizer. Called from init() before
    // the file name, so the target is the first thing read and the first thing tabbed to.
    void build_device_ui();
    // Asks the picked device what is loaded and rebuilds the mapping table. Blocking, with a busy
    // cursor and a short timeout - the same shape as the devices dialog's Test button.
    void refresh_device_status();
    void rebuild_mapping_rows();

    wxTextCtrl *txt_filename;
    wxComboBox *combo_groups;
    wxComboBox* combo_storage;
    // The devices of this printer model, the one that is picked, and what it said.
    std::string                           m_model_key;
    std::vector<PrintHostDevices::Device>  m_devices;
    std::string                           m_preselect_id;
    int                                   m_device_sel { -1 };
    PrintHostDevices::Status              m_device_status;
    std::vector<SendPlateFilament>        m_plate_filaments;
    wxComboBox*                           m_combo_devices { nullptr };
    wxStaticText*                         m_device_note { nullptr };
    wxWindow*                             m_mapping_panel { nullptr };
    wxFlexGridSizer*                      m_mapping_sizer { nullptr };
    std::vector<wxChoice*>                m_mapping_choices;  // one per used plate filament
    std::vector<int>                      m_mapping_filament; // its filament index
    PrintHostPostUploadAction post_upload_action;
    wxString    m_valid_suffix;
    wxString    m_preselected_storage;
    wxArrayString m_paths;
    bool m_switch_to_device_tab;

    boost::filesystem::path m_path;
    PrintHostPostUploadActions m_post_actions;
    wxArrayString m_storage_names;
    boost::filesystem::path  m_ori_file_path;
};


class PrintHostQueueDialog : public DPIDialog
{
public:
    class Event : public wxEvent
    {
    public:
        size_t job_id;
        int progress = 0;    // in percent
        wxString tag;
        wxString status;

        Event(wxEventType eventType, int winid, size_t job_id);
        Event(wxEventType eventType, int winid, size_t job_id, int progress);
        Event(wxEventType eventType, int winid, size_t job_id, wxString error);
        Event(wxEventType eventType, int winid, size_t job_id, wxString tag, wxString status);

        virtual wxEvent *Clone() const;
    };


    PrintHostQueueDialog(wxWindow *parent);

    void append_job(const PrintHostJob &job);
    void get_active_jobs(std::vector<std::pair<std::string, std::string>>& ret);

    virtual bool Show(bool show = true) override
    {
        if(!show)
            save_user_data(UDT_SIZE | UDT_POSITION | UDT_COLS);
        return DPIDialog::Show(show);
    }
protected:
    void on_dpi_changed(const wxRect &suggested_rect) override;
    void on_sys_color_changed() override;

private:
    enum Column {
        COL_ID,
        COL_PROGRESS,
        COL_STATUS,
        COL_HOST,
        COL_SIZE,
        COL_FILENAME,
        COL_ERRORMSG
    };

    enum JobState {
        ST_NEW,
        ST_PROGRESS,
        ST_ERROR,
        ST_CANCELLING,
        ST_CANCELLED,
        ST_COMPLETED,
    };

    enum { HEIGHT = 60, WIDTH = 30, SPACING = 5 };

    enum UserDataType{
        UDT_SIZE = 1,
        UDT_POSITION = 2,
        UDT_COLS = 4
    };

    wxButton *btn_cancel;
    wxButton *btn_error;
    wxDataViewListCtrl *job_list;
    // Note: EventGuard prevents delivery of progress evts to a freed PrintHostQueueDialog
    EventGuard on_progress_evt;
    EventGuard on_error_evt;
    EventGuard on_cancel_evt;
    EventGuard on_info_evt;

    JobState get_state(int idx);
    void set_state(int idx, JobState);
    void on_list_select();
    void on_progress(Event&);
    void on_error(Event&);
    void on_cancel(Event&);
    void on_info(Event&);
    // This vector keep adress and filename of uploads. It is used when checking for running uploads during exit.
    std::vector<std::pair<std::string, std::string>> upload_names;
    void save_user_data(int);
    bool load_user_data(int, std::vector<int>&);
};

class ElegooPrintHostSendDialog : public PrintHostSendDialog
{
public:
    ElegooPrintHostSendDialog(const boost::filesystem::path& path,
                              PrintHostPostUploadActions     post_actions,
                              const wxArrayString&           groups,
                              const wxArrayString&           storage_paths,
                              const wxArrayString&           storage_names,
                              bool                           switch_to_device_tab);

    virtual void EndModal(int ret) override;
    int          timeLapse() const { return m_timeLapse; }
    int          heatedBedLeveling() const { return m_heatedBedLeveling; }
    BedType      bedType() const { return m_BedType; }

    virtual void                               init() override;
    virtual std::map<std::string, std::string> extendedInfo() const
    {
        return {{"bedType", std::to_string(static_cast<int>(m_BedType))},
                {"timeLapse", std::to_string(m_timeLapse)},
                {"heatedBedLeveling", std::to_string(m_heatedBedLeveling)}};
    }

private:
    BedType appBedType() const;
    void    refresh();

    const char* CONFIG_KEY_UPLOADANDPRINT    = "elegoolink_upload_and_print";
    const char* CONFIG_KEY_TIMELAPSE         = "elegoolink_timelapse";
    const char* CONFIG_KEY_HEATEDBEDLEVELING = "elegoolink_heated_bed_leveling";
    const char* CONFIG_KEY_BEDTYPE           = "elegoolink_bed_type";

private:
    wxStaticText* warning_text{nullptr};
    wxBoxSizer*   uploadandprint_sizer{nullptr};

    int     m_timeLapse;
    int     m_heatedBedLeveling;
    BedType m_BedType;
};

wxDECLARE_EVENT(EVT_PRINTHOST_PROGRESS, PrintHostQueueDialog::Event);
wxDECLARE_EVENT(EVT_PRINTHOST_ERROR, PrintHostQueueDialog::Event);
wxDECLARE_EVENT(EVT_PRINTHOST_CANCEL, PrintHostQueueDialog::Event);
wxDECLARE_EVENT(EVT_PRINTHOST_INFO, PrintHostQueueDialog::Event);
}}

#endif
