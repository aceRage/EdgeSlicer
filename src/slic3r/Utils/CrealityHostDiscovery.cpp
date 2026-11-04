#include "CrealityHostDiscovery.hpp"

#include "CrealityPrint.hpp"
#include "CrealityPrintModel.hpp"

#include <boost/log/trivial.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <future>

namespace Slic3r {

std::string CrealityHost::label() const
{
    std::string name = model_name.empty() ? std::string("Creality") : "Creality " + model_name;
    if (!suffix.empty())
        return name + " (" + suffix + ")";
    return model_name.empty() ? ip : name;
}

std::string CrealityHost::cfs_text() const
{
    if (cfs_boxes < 0)
        return {};
    if (cfs_boxes == 0)
        return "No";
    const std::string v = cfs_variant.empty() ? std::string("CFS") : cfs_variant;
    return cfs_boxes == 1 ? "Yes (" + v + ")" : std::to_string(cfs_boxes) + " x " + v;
}

std::string CrealityHostDiscovery::device_suffix(const std::string& service_name)
{
    const size_t dash = service_name.find_last_of('-');
    if (dash == std::string::npos)
        return {};
    const size_t dot = service_name.find('.', dash);
    std::string  id  = service_name.substr(dash + 1, dot == std::string::npos ? std::string::npos : dot - dash - 1);
    if (id.size() > 4)
        id = id.substr(id.size() - 4);
    return id;
}

std::vector<CrealityHost> CrealityHostDiscovery::hosts_from_answers(const std::vector<cxnet::machine_info>& answers)
{
    std::vector<CrealityHost> hosts;
    for (const cxnet::machine_info& m : answers) {
        if (m.machineIp.empty())
            continue;
        // One printer answers on every interface and to every re-sent query.
        if (std::any_of(hosts.begin(), hosts.end(), [&m](const CrealityHost& h) { return h.ip == m.machineIp; }))
            continue;
        CrealityHost h;
        h.ip           = m.machineIp;
        h.service_name = m.answer;
        h.suffix       = device_suffix(m.answer);
        hosts.push_back(std::move(h));
    }
    return hosts;
}

std::string CrealityHostDiscovery::printer_model_for(const std::string& info_model)
{
    const std::string name = CrealityPrintModel::model_display_name(info_model);
    return name.empty() ? std::string() : "Creality " + name;
}

void CrealityHostDiscovery::apply_info(CrealityHost& host, const std::string& info_body)
{
    try {
        const nlohmann::json j = nlohmann::json::parse(info_body);
        if (!j.is_object())
            return;
        host.info_ok = true;
        if (j.contains("model") && j["model"].is_string())
            host.model_code = j["model"].get<std::string>();
        if (j.contains("mac") && j["mac"].is_string())
            host.mac = j["mac"].get<std::string>();
    } catch (...) {
        return;
    }
    host.model_name    = CrealityPrintModel::model_display_name(host.model_code);
    host.printer_model = printer_model_for(host.model_code);
}

void CrealityHostDiscovery::apply_boxes(CrealityHost& host, const std::string& boxs_info_response)
{
    const CrealityPrintModel::CfsParseResult r = CrealityPrintModel::parse_cfs_response(boxs_info_response);
    if (!r.ok)
        return;
    host.cfs_boxes   = r.box_count;
    host.cfs_variant = r.box_count > 0 ? CrealityPrintModel::cfs_variant_name(r.variant) : std::string();
}

std::string CrealityHostDiscovery::pick_printer_preset(const std::vector<PresetRef>& presets, const std::string& printer_model)
{
    if (printer_model.empty())
        return {};
    const PresetRef* best = nullptr;
    auto             rank = [](const PresetRef& p) { return p.nozzle == "0.4" ? 0 : 1; };
    for (const PresetRef& p : presets) {
        if (!p.is_system || !p.is_visible || p.printer_model != printer_model)
            continue;
        if (best == nullptr || rank(p) < rank(*best) || (rank(p) == rank(*best) && p.name < best->name))
            best = &p;
    }
    return best ? best->name : std::string();
}

bool CrealityHostDiscovery::applies_to(const std::string& host_type_key, const std::string& printer_model)
{
    if (host_type_key == "crealityprint")
        return true;
    return CrealityPrintModel::preset_model_may_have_cfs(printer_model);
}

std::vector<CrealityHost> CrealityHostDiscovery::scan_with(const Options& opt, const DiscoverFn& discover, const InfoFn& info,
                                                           const BoxesFn& boxes, const std::atomic<bool>* cancel)
{
    auto cancelled = [cancel]() { return cancel != nullptr && cancel->load(); };
    if (cancelled() || !discover)
        return {};

    const std::vector<cxnet::machine_info> answers = discover(opt.mdns_timeout_ms, cancel);
    std::vector<CrealityHost>              hosts   = hosts_from_answers(answers);
    BOOST_LOG_TRIVIAL(info) << "CrealityHostDiscovery: " << answers.size() << " answer(s), " << hosts.size() << " host(s)";
    if (hosts.empty() || cancelled() || !opt.probe_info || !info)
        return hosts;

    // One probe round in parallel, so two printers cost no more than one.
    std::vector<std::future<void>> probes;
    probes.reserve(hosts.size());
    for (CrealityHost& h : hosts) {
        probes.push_back(std::async(std::launch::async, [&h, &opt, &info, &boxes, cancelled]() {
            std::string body;
            if (!info(h.ip, opt.probe_timeout_s, body))
                return;
            apply_info(h, body);
            if (opt.probe_cfs && boxes && !cancelled() && CrealityPrintModel::model_supports_multi_color(h.model_code))
                apply_boxes(h, boxes(h.ip, opt.probe_timeout_s));
        }));
    }
    for (std::future<void>& f : probes)
        f.wait();
    return hosts;
}

std::vector<CrealityHost> CrealityHostDiscovery::scan(const Options& opt, const std::atomic<bool>* cancel)
{
    static const std::vector<std::string> prefixes{"Creality", "creality"};
    return scan_with(
        opt,
        [](int timeout_ms, const std::atomic<bool>* c) { return cxnet::syncDiscoveryService(prefixes, timeout_ms, c); },
        [](const std::string& ip, int timeout_s, std::string& body) {
            std::string error;
            const bool  ok = CrealityPrint::fetch_info(ip, body, error, timeout_s);
            if (!ok)
                BOOST_LOG_TRIVIAL(info) << "CrealityHostDiscovery: /info failed: " << error;
            return ok;
        },
        [](const std::string& ip, int timeout_s) { return CrealityPrint::query_boxes_info(ip, timeout_s); }, cancel);
}

} // namespace Slic3r
