#ifndef slic3r_PrintHostSendDialog_hpp_
#define slic3r_PrintHostSendDialog_hpp_

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <boost/filesystem/path.hpp>

#include <wx/string.h>
#include <wx/event.h>
#include <wx/dialog.h>

#include "GUI_Utils.hpp"
#include "MsgDialog.hpp"
#include "../Utils/PrintHost.hpp"
#include "../Utils/PrintHostDeviceStatus.hpp"
#include "../Utils/ElegooCanvasMapping.hpp"
#include "../Utils/Flashforge.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/ProjectTask.hpp"
class wxButton;
class wxTextCtrl;
class wxComboBox;
class ComboBox;
class wxDataViewListCtrl;
class wxFlexGridSizer;
class wxStaticText;
class wxWrapSizer;
class wxBoxSizer;
class wxCheckBox;

namespace Slic3r {

namespace GUI {

// One filament of the plate being sent, as the mapping table shows it.
struct SendPlateFilament
{
    int         index { 0 };   // 0-based, the slicer's filament/extruder number
    std::string type;          // "PLA"
    std::string colour;        // "#RRGGBB" or "#RRGGBBAA"
    bool        used { false }; // this plate's slice result actually used it
    std::string alias;         // its filament preset's alias (or name), for remembered mappings
    int         nozzle_temp { 0 }; // its nozzle temperature, 0 = unknown
};

class PrintHostSendDialog : public GUI::MsgDialog
{
public:
    PrintHostSendDialog(const boost::filesystem::path &path, PrintHostPostUploadActions post_actions, const wxArrayString& groups, const wxArrayString& storage_paths, const wxArrayString& storage_names, bool switch_to_device_tab);
    virtual ~PrintHostSendDialog();
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

    // ---- "unload filaments when the print ends" (Snapmaker tool changers) ----
    //
    // Call before init() with the printer preset's unload_filaments_at_end for a Snapmaker tool
    // changer, and not at all for anything else: the checkbox exists only when offered. The preset
    // is the default, this dialog is the override, and the answer travels with the job.
    void offer_unload_at_end(bool preset_default);
    // What the checkbox says. False when it was never offered, and false for a plain Upload: there
    // is no print to end.
    bool unload_at_end() const;

    const boost::filesystem::path origin_path() { return m_ori_file_path; }

    virtual void EndModal(int ret) override;
    virtual void init();
    // CrealityPrint: useCFS, enableSelfTest, colorMatch_n. Empty for every other host.
    virtual std::map<std::string, std::string> extendedInfo() const;

protected:
    // Builds the device row and the mapping table into content_sizer. Called from init() before
    // the file name, so the target is the first thing read and the first thing tabbed to.
    void build_device_ui();
    // Asks the picked device what is loaded and rebuilds the mapping table. The Creality
    // (and other) probe runs off the UI thread; apply_device_status() paints the result.
    void refresh_device_status();
    void apply_device_status();
    void rebuild_mapping_rows();
    // CrealityPrint options (Use CFS, Calibrate before printing), shown when the picked
    // device is a crealityprint host. Called with the mapping rows.
    void rebuild_creality_options();
    void update_creality_mapping_enabled();
    void set_send_enabled(bool on);
    // A dialog's own first choice for a row: a position in m_device_status.slots, or -1 to let the
    // closest-colour rule pick. The Elegoo dialog answers with the remembered mapping.
    virtual int preselect_slot(const SendPlateFilament& f) const { return -1; }
    // One or more lines under the mapping table, refreshed whenever a choice changes; "" = none.
    virtual wxString mapping_warning() const { return wxString(); }
    void update_mapping_note();
    // The rows' choices as positions in m_device_status.slots, -1 = "As sliced"; and the plate
    // filament each row is for.
    std::vector<int> mapping_choice_positions() const;
    const SendPlateFilament* plate_filament(int index) const;

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
    wxStaticText*                         m_mapping_note { nullptr };
    // CrealityPrint: Use CFS + Calibrate, null when the device is not a Creality host.
    wxWindow*                             m_creality_panel { nullptr };
    wxBoxSizer*                           m_creality_sizer { nullptr };
    wxCheckBox*                           m_creality_use_cfs { nullptr };
    wxCheckBox*                           m_creality_calibrate { nullptr };
    // Lets a late probe result drop itself if the dialog closed or the user switched devices.
    // alive is read/written under mtx; the worker is joinable and joined on close.
    struct ProbeToken {
        std::mutex        mtx;
        int               gen { 0 };
        bool              alive { true };
    };
    std::shared_ptr<ProbeToken>           m_probe_token;
    std::thread                           m_probe_thread;
    bool                                  m_probe_pending { false };
    // The end-of-print unload, when this printer is a Snapmaker tool changer.
    bool                                  m_offer_unload { false };
    bool                                  m_unload_default { false };
    wxCheckBox*                           m_check_unload { nullptr };
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
    // The start command's options, plus the CANVAS slot map (ElegooStartPrint.hpp) when the dialog
    // read a CANVAS from the printer.
    virtual std::map<std::string, std::string> extendedInfo() const override;

protected:
    int      preselect_slot(const SendPlateFilament& f) const override;
    wxString mapping_warning() const override;

private:
    // The plate's used filaments in the shape the CANVAS mapping code takes.
    std::vector<ElegooCanvasMapping::UsedFilament> used_filaments() const;
    std::vector<ElegooStartPrint::SlotMapEntry>    slot_map() const;
    // On Upload and Print with a slot map: read the CANVAS again and stop if a mapped tray has gone.
    bool confirm_slot_map_still_valid();
    mutable bool                                                  m_memory_loaded { false };
    mutable std::map<std::string, ElegooCanvasMapping::Remembered> m_memory;

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

class FlashforgePrintHostSendDialog : public PrintHostSendDialog
{
public:
    FlashforgePrintHostSendDialog(const boost::filesystem::path&  path,
                                  PrintHostPostUploadActions      post_actions,
                                  const wxArrayString&            groups,
                                  const wxArrayString&            storage_paths,
                                  const wxArrayString&            storage_names,
                                  bool                            switch_to_device_tab,
                                  const Slic3r::Flashforge*       host,
                                  bool                            supports_material_station,
                                  std::vector<Slic3r::FlashforgeMaterialSlot> slots,
                                  const std::vector<FilamentInfo>& project_filaments);

