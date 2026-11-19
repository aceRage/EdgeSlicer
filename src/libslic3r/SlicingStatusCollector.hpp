#pragma once

// Thread-safe collector for slicing statuses (warnings) raised through a PrintBase status callback.
//
// PrintBase::set_status() / status_update_warnings() invoke the registered callback on whatever thread
// raises the status. Print::process() runs several steps (PrintObject::generate_support_material()
// among them) from a tbb::parallel_for over the objects, so the callback runs concurrently on TBB
// workers. A callback that appends to a plain std::vector corrupts the heap: the process either
// crashes (0xC0000005) or deadlocks on the heap lock. The CLI's callbacks used to do exactly that
// with a global vector; they now append here instead.
//
// The Mutex parameter exists for the regression test only (it instantiates a lock-free variant to show
// the failure). Production code uses SlicingStatusCollector.

#include "PrintBase.hpp"

#include <iterator>
#include <mutex>
#include <utility>
#include <vector>

namespace Slic3r {

template<class Mutex>
class BasicSlicingStatusCollector
{
public:
    using Status = PrintBase::SlicingStatus;

    // Safe to call from any thread.
    void add(const Status &status)
    {
        std::lock_guard<Mutex> lock(m_mutex);
        m_items.push_back(status);
    }

    bool empty() const
    {
        std::lock_guard<Mutex> lock(m_mutex);
        return m_items.empty();
    }

    size_t size() const
    {
        std::lock_guard<Mutex> lock(m_mutex);
        return m_items.size();
    }

    // Removes and returns everything collected so far, in arrival order. The caller may then walk the
    // snapshot without holding any lock.
    std::vector<Status> take()
    {
        std::lock_guard<Mutex> lock(m_mutex);
        std::vector<Status> out;
        out.swap(m_items);
        return out;
    }

    // Puts entries previously removed with take() back in front of anything collected since.
    void restore_front(std::vector<Status> &&kept)
    {
        if (kept.empty())
            return;
        std::lock_guard<Mutex> lock(m_mutex);
        m_items.insert(m_items.begin(), std::make_move_iterator(kept.begin()), std::make_move_iterator(kept.end()));
    }

private:
    mutable Mutex       m_mutex;
    std::vector<Status> m_items;
};

using SlicingStatusCollector = BasicSlicingStatusCollector<std::mutex>;

} // namespace Slic3r
