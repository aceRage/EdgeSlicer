#ifndef slic3r_GUI_CadLazy_hpp_
#define slic3r_GUI_CadLazy_hpp_

// Slim stand-in for Orca's LazyInstance (src/slic3r/GUI/Lazy.hpp) used by DesignPanel.
// Edge already defers notebook pages with LazyPanelHolder; the CAD tab is built the first
// time that holder is shown. DesignPanel still needs the if_built / ensure / when_built
// statics that Orca's IdleScheduler Lazy<> registered, so MainFrame, Plater, and BBLTopbar
// can talk to the panel without owning it. There is no idle prebuild in this port (P3).

#include <algorithm>
#include <cstddef>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r { namespace GUI {

template <class Self>
class LazyInstance
{
public:
    LazyInstance() = default;
    ~LazyInstance()
    {
        if (s_instance == static_cast<Self *>(this)) {
            s_instance = nullptr;
            s_deferred.clear();   // nothing pending may outlive the panel
        }
    }

    LazyInstance(const LazyInstance &)            = delete;
    LazyInstance &operator=(const LazyInstance &) = delete;

    // Call at the end of Self's constructor so when_built callbacks see a fully built object.
    void register_instance()
    {
        s_instance = static_cast<Self *>(this);
        auto pending = std::move(s_deferred);
        s_deferred.clear();
        for (auto &d : pending)
            d.second(*s_instance);
    }

    // Null until the Design tab's LazyPanelHolder has constructed the panel.
    static Self *if_built() { return s_instance; }
    // Edge builds on first Show via LazyPanelHolder, so this is the live instance or null.
    static Self *ensure() { return s_instance; }
    // Runs fn now if the panel exists, else when it is built. Deferred callbacks are keyed:
    // a second request with the same key replaces the pending one (latest wins), so the
    // queue is bounded by the number of distinct keys even if the tab is never shown.
    static void when_built(const std::string &key, std::function<void(Self &)> fn)
    {
        if (s_instance) {
            fn(*s_instance);
            return;
        }
        for (auto &d : s_deferred)
            if (d.first == key) {
                d.second = std::move(fn);
                return;
            }
        s_deferred.emplace_back(key, std::move(fn));
    }
    // Forget a pending callback (e.g. a launch action when the document is cleared).
    static void drop_deferred(const std::string &key)
    {
        s_deferred.erase(std::remove_if(s_deferred.begin(), s_deferred.end(),
                                        [&key](const auto &d) { return d.first == key; }),
                         s_deferred.end());
    }
    static void        drop_all_deferred() { s_deferred.clear(); }
    static std::size_t deferred_count() { return s_deferred.size(); }

private:
    inline static Self *s_instance{nullptr};
    inline static std::vector<std::pair<std::string, std::function<void(Self &)>>> s_deferred;
};

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_CadLazy_hpp_
