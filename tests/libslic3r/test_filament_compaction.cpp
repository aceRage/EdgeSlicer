#include <catch2/catch.hpp>

#include "libslic3r/FilamentCompaction.hpp"
#include "libslic3r/MixedFilament.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"

using namespace Slic3r;

// Dense tool numbering for printers whose firmware has no macro past its last tool.
// Ported from Orca #15145 and adapted to MixedFilamentManager (no filament_is_mixed arrays).

namespace {

// A model with one printable object per requested 1-based filament. Instances must be marked
// Inside: Print::apply builds print objects only from printable instances, and used_filament_slots
// mirrors that so an object parked on another plate contributes no filaments.
Model model_using(const std::vector<int>& filaments_1based)
{
    Model model;
    for (int filament : filaments_1based) {
        ModelObject* object = model.add_object();
        object->add_volume(make_cube(10, 10, 10), false);
        object->config.set("extruder", filament);
        object->add_instance();
    }
    return model;
}

// The minimum a plate needs for the collector's feature gates to read as "off".
DynamicPrintConfig plain_config(size_t filament_count)
{
    DynamicPrintConfig config;
    config.set_key_value("enable_support", new ConfigOptionBool(false));
    config.set_key_value("raft_layers", new ConfigOptionInt(0));
    config.set_key_value("enable_prime_tower", new ConfigOptionBool(false));
    config.set_key_value("filament_colour", new ConfigOptionStrings(std::vector<std::string>(filament_count, "#FFFFFF")));
    return config;
}

// Physical filament_count plus one custom mix of the given 1-based components.
// The mix's virtual id is filament_count + 1 (auto-generate is off by default).
DynamicPrintConfig mixed_config(size_t filament_count, unsigned int component_a, unsigned int component_b)
{
    DynamicPrintConfig config = plain_config(filament_count);
    MixedFilamentManager mgr;
    const std::vector<std::string> colors(filament_count, "#FFFFFF");
    mgr.add_custom_filament(component_a, component_b, 50, colors);
    config.set_key_value("mixed_filament_definitions", new ConfigOptionString(mgr.serialize_custom_entries()));
    return config;
}

} // namespace

TEST_CASE("Used filament slots are the plate's filaments, 0-based and ascending", "[FilamentCompaction]")
{
    const Model model = model_using({6, 3, 3});
    CHECK(used_filament_slots(model, plain_config(8)) == std::vector<int>{2, 5});
}

TEST_CASE("An object on another plate contributes no filament", "[FilamentCompaction]")
{
    Model model = model_using({3, 7});
    // Not inside the current plate's print volume -- exactly how Print::apply skips it.
    model.objects[1]->instances[0]->print_volume_state = ModelInstancePVS_Fully_Outside;
    CHECK(used_filament_slots(model, plain_config(8)) == std::vector<int>{2});
}

TEST_CASE("A plate is renumbered only when its highest filament exceeds the namespace", "[FilamentCompaction]")
{
    CHECK(build_filament_compaction(model_using({1, 2}), plain_config(8), 4).is_identity());
    CHECK(build_filament_compaction(model_using({4, 7}), plain_config(8), 32).is_identity());
    CHECK(build_filament_compaction(model_using({4, 7}), plain_config(8), 7).is_identity());

    const FilamentCompaction compaction = build_filament_compaction(model_using({4, 7}), plain_config(8), 4);
    REQUIRE(compaction.slot_of_tool == std::vector<int>{3, 6});
    CHECK(compaction.tool_of_slot(3) == 0);
    CHECK(compaction.tool_of_slot(6) == 1);
    CHECK(compaction.tool_of_slot(0) == -1);
}

TEST_CASE("Compaction renumbers the model's filament references", "[FilamentCompaction]")
{
    Model model = model_using({4, 7});
    model.objects[0]->volumes[0]->config.set("extruder", 4);
    model.objects[1]->config.set("support_filament", 7);

    const FilamentCompaction compaction = build_filament_compaction(model, plain_config(8), 4);
    apply_filament_compaction(model, model, compaction);

    CHECK(model.objects[0]->config.option("extruder")->getInt() == 1);
    CHECK(model.objects[0]->volumes[0]->config.option("extruder")->getInt() == 1);
    CHECK(model.objects[1]->config.option("extruder")->getInt() == 2);
    CHECK(model.objects[1]->config.option("support_filament")->getInt() == 2);
}

TEST_CASE("Compaction gathers per-filament config vectors in dense order", "[FilamentCompaction]")
{
    DynamicPrintConfig config = plain_config(4);
    config.set_key_value("filament_colour", new ConfigOptionStrings({"#000000", "#111111", "#222222", "#333333"}));
    config.set_key_value("filament_type", new ConfigOptionStrings({"PLA", "PETG", "ABS", "TPU"}));
    config.set_key_value("nozzle_temperature", new ConfigOptionInts({200, 240, 260, 220}));
    config.set_key_value("support_filament", new ConfigOptionInt(4));

    FilamentCompaction compaction;
    compaction.slot_of_tool = {1, 3};

    apply_filament_compaction(config, compaction);

    CHECK(config.option<ConfigOptionStrings>("filament_colour")->values == std::vector<std::string>{"#111111", "#333333"});
    CHECK(config.option<ConfigOptionStrings>("filament_type")->values == std::vector<std::string>{"PETG", "TPU"});
    CHECK(config.option<ConfigOptionInts>("nozzle_temperature")->values == std::vector<int>{240, 220});
    CHECK(config.option<ConfigOptionInt>("support_filament")->value == 2);
}

