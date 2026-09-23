#include <catch2/catch.hpp>

#include "libslic3r/BambuConfigCompat.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Semver.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/filesystem.hpp>
#include <boost/nowide/convert.hpp>

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::BambuConfigCompat;

// Bambu Studio's per-object / per-part / layer-range settings in a 3MF (model_settings.config,
// layer_config_ranges.xml) carry one slot per print_extruder_variant and write "nil" into the
// slots the override does not apply to:
//     <metadata key="outer_wall_speed" value="80,nil,80,nil"/>
// Bambu resolves such a slot from its parent (ConfigOptionVector::set_to_index: the print
// settings, or the enclosing object). This fork applies an override as a whole vector, so the
// reader resolves nil slots at load time from the same parents. Before this change the value
// went straight into ConfigOptionFloats::deserialize and the whole project failed to load with
// "Deserializing nil into a non-nullable object".
//
// See docs/bambu-config-compat.md, "Overrides".

static std::vector<std::string> V(std::initializer_list<const char*> items)
{
    std::vector<std::string> out;
    for (const char *s : items)
        out.emplace_back(s);
    return out;
}

static std::vector<double> floats_of(const ConfigBase &cfg, const std::string &key)
{
    const auto *opt = cfg.option<ConfigOptionFloats>(key);
    return opt ? opt->values : std::vector<double>{};
}

static std::vector<double> floats_of(const ModelConfig &cfg, const std::string &key)
{
    return floats_of(cfg.get(), key);
}

// ---------------------------------------------------------------------------------------------
// translate_nil_override: the pure rule.
// ---------------------------------------------------------------------------------------------

TEST_CASE("an override's nil slot in the middle takes the parent's slot", "[BambuCompat][BambuOverride]")
{
    const ConfigOptionDef *def = print_config_def.get("outer_wall_speed");
    REQUIRE(def != nullptr);
    const NilResult r = translate_nil_override(def, V({"80", "nil", "80", "nil"}), { V({"200", "250", "200", "250"}) });
    CHECK(r.fix == NilFix::Inherited);
    CHECK_FALSE(r.lossy());
    CHECK(r.value == "80,250,80,250");
    CHECK(r.original == "80,nil,80,nil");
}

TEST_CASE("an override with nil in slot 0 does not leak into the slot the slicer reads", "[BambuCompat][BambuOverride]")
{
    // The fork reads slot 0 for a standard-flow filament. Bambu's override did not cover that
    // variant, so slot 0 must be the parent's value, not the override's.
    const NilResult r = translate_nil_override(print_config_def.get("sparse_infill_speed"),
                                               V({"nil", "90", "nil", "90"}), { V({"350", "600", "350", "600"}) });
    CHECK(r.fix == NilFix::Inherited);
    CHECK(r.value == "350,90,350,90");
}

TEST_CASE("the nearest parent wins, and a parent without the slot is skipped", "[BambuCompat][BambuOverride]")
{
    const ConfigOptionDef *def = print_config_def.get("inner_wall_speed");
    // Object override (nearest) covers slot 1 only; project covers everything.
    const NilResult r = translate_nil_override(def, V({"80", "nil", "80", "nil"}),
                                               { V({"100", "120", "100", "120"}), V({"300", "300", "300", "300"}) });
    CHECK(r.value == "80,120,80,120");
    // A parent whose slot count differs cannot be lined up slot by slot and is skipped.
    const NilResult r2 = translate_nil_override(def, V({"80", "nil", "80", "nil"}),
                                                { V({"100"}), V({"300", "310", "300", "310"}) });
    CHECK(r2.fix == NilFix::Inherited);
    CHECK(r2.value == "80,310,80,310");
}

TEST_CASE("an all-nil override is no override: the key is dropped, not zeroed", "[BambuCompat][BambuOverride]")
{
    const NilResult r = translate_nil_override(print_config_def.get("outer_wall_speed"), V({"nil", "nil", "nil", "nil"}),
                                               { V({"200", "200", "200", "200"}) });
    CHECK(r.fix == NilFix::AllNil);
    CHECK(r.drop());
    CHECK_FALSE(r.lossy());
    CHECK(r.value.empty());
}

