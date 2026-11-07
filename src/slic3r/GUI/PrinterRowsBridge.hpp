#pragma once

// The slicer's side of the "printers_*" messages a page of ours sends over the "wx" script channel
// (PrintersMonitor.hpp has the message format): reading the printer rows (the same rows as the hub's
// /summary and the app's Devices page, with their pictures), Pause / Resume / Stop and following the
// job, and a job's plate picture. Shared by the Printers tab (PrintersPanel) and the shared device
// page in the Device tab (PrinterWebView, resources/web/orca/device.html), so both answer the same
// way. Everything slow runs on a worker thread; the answer is handed back on the GUI thread.

#include <atomic>
#include <functional>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "slic3r/Utils/PrintersMonitor.hpp"

namespace Slic3r {
namespace GUI {
namespace PrinterRowsBridge {

// GUI thread: run window.<fn>(arg) in the page that asked.
using Reply = std::function<void(const std::string& fn, const nlohmann::json& arg)>;

// printers_get: the rows, answered with __printers(payload). `reading` keeps one read in flight
// (a later ask while one runs is answered by it). `rows_read` (optional) sees the payload first,
// on the GUI thread (the Printers tab keeps what "Open Device page" may open).
// `with_leftovers` (the Printers tab): also the rows only the hub still remembers, marked
// leftover (PrintersMonitor::add_leftovers), so the tab can offer Forget on them.
void read_rows(std::weak_ptr<bool> alive, std::shared_ptr<std::atomic<bool>> reading, Reply reply,
               std::function<void(const nlohmann::json& payload)> rows_read = nullptr, bool with_leftovers = false);

// printers_forget: the hub forgets that leftover row (RemoteHub::forget_printer), answered with
// __printerForget({ id, ok, error }).
void forget(std::weak_ptr<bool> alive, const std::string& printer_id, Reply reply);

// printers_control: answered with __printerControl.
void control(std::weak_ptr<bool> alive, const PrintersMonitor::Message& m, Reply reply);

// printers_job: answered with __printerJob (in memory, answered at once).
void job(int id, Reply reply);

// printers_thumb: answered with __printerThumb.
void thumbnail(std::weak_ptr<bool> alive, const std::string& archive_id, Reply reply);

// The kinds above, for a page that takes nothing else: true when `m` was one of them.
bool handle(const PrintersMonitor::Message& m, std::weak_ptr<bool> alive, std::shared_ptr<std::atomic<bool>> reading, Reply reply);

} // namespace PrinterRowsBridge
} // namespace GUI
} // namespace Slic3r
