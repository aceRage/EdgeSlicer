#include "PrinterRowsBridge.hpp"

#include "GUI_App.hpp"
#include "RemoteAccess.hpp"
#include "RemoteHub.hpp"
#include "DeviceManager.hpp"
#include "SnapmakerLan.hpp"
#include "GcodeArchive.hpp"
#include "slic3r/Utils/PrintHostDevices.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <wx/base64.h>

#include <chrono>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

namespace Slic3r {
namespace GUI {
namespace PrinterRowsBridge {

using json = nlohmann::json;
namespace PMon = Slic3r::PrintersMonitor;

namespace {

long long now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// The vendor profiles' *_cover.png files, indexed once (they only change with an install).
const PMon::CoverIndex& cover_index()
{
    static std::once_flag   once;
    static PMon::CoverIndex index;
    std::call_once(once, []() {
        namespace fs = boost::filesystem;
        std::vector<std::pair<std::string, std::string>> files;
        boost::system::error_code ec;
        const fs::path root = fs::path(resources_dir()) / "profiles";
        for (fs::directory_iterator v(root, ec), end; !ec && v != end; v.increment(ec)) {
            boost::system::error_code ig;
            if (!fs::is_directory(v->path(), ig)) continue;
            for (fs::directory_iterator f(v->path(), ig), fend; !ig && f != fend; f.increment(ig))
                files.emplace_back(v->path().filename().string(), f->path().filename().string());
        }
        index = PMon::index_covers(files);
    });
    return index;
}

// The archive's sidecars, re-read at most every 30 s (each poll would otherwise parse them all).
std::vector<PMon::ArchiveEntry> archive_entries()
{
    static std::mutex                      mtx;
    static long long                       read_at = 0;
    static std::vector<PMon::ArchiveEntry> cached;
    std::lock_guard<std::mutex> lock(mtx);
    const long long now = now_ms();
    if (read_at == 0 || now - read_at > 30000) {
        read_at = now;
        cached.clear();
        try {
            for (const GcodeArchive::Record& r : GcodeArchive::list()) {
                if (!r.has_thumbnail || !r.json.is_object() || !r.json.contains("printer") || !r.json["printer"].is_object()) continue;
                PMon::ArchiveEntry e;
                e.printer_id = r.json["printer"].value("id", std::string());
                e.record_id  = r.id;
                e.sent_name  = r.json.value("sent_name", std::string());
                e.file       = r.file;
                e.time       = r.time;
                cached.push_back(std::move(e));
            }
        } catch (...) {}
    }
    return cached;
}

// The rows plus what a card shows besides them: the printer's picture and the job's plate picture.
void enrich(json& payload)
{
    if (!payload.value("ok", false) || !payload.contains("printers")) return;
    std::map<std::string, std::string> jobs;
    for (const json& p : payload["printers"])
        if (p.is_object() && p.contains("id") && p["id"].is_string()) jobs[p["id"].get<std::string>()] = PMon::row_job(p);
    std::map<std::string, std::string> thumbs;
    try { thumbs = PMon::pick_thumbnails(archive_entries(), jobs); } catch (...) {}
    PMon::enrich_rows(payload["printers"], cover_index(), thumbs);
    // What "Remove printer" will do with each card (remove / log out / only explain).
    PMon::annotate_removal(payload["printers"]);
}

} // namespace

void read_rows(std::weak_ptr<bool> alive, std::shared_ptr<std::atomic<bool>> reading, Reply reply,
               std::function<void(const json& payload)> rows_read, bool with_leftovers)
{
    bool expected = false;
    if (!reading->compare_exchange_strong(expected, true))
        return; // the read in flight answers this tick too
    // api_printers waits for the GUI thread for the Bambu part and probes the network hosts on the
    // calling thread, so it runs here, never on the GUI thread (RemoteAccess.cpp).
    std::thread([alive, reading, reply, rows_read, with_leftovers]() {
        std::pair<int, std::string> res { 503, "" };
        try { res = RemoteAccess::get().monitor_printers(); } catch (...) {}
        auto payload = std::make_shared<json>(PMon::page_payload(res.first, res.second, now_ms()));
        // The rows only the hub still remembers (printers this slicer no longer has), so the tab
        // can offer Forget. One loopback request; no hub, nothing added.
        if (with_leftovers) {
            try {
                const std::pair<int, std::string> hub = RemoteHub::hub_printers();
                PMon::add_leftovers(*payload, hub.first, hub.second);
            } catch (...) {}
        }
        try { enrich(*payload); } catch (...) {}
        *reading = false;
        RemoteAccess::post_to_app([alive, payload, reply, rows_read]() {
            auto a = alive.lock();
            if (!a || !*a) return;
            if (rows_read) rows_read(*payload);
            reply("__printers", *payload);
        });
    }).detach();
}

void forget(std::weak_ptr<bool> alive, const std::string& printer_id, Reply reply)
{
    std::thread([alive, printer_id, reply]() {
        std::pair<int, std::string> res { 503, "" };
        try { res = RemoteHub::forget_printer(printer_id); } catch (...) {}
        json answer = { { "id", printer_id }, { "ok", res.first == 200 } };
        if (res.first != 200) {
            std::string why;
            try {
                const json e = json::parse(res.second);
                if (e.is_object() && e.contains("error") && e["error"].is_string()) why = e["error"].get<std::string>();
            } catch (...) {}
            answer["error"] = why.empty() ? std::string("the hub did not answer") : why;
        }
        RemoteAccess::post_to_app([alive, answer, reply]() {
            auto a = alive.lock();
            if (a && *a) reply("__printerForget", answer);
        });
    }).detach();
}

// The GUI-thread half of a removal: what lives in AppConfig and the device manager.
void remove_on_gui_thread(const PMon::RemovalPlan& plan, std::vector<std::string>& done)
{
    AppConfig* cfg = wxGetApp().app_config;
    if (!plan.bambu_dev_id.empty()) {
        // The Device tab's own LAN Log out (SelectMachinePopup::on_panel_unbind): the saved LAN
        // record and both access codes.
        if (DeviceManager* dm = wxGetApp().getDeviceManager()) {
            MachineObject* m = nullptr;
            auto it = dm->localMachineList.find(plan.bambu_dev_id);
            if (it != dm->localMachineList.end()) m = it->second;
            if (!m) {
                auto ut = dm->userMachineList.find(plan.bambu_dev_id);
                if (ut != dm->userMachineList.end()) m = ut->second;
            }
            MachineObject* selected = dm->get_selected_machine();
            if (selected && selected->dev_id == plan.bambu_dev_id) dm->set_selected_machine("");
            if (m) {
                m->set_access_code("");
                m->erase_user_access_code();
            }
        }
        if (cfg) {
            cfg->erase_local_machine(plan.bambu_dev_id);
            cfg->erase("access_code", plan.bambu_dev_id);
            cfg->erase("user_access_code", plan.bambu_dev_id);
        }
        done.push_back("bambu_lan");
    }
    if (plan.app_config_devices && cfg) {
        // The Device tab's saved Snapmaker devices: one of them is what merge_app_devices would
        // re-add the LAN card from.
        bool any = false;
        for (const DeviceInfo& d : cfg->get_devices()) {
            const bool same = (!plan.sm_id.empty() && (d.sn == plan.sm_id || d.dev_id == plan.sm_id || d.ip == plan.sm_id)) ||
                              (!plan.sm_ip.empty() && d.ip == plan.sm_ip);
            if (same) { cfg->remove_device_info(d.dev_id); any = true; }
        }
        if (any) done.push_back("app_config_devices");
    }
    if (cfg && cfg->dirty()) cfg->save();
}

void remove_files(const PMon::RemovalPlan& plan, const std::string& row_id, std::vector<std::string>& done)
{
    if (!plan.ph_device_id.empty()) {
        bool gone = !plan.ph_model_key.empty() && PrintHostDevices::remove(plan.ph_model_key, plan.ph_device_id);
        if (!gone) // the row did not say its model: look for the device under every model
            for (const auto& kv : PrintHostDevices::all_devices())
                for (const PrintHostDevices::Device& d : kv.second)
                    if (!gone && d.id == plan.ph_device_id) gone = PrintHostDevices::remove(kv.first, d.id);
        if (gone) done.push_back("print_host_devices");
    }
    if (!plan.sm_id.empty() || !plan.sm_ip.empty()) {
        const bool a = !plan.sm_id.empty() && SnapmakerLan::remove(plan.sm_id);
        const bool b = !plan.sm_ip.empty() && SnapmakerLan::remove(plan.sm_ip);
        if (a || b) done.push_back("snapmaker_lan");
    }
    if (plan.tombstone) {
        json store = PMon::load_tombstones(RemoteHub::removed_printers_path());
        PMon::add_tombstone(store, row_id, plan.sm_ip,
                            (long long) std::chrono::duration_cast<std::chrono::seconds>(
                                std::chrono::system_clock::now().time_since_epoch()).count());
        if (PMon::save_tombstones(RemoteHub::removed_printers_path(), store)) done.push_back("tombstone");
    }
}

void remove(std::weak_ptr<bool> alive, const json& row, Reply reply)
{
    const std::string       id   = row.value("id", std::string());
    const PMon::RemovalPlan plan = PMon::removal_plan(row);
    if (plan.mode == PMon::RemovalPlan::Mode::Explain) {
        reply("__printerRemove", json{ { "id", id }, { "ok", false }, { "explain", true }, { "error", plan.reason } });
        return;
    }
    std::thread([alive, id, row, plan, reply]() {
        json answer = { { "id", id }, { "ok", false } };
        try {
            const std::pair<int, std::string> res = RemoteAccess::get().remove_printer(row);
            try { answer = json::parse(res.second); } catch (...) {}
            answer["id"] = id;
            answer["ok"] = res.first == 200;
            if (res.first == 200 && plan.hub_row) {
                const auto hub = RemoteHub::forget_printer(id, /* removed */ true);
                if ((hub.first == 200 || hub.first == 404) && answer.contains("done") && answer["done"].is_array())
                    answer["done"].push_back("hub_printers");
            }
        } catch (...) {}
        if (!answer.value("ok", false) && !answer.contains("error")) answer["error"] = "the printer could not be removed";
        RemoteAccess::post_to_app([alive, answer, reply]() {
            auto a = alive.lock();
            if (a && *a) reply("__printerRemove", answer);
        });
    }).detach();
}

void rename(std::weak_ptr<bool> alive, const std::string& printer_id, const std::string& name, Reply reply)
{
    json store = PMon::load_store_file(RemoteHub::printer_aliases_path());
    const bool ok = PMon::set_alias(store, printer_id, name) && PMon::save_store_file(RemoteHub::printer_aliases_path(), store);
    json answer = { { "id", printer_id }, { "ok", ok }, { "name", PMon::alias_of(store, printer_id) } };
    if (!ok) answer["error"] = "the name could not be saved";
    reply("__printerRename", answer);
    (void) alive;
}

void thumbnail(std::weak_ptr<bool> alive, const std::string& archive_id, Reply reply)
{
    std::thread([alive, archive_id, reply]() {
        std::string data;
        try {
            const GcodeArchive::Record r = GcodeArchive::find(archive_id);
            boost::system::error_code  ec;
            if (!r.id.empty() && r.has_thumbnail && boost::filesystem::is_regular_file(r.thumbnail_path, ec) &&
                boost::filesystem::file_size(r.thumbnail_path, ec) <= 2u * 1024 * 1024) {
                std::ifstream     f(boost::filesystem::path(r.thumbnail_path).string(), std::ios::binary);
                std::stringstream ss;
                ss << f.rdbuf();
                const std::string png = ss.str();
                // A PNG and nothing else (the page puts it in an <img> as a data: URL).
                if (png.size() > 8 && png.compare(0, 8, "\x89PNG\r\n\x1a\n") == 0)
                    data = "data:image/png;base64," + std::string(wxBase64Encode(png.data(), png.size()).ToUTF8().data());
            }
        } catch (...) {}
        const json answer = { { "id", archive_id }, { "data", data } };
        RemoteAccess::post_to_app([alive, answer, reply]() {
            auto a = alive.lock();
            if (a && *a) reply("__printerThumb", answer);
        });
    }).detach();
}

void control(std::weak_ptr<bool> alive, const PMon::Message& m, Reply reply)
{
    if (m.action == "stop" && !m.confirm) {
        reply("__printerControl", json { { "id", m.id }, { "action", m.action }, { "ok", false }, { "error", "Stopping a print needs a confirmation." } });
        return;
    }
    const std::string id = m.id, action = m.action;
    const bool        confirm = m.confirm;
    std::thread([alive, id, action, confirm, reply]() {
        std::pair<int, std::string> res { 500, "" };
        try { res = RemoteAccess::get().monitor_control(id, action, confirm); } catch (...) {}
        json answer = { { "id", id }, { "action", action }, { "ok", res.first == 200 } };
        try {
            const json body = json::parse(res.second);
            if (res.first == 200) {
                answer["job"]     = body.value("job", 0);
                answer["dry_run"] = body.value("dry_run", false);
            } else {
                answer["error"] = body.value("error", std::string("the command was refused"));
            }
        } catch (...) {
            if (res.first != 200) answer["error"] = "the command was refused";
        }
        RemoteAccess::post_to_app([alive, answer, reply]() {
            auto a = alive.lock();
            if (a && *a) reply("__printerControl", answer);
        });
    }).detach();
}

void job(int id, Reply reply)
{
    // In memory, no GUI-thread work: cheap enough to answer right here.
    const std::pair<int, std::string> res = RemoteAccess::get().monitor_job(id);
    json answer = { { "id", id }, { "state", "error" }, { "error", "no such job" } };
    if (res.first == 200) {
        try {
            const json j = json::parse(res.second);
            answer       = { { "id", id }, { "state", j.value("state", std::string("error")) }, { "text", j.value("text", std::string()) },
                             { "error", j.value("error", std::string()) }, { "printer", j.value("printer", std::string()) } };
        } catch (...) {}
    }
    reply("__printerJob", answer);
}

bool handle(const PMon::Message& m, std::weak_ptr<bool> alive, std::shared_ptr<std::atomic<bool>> reading, Reply reply)
{
    switch (m.kind) {
    case PMon::Message::Kind::Get: read_rows(alive, reading, reply); return true;
    case PMon::Message::Kind::Control: control(alive, m, reply); return true;
    case PMon::Message::Kind::Job: job(m.job, reply); return true;
    case PMon::Message::Kind::Thumb: thumbnail(alive, m.id, reply); return true;
    default: return false;
    }
}

} // namespace PrinterRowsBridge
} // namespace GUI
} // namespace Slic3r