TEST_CASE("an override without a usable parent is backfilled from itself and reported lossy", "[BambuCompat][BambuOverride]")
{
    const ConfigOptionDef *def = print_config_def.get("outer_wall_speed");
    // No parent at all.
    NilResult r = translate_nil_override(def, V({"80", "nil", "80", "nil"}), {});
    CHECK(r.fix == NilFix::BackfilledLossy);
    CHECK(r.lossy());
    CHECK(r.value == "80,80,80,80");
    // Parent with the wrong slot count.
    r = translate_nil_override(def, V({"nil", "70", "nil"}), { V({"200", "200"}) });
    CHECK(r.fix == NilFix::BackfilledLossy);
    CHECK(r.value == "70,70,70");
    // Partly inheritable: slots the parent knows are inherited, the rest guessed - still lossy.
    r = translate_nil_override(def, V({"80", "nil", "80", "nil"}), { V({"200", "nil", "200", "250"}) });
    CHECK(r.fix == NilFix::BackfilledLossy);
    CHECK(r.value == "80,80,80,250");
}

TEST_CASE("an override never invents a zero", "[BambuCompat][BambuOverride]")
{
    for (const char *key : { "outer_wall_speed", "inner_wall_speed", "sparse_infill_speed", "internal_solid_infill_speed",
                             "top_surface_speed", "outer_wall_acceleration", "travel_speed" }) {
        const ConfigOptionDef *def = print_config_def.get(key);
        REQUIRE(def != nullptr);
        for (const auto &parents : { std::vector<std::vector<std::string>>{}, { V({"150", "160", "170"}) } }) {
            const NilResult r = translate_nil_override(def, V({"nil", "45", "nil"}), parents);
            INFO(key << " -> " << r.value);
            REQUIRE(r.changed());
            std::vector<std::string> slots;
            boost::split(slots, r.value, boost::is_any_of(","));
            CHECK(slots.size() == 3);
            for (const std::string &slot : slots) {
                CHECK(slot != "nil");
                CHECK(std::atof(slot.c_str()) > 0.);
            }
        }
    }
}

TEST_CASE("nullable and string options are never rewritten as overrides", "[BambuCompat][BambuOverride]")
{
    const ConfigOptionDef *nullable = print_config_def.get("filament_retraction_length");
    REQUIRE(nullable != nullptr);
    REQUIRE(nullable->nullable);
    CHECK(translate_nil_override(nullable, V({"nil", "0.8"}), { V({"1", "1"}) }).fix == NilFix::None);
    CHECK(translate_nil_override(print_config_def.get("filament_notes"), V({"nil"}), {}).fix == NilFix::None);
    CHECK(translate_nil_override(nullptr, V({"nil"}), {}).fix == NilFix::None);
    CHECK(translate_nil_override(print_config_def.get("outer_wall_speed"), V({"80", "90"}), {}).fix == NilFix::None);
}

static const ConfigOptionDef* live_scalar_float_key()
{
    for (const char *key : { "travel_speed_z", "top_solid_infill_flow_ratio", "nozzle_volume", "flush_multiplier" }) {
        const ConfigOptionDef *def = print_config_def.get(key);
        if (def != nullptr && def->is_scalar() && def->type == coFloat && ! def->nullable)
            return def;
    }
    return nullptr;
}

TEST_CASE("a scalar override is clean only when the uncovered variants would inherit the same value", "[BambuCompat][BambuOverride]")
{
    const ConfigOptionDef *def = live_scalar_float_key();
    REQUIRE(def != nullptr);
    NilResult r = translate_nil_override(def, V({"12", "nil", "12", "nil"}), { V({"12"}) });
    CHECK(r.fix == NilFix::Collapsed);
    CHECK(r.value == "12");
    r = translate_nil_override(def, V({"12", "nil", "12", "nil"}), { V({"20"}) });
    CHECK(r.fix == NilFix::CollapsedLossy);
    CHECK(r.value == "12");
    r = translate_nil_override(def, V({"12", "nil", "12", "nil"}), {});
    CHECK(r.fix == NilFix::CollapsedLossy);
}

