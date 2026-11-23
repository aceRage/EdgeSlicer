#include "PresetDiffSession.hpp"

#include <algorithm>

#include "Preset.hpp"

namespace Slic3r {

std::string PresetDiffSession::pure_key(const std::string &row_key)
{
    const size_t pos = row_key.find('#');
    return pos == std::string::npos ? row_key : row_key.substr(0, pos);
}

int PresetDiffSession::key_index(const std::string &row_key)
{
    const size_t pos = row_key.find('#');
    if (pos == std::string::npos || pos + 1 >= row_key.size())
        return -1;
    try {
        return std::stoi(row_key.substr(pos + 1));
    } catch (...) {
        return -1;
    }
}

bool configs_identical(const DynamicPrintConfig &a, const DynamicPrintConfig &b)
{
    const t_config_option_keys keys_a = a.keys();
    if (keys_a.size() != b.keys().size())
        return false;
    for (const std::string &key : keys_a) {
        const ConfigOption *oa = a.option(key);
        const ConfigOption *ob = b.option(key);
        if (oa == nullptr || ob == nullptr || oa->type() != ob->type() || !(*oa == *ob))
            return false;
    }
    return true;
}

static size_t extruder_count(const DynamicPrintConfig &cfg)
{
    const auto *opt = cfg.option<ConfigOptionStrings>("extruder_colour");
    return opt == nullptr ? 0 : opt->values.size();
}

std::vector<std::string> preset_diff_keys(const DynamicPrintConfig &left, const DynamicPrintConfig &right, bool deep)
{
    Preset l(Preset::TYPE_INVALID, "left");
    Preset r(Preset::TYPE_INVALID, "right");
    l.config = left;
    r.config = right;
    std::vector<std::string> keys = PresetCollection::dirty_options(&l, &r, deep);
    if (deep) {
        // deep_diff walks the elements of its first config only.
        std::vector<std::string> other = PresetCollection::dirty_options(&r, &l, deep);
        keys.insert(keys.end(), other.begin(), other.end());
    }
    const size_t ext_l = extruder_count(left);
    const size_t ext_r = extruder_count(right);
    if (ext_l != 0 && ext_r != 0 && ext_l != ext_r)
        keys.emplace_back(PresetDiffSession::ExtrudersCountKey);
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    return keys;
}

void PresetDiffSession::reset(const DynamicPrintConfig &left, const DynamicPrintConfig &right, bool deep)
{
    m_left       = left;
    m_right      = right;
    m_left_orig  = left;
    m_right_orig = right;
    m_deep       = deep;
    m_steps.clear();
    m_undo_pos = 0;
    recompute_diff();
    m_initial_diff_keys = m_diff_keys;
}

void PresetDiffSession::recompute_diff() { m_diff_keys = preset_diff_keys(m_left, m_right, m_deep); }

bool PresetDiffSession::differs(const std::string &row_key) const
{
    if (std::binary_search(m_diff_keys.begin(), m_diff_keys.end(), row_key))
        return true;
    if (row_key.find('#') != std::string::npos)
        return false;
    // A plain key differs when any "key#idx" element row does.
    const std::string prefix = row_key + "#";
    auto it = std::lower_bound(m_diff_keys.begin(), m_diff_keys.end(), prefix);
    return it != m_diff_keys.end() && it->compare(0, prefix.size(), prefix) == 0;
}

bool PresetDiffSession::can_copy(const std::string &row_key, Side to) const
{
    if (row_key == ExtrudersCountKey)
        return false;
    const std::string         key  = pure_key(row_key);
    const int                 idx  = key_index(row_key);
    const DynamicPrintConfig &from = config(to == Side::Left ? Side::Right : Side::Left);
    const DynamicPrintConfig &dest = config(to);
    const ConfigOption       *src  = from.option(key);
    if (idx < 0)
        return true; // whole option, also "remove the key" when the source has none
    const ConfigOption *dst = dest.option(key);
    if (src == nullptr || dst == nullptr || !src->is_vector() || !dst->is_vector() || src->type() != dst->type())
        return false;
    const auto *src_vec = static_cast<const ConfigOptionVectorBase *>(src);
    const auto *dst_vec = static_cast<const ConfigOptionVectorBase *>(dst);
    // The target must have the element; the source falls back to its first one, as the diff does.
    return src_vec->size() > 0 && size_t(idx) < dst_vec->size();
}

void PresetDiffSession::set_option(DynamicPrintConfig &cfg, const std::string &key, const ConfigOption *value)
{
    if (value == nullptr)
        cfg.erase(key);
    else
        cfg.set_key_value(key, value->clone());
}

size_t PresetDiffSession::copy(const std::vector<std::string> &row_keys, Side to)
{
    const DynamicPrintConfig &from = config(to == Side::Left ? Side::Right : Side::Left);
    DynamicPrintConfig       &dest = config_mut(to);

    Step   step;
    size_t copied = 0;
    for (const std::string &row_key : row_keys) {
        if (!differs(row_key) || !can_copy(row_key, to))
            continue;
        const std::string key = pure_key(row_key);
        const int         idx = key_index(row_key);

        const ConfigOption           *before_opt = dest.option(key);
        std::unique_ptr<ConfigOption> before(before_opt ? before_opt->clone() : nullptr);
        std::unique_ptr<ConfigOption> after;
        const ConfigOption           *src = from.option(key);
        if (idx < 0) {
            after.reset(src ? src->clone() : nullptr);
        } else {
            after.reset(before_opt->clone());
            static_cast<ConfigOptionVectorBase *>(after.get())->set_at(src, size_t(idx), size_t(idx));
        }
        set_option(dest, key, after.get());
        step.push_back(Op{to, key, std::move(before), std::move(after)});
        ++copied;
    }
    if (step.empty())
        return 0;

    m_steps.resize(m_undo_pos); // a new step drops the redo tail
    m_steps.emplace_back(std::move(step));
    m_undo_pos = m_steps.size();
    recompute_diff();
    return copied;
}

bool PresetDiffSession::undo()
{
    if (!can_undo())
        return false;
    Step &step = m_steps[--m_undo_pos];
    for (auto it = step.rbegin(); it != step.rend(); ++it)
        set_option(config_mut(it->to), it->key, it->before.get());
    recompute_diff();
    return true;
}

bool PresetDiffSession::redo()
{
    if (!can_redo())
        return false;
    Step &step = m_steps[m_undo_pos++];
    for (Op &op : step)
        set_option(config_mut(op.to), op.key, op.after.get());
    recompute_diff();
    return true;
}

bool PresetDiffSession::is_modified(Side side) const
{
    return side == Side::Left ? !configs_identical(m_left, m_left_orig) : !configs_identical(m_right, m_right_orig);
}

std::vector<std::string> PresetDiffSession::changed_keys(Side side) const
{
    const DynamicPrintConfig &cur  = config(side);
    const DynamicPrintConfig &orig = side == Side::Left ? m_left_orig : m_right_orig;
    std::set<std::string>     keys;
    for (const std::string &key : cur.keys()) {
        const ConfigOption *a = cur.option(key);
        const ConfigOption *b = orig.option(key);
        if (b == nullptr || a->type() != b->type() || !(*a == *b))
            keys.insert(key);
    }
    for (const std::string &key : orig.keys())
        if (!cur.has(key))
            keys.insert(key);
    return {keys.begin(), keys.end()};
}

void PresetDiffSession::mark_saved(Side side)
{
    if (side == Side::Left)
        m_left_orig = m_left;
    else
        m_right_orig = m_right;
}

} // namespace Slic3r