TEST_CASE("Compaction gathers the flush volume matrix on both axes", "[FilamentCompaction]")
{
    DynamicPrintConfig config = plain_config(3);
    config.set_key_value("flush_volumes_matrix",
                         new ConfigOptionFloats({0, 1, 2, 10, 11, 12, 20, 21, 22}));
    config.set_key_value("flush_volumes_vector", new ConfigOptionFloats({100, 200, 300}));

    FilamentCompaction compaction;
    compaction.slot_of_tool = {0, 2};
    apply_filament_compaction(config, compaction);

    CHECK(config.option<ConfigOptionFloats>("flush_volumes_matrix")->values == std::vector<double>{0, 2, 20, 22});
    CHECK(config.option<ConfigOptionFloats>("flush_volumes_vector")->values == std::vector<double>{100, 300});
}

TEST_CASE("Compaction gathers every per-head flush block on a multi-head printer", "[FilamentCompaction]")
{
    DynamicPrintConfig config = plain_config(3);
    config.set_key_value("flush_volumes_matrix",
                         new ConfigOptionFloats({  0,   1,   2,  10,  11,  12,  20,  21,  22,
                                                 100, 101, 102, 110, 111, 112, 120, 121, 122}));
    config.set_key_value("flush_volumes_vector", new ConfigOptionFloats({10, 11, 20, 21, 30, 31}));

    FilamentCompaction compaction;
    compaction.slot_of_tool = {0, 2};
    apply_filament_compaction(config, compaction);

    CHECK(config.option<ConfigOptionFloats>("flush_volumes_matrix")->values ==
          std::vector<double>{0, 2, 20, 22, 100, 102, 120, 122});
    CHECK(config.option<ConfigOptionFloats>("flush_volumes_vector")->values ==
          std::vector<double>{10, 11, 30, 31});
}

TEST_CASE("Recompacting an unchanged source leaves every timestamp unchanged", "[FilamentCompaction]")
{
    Model model = model_using({4, 7});
    const DynamicPrintConfig config = plain_config(8);

    Model first  = model;
    Model second = model;
    apply_filament_compaction(first, model, build_filament_compaction(model, config, 4));
    apply_filament_compaction(second, model, build_filament_compaction(model, config, 4));

    auto config_stamp = [](const ModelObject* object) {
        return static_cast<const ModelConfig&>(object->config).timestamp();
    };
    for (size_t i = 0; i < model.objects.size(); ++i) {
        CHECK(config_stamp(first.objects[i]) == config_stamp(model.objects[i]));
        CHECK(config_stamp(second.objects[i]) == config_stamp(model.objects[i]));
    }
}

TEST_CASE("A filament the plate does not print keeps its number", "[FilamentCompaction]")
{
    Model model = model_using({4, 7});
    model.objects[0]->config.set("support_filament", 2);
    const FilamentCompaction compaction = build_filament_compaction(model, plain_config(8), 4);
    apply_filament_compaction(model, model, compaction);
    CHECK(model.objects[0]->config.option("support_filament")->getInt() == 2);
}

TEST_CASE("A mixed slot is used through its components", "[FilamentCompaction]")
{
    // Four physical filaments; the mix of 1 and 3 is virtual id 5.
    const Model model = model_using({5});
    CHECK(used_filament_slots(model, mixed_config(4, 1, 3)) == std::vector<int>{0, 2});
}

TEST_CASE("A used mix is numbered after the physical tools", "[FilamentCompaction]")
{
    const FilamentCompaction compaction = build_filament_compaction(model_using({5}), mixed_config(4, 1, 3), 2);
    REQUIRE(compaction.slot_of_tool == std::vector<int>{0, 2, 4});
    CHECK(compaction.tool_of_slot(4) == 2);
}

TEST_CASE("Four physical filaments and their mixes need no renumbering", "[FilamentCompaction]")
{
    const FilamentCompaction compaction = build_filament_compaction(model_using({1, 2, 3, 4, 5}), mixed_config(4, 2, 4), 4);
    CHECK(compaction.slot_of_tool.empty());
}

TEST_CASE("Compaction renumbers a mix's components", "[FilamentCompaction]")
{
    DynamicPrintConfig config = mixed_config(4, 1, 3);
    FilamentCompaction compaction;
    compaction.slot_of_tool = {0, 2, 4};
    apply_filament_compaction(config, compaction);

    MixedFilamentManager mgr;
    const auto* colors = config.option<ConfigOptionStrings>("filament_colour");
    REQUIRE(colors != nullptr);
    REQUIRE(colors->size() == 2);
    mgr.load_custom_entries(config.opt_string("mixed_filament_definitions"), colors->values);
    REQUIRE(mgr.enabled_count() == 1);
    CHECK(mgr.mixed_filaments()[0].component_a == 1);
    CHECK(mgr.mixed_filaments()[0].component_b == 2);
}

TEST_CASE("A painted mix is renumbered in the copy's painted-filament list", "[FilamentCompaction]")
{
    // Filament-1 cube painted with virtual mix id 8 (mix of physical 4 and 5 on a 7-physical project).
    Model         model  = model_using({1});
    ModelVolume*  volume = model.objects[0]->volumes[0];
    TriangleSelector selector(volume->mesh());
    selector.set_facet(0, EnforcerBlockerType::Extruder8);
    volume->mmu_segmentation_facets.set(selector);
    REQUIRE(volume->get_extruders() == std::vector<int>{8, 1});

    const DynamicPrintConfig config = mixed_config(7, 4, 5);
    CHECK(used_filament_slots(model, config) == std::vector<int>{0, 3, 4});

    const FilamentCompaction compaction = build_filament_compaction(model, config, 4);
    REQUIRE(compaction.slot_of_tool == std::vector<int>{0, 3, 4, 7});

    Model copy = model;
    apply_filament_compaction(copy, model, compaction);
    CHECK(copy.objects[0]->volumes[0]->get_extruders() == std::vector<int>{4, 1});
}
