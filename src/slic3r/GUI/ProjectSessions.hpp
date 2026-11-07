#ifndef slic3r_GUI_ProjectSessions_hpp_
#define slic3r_GUI_ProjectSessions_hpp_

// The open projects of one window (project strip, tests/design_multi_project_sidebar.md 4.1), kept free of wxWidgets so
// the rules are unit tested (tests/slic3rutils/project_sessions_tests.cpp): open with the same file selecting its slot,
// "Untitled 2" naming, the cap on open projects, which slot becomes active when one closes, next / previous with wrap,
// and the unsaved ones the app-close dialog lists. The GUI side (ProjectStrip) owns the files and the switch itself.

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r {
namespace GUI {

struct ProjectSlot
{
    enum class State { Active, Parked };

    int         id{ 0 };
    std::string name;          // shown in the strip (UTF-8)
    std::string origin_path;   // the project's own file; empty = never saved (Untitled)
    std::string snapshot_path; // the parked 3MF; empty until the slot was parked once
    // Valid plates' G-code parked next to the snapshot: plate index, file.
    std::vector<std::pair<int, std::string>> plate_gcodes;
    bool          dirty{ false };
    bool          empty{ true };     // no objects (an empty Untitled is reused by New / Open)
    int           plate{ 0 };        // current plate when parked
    int           main_tab{ 1 };     // MainTabs::Editor3D, Preview or Showcase when parked
    std::uint64_t last_used{ 0 };
    State         state{ State::Parked };
};

class ProjectSessions
{
public:
    static constexpr int default_max_open = 8;
    static constexpr int min_max_open     = 2;
    static constexpr int max_max_open     = 16;

    explicit ProjectSessions(int max_open = default_max_open) { set_max_open(max_open); }

    // D3: 8 by default, 2-16 from the app config ("project_strip_max_open"); anything else is the default.
    static int clamp_max_open(int v) { return std::max(min_max_open, std::min(max_max_open, v)); }
    static int parse_max_open(const std::string &text)
    {
        if (text.empty() || text.size() > 3 || !std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; }))
            return default_max_open;
        return clamp_max_open(std::stoi(text));
    }

    int  max_open() const { return m_max_open; }
    void set_max_open(int v) { m_max_open = clamp_max_open(v); }

    const std::vector<ProjectSlot> &slots() const { return m_slots; }
    size_t                          size() const { return m_slots.size(); }
    bool                            empty() const { return m_slots.empty(); }
    bool                            can_add() const { return int(m_slots.size()) < m_max_open; }

    int index_of(int id) const
    {
        for (size_t i = 0; i < m_slots.size(); ++i)
            if (m_slots[i].id == id)
                return int(i);
        return -1;
    }
    ProjectSlot *find(int id)
    {
        const int i = index_of(id);
        return i < 0 ? nullptr : &m_slots[size_t(i)];
    }
    const ProjectSlot *find(int id) const
    {
        const int i = index_of(id);
        return i < 0 ? nullptr : &m_slots[size_t(i)];
    }

    int                active_id() const { return m_active_id; }
    int                active_index() const { return index_of(m_active_id); }
    ProjectSlot       *active() { return find(m_active_id); }
    const ProjectSlot *active() const { return find(m_active_id); }

    // Two spellings of one file are one project: separators and, on Windows, case do not count.
    static std::string normalized_path(const std::string &path, bool case_insensitive)
    {
        std::string out = path;
        for (char &c : out) {
            if (c == '\\')
                c = '/';
            if (case_insensitive && c >= 'A' && c <= 'Z')
                c = char(c - 'A' + 'a');
        }
        while (out.size() > 1 && out.back() == '/')
            out.pop_back();
        return out;
    }

    // The slot holding this file, or -1. An empty path (Untitled) never matches.
    int find_by_origin(const std::string &path, bool case_insensitive = default_case_insensitive()) const
    {
        if (path.empty())
            return -1;
        const std::string key = normalized_path(path, case_insensitive);
        for (const ProjectSlot &s : m_slots)
            if (!s.origin_path.empty() && normalized_path(s.origin_path, case_insensitive) == key)
                return s.id;
        return -1;
    }

    // "Untitled", then "Untitled 2", "Untitled 3"...: the lowest number no open slot shows.
    std::string next_untitled_name(const std::string &base) const
    {
        auto taken = [this](const std::string &name) {
            return std::any_of(m_slots.begin(), m_slots.end(), [&](const ProjectSlot &s) { return s.name == name; });
        };
        if (!taken(base))
            return base;
        for (int n = 2;; ++n) {
            const std::string name = base + " " + std::to_string(n);
            if (!taken(name))
                return name;
        }
    }

    // A new slot right after the active one (at the end when none is active). Its id, or -1 when the cap is reached.
    // It is not selected: the switch parks the active project first.
    int add(const std::string &origin_path, const std::string &name)
    {
        if (!can_add())
            return -1;
        ProjectSlot slot;
        slot.id          = m_next_id++;
        slot.name        = name;
        slot.origin_path = origin_path;
        slot.last_used   = ++m_clock;
        const int at     = active_index();
        if (at < 0)
            m_slots.push_back(std::move(slot));
        else
            m_slots.insert(m_slots.begin() + at + 1, std::move(slot));
        return m_next_id - 1;
    }

    // Makes the slot the active one; the previous active one becomes Parked.
    bool select(int id)
    {
        ProjectSlot *slot = find(id);
        if (slot == nullptr)
            return false;
        if (ProjectSlot *old = active(); old != nullptr && old != slot)
            old->state = ProjectSlot::State::Parked;
        slot->state     = ProjectSlot::State::Active;
        slot->last_used = ++m_clock;
        m_active_id     = id;
        return true;
    }

    // The slot `step` places from the active one, wrapping (Ctrl+PgDn = +1, Ctrl+PgUp = -1). -1 with fewer than two slots.
    int neighbour_id(int step) const
    {
        const int n = int(m_slots.size());
        const int i = active_index();
        if (n < 2 || i < 0)
            return -1;
        const int j = ((i + step) % n + n) % n;
        return j == i ? -1 : m_slots[size_t(j)].id;
    }

    // The slot that becomes active when `id` closes: the next one, else the previous one; -1 when it is the last slot.
    int successor_on_close(int id) const
    {
        const int i = index_of(id);
        if (i < 0 || m_slots.size() < 2)
            return -1;
        return size_t(i + 1) < m_slots.size() ? m_slots[size_t(i + 1)].id : m_slots[size_t(i - 1)].id;
    }

    // Removes the slot. Closing the active one leaves no active slot: the caller loads successor_on_close() first.
    bool close(int id)
    {
        const int i = index_of(id);
        if (i < 0)
            return false;
        m_slots.erase(m_slots.begin() + i);
        if (m_active_id == id)
            m_active_id = -1;
        return true;
    }

    // Moves a slot to another place in the strip (drag reorder is P3; kept here with the other rules).
    bool move(int id, int new_index)
    {
        const int i = index_of(id);
        if (i < 0 || new_index < 0 || new_index >= int(m_slots.size()))
            return false;
        ProjectSlot slot = std::move(m_slots[size_t(i)]);
        m_slots.erase(m_slots.begin() + i);
        m_slots.insert(m_slots.begin() + new_index, std::move(slot));
        return true;
    }

    void set_dirty(int id, bool dirty)
    {
        if (ProjectSlot *slot = find(id))
            slot->dirty = dirty;
    }

    // The projects the app-close dialog lists, in strip order.
    std::vector<int> dirty_ids() const
    {
        std::vector<int> out;
        for (const ProjectSlot &s : m_slots)
            if (s.dirty)
                out.push_back(s.id);
        return out;
    }

    // The display name for a file: its name without the folder and the extension (".3mf", ".gcode.3mf" stays ".gcode").
    static std::string name_from_path(const std::string &path)
    {
        const size_t slash = path.find_last_of("/\\");
        std::string  name  = slash == std::string::npos ? path : path.substr(slash + 1);
        const size_t dot   = name.find_last_of('.');
        if (dot != std::string::npos && dot > 0)
            name.erase(dot);
        return name;
    }

    static constexpr bool default_case_insensitive()
    {
#ifdef _WIN32
        return true;
#else
        return false;
#endif
    }

private:
    std::vector<ProjectSlot> m_slots;
    int                      m_active_id{ -1 };
    int                      m_next_id{ 1 };
    int                      m_max_open{ default_max_open };
    std::uint64_t            m_clock{ 0 };
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_ProjectSessions_hpp_
