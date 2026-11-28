#include <catch2/catch.hpp>

#include "libslic3r/PlateBedTypes.hpp"
#include "libslic3r/PrintConfig.hpp"

using namespace Slic3r;

TEST_CASE("CLI --bed-type resolves short keys and full names", "[PlateBedTypes][cli]")
{
    BedType t = btDefault;
    for (const auto &kv : PlateBedTypes::cli_bed_type_keys()) {
        t = btDefault;
        CHECK(PlateBedTypes::parse_cli_bed_type(kv.first, t));
        CHECK(t == kv.second);
    }
    CHECK(PlateBedTypes::parse_cli_bed_type("textured_pei", t));
    CHECK(t == btPTE);
    CHECK(PlateBedTypes::parse_cli_bed_type("Textured PEI Plate", t));
    CHECK(t == btPTE);
    CHECK(PlateBedTypes::parse_cli_bed_type("textured-cool-plate", t));
    CHECK(t == btPCT);
    CHECK(PlateBedTypes::parse_cli_bed_type("  Supertack Plate ", t));
    CHECK(t == btSuperTack);
    CHECK(PlateBedTypes::parse_cli_bed_type("High Temp Plate", t));
    CHECK(t == btPEI);
    CHECK(PlateBedTypes::parse_cli_bed_type("Graphic Effect Plate", t));
    CHECK(t == btGESP);
    CHECK(PlateBedTypes::parse_cli_bed_type("Cool Plate", t));
    CHECK(t == btPC);
}

TEST_CASE("CLI --bed-type rejects unknown values", "[PlateBedTypes][cli]")
{
    BedType t = btEP;
    CHECK_FALSE(PlateBedTypes::parse_cli_bed_type("", t));
    CHECK_FALSE(PlateBedTypes::parse_cli_bed_type("glass", t));
    CHECK_FALSE(PlateBedTypes::parse_cli_bed_type("Default Plate", t));
    CHECK(t == btEP);
    const std::string keys = PlateBedTypes::cli_bed_type_key_list();
    for (const char *k : { "cool", "engineering", "high_temp", "textured_pei", "textured_cool", "supertack" })
        CHECK(keys.find(k) != std::string::npos);
}

TEST_CASE("--bed-type is a CLI option", "[PlateBedTypes][cli]")
{
    CHECK(cli_misc_config_def.has("bed_type"));
    CHECK(cli_misc_config_def.get("bed_type")->type == coString);
}
