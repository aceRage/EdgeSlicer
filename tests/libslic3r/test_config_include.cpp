#include <catch2/catch.hpp>

#include "libslic3r/PrintConfig.hpp"

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>

#include <map>
#include <string>

using namespace Slic3r;

// ConfigBase::load_from_json resolves a preset's "include" entries before it
// deserializes anything. Bambu Studio resolves those entries by NAME against the
// preset map it has already loaded, and registers the fdm_filament_template_*
// files as non-instantiated entries at the top of filament_list, so a bare
// template name works from any subfolder of the vendor's filament/ directory.
// This fork resolves a path relative to the including file first, then - since
// the templates all live at the category root - falls back to the nearest
// ancestor directory named filament/process/machine. These cases pin both halves
// down, plus the rule that an include which resolves nowhere is a warning, not a
// failure.

namespace {

// a throwaway vendor tree under the system temp directory, removed on scope exit
struct TempTree
{
    boost::filesystem::path root;

    explicit TempTree(const std::string &tag)
        : root(boost::filesystem::temp_directory_path() / "snorca_tests" /
               boost::filesystem::unique_path(tag + "_%%%%%%%%"))
    {
        boost::filesystem::create_directories(root);
    }
    ~TempTree()
    {
        boost::system::error_code ec;
        boost::filesystem::remove_all(root, ec);
    }

    // writes body to <root>/<rel>, creating parent directories
    boost::filesystem::path write(const std::string &rel, const std::string &body) const
    {
        const boost::filesystem::path path = root / rel;
        boost::filesystem::create_directories(path.parent_path());
        boost::nowide::ofstream ofs(path.string());
        ofs << body;
        ofs.close();
        return path;
    }
};

// the shared template: two keys the child never mentions, and one it overrides
const char *k_template_json = R"({
    "type": "filament",
    "name": "test_template",
    "instantiation": "false",
    "filament_max_volumetric_speed": ["12", "12"],
    "nozzle_temperature": ["250", "250"],
    "filament_flow_ratio": ["0.98", "0.98"]
})";

// loads path through the same entry point PresetBundle uses for vendor presets
int load(DynamicPrintConfig &config, const boost::filesystem::path &path)
{
    ConfigSubstitutionContext ctxt(ForwardCompatibilitySubstitutionRule::Enable);
    std::map<std::string, std::string> key_values;
    std::string reason;
    return config.load_from_json(path.string(), ctxt, true, key_values, reason);
}

} // namespace

SCENARIO("A preset resolves its include templates", "[Config][ConfigInclude]")
{
    GIVEN("a vendor tree with the template at the filament/ category root")
    {
        TempTree tree("include_lookup");
        tree.write("filament/template.json", k_template_json);

        WHEN("a preset in a subfolder includes it by bare name, as upstream writes it")
        {
            // filament/Sub/child.json - exactly the shape of the Fiberon @BBL H2D
            // presets, which sit in BBL/filament/Polymaker/ and include
            // "fdm_filament_template_direct_dual" from BBL/filament/.
            const auto child = tree.write("filament/Sub/child.json", R"({
    "type": "filament",
    "name": "child",
    "include": ["template"],
    "filament_flow_ratio": ["0.88", "0.88"]
})");
            DynamicPrintConfig config;
            const int ret = load(config, child);

            THEN("the load succeeds and the template's keys arrive")
            {
                REQUIRE(ret == 0);
                REQUIRE(config.has("filament_max_volumetric_speed"));
                CHECK(config.opt_serialize("filament_max_volumetric_speed") == "12,12");
                REQUIRE(config.has("nozzle_temperature"));
                CHECK(config.opt_serialize("nozzle_temperature") == "250,250");
            }
            THEN("the including preset still wins over the template")
            {
                REQUIRE(config.has("filament_flow_ratio"));
                CHECK(config.opt_serialize("filament_flow_ratio") == "0.88,0.88");
            }
            THEN("the meta keys of the template are not merged in")
            {
                // "instantiation": "false" belongs to the template, not the child
                CHECK(! config.has("instantiation"));
            }
        }

        WHEN("a preset at the category root itself includes it by bare name")
        {
            const auto sibling = tree.write("filament/sibling.json", R"({
    "type": "filament",
    "name": "sibling",
    "include": ["template"]
})");
            DynamicPrintConfig config;
            const int ret = load(config, sibling);

            THEN("the plain relative-path resolution still does the job")
            {
                REQUIRE(ret == 0);
                REQUIRE(config.has("filament_max_volumetric_speed"));
                CHECK(config.opt_serialize("filament_max_volumetric_speed") == "12,12");
            }
        }
    }

    GIVEN("a template next to the including preset")
    {
        TempTree tree("include_relative");
        tree.write("filament/Sub/local.json", k_template_json);
        // a decoy at the category root: if the relative path stopped winning, the
        // preset would silently pick this one up instead
        tree.write("filament/local.json", R"({
    "type": "filament",
    "name": "decoy",
    "filament_max_volumetric_speed": ["99", "99"]
})");

        WHEN("the preset includes it by a path relative to its own directory")
        {
            const auto child = tree.write("filament/Sub/child.json", R"({
    "type": "filament",
    "name": "child",
    "include": ["local"]
})");
            DynamicPrintConfig config;
            const int ret = load(config, child);

            THEN("the neighbouring file is what resolves, not the category root")
            {
                REQUIRE(ret == 0);
                REQUIRE(config.has("filament_max_volumetric_speed"));
                CHECK(config.opt_serialize("filament_max_volumetric_speed") == "12,12");
            }
        }

        WHEN("the preset includes the category root copy with an explicit ../")
        {
            const auto child = tree.write("filament/Sub/child_up.json", R"({
    "type": "filament",
    "name": "child_up",
    "include": ["../local"]
})");
            DynamicPrintConfig config;
            const int ret = load(config, child);

            THEN("the explicit path is honoured")
            {
                REQUIRE(ret == 0);
                REQUIRE(config.has("filament_max_volumetric_speed"));
                CHECK(config.opt_serialize("filament_max_volumetric_speed") == "99,99");
            }
        }
    }

    GIVEN("a preset whose include names nothing that exists")
    {
        TempTree tree("include_missing");

        WHEN("it is loaded")
        {
            const auto child = tree.write("filament/Sub/orphan.json", R"({
    "type": "filament",
    "name": "orphan",
    "include": ["no_such_template"],
    "filament_flow_ratio": ["0.88", "0.88"]
})");
            DynamicPrintConfig config;
            int ret = -2;

            THEN("the loader warns and carries on - it neither throws nor fails")
            {
                REQUIRE_NOTHROW(ret = load(config, child));
                CHECK(ret == 0);
                // the preset's own keys still arrive, and "include" itself is never
                // handed to the deserializer (it has no ConfigOptionDef, which would
                // fail the whole load)
                REQUIRE(config.has("filament_flow_ratio"));
                CHECK(config.opt_serialize("filament_flow_ratio") == "0.88,0.88");
                CHECK(! config.has("include"));
            }
        }
    }
}
