#ifndef slic3r_GUI_AMXCONTROL_hpp_
#define slic3r_GUI_AMXCONTROL_hpp_

#include "../wxExtensions.hpp"
#include "StaticBox.hpp"
#include "StepCtrl.hpp"
#include "Button.hpp"
#include "AMSItem.hpp"
#include "../DeviceManager.hpp"
#include "slic3r/GUI/Event.hpp"
#include "slic3r/GUI/AmsMappingPopup.hpp"
#include "AMSDualView.hpp"
#include <wx/simplebook.h>
#include <wx/hyperlink.h>
#include <wx/animate.h>
#include <wx/dynarray.h>


namespace Slic3r { namespace GUI {

//Previous definitions
class uiAmsPercentHumidityDryPopup;
struct uiAmsHumidityInfo;
class AMSDryCtrlDialog;

class AMSControl : public wxSimplebook
{
public:
    AMSControl(wxWindow *parent, wxWindowID id = wxID_ANY, const wxPoint &pos = wxDefaultPosition, const wxSize &size = wxDefaultSize);

    ~AMSControl();

    void on_retry();
    void init_scaled_buttons();

protected:
    std::string  m_current_ams;
    std::string  m_current_show_ams;
    std::map<std::string, int> m_ams_selection;

    std::map<std::string, AMSPreview*> m_ams_preview_list;

    std::vector<AMSinfo>       m_ams_info;
    std::map<std::string, AmsItem*> m_ams_item_list;
    std::map<std::string, AmsItem*> m_ams_generic_item_list;
    std::map<std::string, AmsItem*> m_ams_extra_item_list;

    AMSextruder *m_extruder{nullptr};

    AmsIntroducePopup m_ams_introduce_popup;

    wxSimplebook *m_simplebook_calibration = {nullptr};
    wxSimplebook *m_simplebook_amsprvs    = {nullptr};
    wxSimplebook *m_simplebook_ams         = {nullptr};
    wxSimplebook* m_simplebook_generic_ams = {nullptr};
    wxSimplebook* m_simplebook_extra_ams   = {nullptr};

    wxSimplebook *m_simplebook_bottom      = {nullptr};

    wxStaticText *m_text_calibration_percent = {nullptr};
    wxWindow *    m_none_ams_panel           = {nullptr};
    wxWindow*     m_panel_prv                = {nullptr};
    wxWindow *    m_amswin                   = {nullptr}; 
    wxBoxSizer*   m_vams_sizer               = {nullptr};
    wxBoxSizer*   m_sizer_vams_tips          = {nullptr};

    Label*          m_ams_backup_tip = {nullptr};
    Label*          m_ams_tip       = {nullptr};

    Caninfo         m_vams_info;
    StaticBox*      m_panel_virtual = {nullptr};
    AMSLib*         m_vams_lib      = {nullptr};
    AMSRoad*        m_vams_road     = {nullptr};
    AMSVirtualRoad* m_vams_extra_road = {nullptr};

    StaticBox * m_panel_can       = {nullptr};
    wxBoxSizer* m_sizer_prv       = {nullptr};
    wxBoxSizer *m_sizer_cans      = {nullptr};
    wxBoxSizer* m_sizer_ams_tips  = {nullptr};

    Button *m_button_extruder_feed = {nullptr};
    Button *m_button_extruder_back = {nullptr};
    wxStaticBitmap* m_button_ams_setting   = {nullptr};
    wxStaticBitmap* m_img_ams_backup  = {nullptr};
    wxStaticBitmap* m_img_amsmapping_tip = {nullptr};
    wxStaticBitmap* m_img_vams_tip = {nullptr};
    ScalableBitmap m_button_ams_setting_normal;
    ScalableBitmap m_button_ams_setting_hover;
    ScalableBitmap m_button_ams_setting_press;
    wxWindow* m_button_area = {nullptr};

    wxHyperlinkCtrl *m_hyperlink = {nullptr};
    AmsHumidityTipPopup m_Humidity_tip_popup;
    uiAmsPercentHumidityDryPopup* m_percent_humidity_dry_popup;

