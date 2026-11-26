#ifndef slic3r_GUI_BirdsEyeCamera_hpp_
#define slic3r_GUI_BirdsEyeCamera_hpp_

// The BirdsEye camera of a Bambu printer, kept free of wx, DeviceManager and the network so the push
// parsing and the command JSON can be unit tested on their own
// (tests/slic3rutils/birdseye_camera_tests.cpp). Design: tests/design_birds_eye_camera.md.
//
// H2-era printers (H2D/H2S/H2C/X2D/H3C, maybe P2S) have a second live-view camera, the "BirdsEye"
// camera. In the printer's report it is the "deputy" camera, id 5; the normal chamber camera is the
// "main" camera, id 4. The printer has one live-view stream and itself picks which sensor feeds it:
// the push lists the cameras it can stream (`ipcam.support_liveview_cams`, e.g. [4,5]) and the one it
// streams now (`ipcam.current_liveview`), and a `change_liveview_cam_idx` camera command switches it.
// The stream URL never changes, so no player has to be told anything.
//
// The only test for "has a BirdsEye camera" is the push listing two or more live-view cameras - never
// a model list (decision D1). Camera ids other than 4 and 5 are accepted and labelled generically.

#include <nlohmann/json.hpp>

#include <algorithm>
#include <string>
#include <vector>