TEST_CASE("split_nil_value only splits values that can carry a Bambu nil", "[BambuCompat][BambuOverride]")
{
    std::vector<std::string> slots;
    CHECK(split_nil_value(*print_config_def.get("outer_wall_speed"), "80,nil, 80 ,nil", slots));
    CHECK(slots == V({"80", "nil", "80", "nil"}));
    CHECK_FALSE(split_nil_value(*print_config_def.get("outer_wall_speed"), "80,90", slots));
    CHECK(slots.empty());
    CHECK_FALSE(split_nil_value(*print_config_def.get("outer_wall_speed"), "80,nil_x", slots));
    CHECK_FALSE(split_nil_value(*print_config_def.get("filament_retraction_length"), "nil,0.8", slots));
    CHECK_FALSE(split_nil_value(*print_config_def.get("filament_notes"), "nil", slots));
}

// ---------------------------------------------------------------------------------------------
// The shared deserialization layer: ConfigBase::set_deserialize.
// ---------------------------------------------------------------------------------------------

TEST_CASE("set_deserialize resolves an override's nil slots from the scoped parents", "[BambuCompat][BambuOverride]")
{
    DynamicPrintConfig project;
    project.set_deserialize_strict("outer_wall_speed", "200,250,200,250");
    project.set_deserialize_strict("inner_wall_speed", "300,310,300,310");
    DynamicPrintConfig object;
    object.set_deserialize_strict("inner_wall_speed", "100,120,100,120");

    ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::Enable };
    DynamicPrintConfig part;
    {
        OverrideScope project_scope(ctxt, &project);
        OverrideScope object_scope(ctxt, &object);
        REQUIRE_NOTHROW(part.set_deserialize("outer_wall_speed", "80,nil,80,nil", ctxt));
        REQUIRE_NOTHROW(part.set_deserialize("inner_wall_speed", "nil,90,nil,90", ctxt));
        REQUIRE_NOTHROW(part.set_deserialize("top_surface_speed", "nil,nil,nil,nil", ctxt));
    }
    CHECK(ctxt.bambu_override_parents.empty());
    CHECK(floats_of(part, "outer_wall_speed") == std::vector<double>{ 80, 250, 80, 250 });   // from the project
    CHECK(floats_of(part, "inner_wall_speed") == std::vector<double>{ 100, 90, 100, 90 });   // from the object
    CHECK_FALSE(part.has("top_surface_speed"));                                             // all nil: no override
    CHECK(ctxt.substitutions.empty());                                                      // nothing guessed
}

TEST_CASE("set_deserialize reports an override it had to guess", "[BambuCompat][BambuOverride]")
{
    ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::Enable };
    DynamicPrintConfig part;
    {
        OverrideScope scope(ctxt, nullptr);   // override semantics, parent unknown
        REQUIRE_NOTHROW(part.set_deserialize("outer_wall_speed", "80,nil,80,nil", ctxt));
    }
    CHECK(floats_of(part, "outer_wall_speed") == std::vector<double>{ 80, 80, 80, 80 });
    REQUIRE(ctxt.substitutions.size() == 1);
    CHECK(ctxt.substitutions.front().opt_def == print_config_def.get("outer_wall_speed"));
    CHECK(ctxt.substitutions.front().old_value == "80,nil,80,nil");
    CHECK(ctxt.substitutions.front().new_value->serialize() == "80,80,80,80");

    // The silent rules still translate, they just do not queue a dialog entry.
    ConfigSubstitutionContext silent{ ForwardCompatibilitySubstitutionRule::EnableSilent };
    DynamicPrintConfig part2;
    {
        OverrideScope scope(silent, nullptr);
        REQUIRE_NOTHROW(part2.set_deserialize("outer_wall_speed", "80,nil,80,nil", silent));
    }
    CHECK(silent.substitutions.empty());
    CHECK(floats_of(part2, "outer_wall_speed") == std::vector<double>{ 80, 80, 80, 80 });
}

