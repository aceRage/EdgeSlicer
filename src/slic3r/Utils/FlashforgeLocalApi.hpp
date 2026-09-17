#ifndef slic3r_FlashforgeLocalApi_hpp_
#define slic3r_FlashforgeLocalApi_hpp_

// The wx-free part of the Flashforge print host: the value types and the pure decisions of the
// port-8898 local API. Flashforge.hpp includes it; so does tests/slic3rutils/flashforge_tests.cpp,
// which cannot include Flashforge.hpp itself because that header carries wxString and the test
// executable is built without the wxWidgets include path.

#include <map>
#include <string>
#include <vector>

namespace Slic3r {

struct FlashforgeMaterialSlot
{
    int         slot_id {0}; // API is 1-based.
    bool        has_filament {false};
    std::string material_name;
    std::string material_color;
};

struct FlashforgeDiscoveredPrinter
{
    std::string name;
    std::string serial_number;
    std::string ip_address;
};

// The pure parts of the port-8898 local API: the pieces that only look at bytes the printer sent
// (or at a name the user typed) and decide what they mean. They live here rather than in an
// anonymous namespace inside Flashforge.cpp so tests can reach them without a printer, a socket or
// wx - see tests/slic3rutils/flashforge_tests.cpp.
namespace FlashforgeLocalApi {

// The AD5-series decision. The HTTP local API can only be spoken with a serial number AND a check
// code in hand, so a printer missing either stays on the legacy TCP-8899 M-code console. This is
// the single place that answer is made; test(), upload() and the send dialog all defer to it.
bool uses_local_api(const std::string& serial_number, const std::string& check_code);

// "http://1.2.3.4:8898/detail" out of whatever the user put in the Hostname field.
std::string host_name_of(const std::string& host);
std::string url_for(const std::string& host, const std::string& path);

// The printer rejects '=' and friends in an upload name; everything outside [A-Za-z0-9._-] becomes
// '_', and an empty name becomes "print" plus the fallback extension.
std::string sanitize_filename(const std::string& filename, const std::string& fallback_extension = {});

// The name a plate-sliced 3mf must carry on the way to the printer. The Creator 5 tells a sliced
// plate from a plain project 3mf by the double extension: given "<name>.3mf" it lists the file
// without a thumbnail and hangs its touchscreen when the file is opened, while "<name>.gcode.3mf"
// - what Flash Studio, upstream OrcaSlicer and this program's own File > Export plate sliced file
// all write - opens normally. Replaces whatever extension the name arrives with, and leaves a
// name that already ends in ".gcode.3mf" alone. See tests/slic3rutils/flashforge_tests.cpp.
std::string sliced_3mf_name(const std::string& filename);

// A /uploadGcode boolean header out of the send dialog's extended-info map. The dialog speaks the
// app_config dialect ("1"/"0"), the printer wants JSON-ish "true"/"false", and an option the dialog
// never set at all (an older config, or a print host that filled the map itself) must read as off.
// One place makes that conversion so levelingBeforePrint, flowCalibration, firstLayerInspection,
// timeLapseVideo and useMatlStation cannot drift apart - see tests/slic3rutils/flashforge_tests.cpp.
const char* upload_header_flag(const std::map<std::string, std::string>& extended_info, const std::string& key);

// `code`/`err` != 0 in a /detail or /uploadGcode reply is a printer-side refusal and the message it
// carries is what the user must see. Returns false and fills error_text then, and also on a body
// that is not a JSON object at all.
bool validate_response(const std::string& response_body, std::string& error_text);

// The IFS (material station) part of a /detail reply: how many slots, what is in them, and whether
// this printer reports a station at all. Returns false only on a body that will not parse.
bool parse_detail_material_slots(const std::string&                   response_body,
                                 std::vector<FlashforgeMaterialSlot>& slots,
                                 bool&                                supports_material_station);

} // namespace FlashforgeLocalApi

} // namespace Slic3r

#endif // slic3r_FlashforgeLocalApi_hpp_
