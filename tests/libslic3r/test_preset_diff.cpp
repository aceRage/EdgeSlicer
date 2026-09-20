#include <catch2/catch.hpp>

#include "libslic3r/Preset.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <string>
#include <vector>

using namespace Slic3r;

TEST_CASE("deep_diff distinguishes absolute and percentage speeds for each variant", "[PresetDiff][Config]")
{
    const size_t changed_index = GENERATE(size_t(0), size_t(1));
    Preset       reference(Preset::TYPE_PRINT, "ref");
    reference.config.set_key_value("small_perimeter_speed", new ConfigOptionFloatsOrPercents{{50., false}, {50., false}});

    Preset edited = reference;
    edited.config.option<ConfigOptionFloatsOrPercents>("small_perimeter_speed")->values[changed_index].percent = true;

    const auto diff = PresetCollection::dirty_options(&edited, &reference, /*deep_compare=*/true);
    REQUIRE(diff == std::vector<std::string>{"small_perimeter_speed#" + std::to_string(changed_index)});

    DynamicPrintConfig transferred = reference.config;
    transferred.apply_only(edited.config, diff);
    REQUIRE(*transferred.option("small_perimeter_speed") == *edited.config.option("small_perimeter_speed"));
}
