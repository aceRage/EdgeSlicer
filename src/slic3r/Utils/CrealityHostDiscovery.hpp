#ifndef slic3r_CrealityHostDiscovery_hpp_
#define slic3r_CrealityHostDiscovery_hpp_

#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "cxmdns.h"

// Creality K-series LAN discovery.
//
// Port of OrcaSlicer #13752 (598df9ff0b, merged as fcfadf0e8c): K-series firmware announces each
// printer under a per-device service type "_Creality-<MAC-derived hex>._udp.local.", which no
// fixed-name Bonjour query can target, so a DNS-SD meta-query (cxmdns) finds them and a GET /info
// on each one names the model.
//
// EdgeSlicer differences: the model table is CrealityPrintModel's (the one the send path already
// uses, so K1-family and SPARKX i7 codes are named too), the probes run in parallel with short
// timeouts, a CFS probe (the same boxsInfo query the filament sync uses) says whether a CFS is
// attached, the whole scan is cancellable, and the network calls are injectable so the flow is
// unit-tested without a network.

namespace Slic3r {

// One discovered Creality host.
struct CrealityHost
{
    std::string ip;            // dotted-quad IPv4
    std::string service_name;  // raw service type, e.g. "_Creality-543324280CDB19._udp.local."
    std::string suffix;        // short device id from the service name, e.g. "DB19"
    std::string model_code;    // /info "model": "F008", "K1C", ... (empty if the probe failed)
    std::string model_name;    // "K2 Plus" (empty when the code is not in the model table)
    std::string printer_model; // the matching preset printer_model, "Creality K2 Plus"; empty = unknown
    std::string mac;           // /info "mac", if reported
    bool        info_ok = false;  // /info answered with JSON
    int         cfs_boxes = -1;   // CFS units attached: -1 unknown (not probed / no answer), 0 none
    std::string cfs_variant;      // "CFS", "CFS-C", ... when cfs_boxes > 0

    // "Creality K2 Plus (DB19)", "Creality (DB19)" or the IP.
    std::string label() const;
    // "Yes (CFS-C)", "2 x CFS", "No", "" when unknown.
    std::string cfs_text() const;
};

class CrealityHostDiscovery
{
public:
    struct Options
    {
        int  mdns_timeout_ms = 4000; // how long to listen for DNS-SD answers
        bool probe_info      = true; // GET /info on each host for the model
        bool probe_cfs       = true; // boxsInfo on CFS-capable models
        int  probe_timeout_s = 3;    // per probe
    };

    // The network, as functions, so tests can replace it.
    using DiscoverFn = std::function<std::vector<cxnet::machine_info>(int timeout_ms, const std::atomic<bool>* cancel)>;
    using InfoFn     = std::function<bool(const std::string& ip, int timeout_s, std::string& body)>;
    using BoxesFn    = std::function<std::string(const std::string& ip, int timeout_s)>;

    // The real scan: cxnet::syncDiscoveryService, CrealityPrint::fetch_info and
    // CrealityPrint::query_boxes_info. Blocks for about mdns_timeout_ms plus one probe round; call
    // it from a worker thread. With no printer on the network it returns after mdns_timeout_ms.
    static std::vector<CrealityHost> scan(const Options& opt, const std::atomic<bool>* cancel = nullptr);
    static std::vector<CrealityHost> scan_with(const Options& opt, const DiscoverFn& discover, const InfoFn& info,
                                               const BoxesFn& boxes, const std::atomic<bool>* cancel = nullptr);

    // ---- pure helpers (unit-tested) ----

    // One host per IP, in answer order; answers without an IP are dropped.
    static std::vector<CrealityHost> hosts_from_answers(const std::vector<cxnet::machine_info>& answers);
    // "_Creality-543324280CDB19._udp.local." -> "DB19" (last four characters of the device id).
    static std::string device_suffix(const std::string& service_name);
    // Fill model_code / model_name / printer_model / mac from an /info body. Never throws.
    static void apply_info(CrealityHost& host, const std::string& info_body);
    // Fill cfs_boxes / cfs_variant from a boxsInfo response; an unusable response leaves them unknown.
    static void apply_boxes(CrealityHost& host, const std::string& boxs_info_response);
    // The preset printer_model for an /info model code ("F008" -> "Creality K2 Plus"), "" when the
    // code is not in CrealityPrintModel's table. Never guesses from partial matches.
    static std::string printer_model_for(const std::string& info_model);

    struct PresetRef
    {
        std::string name;
        std::string printer_model;
        std::string nozzle;      // printer_variant, "0.4"
        bool        is_system  = true;
        bool        is_visible = true;
    };
    // The visible system printer preset for `printer_model`, preferring the 0.4 nozzle, then the
    // lowest name. "" when there is none (the model is not installed or not enabled).
    static std::string pick_printer_preset(const std::vector<PresetRef>& presets, const std::string& printer_model);

    // Whether the Creality LAN search belongs on a printer with this host type / printer_model.
    // Every other host keeps the dialogs it had.
    static bool applies_to(const std::string& host_type_key, const std::string& printer_model);
};

} // namespace Slic3r

#endif