    std::string m_last_ams_id;
    std::string m_last_tray_id;

    /* two-extruder layout (H2D, H2D Pro, H2C, X2D): page 2 of this book */
    AMSDualView*      m_dual_view{nullptr};
    bool              m_dual_mode{false};
    MachineObject*    m_obj{nullptr};
    AMSDryCtrlDialog* m_dry_dlg{nullptr};
    MachineObject*    m_dry_dlg_obj{nullptr};

    void show_humidity_popup(const uiAmsHumidityInfo& info);
    bool try_open_dry_dialog(const std::string& ams_id);

public:
    std::string GetCurentAms();
    std::string GetCurentShowAms();
    std::string GetCurrentCan(std::string amsid);
	wxColour GetCanColour(std::string amsid, std::string canid);

    AMSModel m_ams_model{AMSModel::EXT_AMS};
    AMSModel m_ext_model{AMSModel::EXT_AMS};
    AMSModel m_is_none_ams_mode{AMSModel::EXT_AMS};

    void SetAmsModel(AMSModel mode, AMSModel ext_mode) {m_ams_model = mode; m_ext_model = ext_mode;};
    void AmsSelectedSwitch(wxCommandEvent& event);

	void SetActionState(bool button_status[]);
    void EnterNoneAMSMode();
    void EnterGenericAMSMode();
    void EnterExtraAMSMode();

    void EnterCalibrationMode(bool read_to_calibration);
    void ExitcClibrationMode();

    void SetClibrationpercent(int percent);
    void SetClibrationLink(wxString link);

    void PlayRridLoading(wxString amsid, wxString canid);
    void StopRridLoading(wxString amsid, wxString canid);

    void SetFilamentStep(int item_idx, FilamentStepType f_type);
    void ShowFilamentTip(bool hasams = true);

    void UpdateStepCtrl(bool is_extrusion_exist);
    void CreateAms();
    void CreateAmsSingleNozzle();
    void ClearAms();
    void UpdateAms(std::vector<AMSinfo> info, bool is_reset = true);
    void AddAms(AMSinfo info);
    void AddAmsPreview(AMSinfo info);
    void AddExtraAms(AMSinfo info);
    void SetExtruder(bool on_off, bool is_vams, std::string ams_now, wxColour col);
    void SetAmsStep(std::string ams_id, std::string canid, AMSPassRoadType type, AMSPassRoadSTEP step);
    void SwitchAms(std::string ams_id);

    void msw_rescale();
    void on_filament_load(wxCommandEvent &event);
    void on_filament_unload(wxCommandEvent &event);
    void on_ams_setting_click(wxMouseEvent &event);
    void on_extrusion_cali(wxCommandEvent &event);
    void on_ams_setting_click(wxCommandEvent &event);
    void on_clibration_again_click(wxMouseEvent &event);
    void on_clibration_cancel_click(wxMouseEvent &event);
    void Reset();

    void show_noams_mode();
    void show_auto_refill(bool show);
    void show_vams(bool show);
    void show_vams_kn_value(bool show);
    void update_vams_kn_value(AmsTray tray, MachineObject* obj);

    void reset_vams();

    // The printer this control shows; the drying dialog and the dual layout read it.
    void SetMachine(MachineObject* obj) { m_obj = obj; }
    // Two-extruder Bambu printers get the per-extruder layout; everything else keeps the classic one.
    void SetDualMode(bool dual);
    bool IsDualMode() const { return m_dual_mode; }
    AMSDualView* GetDualView() const { return m_dual_view; }
    void UpdateDual(MachineObject* obj);
    // Keep an open AMS Dryness Control dialog in step with the printer's reports.
    void UpdateDryDialog(MachineObject* obj);
    void post_event(wxEvent&& event);

    virtual bool Enable(bool enable = true);

public:
    std::string m_current_select;
};

}} // namespace Slic3r::GUI

#endif // !slic3r_GUI_amscontrol_hpp_
