// Creality K-series LAN discovery (OrcaSlicer #13752 port): the DNS-SD reply parser in
// deps_src/mdns/cxmdns.cpp and the host / model / preset logic in slic3r/Utils/CrealityHostDiscovery.
//
// No network: replies are canned datagrams built below, and the scan runs with fake discover /
// probe functions. Addresses are from the documentation range 192.0.2.0/24.

#include <catch2/catch.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <boost/filesystem.hpp>

#include "cxmdns.h"
#include "slic3r/Utils/CrealityHostDiscovery.hpp"
#include "slic3r/Utils/CrealityPrintModel.hpp"

using Slic3r::CrealityHost;
using Slic3r::CrealityHostDiscovery;

namespace {

using Bytes = std::vector<uint8_t>;

void put16(Bytes& b, uint16_t v)
{
    b.push_back(uint8_t(v >> 8));
    b.push_back(uint8_t(v & 0xff));
}

void put32(Bytes& b, uint32_t v)
{
    put16(b, uint16_t(v >> 16));
    put16(b, uint16_t(v & 0xffff));
}

// "a.b.local." -> 1a 1b 5local 0
Bytes dns_name(const std::string& dotted)
{
    Bytes        out;
    std::string  label;
    std::istringstream in(dotted);
    while (std::getline(in, label, '.')) {
        if (label.empty())
            continue;
        out.push_back(uint8_t(label.size()));
        out.insert(out.end(), label.begin(), label.end());
    }
    out.push_back(0);
    return out;
}

const uint16_t TYPE_A = 1, TYPE_PTR = 12, TYPE_TXT = 16, TYPE_SRV = 33;

void record(Bytes& b, const Bytes& name, uint16_t type, const Bytes& rdata, uint16_t rclass = 0x8001 /* cache-flush, IN */)
{
    b.insert(b.end(), name.begin(), name.end());
    put16(b, type);
    put16(b, rclass);
    put32(b, 120);
    put16(b, uint16_t(rdata.size()));
    b.insert(b.end(), rdata.begin(), rdata.end());
}

const std::string SERVICES = "_services._dns-sd._udp.local.";
const std::string K2_TYPE  = "_Creality-543324280CDB19._udp.local.";
const std::string IP       = "192.0.2.41";

// A legacy-unicast reply to the meta-query: our question echoed, then the answers, then SRV / TXT
// / A records in the additional section (which the parser must step over, not read as answers).
Bytes reply(const std::vector<std::string>& ptr_targets, uint16_t query_id = 0, uint16_t flags = 0x8400, bool echo_question = true)
{
    Bytes b;
    put16(b, query_id);
    put16(b, flags);
    put16(b, echo_question ? 1 : 0);
    put16(b, uint16_t(ptr_targets.size()));
    put16(b, 0);
    put16(b, 3);
    if (echo_question) {
        const Bytes q = dns_name(SERVICES);
        b.insert(b.end(), q.begin(), q.end());
        put16(b, TYPE_PTR);
        put16(b, 0x8001); // QU, IN
    }
    for (const std::string& target : ptr_targets) {
        // The answer's owner name as a compression pointer to the question at offset 12, the way
        // responders write it, when there is a question to point at.
        Bytes owner = echo_question ? Bytes{ 0xC0, 0x0C } : dns_name(SERVICES);
        record(b, owner, TYPE_PTR, dns_name(target), 0x0001);
    }
    Bytes srv;
    put16(srv, 0);
    put16(srv, 0);
    put16(srv, 9999);
    const Bytes host = dns_name("K2-DB19.local.");
    srv.insert(srv.end(), host.begin(), host.end());
    record(b, dns_name("K2-DB19." + K2_TYPE), TYPE_SRV, srv);
    Bytes txt;
    const std::string kv = "model=F008";
    txt.push_back(uint8_t(kv.size()));
    txt.insert(txt.end(), kv.begin(), kv.end());
    record(b, dns_name("K2-DB19." + K2_TYPE), TYPE_TXT, txt);
    record(b, dns_name("K2-DB19.local."), TYPE_A, Bytes{ 192, 0, 2, 41 });
    return b;
}

std::vector<cxnet::machine_info> parse(const Bytes& b, const std::string& from = IP,
                                       const std::vector<std::string>& prefix = { "Creality", "creality" })
{
    std::vector<cxnet::machine_info> out;
    cxnet::parse_discovery_reply(b.data(), b.size(), from, prefix, out);
    return out;
}

const std::string K2PLUS_INFO = R"({"model":"F008","mac":"00:00:5e:00:53:19","version":"1.1.5.5"})";
const std::string K2_BOXS     = R"({"boxsInfo":{"materialBoxs":[
    {"id":0,"state":1,"type":1,"materials":[{"id":0,"state":1,"vendor":"Creality","type":"PLA","name":"PLA","color":"#FFFFFF"}]},
    {"id":1,"state":1,"type":0,"model":"MF050","materials":[{"id":0,"state":1,"vendor":"Creality","type":"PLA","name":"Hyper PLA","color":"#FF0000"}]}
]}})";

} // namespace

