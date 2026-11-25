#include "SettingsView.hpp"

#include "Preset.hpp"
#include "PrintConfig.hpp"
#include "Format/BambuKeyAliases.hpp"
#include "Format/BambuUiKeys.hpp"

#include <memory>
#include <unordered_set>

namespace Slic3r {

SettingsView settings_view_from_string(const std::string &value)
{
    return value == "bambu" ? SettingsView::Bambu : SettingsView::Default;
}

std::string settings_view_to_string(SettingsView view)
{
    return view == SettingsView::Bambu ? "bambu" : "";
}

ViewChoice view_choice(const std::string &user_mode, const std::string &settings_view)
{
    if (settings_view_from_string(settings_view) == SettingsView::Bambu)
        return ViewChoice::Bambu;
    return user_mode == "advanced" || user_mode == "develop" ? ViewChoice::Advanced : ViewChoice::Basic;
}

ViewChoiceConfig view_choice_config(ViewChoice choice)
{
    switch (choice) {
    case ViewChoice::Basic: return { "simple", "" };
    case ViewChoice::Advanced: return { "advanced", "" };
    case ViewChoice::Bambu: break;
    }
    return { "advanced", settings_view_to_string(SettingsView::Bambu) };
}

ConfigOptionMode effective_level(ViewChoice choice, bool developer_mode)
{
    if (choice == ViewChoice::Basic)
        return comSimple;
    return developer_mode ? comDevelop : comAdvanced;
}

bool migrate_legacy_developer_user_mode(std::string &user_mode, bool &developer_mode)
{
    if (user_mode != "develop")
        return false;
    user_mode      = "advanced";
    developer_mode = true;
    return true;
}

namespace SettingsViewKeys {

std::string base_key(const std::string &opt_id)
{
    const size_t hash = opt_id.find('#');
    return hash == std::string::npos ? opt_id : opt_id.substr(0, hash);
}

namespace {

// Bambu key -> our key: a BambuKeyAliases rename, an inferred equivalent, or the same name.
std::string our_key_for(const std::string &bambu_key)
{
    if (const BambuKeyAliases::Alias *alias = BambuKeyAliases::by_bambu(bambu_key))
        return alias->ours;
    for (const BambuUiKeys::InferredEquivalent &e : BambuUiKeys::inferred_equivalents())
        if (bambu_key == e.bambu)
            return e.ours;
    return bambu_key;
}

const std::unordered_set<std::string> &bambu_list()
{
    static const std::unordered_set<std::string> keys = [] {
        std::unordered_set<std::string> out;
        for (const BambuUiKeys::Entry &e : BambuUiKeys::entries())
            if (e.level != BambuUiKeys::Level::Develop)
                out.insert(our_key_for(e.key));
        return out;
    }();
    return keys;
}

const std::unordered_set<std::string> &bbl_hidden()
{
    static const std::unordered_set<std::string> keys = [] {
        std::unordered_set<std::string> out;
        for (const char *k : BambuUiKeys::bbl_printer_hidden())
            out.insert(our_key_for(k));
        return out;
    }();
    return keys;
}

} // namespace

bool on_bambu_list(const std::string &our_key) { return bambu_list().count(our_key) != 0; }
bool hidden_by_bambu_on_bbl(const std::string &our_key) { return bbl_hidden().count(our_key) != 0; }
size_t bambu_list_size() { return bambu_list().size(); }

} // namespace SettingsViewKeys

LineShow line_show(const ViewFilter &filter, const std::vector<std::string> &option_ids, ConfigOptionMode line_mode)
{
    if (!filter.bambu())
        return line_mode <= filter.level ? LineShow::Shown : LineShow::Hidden;

    // Bambu view. Our Developer lines are never part of it (support_air_filtration is Advanced in Bambu
    // Studio but Developer here); with Developer mode on, one still shows as an exception (changed or
    // revealed), like any line outside Bambu Studio's list. Without Developer mode it stays hidden.
    const bool develop_line = line_mode == comDevelop;
    if (develop_line && filter.level < comDevelop)
        return LineShow::Hidden;

    bool listed = false, exception = false;
    for (const std::string &id : option_ids) {
        const std::string key = SettingsViewKeys::base_key(id);
        // On a Bambu Lab printer, a line Bambu Studio hides there counts as not listed: it still shows
        // (with the chip) once changed, so a value nobody can see is never left in force.
        if (SettingsViewKeys::on_bambu_list(key) && !(filter.bbl_printer && SettingsViewKeys::hidden_by_bambu_on_bbl(key)))
            listed = true;
        else if ((filter.changed != nullptr && filter.changed->count(key) != 0) ||
                 (filter.revealed != nullptr && filter.revealed->count(key) != 0))
            exception = true;
    }
    if (listed && !develop_line)
        return LineShow::Shown;
    return exception ? LineShow::ShownNotInBambu : LineShow::Hidden;
}

LineShow widget_line_show(const ViewFilter &filter, bool group_has_visible_option_line)
{
    if (!filter.bambu())
        return comSimple <= filter.level ? LineShow::Shown : LineShow::Hidden;
    return group_has_visible_option_line ? LineShow::Shown : LineShow::Hidden;
}

bool group_visible(const ViewFilter &filter, const std::vector<ConfigOptionMode> &line_modes,
                   const std::vector<std::vector<std::string>> &line_ids)
{
    if (line_modes.empty())
        return true;
    static const std::vector<std::string> no_ids;
    for (size_t i = 0; i < line_modes.size(); ++i)
        if (shown(line_show(filter, i < line_ids.size() ? line_ids[i] : no_ids, line_modes[i])))
            return true;
    return false;
}

const Preset *settings_view_reference(const PresetCollection &presets)
{
    const Preset *parent = presets.get_selected_preset_parent();
    if (parent == nullptr)
        return nullptr; // external preset with no "inherits": no system preset to compare with
    if (parent == &presets.get_selected_preset() && !parent->is_system && !parent->is_default)
        return nullptr; // root user preset: its own base, every saved value would look unchanged
    return parent;
}

std::set<std::string> settings_view_changed_options(const PresetCollection &presets, bool deep_compare)
{
    std::set<std::string> out;
    if (settings_view_reference(presets) != nullptr) {
        // Same list the "modified" label colour and the undo-to-system arrow use.
        for (const std::string &key : presets.current_different_from_parent_options(deep_compare))
            out.insert(SettingsViewKeys::base_key(key));
        return out;
    }
    return options_differing_from_defaults(presets.get_edited_preset().config);
}

std::set<std::string> options_differing_from_defaults(const ConfigBase &config)
{
    std::set<std::string> out;
    const ConfigDef *def = config.def();
    if (def == nullptr)
        def = &print_config_def;
    for (const std::string &key : config.keys()) {
        const ConfigOptionDef *opt_def = def->get(key);
        const ConfigOption    *value   = config.option(key);
        if (opt_def == nullptr || value == nullptr || !opt_def->default_value)
            continue;
        const ConfigOption *dflt = opt_def->default_value.get();
        if (value->type() != dflt->type())
            continue; // operator== throws on a type mismatch; such a key is not a setting line anyway
        if (value->is_vector() && dflt->is_vector()) {
            const auto *vec = static_cast<const ConfigOptionVectorBase *>(value);
            std::unique_ptr<ConfigOption> sized(dflt->clone());
            if (static_cast<const ConfigOptionVectorBase *>(dflt)->size() > 0) {
                // Default vectors hold one value per extruder / filament; stretch to the edited length.
                static_cast<ConfigOptionVectorBase *>(sized.get())->clear();
                static_cast<ConfigOptionVectorBase *>(sized.get())->resize(vec->size(), dflt);
            }
            if (!(*value == *sized))
                out.insert(key);
        } else if (!(*value == *dflt)) {
            out.insert(key);
        }
    }
    return out;
}

void add_override_keys(const ConfigBase &item_config, std::set<std::string> &out)
{
    for (const std::string &key : item_config.keys())
        out.insert(SettingsViewKeys::base_key(key));
}

} // namespace Slic3r
