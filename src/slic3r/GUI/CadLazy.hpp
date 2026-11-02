#ifndef slic3r_GUI_CadLazy_hpp_
#define slic3r_GUI_CadLazy_hpp_

// Slim stand-in for Orca's LazyInstance (src/slic3r/GUI/Lazy.hpp) used by DesignPanel.
// Edge already defers notebook pages with LazyPanelHolder; the CAD tab is built the first
// time that holder is shown. DesignPanel still needs the if_built / ensure / when_built
// statics that Orca's IdleScheduler Lazy<> registered, so MainFrame, Plater, and BBLTopbar
// can talk to the panel without owning it. There is no idle prebuild in this port (P3).

#include <functional>
#include <vector>

namespace Slic3r { namespace GUI {

template <class Self>
class LazyInstance
{
public:
    LazyInstance() = default;
    ~LazyInstance()
    {
        if (s_instance == static_cast<Self *>(this))
            s_instance = nullptr;
    }

    LazyInstance(const LazyInstance &)            = delete;
    LazyInstance &operator=(const LazyInstance &) = delete;

    // Call at the end of Self's constructor so when_built callbacks see a fully built object.
    void register_instance()
    {
        s_instance = static_cast<Self *>(this);
        for (auto &fn : s_deferred)
            fn(*s_instance);
        s_deferred.clear();
    }

    // Null until the Design tab's LazyPanelHolder has constructed the panel.
    static Self *if_built() { return s_instance; }
    // Edge builds on first Show via LazyPanelHolder, so this is the live instance or null.
    static Self *ensure() { return s_instance; }
    static void  when_built(std::function<void(Self &)> fn)
    {
        if (s_instance)
            fn(*s_instance);
        else
            s_deferred.push_back(std::move(fn));
    }

private:
    inline static Self *                                 s_instance{nullptr};
    inline static std::vector<std::function<void(Self &)>> s_deferred;
};

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_CadLazy_hpp_
