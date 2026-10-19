#ifndef slic3r_AnycubicPrintHost_hpp_
#define slic3r_AnycubicPrintHost_hpp_

#include <string>

namespace Slic3r {

// Network-free helpers for Anycubic print-host setup. Stock firmware speaks an OctoPrint-compatible
// API on TCP 80; Rinkhals (custom firmware) moved slicer uploads to Moonraker on TCP 7125
// (Rinkhals PR #68). Native LAN mode (SSDP / signed /ctrl / MQTT :9883) is Anycubic Slicer Next
// only — this fork does not implement it.
//
// #613 adds a native Moonraker host (config key "moonraker"). Call anycubic_apply_default_port()
// from that host's constructor when it merges; do not copy MoonrakerHost here.

bool is_anycubic_printer(const std::string &printer_model);

// 7125 for host_type "moonraker" (Rinkhals / Fluidd). 80 for "octoprint" (stock compat).
// 0 = no Anycubic-specific default (leave the address alone).
int anycubic_default_port(const std::string &host_type_key);

bool print_host_has_explicit_port(const std::string &address);
std::string print_host_with_port(const std::string &address, int port);

// Anycubic + moonraker with no port → append :7125. Anycubic + octoprint is left as-is so stock
// firmware keeps implicit port 80. Non-Anycubic addresses are never rewritten (a Voron on :80
// behind nginx must not pick up 7125).
std::string anycubic_apply_default_port(const std::string &address,
                                        const std::string &host_type_key,
                                        const std::string &printer_model);

// Stock Anycubic needs no API key; Moonraker's octoprint_compat usually doesn't either.
// An empty X-Api-Key header has been seen to confuse shims — only send when the key is set
// (same idea as #613's MoonrakerHost).
bool should_send_api_key(const std::string &apikey);

// Version JSON from stock octoprint-compat or Moonraker's [octoprint_compat], including a
// {"result":...} envelope. Hostile / HTML bodies return ok=false.
struct AnycubicOctoVersion {
    bool        ok { false };
    std::string api;
    std::string text;
    std::string error;
};
AnycubicOctoVersion parse_octoprint_compat_version(const std::string &body, bool anycubic);

// Rinkhals web portal on :80 (login page) instead of the slicer API.
bool looks_like_http_portal(unsigned status, const std::string &body);

// HTTP 2xx with no error envelope. HTML / 401 from the portal is a failure.
bool anycubic_upload_reply_ok(unsigned status, const std::string &body);

// OrcaSlicer #14349: stock Kobra hangs on "Checking files" after upload-only. Upload and print
// works. Research found no slicer-side root cause (firmware), so the send dialog prefers StartPrint.
bool anycubic_prefer_start_print(const std::string &printer_model);

// English, for tests and for the GUI to wrap with _L().
const char *anycubic_connection_hint();
const char *anycubic_upload_only_warning();
const char *anycubic_rinkhals_port_hint();

} // namespace Slic3r

#endif