TEST_CASE("set_deserialize outside an override scope translates with the preset rule", "[BambuCompat][BambuOverride]")
{
    // Every path that feeds set_deserialize a comma-joined string (ini, G-code config block,
    // CLI) used to throw on a Bambu nil. It now takes the same rule as the JSON preset loader.
    ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::Disable };
    DynamicPrintConfig cfg;
    REQUIRE_NOTHROW(cfg.set_deserialize("outer_wall_speed", "80,nil,80,nil", ctxt));
    CHECK(floats_of(cfg, "outer_wall_speed") == std::vector<double>{ 80, 80, 80, 80 });
    REQUIRE_NOTHROW(cfg.set_deserialize("sparse_infill_speed", "nil,nil", ctxt));
    CHECK_FALSE(cfg.has("sparse_infill_speed"));
    // Nullable options keep their nil.
    REQUIRE_NOTHROW(cfg.set_deserialize("filament_retraction_length", "nil,0.8", ctxt));
    const auto *nullable = cfg.option<ConfigOptionFloatsNullable>("filament_retraction_length");
    REQUIRE(nullable != nullptr);
    CHECK(nullable->is_nil(0));
    // A value that is not a nil marker is still rejected as before.
    CHECK_THROWS(cfg.set_deserialize("outer_wall_speed", "80,zzz", ctxt));
}

// ---------------------------------------------------------------------------------------------
// The real Bambu Studio project that failed to load.
// ---------------------------------------------------------------------------------------------

static boost::filesystem::path tmp_root()
{
    const boost::filesystem::path root = boost::filesystem::temp_directory_path() /
        ("snorca_tests_nilobj_" + std::to_string(get_current_pid()));
    boost::filesystem::create_directories(root);
    Slic3r::set_temporary_dir(root.string());
    return root;
}

struct Loaded
{
    bool                      ok { false };
    std::string               error;
    Model                     model;
    DynamicPrintConfig        config;
    ConfigSubstitutionContext ctxt { ForwardCompatibilitySubstitutionRule::Enable };
};

static void load_3mf(const std::string &path, LoadStrategy strategy, Loaded &out)
{
    PlateDataPtrs        plate_data;
    std::vector<Preset*> project_presets;
    bool                 is_bbl_3mf = false;
    Semver               file_version;
    try {
        out.ok = load_bbs_3mf(path.c_str(), &out.config, &out.ctxt, &out.model, &plate_data, &project_presets,
                              &is_bbl_3mf, &file_version, nullptr, strategy);
    } catch (const std::exception &ex) {
        out.ok    = false;
        out.error = ex.what();
    }
    release_PlateData_list(plate_data);
}

static const std::string AUTO_PA_DUAL = (boost::filesystem::path(TEST_DATA_DIR) / "bambu_compat_3mf" / "auto_pa_line_dual.3mf").string();

// Bambu wrote each modifier as "80,nil,80,nil" over a project whose 4 print_extruder_variant
// slots are Standard/HighFlow x left/right. Bambu slices the uncovered HighFlow slots with the
// project's values, so these are what the fork must hold.
static void check_auto_pa_modifiers(const Model &model)
{
    const std::vector<std::pair<std::string, std::vector<double>>> expected {
        { "outer_wall_speed",            { 80, 200, 80, 200 } },
        { "inner_wall_speed",            { 80, 300, 80, 300 } },
        { "internal_solid_infill_speed", { 80, 300, 80, 300 } },
        { "sparse_infill_speed",         { 80, 600, 80, 600 } },
        { "top_surface_speed",           { 80, 200, 80, 200 } },
    };
    size_t modifiers = 0;
    for (const ModelObject *mo : model.objects)
        for (const ModelVolume *mv : mo->volumes) {
            if (! mv->is_modifier())
                continue;
            ++modifiers;
            for (const auto &[key, values] : expected) {
                INFO("object " << mo->name << ", modifier " << mv->name << ", " << key);
                CHECK(floats_of(mv->config, key) == values);
            }
        }
    CHECK(modifiers == model.objects.size());
    CHECK(modifiers >= 2);
}

TEST_CASE("Bambu Studio's dual-extruder auto-PA project loads with its per-part overrides", "[BambuCompat][BambuOverride][3mf]")
{
    tmp_root();
    Loaded loaded;
    load_3mf(AUTO_PA_DUAL, LoadStrategy::LoadModel | LoadStrategy::LoadConfig | LoadStrategy::AddDefaultInstances | LoadStrategy::Silence, loaded);
    INFO(loaded.error);
    REQUIRE(loaded.ok);
    REQUIRE(loaded.model.objects.size() == 8);
    check_auto_pa_modifiers(loaded.model);

    // Every nil slot had a parent, so nothing was guessed and nothing needs the user's attention.
    for (const ConfigSubstitution &s : loaded.ctxt.substitutions)
        CHECK(s.old_value.find("nil") == std::string::npos);
}