    virtual void init() override;
    virtual void EndModal(int ret) override;
    virtual std::map<std::string, std::string> extendedInfo() const override;

private:
    struct MappingRow {
        int       tool_id {-1};
        wxWindow* card {nullptr};
    };

    void load_slots();
    bool ensure_slots_loaded(bool force_reload = false);
    void rebuild_mapping_rows();
    void auto_assign_mappings();
    void refresh_mapping_card(MappingRow& row);
    void sync_mapping_section_visibility();
    const Slic3r::FlashforgeMaterialSlot* find_slot_by_id(const std::string& slot_id_text) const;
    const FilamentInfo* find_filament_by_tool_id(int tool_id) const;
    bool slot_matches_filament(const Slic3r::FlashforgeMaterialSlot& slot, const FilamentInfo& filament) const;
    bool validate_before_close();
    std::string normalize_material(const std::string& material) const;
    wxColour to_wx_colour(const std::string& color) const;

private:
    const Slic3r::Flashforge*        m_host {nullptr};
    std::vector<FilamentInfo>        m_project_filaments;
    std::vector<Slic3r::FlashforgeMaterialSlot> m_slots;
    std::vector<MappingRow>          m_mapping_rows;
    wxBoxSizer*                      m_flashforge_options_sizer {nullptr};
    wxBoxSizer*                      m_mapping_section_sizer {nullptr};
    wxWrapSizer*                     m_mapping_wrap_sizer {nullptr};
    wxStaticText*                    m_status_text {nullptr};
    ::CheckBox*                      m_checkbox_leveling {nullptr};
    ::CheckBox*                      m_checkbox_timelapse {nullptr};
    ::CheckBox*                      m_checkbox_flow_calibration {nullptr};
    ::CheckBox*                      m_checkbox_first_layer_inspection {nullptr};
    ::CheckBox*                      m_checkbox_ifs {nullptr};
    bool                             m_leveling_before_print {true};
    bool                             m_time_lapse_video {false};
    bool                             m_flow_calibration {false};
    bool                             m_first_layer_inspection {false};
    bool                             m_use_material_station {false};
    bool                             m_supports_material_station {false};
    bool                             m_slots_loaded {false};

    const char* CONFIG_KEY_LEVELING  = "flashforge_leveling_before_print";
    const char* CONFIG_KEY_TIMELAPSE = "flashforge_timelapse_video";
    const char* CONFIG_KEY_FLOW_CALIBRATION      = "flashforge_flow_calibration";
    const char* CONFIG_KEY_FIRST_LAYER_INSPECTION = "flashforge_first_layer_inspection";
    const char* CONFIG_KEY_IFS       = "flashforge_use_material_station";
};

wxDECLARE_EVENT(EVT_PRINTHOST_PROGRESS, PrintHostQueueDialog::Event);
wxDECLARE_EVENT(EVT_PRINTHOST_ERROR, PrintHostQueueDialog::Event);
wxDECLARE_EVENT(EVT_PRINTHOST_CANCEL, PrintHostQueueDialog::Event);
wxDECLARE_EVENT(EVT_PRINTHOST_INFO, PrintHostQueueDialog::Event);
}}

#endif
