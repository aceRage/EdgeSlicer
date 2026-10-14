// Range checks and tolerant float comparison of config options (Orca #11915, #12478, #10312).
//
// #11915 made ConfigOptionDef::min/max floats. Before, they were ints, so every fractional bound
// was truncated (fuzzy_skin_point_distance's 0.01 minimum was really 0, which let a point distance
// of 0 through: an endless division of every wall segment and an out-of-memory crash). The last
// case scans every shipped preset, inheritance resolved, for a value the int bounds accepted and
// the float bounds reject.

#include <catch2/catch.hpp>

#include <cfloat>
#include <climits>
#include <cmath>
#include <map>
#include <string>
#include <vector>

#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

#include "libslic3r/Config.hpp"
#include "libslic3r/PrintConfig.hpp"

#include "../fff_print/test_data.hpp"

using namespace Slic3r;

namespace {

const ConfigOptionDef &range_def_of(const char *key)
{
    const ConfigOptionDef *def = print_config_def.get(key);
    REQUIRE(def != nullptr);
    return *def;
}

// What the range check did before #11915: the same comparison against the int-truncated bounds,
// with the old INT_MIN / INT_MAX "unset" values.
bool range_valid_with_int_bounds(const ConfigOptionDef &def, double v)
{
    const double lo = def.min <= -FLT_MAX ? double(INT_MIN) : double(int(def.min));
    const double hi = def.max >= FLT_MAX ? double(INT_MAX) : double(int(def.max));
    return !(v < lo || v > hi);
}

} // namespace

TEST_CASE("Fractional option bounds are kept, not truncated", "[ConfigRange]")
{
    const ConfigOptionDef &pd = range_def_of("fuzzy_skin_point_distance");
    CHECK(pd.min == Approx(0.01));
    CHECK(pd.max == Approx(5.));
    CHECK_FALSE(pd.is_value_valid(0.));     // the infinite-loop value
    CHECK_FALSE(pd.is_value_valid(0.005));
    CHECK(pd.is_value_valid(0.01));         // the float minimum itself, 0.0099999998 as a double
    CHECK(pd.is_value_valid(0.3));
    CHECK(pd.is_value_valid(5.));
    CHECK_FALSE(pd.is_value_valid(5.1));
    // Before #11915 the minimum was int(0.01) = 0, so 0 passed.
    CHECK(range_valid_with_int_bounds(pd, 0.));

    const ConfigOptionDef &jd = range_def_of("machine_max_junction_deviation");
    CHECK(jd.is_value_valid(0.));
    CHECK(jd.is_value_valid(0.5));
    CHECK_FALSE(jd.is_value_valid(0.6));
}

TEST_CASE("Range check edge cases: zero minimum, tolerance at the bounds, nil values", "[ConfigRange]")
{
    // min == 0 rejects any negative value, even one within the tolerance of 0.
    const ConfigOptionDef &xy = range_def_of("support_object_xy_distance");
    REQUIRE(xy.min == 0.f);
    CHECK_FALSE(xy.is_value_valid(-1e-6));
    CHECK(xy.is_value_valid(0.));
    // Values within 1e-4 of a bound count as on it (typed values are rounded to 4 decimals).
    CHECK(xy.is_value_valid(double(xy.max) + 5e-5));
    CHECK_FALSE(xy.is_value_valid(double(xy.max) + 1e-3));

    // An int option.
    const ConfigOptionDef &raft = range_def_of("raft_layers");
    CHECK(raft.is_value_valid(0));
    CHECK(raft.is_value_valid(double(raft.max)));
    CHECK_FALSE(raft.is_value_valid(-1));
    CHECK_FALSE(raft.is_value_valid(double(raft.max) + 1));

    // An unbounded option accepts anything finite.
    ConfigOptionDef unbounded;
    CHECK(unbounded.min == -FLT_MAX);
    CHECK(unbounded.max == FLT_MAX);
    CHECK(unbounded.is_value_valid(-1e30));
    CHECK(unbounded.is_value_valid(1e30));

    // nil (NaN) is a value only a nullable option may hold.
    ConfigOptionDef plain;
    plain.min = 0.f;
    plain.max = 10.f;
    CHECK_FALSE(plain.is_value_valid(std::nan("")));
    plain.nullable = true;
    CHECK(plain.is_value_valid(std::nan("")));
}