// ---- the DNS-SD reply parser ------------------------------------------------------------------

TEST_CASE("a K-series PTR answer to the meta-query is found among SRV, TXT and A records", "[CrealityDiscovery]")
{
    const auto out = parse(reply({ K2_TYPE }));
    REQUIRE(out.size() == 1);
    CHECK(out[0].machineIp == IP);
    CHECK(out[0].answer == K2_TYPE);
}

TEST_CASE("an answer after a foreign one is still read (each record's data is stepped over)", "[CrealityDiscovery]")
{
    // Upstream never advanced past a record's data, so the second answer here was read from the
    // middle of the first one's.
    const auto out = parse(reply({ "_http._tcp.local.", "_octoprint._tcp.local.", K2_TYPE }));
    REQUIRE(out.size() == 1);
    CHECK(out[0].answer == K2_TYPE);
}

TEST_CASE("a reply without the echoed question and with a lower-case prefix is accepted", "[CrealityDiscovery]")
{
    const std::string lower = "_creality-0A1B2C3D4E5F._udp.local.";
    const auto        out   = parse(reply({ lower }, 0, 0x8400, false));
    REQUIRE(out.size() == 1);
    CHECK(out[0].answer == lower);
}

TEST_CASE("replies that are not answers to our query add nothing", "[CrealityDiscovery]")
{
    CHECK(parse(reply({ "_http._tcp.local." })).empty());          // no Creality service
    CHECK(parse(reply({ K2_TYPE }, 0x1234)).empty());              // someone else's query id
    CHECK(parse(reply({ K2_TYPE }, 0, 0x0000)).empty());           // a query, not a response
    CHECK(parse(reply({ K2_TYPE }), "").empty());                  // no IPv4 sender
    CHECK(parse(reply({ K2_TYPE }), IP, { "Bambu" }).empty());     // other prefixes
    CHECK(parse(Bytes{}).empty());
    std::vector<cxnet::machine_info> out;
    cxnet::parse_discovery_reply(nullptr, 64, IP, { "Creality" }, out);
    CHECK(out.empty());
}

TEST_CASE("every truncation of a reply parses without reading past its end", "[CrealityDiscovery]")
{
    const Bytes full = reply({ "_http._tcp.local.", K2_TYPE });
    for (size_t n = 0; n < full.size(); ++n) {
        // A copy of exactly n bytes, so a read past the end is a read past the allocation.
        const Bytes cut(full.begin(), full.begin() + n);
        const auto  out = parse(cut);
        CHECK(out.size() <= 1);
    }
    CHECK(parse(full).size() == 1);
}

TEST_CASE("a record length that runs past the datagram stops the parse", "[CrealityDiscovery]")
{
    Bytes b = reply({ K2_TYPE });
    // The answer's RDLENGTH sits right after its 2-byte pointer name, type, class and TTL.
    const size_t question_end = 12 + dns_name(SERVICES).size() + 4;
    const size_t rdlength_at  = question_end + 2 + 2 + 2 + 4;
    b[rdlength_at]            = 0xFF;
    b[rdlength_at + 1]        = 0xFF;
    CHECK(parse(b).empty());
}

// ---- no-network scan paths ----------------------------------------------------------------------

TEST_CASE("a scan that cannot run opens nothing and returns at once", "[CrealityDiscovery]")
{
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(cxnet::syncDiscoveryService({ "Creality" }, 0).empty());
    std::atomic<bool> cancel { true };
    CHECK(cxnet::syncDiscoveryService({ "Creality" }, 4000, &cancel).empty());
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(1));
}

TEST_CASE("no printer on the network: the scan ends after discovery with no probes", "[CrealityDiscovery]")
{
    CrealityHostDiscovery::Options opt;
    opt.mdns_timeout_ms = 1234;
    int  asked_timeout  = -1;
    int  info_calls     = 0;
    auto hosts          = CrealityHostDiscovery::scan_with(
        opt,
        [&](int timeout_ms, const std::atomic<bool>*) {
            asked_timeout = timeout_ms;
            return std::vector<cxnet::machine_info>{};
        },
        [&](const std::string&, int, std::string&) { ++info_calls; return false; },
        [](const std::string&, int) { return std::string(); });
    CHECK(hosts.empty());
    CHECK(asked_timeout == 1234);
    CHECK(info_calls == 0);
}