namespace Slic3r { namespace GUI { namespace BirdsEye {

constexpr int CAMERA_UNKNOWN = -1; // the push did not say which camera is streaming
constexpr int CAMERA_MAIN    = 4;  // "Live View", the chamber camera
constexpr int CAMERA_DEPUTY  = 5;  // "BirdsEye"

// EdgeSlicer's own peer name in the switch command (decision D3). Never "studio": EdgeSlicer does
// not impersonate Bambu Studio.
constexpr const char* PEER_NAME = "edgeslicer";

// What one push_status `ipcam` object says about the cameras. A `has_*` flag is false when the push
// did not carry that field (or carried it in a shape this code does not read), so the caller keeps
// what it had - except `current`, which, as in Bambu Studio, falls back to CAMERA_UNKNOWN whenever
// an `ipcam` object comes without `current_liveview`.
struct IpcamCameras
{
    int              current { CAMERA_UNKNOWN };
    bool             has_liveview { false };
    std::vector<int> liveview; // support_liveview_cams
    bool             has_record { false };
    std::vector<int> record;   // support_record_cams
    bool             has_enabled { false };
    std::vector<int> enabled;  // enable_record_cams
};

namespace detail {
// An int array: the integer entries, in order; anything else in it is skipped. False when `j` is not
// an array, which leaves the caller's list alone.
inline bool read_int_array(const nlohmann::json& j, std::vector<int>& out)
{
    if (!j.is_array()) return false;
    out.clear();
    for (const nlohmann::json& e : j)
        if (e.is_number_integer() || e.is_number_unsigned()) out.push_back(e.get<int>());
    return true;
}
} // namespace detail

// Reads the camera fields of one `ipcam` object. Never throws; a non-object yields "nothing said".
inline IpcamCameras parse_ipcam_cameras(const nlohmann::json& ipcam)
{
    IpcamCameras r;
    if (!ipcam.is_object()) return r;
    if (auto it = ipcam.find("current_liveview"); it != ipcam.end() && (it->is_number_integer() || it->is_number_unsigned()))
        r.current = it->get<int>();
    if (auto it = ipcam.find("support_liveview_cams"); it != ipcam.end())
        r.has_liveview = detail::read_int_array(*it, r.liveview);
    if (auto it = ipcam.find("support_record_cams"); it != ipcam.end())
        r.has_record = detail::read_int_array(*it, r.record);
    if (auto it = ipcam.find("enable_record_cams"); it != ipcam.end())
        r.has_enabled = detail::read_int_array(*it, r.enabled);
    return r;
}

// The swap button is shown only when the printer lists two or more live-view cameras (D1).
inline bool has_birdseye(const std::vector<int>& liveview_cams) { return liveview_cams.size() >= 2; }

// The camera a swap switches to: the one after `current` in the printer's list, wrapping round. When
// `current` is not in the list (unknown, or a camera the list does not name) it is the first entry, as
// in Bambu Studio. CAMERA_UNKNOWN for an empty list.
inline int next_camera(const std::vector<int>& liveview_cams, int current)
{
    if (liveview_cams.empty()) return CAMERA_UNKNOWN;
    auto it = std::find(liveview_cams.begin(), liveview_cams.end(), current);
    if (it == liveview_cams.end()) return liveview_cams.front();
    ++it;
    return it == liveview_cams.end() ? liveview_cams.front() : *it;
}

enum class CameraKind { Main, BirdsEye, Other };

inline CameraKind camera_kind(int id)
{
    if (id == CAMERA_MAIN) return CameraKind::Main;
    if (id == CAMERA_DEPUTY) return CameraKind::BirdsEye;
    return CameraKind::Other;
}

// The English name of a camera, for logs and tests: "Live View", "BirdsEye" or "Camera <id>". The GUI
// translates the first two itself.
inline std::string camera_label(int id)
{
    switch (camera_kind(id)) {
    case CameraKind::Main: return "Live View";
    case CameraKind::BirdsEye: return "BirdsEye";
    default: return "Camera " + std::to_string(id);
    }
}

// {"camera":{"command":"change_liveview_cam_idx","sequence_id":"<seq>","camera_id":<id>,
//            "peer_name":"edgeslicer","version":1}}
// The `camera` namespace needs Bambu's signature on a secured printer, so MachineObject::publish_json
// sends this only to a printer in LAN Only + Developer Mode (D6).
inline nlohmann::json switch_camera_command(int camera_id, const std::string& sequence_id, const std::string& peer_name = PEER_NAME)
{
    nlohmann::json j;
    j["camera"]["command"]     = "change_liveview_cam_idx";
    j["camera"]["sequence_id"] = sequence_id;
    j["camera"]["camera_id"]   = camera_id;
    if (!peer_name.empty()) j["camera"]["peer_name"] = peer_name;
    j["camera"]["version"] = 1;
    return j;
}

// {"camera":{"command":"ipcam_record_set","sequence_id":"<seq>","camera_id":<id>,"control":"enable"|"disable"}}
// Per-camera "record while printing" (Device page camera settings, D5). The same command without
// camera_id is the older single "Auto-record Monitoring" switch (MachineObject::command_ipcam_record).
inline nlohmann::json record_camera_command(int camera_id, bool on, const std::string& sequence_id)
{
    nlohmann::json j;
    j["camera"]["command"]     = "ipcam_record_set";
    j["camera"]["sequence_id"] = sequence_id;
    j["camera"]["camera_id"]   = camera_id;
    j["camera"]["control"]     = on ? "enable" : "disable";
    return j;
}

// The printer's enable_record_cams after a local toggle, before the printer confirms it: `camera_id`
// added (once) or removed. The order of the other entries is kept.
inline std::vector<int> with_recording(std::vector<int> enabled, int camera_id, bool on)
{
    auto it = std::find(enabled.begin(), enabled.end(), camera_id);
    if (on && it == enabled.end()) enabled.push_back(camera_id);
    if (!on && it != enabled.end()) enabled.erase(it);
    return enabled;
}

// Whether a push's enable_record_cams (or a reply's camera_id) is taken. For 3 s after a local toggle
// the printer may still report the old list, so it is ignored then, as Bambu Studio does. `last_toggle`
// and `now` are seconds (time(nullptr)); `last_toggle` 0 means "never toggled here".
constexpr long long RECORD_HOLD_SECONDS = 3;
inline bool take_pushed_record_list(long long last_toggle, long long now)
{
    return last_toggle == 0 || now - last_toggle > RECORD_HOLD_SECONDS;
}

// Which per-camera recording switches the camera settings show: the cameras the printer lists in
// support_record_cams that EdgeSlicer has a switch for (Live View 4, BirdsEye 5). When neither is
// listed, the older single switch is shown instead.
struct RecordSwitches
{
    bool main { false };
    bool birdseye { false };
    bool legacy() const { return !main && !birdseye; }
};

inline RecordSwitches record_switches(const std::vector<int>& support_record_cams)
{
    RecordSwitches r;
    for (int id : support_record_cams) {
        if (id == CAMERA_MAIN) r.main = true;
        if (id == CAMERA_DEPUTY) r.birdseye = true;
    }
    return r;
}

inline bool is_recording(const std::vector<int>& enable_record_cams, int camera_id)
{
    return std::find(enable_record_cams.begin(), enable_record_cams.end(), camera_id) != enable_record_cams.end();
}

}}} // namespace Slic3r::GUI::BirdsEye

#endif // slic3r_GUI_BirdsEyeCamera_hpp_
