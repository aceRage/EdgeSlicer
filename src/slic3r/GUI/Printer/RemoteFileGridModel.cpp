#include "RemoteFileGridModel.h"

#include <algorithm>
#include <cctype>

namespace Slic3r {
namespace GUI {

// No using-directive: in a unity build it would leak into the neighbouring sources.
namespace pstore = Slic3r::PrinterStorage;

namespace {

std::string grid_lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char) std::tolower(c); });
    return s;
}

} // namespace

RemoteFileGridModel::RemoteFileGridModel(std::shared_ptr<pstore::IPrinterFiles> files, std::shared_ptr<FileGridExecutor> executor,
                                         std::string not_supported_reason)
    : m_files(std::move(files)), m_executor(std::move(executor)), m_not_supported_reason(std::move(not_supported_reason))
{
    if (!m_files) {
        m_status  = FileGridStatus::NotSupported;
        m_message = m_not_supported_reason.empty() ? std::string("This printer has no file storage the Storage tab can read.") :
                                                     m_not_supported_reason;
    }
}

RemoteFileGridModel::~RemoteFileGridModel() { m_lifetime.reset(); }

const FileGridCard* RemoteFileGridModel::GetCard(size_t index) const
{
    if (index >= m_order.size())
        return nullptr;
    auto it = m_cards.find(m_order[index]);
    return it == m_cards.end() ? nullptr : &it->second;
}

const FileGridCard* RemoteFileGridModel::FindCard(const std::string& id) const
{
    auto it = m_cards.find(id);
    return it == m_cards.end() ? nullptr : &it->second;
}

size_t RemoteFileGridModel::IndexOf(const std::string& id) const
{
    auto it = std::find(m_order.begin(), m_order.end(), id);
    return it == m_order.end() ? m_order.size() : (size_t) (it - m_order.begin());
}

void RemoteFileGridModel::set_status(FileGridStatus status, const std::string& message)
{
    m_status  = status;
    m_message = message;
    notify(FileGridChange::Status);
}

void RemoteFileGridModel::notify(FileGridChange change, const std::string& id)
{
    if (m_change)
        m_change(change, id);
}

void RemoteFileGridModel::SetSort(FileGridSort sort)
{
    if (sort == m_sort)
        return;
    m_sort = sort;
    sort_cards();
    notify(FileGridChange::Files);
}

void RemoteFileGridModel::sort_cards()
{
    m_order.clear();
    m_order.reserve(m_cards.size());
    for (const auto& kv : m_cards)
        m_order.push_back(kv.first);
    const FileGridSort sort = m_sort;
    std::stable_sort(m_order.begin(), m_order.end(), [this, sort](const std::string& a, const std::string& b) {
        const FileGridCard& ca = m_cards.at(a);
        const FileGridCard& cb = m_cards.at(b);
        switch (sort) {
        case FileGridSort::Newest:
            if (ca.time != cb.time)
                return ca.time > cb.time;
            break;
        case FileGridSort::Oldest:
            if (ca.time != cb.time)
                return ca.time < cb.time;
            break;
        case FileGridSort::Size:
            if (ca.size != cb.size)
                return ca.size > cb.size;
            break;
        case FileGridSort::Name: break;
        }
        const std::string la = grid_lower(a), lb = grid_lower(b);
        return la != lb ? la < lb : a < b;
    });
}

void RemoteFileGridModel::Refresh()
{
    if (!m_files) {
        set_status(FileGridStatus::NotSupported, m_message);
        notify(FileGridChange::Files);
        return;
    }
    const std::uint64_t request = ++m_request;
    // Whatever was waiting belonged to the old list; what is in flight finishes on its own.
    m_queue.clear();
    m_queued.clear();
    set_status(FileGridStatus::Loading, std::string());

    auto                     files    = m_files;
    auto                     result   = std::make_shared<pstore::Result>();
    auto                     entries  = std::make_shared<std::vector<pstore::FileEntry>>();
    const std::weak_ptr<int> lifetime = m_lifetime;
    m_executor->run([files, result, entries]() { *result = files->list(*entries); },
                    [this, lifetime, request, result, entries]() {
                        if (lifetime.expired())
                            return;
                        on_list(request, *result, std::move(*entries));
                    });
}

