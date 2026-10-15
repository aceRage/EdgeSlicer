#ifndef slic3r_GUI_Printer_IFileGridModel_h_
#define slic3r_GUI_Printer_IFileGridModel_h_

// The data behind a printer-files view: what files there are, in what order, what each one's
// details and preview picture are, and the actions on them. Adapted from OrcaSlicer PR #16156
// (IFileGridModel / RemoteFileGridModel / BambuFileGridModel, merge 029bc7928f).
//
// Unlike upstream's, this interface carries no wx type: a card holds its preview picture as the
// encoded bytes the printer sent, and the view decodes and scales them. That keeps the model
// unit-testable without a display (tests/slic3rutils/printer_storage_tests.cpp) and lets any
// view - the Device page's Storage tab today, a hub route or a dialog later - sit on it.
//
// Implemented by RemoteFileGridModel over PrinterStorage::IPrinterFiles (Moonraker). Bambu's
// storage page (MediaFilePanel / ImageGrid over PrinterFileSystem, the UltraNet storage module)
// keeps its own model: wrapping it as a BambuFileGridModel would mean rebuilding ImageGrid on this
// interface, which upstream did and this fork deliberately has not (it would touch the Bambu
// storage and camera path for no user-visible gain).

#include "slic3r/Utils/PrinterStorage.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace Slic3r {
namespace GUI {

enum class FileGridStatus {
    Idle,         // nothing asked yet
    Loading,      // the list is on its way
    Ready,        // files listed
    Empty,        // the printer has none
    Failed,       // the list failed; GetStatusMessage() says why (Retry/Refresh asks again)
    NotSupported, // this printer has no file API this model speaks; GetStatusMessage() says why
};

enum class FileGridSort { Newest, Oldest, Name, Size };

enum class ThumbnailState {
    None,    // not asked yet
    Loading, // on its way
    Ready,   // FileGridCard::thumbnail holds the picture
    Missing, // the file has none, or it could not be fetched
};

// One file, as a view shows it.
struct FileGridCard
{
    std::string   id;   // what the printer calls it: the path relative to its gcodes root
    std::string   name; // for display
    std::int64_t  time { 0 }; // unix seconds, 0 = unknown
    std::uint64_t size { 0 };
    bool          printable { false };

    bool                           details_loaded { false }; // `meta` has been asked for (valid or not)
    PrinterStorage::FileMetadata   meta;
    ThumbnailState                 thumbnail_state { ThumbnailState::None };
    std::shared_ptr<const std::string> thumbnail; // encoded PNG / JPEG, when Ready

    // Ready-made text for the columns (English formatting of numbers, no words to translate).
    std::string print_time_text() const { return PrinterStorage::format_duration(meta.estimated_time_s); }
    std::string filament_text() const { return PrinterStorage::format_filament(meta.filament_total_mm, meta.filament_weight_g); }
    std::string size_text() const { return PrinterStorage::format_size(size); }
};

// What changed, for the view's handler. `Files`: the list or its order (rebuild every row);
// `Card`: one card's details or picture (id names it); `Status`: GetStatus() / GetStatusMessage().
enum class FileGridChange { Files, Card, Status };

// Runs the blocking work off the GUI thread and comes back with the result. The model only ever
// runs `then` (and touches its own state) on the thread the view lives on. The view supplies the
// real one (a worker thread plus wx's CallAfter); tests run both halves at once, or queue them.
class FileGridExecutor
{
public:
    virtual ~FileGridExecutor() = default;
    virtual void run(std::function<void()> work, std::function<void()> then) = 0;
};

// No member is named after a Win32 API (DeleteFile, GetMessage, ...): windows.h turns those into
// macros in some translation units and not in others, and the two then disagree on the class.
class IFileGridModel
{
public:
    virtual ~IFileGridModel() = default;

    virtual FileGridStatus     GetStatus() const  = 0;
    virtual const std::string& GetStatusMessage() const = 0;

    // Cards in display order.
    virtual size_t              GetCount() const               = 0;
    virtual const FileGridCard* GetCard(size_t index) const    = 0;
    virtual const FileGridCard* FindCard(const std::string& id) const = 0;
    virtual size_t              IndexOf(const std::string& id) const = 0; // GetCount() when absent

    virtual FileGridSort GetSort() const        = 0;
    virtual void         SetSort(FileGridSort sort) = 0;

    // Ask the printer for the list again. Details and pictures already fetched are kept for files
    // that are still there with the same size and time.
    virtual void Refresh() = 0;
    // The rows the view shows: their details and pictures are fetched first, a few at a time.
    virtual void SetFocusRange(size_t start, size_t count) = 0;

    virtual bool CanDelete() const = 0;
    virtual bool CanPrint() const  = 0;
    // Delete one file. The card goes away when the printer confirms; `done` gets the result.
    virtual void DeletePrinterFile(const std::string& id, std::function<void(const PrinterStorage::Result&)> done) = 0;
    // What the printer is doing ("standby", "printing", ...), for the confirmation before a print.
    virtual void QueryPrinter(std::function<void(const PrinterStorage::Result&, const std::string& state,
                                                 const std::string& current_file)> done) = 0;
    // Start one file as it was sliced. Refused (Busy) while the printer prints or is paused.
    virtual void StartPrint(const std::string& id, std::function<void(const PrinterStorage::Result&)> done) = 0;

    virtual void SetChangeHandler(std::function<void(FileGridChange, const std::string& id)> handler) = 0;
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_Printer_IFileGridModel_h_