TEST_CASE("validate() rejects a zero fuzzy skin point distance and out-of-range percents", "[ConfigRange]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    CHECK(config.validate().count("fuzzy_skin_point_distance") == 0);

    config.set_deserialize_strict({ { "fuzzy_skin_point_distance", "0" } });
    CHECK(config.validate().count("fuzzy_skin_point_distance") == 1);

    config.set_deserialize_strict({ { "fuzzy_skin_point_distance", "0.01" } });
    CHECK(config.validate().count("fuzzy_skin_point_distance") == 0);

    config.set_deserialize_strict({ { "sparse_infill_density", "101%" } });
    CHECK(config.validate().count("sparse_infill_density") == 1);
    config.set_deserialize_strict({ { "sparse_infill_density", "100%" } });
    CHECK(config.validate().count("sparse_infill_density") == 0);

    // The junction deviation vectors are range checked per value.
    config.set_deserialize_strict({ { "machine_max_junction_deviation", "0.01,0.6" } });
    CHECK(config.validate().count("machine_max_junction_deviation") == 1);
    config.set_deserialize_strict({ { "machine_max_junction_deviation", "0.01,0.5" } });
    CHECK(config.validate().count("machine_max_junction_deviation") == 0);
}

TEST_CASE("Float options compare with a tolerance (Orca #12478)", "[ConfigRange]")
{
    // 0.1 + 0.2 is 0.30000000000000004: the same setting, typed or computed.
    CHECK(ConfigOptionFloat(0.1 + 0.2) == ConfigOptionFloat(0.3));
    CHECK_FALSE(ConfigOptionFloat(0.301) == ConfigOptionFloat(0.3));
    // Through the virtual comparison the preset dirty flags use.
    const ConfigOption &a = ConfigOptionFloat(0.1 + 0.2);
    const ConfigOption &b = ConfigOptionFloat(0.3);
    CHECK(a == b);

    // Vectors: upstream made only the nullable comparison tolerant; plain vectors stay exact.
    CHECK(ConfigOptionFloatsNullable({ 0.1 + 0.2, 1. }) == ConfigOptionFloatsNullable({ 0.3, 1. }));
    CHECK_FALSE(ConfigOptionFloatsNullable({ 0.31, 1. }) == ConfigOptionFloatsNullable({ 0.3, 1. }));
    CHECK(ConfigOptionPercentsNullable({ 50. + 1e-9 }) == ConfigOptionPercentsNullable({ 50. }));
    CHECK_FALSE(ConfigOptionFloats({ 0.1 + 0.2, 1. }) == ConfigOptionFloats({ 0.3, 1. }));

    // Nullable: nil equals nil, nil never equals a value.
    ConfigOptionFloatsNullable n1({ ConfigOptionFloatsNullable::nil_value(), 0.3 });
    ConfigOptionFloatsNullable n2({ ConfigOptionFloatsNullable::nil_value(), 0.1 + 0.2 });
    ConfigOptionFloatsNullable n3({ 0.3, 0.3 });
    CHECK(n1 == n2);
    CHECK_FALSE(n1 == n3);

    CHECK(ConfigOptionFloatOrPercent(0.1 + 0.2, false) == ConfigOptionFloatOrPercent(0.3, false));
    CHECK_FALSE(ConfigOptionFloatOrPercent(30., true) == ConfigOptionFloatOrPercent(30., false));
    CHECK(FloatOrPercent{ 0.1 + 0.2, true } == FloatOrPercent{ 0.3, true });
}

TEST_CASE("Fuzzy skin has a Disabled state, the default (Orca #10312)", "[ConfigRange][FuzzySkin]")
{
    const ConfigOptionDef &fz = range_def_of("fuzzy_skin");
    REQUIRE(fz.default_value);
    CHECK(fz.default_value->getInt() == int(FuzzySkinType::Disabled_fuzzy));

    ConfigOptionEnum<FuzzySkinType> opt;
    REQUIRE(opt.deserialize("disabled_fuzzy"));
    CHECK(opt.value == FuzzySkinType::Disabled_fuzzy);
    CHECK(opt.serialize() == "disabled_fuzzy");
    // The values older projects and every shipped profile carry keep their meaning.
    REQUIRE(opt.deserialize("none"));
    CHECK(opt.value == FuzzySkinType::None);
    REQUIRE(opt.deserialize("hole"));
    CHECK(opt.value == FuzzySkinType::Hole);
}

