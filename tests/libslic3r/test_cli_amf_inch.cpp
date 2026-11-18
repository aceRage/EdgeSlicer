#include <catch2/catch.hpp>

#include "libslic3r/CliAmfInch.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/system/error_code.hpp>

#include <algorithm>
#include <string>

using namespace Slic3r;
namespace fs = boost::filesystem;

namespace {

std::string triangle_amf(const char *unit)
{
    return std::string("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                       "<amf unit=\"") +
           unit +
           "\">\n"
           "  <object id=\"0\">\n"
           "    <mesh>\n"
           "      <vertices>\n"
           "        <vertex><coordinates><x>0</x><y>0</y><z>0</z></coordinates></vertex>\n"
           "        <vertex><coordinates><x>1</x><y>0</y><z>0</z></coordinates></vertex>\n"
           "        <vertex><coordinates><x>0</x><y>1</y><z>0</z></coordinates></vertex>\n"
           "      </vertices>\n"
           "      <volume>\n"
           "        <triangle><v1>0</v1><v2>1</v2><v3>2</v3></triangle>\n"
           "      </volume>\n"
           "    </mesh>\n"
           "  </object>\n"
           "</amf>\n";
}

void write_temp_amf(const fs::path &path, const std::string &xml)
{
    boost::nowide::ofstream out(path.string(), std::ios::binary | std::ios::trunc);
    REQUIRE(out);
    out << xml;
    REQUIRE(out);
}

float max_vertex_x(const Model &model)
{
    REQUIRE_FALSE(model.objects.empty());
    REQUIRE_FALSE(model.objects.front()->volumes.empty());
    float max_x = 0.f;
    for (const Vec3f &v : model.objects.front()->volumes.front()->mesh().its.vertices)
        max_x = std::max(max_x, v.x());
    return max_x;
}

} // namespace

TEST_CASE("CLI treats only AMF paths as inch-flag files", "[CLI][AMF][Regression]")
{
    CHECK(cli_path_is_amf("model.amf"));
    CHECK(cli_path_is_amf("model.AMF"));
    CHECK(cli_path_is_amf("model.zip.amf"));
    CHECK_FALSE(cli_path_is_amf("model.3mf"));
    CHECK_FALSE(cli_path_is_amf("model.stl"));
}

TEST_CASE("CLI AMF inch helper leaves a 3MF project flag alone", "[CLI][AMF][Regression]")
{
    Model model;
    bool  is_bbl_3mf = true;
    cli_apply_amf_inch_flag("project.3mf", model, is_bbl_3mf);
    CHECK(is_bbl_3mf);
}

TEST_CASE("CLI AMF inch helper always clears the reused Bambu-3MF flag", "[CLI][AMF][Regression]")
{
    Model model;
    bool  is_bbl_3mf = true;
    cli_apply_amf_inch_flag("part.amf", model, is_bbl_3mf);
    CHECK_FALSE(is_bbl_3mf);

    is_bbl_3mf = false;
    cli_apply_amf_inch_flag("part.amf", model, is_bbl_3mf);
    CHECK_FALSE(is_bbl_3mf);
}

TEST_CASE("Model::read_from_file reports AMF inches through is_bbl_3mf; CLI converts and clears it",
          "[CLI][AMF][Regression]")
{
    const fs::path dir = fs::temp_directory_path() / fs::unique_path("edgeslicer_cli_amf_%%%%%%%%");
    fs::create_directories(dir);
    const fs::path inch_path = dir / "inch.amf";
    const fs::path mm_path   = dir / "mm.amf";
    write_temp_amf(inch_path, triangle_amf("inch"));
    write_temp_amf(mm_path, triangle_amf("millimeter"));

    DynamicPrintConfig        config;
    ConfigSubstitutionContext subst(ForwardCompatibilitySubstitutionRule::Enable);

    bool  inch_flag = false;
    Model inch_model =
        Model::read_from_file(inch_path.string(), &config, &subst, LoadStrategy::AddDefaultInstances, nullptr, nullptr, &inch_flag);
    // The reused flag is the bug: inches come back as "this is a Bambu 3MF".
    REQUIRE(inch_flag);
    REQUIRE_THAT(max_vertex_x(inch_model), Catch::Matchers::WithinAbs(1.f, 1e-4f));

    cli_apply_amf_inch_flag(inch_path.string(), inch_model, inch_flag);
    CHECK_FALSE(inch_flag);
    REQUIRE_THAT(max_vertex_x(inch_model), Catch::Matchers::WithinAbs(25.4f, 1e-3f));

    bool  mm_flag = true;
    Model mm_model =
        Model::read_from_file(mm_path.string(), &config, &subst, LoadStrategy::AddDefaultInstances, nullptr, nullptr, &mm_flag);
    CHECK_FALSE(mm_flag);
    cli_apply_amf_inch_flag(mm_path.string(), mm_model, mm_flag);
    CHECK_FALSE(mm_flag);
    REQUIRE_THAT(max_vertex_x(mm_model), Catch::Matchers::WithinAbs(1.f, 1e-4f));

    boost::system::error_code ec;
    fs::remove_all(dir, ec);
}