TEST_CASE("the auto-PA project also loads without its config", "[BambuCompat][BambuOverride][3mf]")
{
    tmp_root();
    Loaded loaded;
    load_3mf(AUTO_PA_DUAL, LoadStrategy::LoadModel | LoadStrategy::AddDefaultInstances | LoadStrategy::Silence, loaded);
    INFO(loaded.error);
    REQUIRE(loaded.ok);
    REQUIRE(loaded.model.objects.size() == 8);
    for (const ModelObject *mo : loaded.model.objects)
        for (const ModelVolume *mv : mo->volumes)
            if (mv->is_modifier()) {
                const std::vector<double> v = floats_of(mv->config, "outer_wall_speed");
                REQUIRE(v.size() == 4);
                CHECK(v[0] == 80.);
                for (double s : v)
                    CHECK(s > 0.);
            }
}

TEST_CASE("an auto-PA project loaded from Bambu round-trips through the fork's own 3MF", "[BambuCompat][BambuOverride][3mf]")
{
    const boost::filesystem::path root = tmp_root();
    Loaded src;
    load_3mf(AUTO_PA_DUAL, LoadStrategy::LoadModel | LoadStrategy::LoadConfig | LoadStrategy::AddDefaultInstances | LoadStrategy::Silence, src);
    REQUIRE(src.ok);

    const std::string out_file = (root / "auto_pa_line_dual_roundtrip.3mf").string();
    StoreParams store_params;
    store_params.path     = out_file.c_str();
    store_params.model    = &src.model;
    store_params.config   = &src.config;
    store_params.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SkipAuxiliary;
    REQUIRE(store_bbs_3mf(store_params));

    Loaded dst;
    load_3mf(out_file, LoadStrategy::LoadModel | LoadStrategy::LoadConfig | LoadStrategy::AddDefaultInstances | LoadStrategy::Silence, dst);
    boost::filesystem::remove(out_file);
    INFO(dst.error);
    REQUIRE(dst.ok);
    REQUIRE(dst.model.objects.size() == src.model.objects.size());
    check_auto_pa_modifiers(dst.model);
    // Our own file carries no nil, so the reload translates nothing.
    for (const ConfigSubstitution &s : dst.ctxt.substitutions)
        CHECK(s.old_value.find("nil") == std::string::npos);
}

// ---------------------------------------------------------------------------------------------
// Corpus harness (hidden): load every .3mf under $EDGE_3MF_CORPUS and print one line per file.
//   libslic3r_tests "[.corpus3mf]"  with EDGE_3MF_CORPUS=<dir>
// ---------------------------------------------------------------------------------------------
TEST_CASE("load a corpus of 3MF files and report", "[.corpus3mf]")
{
    const char *dir = std::getenv("EDGE_3MF_CORPUS");
    if (dir == nullptr || *dir == 0) {
        WARN("EDGE_3MF_CORPUS is not set");
        return;
    }
    tmp_root();
    size_t total = 0, ok = 0;
    for (auto &entry : boost::filesystem::recursive_directory_iterator(dir)) {
        if (! boost::filesystem::is_regular_file(entry) || ! boost::iequals(entry.path().extension().string(), ".3mf"))
            continue;
        ++total;
        Loaded loaded;
        // load_bbs_3mf takes UTF-8; path::string() is the ANSI code page on Windows.
        load_3mf(boost::nowide::narrow(entry.path().wstring()), LoadStrategy::LoadModel | LoadStrategy::LoadConfig | LoadStrategy::AddDefaultInstances | LoadStrategy::Silence, loaded);
        size_t nil_subs = 0;
        for (const ConfigSubstitution &s : loaded.ctxt.substitutions)
            if (s.old_value.find("nil") != std::string::npos)
                ++nil_subs;
        if (loaded.ok)
            ++ok;
        std::cout << "CORPUS " << (loaded.ok ? "LOADED " : "FAILED ") << entry.path().filename().string()
                  << " objects=" << loaded.model.objects.size() << " nil_substitutions=" << nil_subs
                  << (loaded.error.empty() ? "" : " error=" + loaded.error) << std::endl;
    }
    std::cout << "CORPUS TOTAL " << ok << "/" << total << " loaded" << std::endl;
}