// Every instantiable preset under resources/profiles, resolved through "inherits" within its vendor
// folder (filaments may also inherit from OrcaFilamentLibrary). Numeric values only; "nil" skipped.
TEST_CASE("No shipped preset value is newly out of range with float bounds", "[ConfigRange][Profiles]")
{
    namespace fs = boost::filesystem;
    const fs::path profiles = fs::path(TEST_DATA_DIR) / ".." / ".." / "resources" / "profiles";
    REQUIRE(fs::is_directory(profiles));

    auto load_dir = [](const fs::path &dir) {
        std::map<std::string, nlohmann::json> by_name;
        if (!fs::is_directory(dir))
            return by_name;
        for (fs::recursive_directory_iterator it(dir); it != fs::recursive_directory_iterator(); ++it) {
            if (!fs::is_regular_file(it->path()) || it->path().extension() != ".json")
                continue;
            boost::nowide::ifstream ifs(it->path().string());
            nlohmann::json j = nlohmann::json::parse(ifs, nullptr, false);
            if (j.is_object() && j.contains("name") && j["name"].is_string()) {
                const std::string preset_name = j["name"].get<std::string>();
                by_name[preset_name]          = std::move(j);
            }
        }
        return by_name;
    };
    const auto library = load_dir(profiles / "OrcaFilamentLibrary");

    size_t                              presets = 0, values = 0;
    std::map<std::string, std::string>  newly_rejected; // key=value -> an example preset
    for (fs::directory_iterator vit(profiles); vit != fs::directory_iterator(); ++vit) {
        if (!fs::is_directory(vit->path()))
            continue;
        const auto by_name = load_dir(vit->path());
        for (const auto &[name, j] : by_name) {
            if (!j.contains("instantiation") || j["instantiation"] != "true")
                continue;
            ++presets;
            // Resolve: walk up the chain, the nearest definition of a key wins.
            std::vector<const nlohmann::json *> chain;
            for (const nlohmann::json *cur = &j; cur != nullptr && chain.size() < 40;) {
                chain.push_back(cur);
                if (!cur->contains("inherits") || !(*cur)["inherits"].is_string())
                    break;
                const std::string parent = (*cur)["inherits"].get<std::string>();
                const nlohmann::json *next = nullptr;
                if (auto it = by_name.find(parent); it != by_name.end())
                    next = &it->second;
                else if (auto lt = library.find(parent); lt != library.end())
                    next = &lt->second;
                cur = next;
            }
            std::map<std::string, const nlohmann::json *> resolved;
            for (auto it = chain.rbegin(); it != chain.rend(); ++it)
                for (auto kv = (*it)->begin(); kv != (*it)->end(); ++kv)
                    resolved[kv.key()] = &kv.value();

            for (const auto &[key, value] : resolved) {
                const ConfigOptionDef *def = print_config_def.get(key);
                if (def == nullptr)
                    continue;
                switch (def->type) {
                case coFloat: case coFloats: case coPercent: case coPercents:
                case coFloatOrPercent: case coFloatsOrPercents: case coInt: case coInts: break;
                default: continue;
                }
                std::vector<std::string> items;
                if (value->is_array()) {
                    for (const auto &e : *value)
                        if (e.is_string())
                            items.push_back(e.get<std::string>());
                } else if (value->is_string())
                    items.push_back(value->get<std::string>());
                for (const std::string &item : items) {
                    std::vector<std::string> parts;
                    boost::split(parts, item, boost::is_any_of(","));
                    for (std::string part : parts) {
                        boost::trim(part);
                        if (!part.empty() && part.back() == '%')
                            part.pop_back();
                        if (part.empty() || part == "nil")
                            continue;
                        char  *end = nullptr;
                        double v   = std::strtod(part.c_str(), &end);
                        if (end == part.c_str() || *end != '\0')
                            continue;
                        ++values;
                        if (range_valid_with_int_bounds(*def, v) && !def->is_value_valid(v))
                            newly_rejected.emplace(key + "=" + part, vit->path().filename().string() + "/" + name);
                    }
                }
            }
        }
    }
    INFO(presets << " presets, " << values << " numeric values checked");
    for (const auto &[kv, example] : newly_rejected)
        UNSCOPED_INFO("newly out of range: " << kv << " (e.g. " << example << ")");
    CHECK(presets > 1000);
    CHECK(newly_rejected.empty());
}
