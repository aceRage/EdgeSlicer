#ifndef slic3r_GUI_Printer_RemoteFileGridModel_h_
#define slic3r_GUI_Printer_RemoteFileGridModel_h_

// IFileGridModel over a printer's storage API (PrinterStorage::IPrinterFiles), adapted from
// OrcaSlicer PR #16156's RemoteFileGridModel. No wx: see IFileGridModel.h.

#include "IFileGridModel.h"

#include <deque>
#include <map>
#include <set>
#include <vector>

namespace Slic3r {
namespace GUI {

class RemoteFileGridModel : public IFileGridModel
{
public:
    // How many detail requests (metadata, then the picture) run side by side. A Moonraker on a
    // Raspberry Pi answers these quickly, but a printer busy printing should not be flooded.
    static constexpr size_t MAX_IN_FLIGHT = 3;

    // `files` may be null: the model then reports NotSupported with `not_supported_reason`.
    RemoteFileGridModel(std::shared_ptr<PrinterStorage::IPrinterFiles> files, std::shared_ptr<FileGridExecutor> executor,
                        std::string not_supported_reason = std::string());
    ~RemoteFileGridModel() override;

    FileGridStatus     GetStatus() const override { return m_status; }
    const std::string& GetMessage() const override { return m_message; }

    size_t              GetCount() const override { return m_order.size(); }
    const FileGridCard* GetCard(size_t index) const override;
    const FileGridCard* FindCard(const std::string& id) const override;
    size_t              IndexOf(const std::string& id) const override;

    FileGridSort GetSort() const override { return m_sort; }
    void         SetSort(FileGridSort sort) override;

    void Refresh() override;
    void SetFocusRange(size_t start, size_t count) override;

    bool CanDelete() const override { return m_files != nullptr; }
    bool CanPrint() const override { return m_files != nullptr; }
    void DeleteFile(const std::string& id, std::function<void(const PrinterStorage::Result&)> done) override;
    void QueryPrinter(std::function<void(const PrinterStorage::Result&, const std::string&, const std::string&)> done) override;
    void StartPrint(const std::string& id, std::function<void(const PrinterStorage::Result&)> done) override;

    void SetChangeHandler(std::function<void(FileGridChange, const std::string&)> handler) override
    {
        m_change = std::move(handler);
    }

    // For tests and logs.
    size_t in_flight() const { return m_in_flight; }
    size_t queued() const { return m_queue.size(); }

private:
    void set_status(FileGridStatus status, const std::string& message);
    void notify(FileGridChange change, const std::string& id = std::string());
    void on_list(std::uint64_t request, const PrinterStorage::Result& result, std::vector<PrinterStorage::FileEntry> files);
    void sort_cards();
    void pump();

    std::shared_ptr<PrinterStorage::IPrinterFiles> m_files;
    std::shared_ptr<FileGridExecutor>              m_executor;
    std::string                                    m_not_supported_reason;

    std::map<std::string, FileGridCard> m_cards; // by id
    std::vector<std::string>            m_order; // ids in display order
    FileGridSort                        m_sort { FileGridSort::Newest };
    FileGridStatus                      m_status { FileGridStatus::Idle };
    std::string                         m_message;

    std::uint64_t          m_request { 0 }; // which list answer is current; older ones are dropped
    std::deque<std::string> m_queue;        // ids waiting for their details
    std::set<std::string>   m_queued;
    size_t                 m_in_flight { 0 };
    bool                   m_pumping { false };

    std::function<void(FileGridChange, const std::string&)> m_change;
    // Completions check this before touching the model: it dies with the model.
    std::shared_ptr<int> m_lifetime { std::make_shared<int>(0) };
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_Printer_RemoteFileGridModel_h_