TEST_CASE("a cancelled scan does not even start discovery", "[CrealityDiscovery]")
{
    std::atomic<bool> cancel { true };
    bool              discovered = false;
    auto              hosts      = CrealityHostDiscovery::scan_with(
        CrealityHostDiscovery::Options(),
        [&](int, const std::atomic<bool>*) {
            discovered = true;
            return std::vector<cxnet::machine_info>{ { IP, K2_TYPE } };
        },
        nullptr, nullptr, &cancel);
    CHECK(hosts.empty());
    CHECK_FALSE(discovered);
}

TEST_CASE("a scan dedupes by address and probes model and CFS only where they apply", "[CrealityDiscovery]")
{
    const std::string unknown_ip = "192.0.2.42", silent_ip = "192.0.2.43";
    // The probes run on worker threads, where Catch2 assertions are not allowed: record, check after.
    std::atomic<int>  boxes_calls { 0 }, boxes_wrong_ip { 0 }, info_wrong_timeout { 0 };
    auto              hosts = CrealityHostDiscovery::scan_with(
        CrealityHostDiscovery::Options(),
        [&](int, const std::atomic<bool>*) {
            // The same printer answers on two interfaces and to two re-sent queries.
            return std::vector<cxnet::machine_info>{ { IP, K2_TYPE },
                                                     { IP, K2_TYPE },
                                                     { unknown_ip, "_Creality-FFFFFFFF0001._udp.local." },
                                                     { "", "_Creality-noaddress._udp.local." },
                                                     { silent_ip, "_Creality-FFFFFFFF0002._udp.local." } };
        },
        [&](const std::string& ip, int timeout_s, std::string& body) {
            if (timeout_s != 3)
                ++info_wrong_timeout;
            if (ip == IP) { body = K2PLUS_INFO; return true; }
            if (ip == unknown_ip) { body = R"({"model":"F999"})"; return true; }
            return false; // silent_ip: /info does not answer
        },
        [&](const std::string& ip, int) {
            ++boxes_calls;
            if (ip != IP)
                ++boxes_wrong_ip;
            return K2_BOXS;
        });
    REQUIRE(hosts.size() == 3);
    CHECK(boxes_calls.load() == 1); // the unknown model and the silent host get no CFS probe
    CHECK(boxes_wrong_ip.load() == 0);
    CHECK(info_wrong_timeout.load() == 0);

    CHECK(hosts[0].ip == IP);
    CHECK(hosts[0].model_code == "F008");
    CHECK(hosts[0].model_name == "K2 Plus");
    CHECK(hosts[0].printer_model == "Creality K2 Plus");
    CHECK(hosts[0].cfs_boxes == 1);
    CHECK(hosts[0].cfs_variant == "CFS-C");
    CHECK(hosts[0].label() == "Creality K2 Plus (DB19)");
    CHECK(hosts[0].cfs_text() == "Yes (CFS-C)");

    CHECK(hosts[1].model_code == "F999");
    CHECK(hosts[1].printer_model.empty()); // never guessed
    CHECK(hosts[1].label() == "Creality (0001)");
    CHECK(hosts[1].cfs_text().empty());

    CHECK_FALSE(hosts[2].info_ok);
    CHECK(hosts[2].printer_model.empty());
}

// ---- hosts, models and presets ------------------------------------------------------------------

TEST_CASE("device suffix and labels", "[CrealityDiscovery]")
{
    CHECK(CrealityHostDiscovery::device_suffix(K2_TYPE) == "DB19");
    CHECK(CrealityHostDiscovery::device_suffix("_Creality-AB._udp.local.") == "AB");
    CHECK(CrealityHostDiscovery::device_suffix("nodash") == "");
    CrealityHost h;
    h.ip = IP;
    CHECK(h.label() == IP);
    h.model_name = "K2";
    CHECK(h.label() == "Creality K2");
    h.cfs_boxes = 0;
    CHECK(h.cfs_text() == "No");
    h.cfs_boxes   = 2;
    h.cfs_variant = "CFS";
    CHECK(h.cfs_text() == "2 x CFS");
}

TEST_CASE("/info bodies: known, unknown and hostile", "[CrealityDiscovery]")
{
    CrealityHost a;
    CrealityHostDiscovery::apply_info(a, K2PLUS_INFO);
    CHECK(a.info_ok);
    CHECK(a.mac == "00:00:5e:00:53:19");
    CHECK(a.printer_model == "Creality K2 Plus");

    for (const std::string& bad : { std::string(""), std::string("<html>"), std::string("[1,2]"), std::string(R"({"model":42})") }) {
        CrealityHost h;
        CrealityHostDiscovery::apply_info(h, bad);
        CHECK(h.model_code.empty());
        CHECK(h.printer_model.empty());
    }

    CrealityHost boxes;
    CrealityHostDiscovery::apply_boxes(boxes, "not json");
    CHECK(boxes.cfs_boxes == -1);
    CrealityHostDiscovery::apply_boxes(boxes, R"({"boxsInfo":{"materialBoxs":[]}})");
    CHECK(boxes.cfs_boxes == 0);
    CHECK(boxes.cfs_text() == "No");
}