void RemoteFileGridModel::on_list(std::uint64_t request, const pstore::Result& result, std::vector<pstore::FileEntry> files)
{
    if (request != m_request)
        return; // a newer Refresh is under way
    if (!result.ok()) {
        m_cards.clear();
        m_order.clear();
        m_queue.clear();
        m_queued.clear();
        notify(FileGridChange::Files);
        set_status(result.status == pstore::Status::NotSupported ? FileGridStatus::NotSupported : FileGridStatus::Failed, result.message);
        return;
    }
    std::map<std::string, FileGridCard> cards;
    for (pstore::FileEntry& f : files) {
        if (cards.count(f.path))
            continue;
        auto old = m_cards.find(f.path);
        // The same file as before (same size, same time) keeps what was already fetched for it; a
        // failed or unfinished fetch is asked again.
        if (old != m_cards.end() && old->second.size == f.size && old->second.time == f.modified && old->second.details_loaded &&
            old->second.meta.valid && old->second.thumbnail_state != ThumbnailState::Loading) {
            FileGridCard keep = old->second;
            keep.name         = f.name;
            cards.emplace(f.path, std::move(keep));
            continue;
        }
        FileGridCard c;
        c.id        = f.path;
        c.name      = f.name;
        c.time      = f.modified;
        c.size      = f.size;
        c.printable = pstore::is_printable_name(f.path);
        cards.emplace(f.path, std::move(c));
    }
    m_cards = std::move(cards);
    sort_cards();
    notify(FileGridChange::Files);
    set_status(m_cards.empty() ? FileGridStatus::Empty : FileGridStatus::Ready, std::string());
}

void RemoteFileGridModel::SetFocusRange(size_t start, size_t count)
{
    if (!m_files || start >= m_order.size())
        return;
    const size_t end = std::min(m_order.size(), start + count);
    // The rows on screen go first, in their order: walk them backwards, pushing each to the front.
    for (size_t i = end; i-- > start;) {
        const std::string& id   = m_order[i];
        const FileGridCard& card = m_cards.at(id);
        if (card.details_loaded || card.thumbnail_state == ThumbnailState::Loading)
            continue;
        if (m_queued.count(id))
            m_queue.erase(std::remove(m_queue.begin(), m_queue.end(), id), m_queue.end());
        else
            m_queued.insert(id);
        m_queue.push_front(id);
    }
    pump();
}

void RemoteFileGridModel::pump()
{
    // A completion that runs inline (the tests' executor) comes back in here: the loop below
    // carries on with it instead.
    if (m_pumping)
        return;
    m_pumping = true;
    while (m_in_flight < MAX_IN_FLIGHT && !m_queue.empty()) {
        const std::string id = m_queue.front();
        m_queue.pop_front();
        m_queued.erase(id);
        auto it = m_cards.find(id);
        if (it == m_cards.end() || it->second.details_loaded || it->second.thumbnail_state == ThumbnailState::Loading)
            continue;
        it->second.thumbnail_state = ThumbnailState::Loading;
        ++m_in_flight;

        const std::uint64_t size = it->second.size;
        const std::int64_t  time = it->second.time;
        auto                files = m_files;
        auto                meta  = std::make_shared<pstore::FileMetadata>();
        auto                bytes = std::make_shared<std::string>();
        auto                ok    = std::make_shared<bool>(false);
        const std::weak_ptr<int> lifetime = m_lifetime;
        m_executor->run(
            [files, id, meta, bytes, ok]() {
                *ok = files->metadata(id, *meta).ok();
                if (!*ok)
                    return;
                if (const pstore::Thumbnail* t = pstore::pick_thumbnail(meta->thumbnails)) {
                    if (!files->thumbnail(t->path, *bytes).ok())
                        bytes->clear();
                }
            },
            [this, lifetime, id, size, time, meta, bytes, ok]() {
                if (lifetime.expired())
                    return;
                if (m_in_flight > 0)
                    --m_in_flight;
                auto c = m_cards.find(id);
                // Only onto the same file: a refresh may have replaced it meanwhile.
                if (c != m_cards.end() && c->second.size == size && c->second.time == time) {
                    FileGridCard& card  = c->second;
                    card.details_loaded = true;
                    if (*ok)
                        card.meta = *meta;
                    if (!bytes->empty()) {
                        card.thumbnail       = std::make_shared<const std::string>(std::move(*bytes));
                        card.thumbnail_state = ThumbnailState::Ready;
                    } else {
                        card.thumbnail.reset();
                        card.thumbnail_state = ThumbnailState::Missing;
                    }
                    notify(FileGridChange::Card, id);
                }
                pump();
            });
    }
    m_pumping = false;
}

