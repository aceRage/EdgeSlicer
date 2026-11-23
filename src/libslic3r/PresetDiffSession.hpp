#pragma once

// Edge: the working state behind the Compare presets dialog (WinMerge style).
// Holds a left and a right working copy of two preset configs, copies single option values
// (whole options or one "key#idx" element of a per-extruder vector) from one side to the other,
// keeps an undo/redo stack of those copies and re-computes which rows still differ.
// It has no wx dependency, so the copy / undo / re-diff rules are unit tested.

#include <memory>
#include <set>
#include <string>
#include <vector>

#include "Config.hpp"
#include "PrintConfig.hpp"

namespace Slic3r {

class PresetDiffSession
{
public:
    enum class Side { Left, Right };

    // Synthetic row of the printer compare: the extruder count. It is shown but never copied.
    static constexpr const char *ExtrudersCountKey = "extruders_count";

    PresetDiffSession() = default;

    // Starts over with two configs. deep = per-extruder ("key#idx") rows, as the dialog uses for
    // printer and filament presets. Drops the undo history and the modified flags.
    void reset(const DynamicPrintConfig &left, const DynamicPrintConfig &right, bool deep);

    const DynamicPrintConfig &config(Side side) const { return side == Side::Left ? m_left : m_right; }

    // Rows that differ now: keys like "layer_height" or "nozzle_diameter#1", sorted.
    const std::vector<std::string> &diff_keys() const { return m_diff_keys; }
    // Row keys that differed when the session was (re)started; never shrinks after a copy, so a
    // row that became equal can still be shown as "now the same".
    const std::vector<std::string> &initial_diff_keys() const { return m_initial_diff_keys; }
    // True when the row (a plain key, or "key#idx") differs now. A plain key differs when any of
    // its elements does.
    bool differs(const std::string &row_key) const;

    // Can the row's value be copied onto `to`? False for the synthetic extruder count and for an
    // element the target vector does not have.
    bool can_copy(const std::string &row_key, Side to) const;

    // Copies the rows' values onto `to` as one undo step. Rows that are equal or cannot be copied
    // are skipped. Returns the number of rows copied.
    size_t copy(const std::vector<std::string> &row_keys, Side to);

    bool can_undo() const { return m_undo_pos > 0; }
    bool can_redo() const { return m_undo_pos < m_steps.size(); }
    bool undo();
    bool redo();

    // The side's working config differs from the config it was started (or last saved) with.
    bool is_modified(Side side) const;
    bool is_modified() const { return is_modified(Side::Left) || is_modified(Side::Right); }
    // The side was written to disk: its working config becomes its new original.
    void mark_saved(Side side);
    // Pure option keys whose value (or presence) on the side differs from its original: what a
    // save has to write onto the stored preset.
    std::vector<std::string> changed_keys(Side side) const;

    // Strips "#idx" from a row key.
    static std::string pure_key(const std::string &row_key);
    // The element index of a "key#idx" row key, or -1 for a plain key.
    static int key_index(const std::string &row_key);

private:
    struct Op
    {
        Side                          to;
        std::string                   key; // pure key
        std::unique_ptr<ConfigOption> before; // nullptr = the key was absent
        std::unique_ptr<ConfigOption> after;  // nullptr = the key is removed
    };
    using Step = std::vector<Op>;

    DynamicPrintConfig &config_mut(Side side) { return side == Side::Left ? m_left : m_right; }
    static void        set_option(DynamicPrintConfig &cfg, const std::string &key, const ConfigOption *value);
    void               recompute_diff();

    DynamicPrintConfig       m_left;
    DynamicPrintConfig       m_right;
    DynamicPrintConfig       m_left_orig;
    DynamicPrintConfig       m_right_orig;
    bool                     m_deep {false};
    std::vector<std::string> m_diff_keys;
    std::vector<std::string> m_initial_diff_keys;
    std::vector<Step>        m_steps;
    size_t                   m_undo_pos {0};
};

// Both configs hold the same keys with equal values.
bool configs_identical(const DynamicPrintConfig &a, const DynamicPrintConfig &b);

// Row keys where two configs differ, as the Compare presets dialog lists them: the union of
// PresetCollection::dirty_options() in both directions (so element rows of the longer vector are
// not missed), plus "extruders_count" when the configs have a different number of extruders.
std::vector<std::string> preset_diff_keys(const DynamicPrintConfig &left, const DynamicPrintConfig &right, bool deep);

} // namespace Slic3r
