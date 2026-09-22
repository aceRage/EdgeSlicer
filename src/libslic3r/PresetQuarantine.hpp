#ifndef slic3r_PresetQuarantine_hpp_
#define slic3r_PresetQuarantine_hpp_

#include <string>
#include <vector>

namespace Slic3r {

// Upstream Bambu (commit 2b6937acbe) answers "this preset file did not parse" by deleting the
// user's .json and its sibling .info from disk. That is silent, unrecoverable data loss: on
// 2026-09-21 it destroyed 208 of this user's presets after a type mismatch made deserialize()
// throw. A preset the loader cannot read is not the same thing as a preset the user asked to
// throw away, and only the user can tell the difference.
//
// So instead of removing the file we MOVE it out of the way, into a sibling "unloadable"
// directory that mirrors the relative name. Why a directory and not an in-place ".unloadable"
// suffix: load_presets() iterates the preset directory itself, and the preset directories are
// also walked by the cloud-sync and bundle-export code. A renamed file left in place keeps
// showing up in those scans and in the user's own folder listing; a sibling directory is
// scanned by nobody, is trivially explained to the user as one path, and can be zipped up and
// sent to a developer as-is. It sits next to the presets rather than in the temp dir so it
// survives a reboot and travels with a backup of the config directory.
//
// Behaviour preserved from before: the preset still does not load, and startup still succeeds.
namespace PresetQuarantine {

// Name of the sibling directory that holds files the loader could not parse.
extern const char *dir_name;

struct Entry {
    // Absolute path the file used to live at.
    std::string original_path;
    // Absolute path it was moved to.
    std::string quarantined_path;
    // Why the loader gave up on it, for the log and the report.
    std::string reason;
};

// Move file_path, and its sibling .info if one exists, into <preset dir>/unloadable/.
//
// The destination keeps the original filename. If a file of that name is already quarantined
// from an earlier run, a numeric suffix is appended (name.json, name.1.json, name.2.json, ...)
// rather than overwriting it - the earlier casualty is evidence too, and clobbering it would
// reintroduce exactly the data loss this function exists to prevent. The .json and its .info
// always take the SAME suffix so the pair stays together.
//
// Returns the entry describing the move. On failure to move (permissions, cross-device, a
// read-only medium) the file is LEFT WHERE IT IS - never deleted as a fallback - and the
// returned entry has an empty quarantined_path.
Entry quarantine(const std::string &file_path, const std::string &reason);

// Collected across one loader pass so the GUI can report a count and one location.
class Report {
public:
    void        add(Entry entry) { m_entries.emplace_back(std::move(entry)); }
    bool        empty() const { return m_entries.empty(); }
    size_t      size() const { return m_entries.size(); }
    const std::vector<Entry>& entries() const { return m_entries; }
    // The directory the files were moved to. Empty when nothing was quarantined, or when every
    // move failed. When presets from more than one directory were quarantined this is the first
    // one; the per-entry paths carry the rest.
    std::string directory() const;
    void        append(const Report &other);
    void        clear() { m_entries.clear(); }

private:
    std::vector<Entry> m_entries;
};

// Process-wide accumulator. load_presets() runs from several places (startup, the config
// wizard, the preset updater, a user switch) and each of them already has its own
// PresetsConfigSubstitutions to carry; rather than thread a new out-parameter through all of
// them and every overload in between, quarantine events land here and the GUI drains them at
// the same moment it shows the substitutions dialog. Guarded by a mutex because the preset
// loader parses vendor files on a worker pool.
void   record(Entry entry);
// Returns everything accumulated since the last call and clears the accumulator.
Report take();
// Look without draining (tests, logging).
Report peek();

} // namespace PresetQuarantine
} // namespace Slic3r

#endif // slic3r_PresetQuarantine_hpp_