TEST_CASE("model codes map to printer models exactly, and unknown codes to nothing", "[CrealityDiscovery]")
{
    CHECK(CrealityHostDiscovery::printer_model_for("F008") == "Creality K2 Plus");
    CHECK(CrealityHostDiscovery::printer_model_for("F012") == "Creality K2 Pro");
    CHECK(CrealityHostDiscovery::printer_model_for("F021") == "Creality K2");
    CHECK(CrealityHostDiscovery::printer_model_for("F022") == "Creality SPARKX i7");
    CHECK(CrealityHostDiscovery::printer_model_for("K1C") == "Creality K1C");
    CHECK(CrealityHostDiscovery::printer_model_for("K1_CFS-C") == "Creality K1_CFS-C");
    CHECK(CrealityHostDiscovery::printer_model_for("").empty());
    CHECK(CrealityHostDiscovery::printer_model_for("F00").empty());
    CHECK(CrealityHostDiscovery::printer_model_for("K2 Plus").empty()); // a name, not a code
    CHECK(CrealityHostDiscovery::printer_model_for("K1 Max").empty());  // no verified /info code
}

TEST_CASE("every model the table maps to ships as a Creality machine preset", "[CrealityDiscovery]")
{
    namespace fs = boost::filesystem;
    const fs::path dir = fs::path(SLIC3R_TEST_RESOURCES_DIR) / "profiles" / "Creality" / "machine";
    REQUIRE(fs::is_directory(dir));
    std::set<std::string> shipped;
    const std::regex      re("\"printer_model\"\\s*:\\s*\"([^\"]+)\"");
    for (const fs::directory_entry& e : fs::directory_iterator(dir)) {
        if (e.path().extension() != ".json")
            continue;
        std::ifstream     f(e.path().string());
        std::stringstream ss;
        ss << f.rdbuf();
        const std::string text = ss.str();
        std::smatch       m;
        if (std::regex_search(text, m, re))
            shipped.insert(m[1].str());
    }
    for (const auto& kv : Slic3r::CrealityPrintModel::cfs_capable_models()) {
        INFO(kv.first);
        CHECK(shipped.count(CrealityHostDiscovery::printer_model_for(kv.first)) == 1);
    }
}

TEST_CASE("the preset picked is the visible system 0.4 nozzle one of that model", "[CrealityDiscovery]")
{
    using Ref = CrealityHostDiscovery::PresetRef;
    const std::vector<Ref> presets = {
        { "Creality K2 Plus 0.2 nozzle", "Creality K2 Plus", "0.2", true, true },
        { "Creality K2 Plus 0.4 nozzle", "Creality K2 Plus", "0.4", true, true },
        { "My K2 Plus", "Creality K2 Plus", "0.4", false, true },
        { "Creality K2 0.4 nozzle", "Creality K2", "0.4", true, false }, // not enabled
        { "Creality K2 Pro 0.6 nozzle", "Creality K2 Pro", "0.6", true, true },
        { "Creality K2 Pro 0.2 nozzle", "Creality K2 Pro", "0.2", true, true },
    };
    CHECK(CrealityHostDiscovery::pick_printer_preset(presets, "Creality K2 Plus") == "Creality K2 Plus 0.4 nozzle");
    CHECK(CrealityHostDiscovery::pick_printer_preset(presets, "Creality K2 Pro") == "Creality K2 Pro 0.2 nozzle"); // no 0.4: lowest name
    CHECK(CrealityHostDiscovery::pick_printer_preset(presets, "Creality K2").empty());   // only an invisible one
    CHECK(CrealityHostDiscovery::pick_printer_preset(presets, "Creality K1C").empty());  // not installed
    CHECK(CrealityHostDiscovery::pick_printer_preset(presets, "").empty());              // unknown model
}

TEST_CASE("the Creality search is offered only to Creality printers", "[CrealityDiscovery]")
{
    CHECK(CrealityHostDiscovery::applies_to("crealityprint", ""));
    CHECK(CrealityHostDiscovery::applies_to("octoprint", "Creality K1 Max"));
    CHECK(CrealityHostDiscovery::applies_to("octoprint", "Creality K2 Plus"));
    CHECK_FALSE(CrealityHostDiscovery::applies_to("octoprint", "Creality Ender-3 V3"));
    CHECK_FALSE(CrealityHostDiscovery::applies_to("octoprint", "Voron 2.4 350"));
    CHECK_FALSE(CrealityHostDiscovery::applies_to("elegoolink", "Elegoo Centauri Carbon"));
    CHECK_FALSE(CrealityHostDiscovery::applies_to("flashforge", "Flashforge Creator 5"));
}
