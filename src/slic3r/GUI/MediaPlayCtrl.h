//
//  MediaPlayCtrl.h
//  libslic3r_gui
//
//  Created by cmguo on 2021/12/7.
//

#ifndef MediaPlayCtrl_h
#define MediaPlayCtrl_h

#include "wxMediaCtrl2.h"

#include <wx/panel.h>

#include <boost/thread.hpp>
#include <boost/thread/condition_variable.hpp>

#include <deque>
#include <functional>
#include <set>

class Button;
class Label;
class wxWebView;

namespace Slic3r {

class MachineObject;

namespace GUI {

class MediaPlayCtrl : public wxPanel
{
public:
    MediaPlayCtrl(wxWindow *parent, wxMediaCtrl2 *media_ctrl, const wxPoint &pos = wxDefaultPosition, const wxSize &size = wxDefaultSize);

    ~MediaPlayCtrl();

    void SetMachineObject(MachineObject * obj);

    bool IsStreaming() const;

    void ToggleStream();

    void msw_rescale();

    void jump_to_play();

    // Ultra: public stop/resume so the print-send flow can quiesce the DirectShow
    // video graph before a modal covers (but doesn't hide) the panel, which otherwise
    // AV-crashes wmp.dll on a stale hwnd. pause=true -> Stop(), false -> Play().
    void ultra_pause(bool pause);

    // "New Bambu LiveView" (liveview_go2rtc, on by default): play Bambu LAN cameras through the
    // hub's go2rtc in a web view instead of through Bambu's player. `make_view` creates the web view
    // on first use (null if it cannot); `show_own(true)` puts it in the player's slot,
    // `show_own(false)` gives the slot back. The setting is read at every Play, so turning it off
    // (or on) needs no restart; off, or a printer the own view cannot play (no LAN address/code, no
    // LAN stream), takes the existing Bambu-player path unchanged.
    void SetOwnView(std::function<wxWebView *()> make_view, std::function<void(bool own)> show_own);
    bool OwnViewBusy() const { return m_own_busy; }

protected:
    void onStateChanged(wxMediaEvent & event);

    void Play();

    void Stop(wxString const &msg = {}, wxString const &msg2 = {});

    void TogglePlay();

    void SetStatus(wxString const &msg, bool hyperlink = true);

private:
    void load();

    void on_show_hide(wxShowEvent & evt);

    void media_proc();

    static bool start_stream_service(bool *need_install = nullptr);

    static bool get_stream_url(std::string *url = nullptr);

    // The own live view (see SetOwnView). try_play_own() returns false when the Bambu-player path
    // should run instead (setting off, printer not playable this way, no web view).
    bool try_play_own();
    void own_ready(int seq, const std::string &machine, int port, int go2rtc_port, const std::string &secret,
                   int status, const std::string &body);
    void stop_own(wxString const &msg);

    std::function<wxWebView *()> m_make_own_view;
    wxWebView *               m_own_view = nullptr;
    std::function<void(bool)> m_show_own;
    bool                      m_own_busy = false; // starting or playing through go2rtc
    int                       m_own_seq  = 0;     // drops a late answer after Stop or a printer switch

private:
    static const wxMediaState MEDIASTATE_IDLE = (wxMediaState) 3;
    // inline const: 4-6 are outside wxMediaState's range, which clang 21 rejects in a constant expression
    static inline const wxMediaState MEDIASTATE_INITIALIZING = static_cast<wxMediaState>(4);
    static inline const wxMediaState MEDIASTATE_LOADING = static_cast<wxMediaState>(5);
    static inline const wxMediaState MEDIASTATE_BUFFERING = static_cast<wxMediaState>(6);

    wxMediaCtrl2 * m_media_ctrl;
    wxMediaState m_last_state = MEDIASTATE_IDLE;
    std::string m_machine;
    int m_lan_proto = 0;
    std::string m_lan_ip;
    std::string m_lan_user;
    std::string m_lan_passwd;
    std::string m_dev_ver;
    std::string m_tutk_state;
    bool m_camera_exists = false;
    bool m_lan_mode = false;
    int m_remote_proto = 0;
    bool m_device_busy = false;
    bool m_disable_lan = false;
    wxString m_url;
    
    std::deque<wxString> m_tasks;
    boost::mutex m_mutex;
    boost::condition_variable m_cond;
    boost::thread m_thread;
    bool m_exiting = false; // set under m_mutex by the destructor; the worker leaves at its next task

    bool m_streaming = false;
    bool m_user_triggered = false;
    int m_failed_retry = 0;
    int m_failed_code = 0;
    std::vector<double> m_stat;
    std::set<int> m_last_failed_codes;
    wxDateTime    m_last_user_play;
    wxDateTime    m_next_retry;

    ::Button *m_button_play;
    ::Label * m_label_stat;
    ::Label * m_label_status;
};

}}

#endif /* MediaPlayCtrl_h */