void RemoteFileGridModel::DeleteFile(const std::string& id, std::function<void(const pstore::Result&)> done)
{
    if (!m_files || !m_cards.count(id)) {
        if (done)
            done(pstore::Result::failure(m_files ? pstore::Status::NotFound : pstore::Status::NotSupported,
                                 m_files ? "That file is no longer listed." : m_message));
        return;
    }
    auto                     files    = m_files;
    auto                     result   = std::make_shared<pstore::Result>();
    const std::weak_ptr<int> lifetime = m_lifetime;
    m_executor->run([files, id, result]() { *result = files->remove(id); },
                    [this, lifetime, id, result, done]() {
                        if (lifetime.expired())
                            return;
                        // Gone either way: deleted now, or already gone from the printer.
                        if (result->ok() || result->status == pstore::Status::NotFound) {
                            if (m_cards.erase(id)) {
                                m_order.erase(std::remove(m_order.begin(), m_order.end(), id), m_order.end());
                                notify(FileGridChange::Files);
                                if (m_cards.empty() && m_status == FileGridStatus::Ready)
                                    set_status(FileGridStatus::Empty, std::string());
                            }
                        }
                        if (done)
                            done(*result);
                    });
}

void RemoteFileGridModel::QueryPrinter(std::function<void(const pstore::Result&, const std::string&, const std::string&)> done)
{
    if (!m_files) {
        if (done)
            done(pstore::Result::failure(pstore::Status::NotSupported, m_message), std::string(), std::string());
        return;
    }
    auto                     files    = m_files;
    auto                     result   = std::make_shared<pstore::Result>();
    auto                     state    = std::make_shared<std::string>();
    auto                     current  = std::make_shared<std::string>();
    const std::weak_ptr<int> lifetime = m_lifetime;
    m_executor->run([files, result, state, current]() { *result = files->printer_state(*state, *current); },
                    [lifetime, result, state, current, done]() {
                        if (lifetime.expired())
                            return;
                        if (done)
                            done(*result, *state, *current);
                    });
}

void RemoteFileGridModel::StartPrint(const std::string& id, std::function<void(const pstore::Result&)> done)
{
    const FileGridCard* card = FindCard(id);
    if (!m_files || !card || !card->printable) {
        if (done)
            done(pstore::Result::failure(!m_files ? pstore::Status::NotSupported : pstore::Status::NotFound,
                                 !m_files ? m_message : !card ? "That file is no longer listed." : "That file is not G-code the printer can print."));
        return;
    }
    auto                     files    = m_files;
    auto                     result   = std::make_shared<pstore::Result>();
    const std::weak_ptr<int> lifetime = m_lifetime;
    m_executor->run([files, id, result]() { *result = files->start_print(id); },
                    [lifetime, result, done]() {
                        if (lifetime.expired())
                            return;
                        if (done)
                            done(*result);
                    });
}

} // namespace GUI
} // namespace Slic3r
