#include <catch2/catch.hpp>

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>

#include "slic3r/GUI/DeviceModelCode.hpp"

using Slic3r::GUI::load_model_subseries;
using Slic3r::GUI::resolve_model_subseries;
using Slic3r::GUI::strip_model_revision;

namespace {
struct TempPrintersDir
{
    boost::filesystem::path dir;
    TempPrintersDir()
    {
        dir = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("printers-%%%%-%%%%");
        boost::filesystem::create_directories(dir);
    }
    ~TempPrintersDir()
    {
        boost::system::error_code ec;
        boost::filesystem::remove_all(dir, ec);
    }
    void write(const char *name, const std::string &body)
    {
        boost::nowide::ofstream f((dir / name).string().c_str());
        f << body;
    }
};
} // namespace

TEST_CASE("A hardware revision suffix is dropped, a dash in the name is not", "[DeviceModelCode]")
{
    // The reported case: a later-batch H2C says O1C2-V2 and must land on the O1C2 definition.
    CHECK(strip_model_revision("O1C2-V2") == "O1C2");
    CHECK(strip_model_revision("O1D-V2") == "O1D");
    CHECK(strip_model_revision("N7-V12") == "N7");
    // Codes without a revision come back untouched, including the X1 family's dashed names.
    CHECK(strip_model_revision("O1C2") == "O1C2");
    CHECK(strip_model_revision("BL-P001") == "BL-P001");
    CHECK(strip_model_revision("C12") == "C12");
    CHECK(strip_model_revision("") == "");
    // Not a revision: no digits, a lower-case v, or nothing after the V.
    CHECK(strip_model_revision("O1C2-VX") == "O1C2-VX");
    CHECK(strip_model_revision("O1C2-v2") == "O1C2-v2");
    CHECK(strip_model_revision("O1C2-V") == "O1C2-V");
    CHECK(strip_model_revision("-V2") == "-V2");
}

TEST_CASE("The subseries table comes from the printer definitions and resolves a sub-series code", "[DeviceModelCode]")
{
    TempPrintersDir t;
    t.write("O1C2.json", R"({"00.00.00.00": {"model_id": "O1C2", "printer_type": "O1C2", "subseries": ["O1C2-V2", "O1C2-V3"]}})");
    t.write("O1D.json", R"({"00.00.00.00": {"model_id": "O1D", "printer_type": "O1D", "subseries": ["O1D-V2"]}})");
    // No subseries key: not in the table.
    t.write("C12.json", R"({"00.00.00.00": {"model_id": "C12", "printer_type": "C12"}})");
    // The blacklist that shares the folder has another shape and must be skipped, not fatal.
    t.write("filaments_blacklist.json", R"({"blacklist": [{"model_id": ["O1C2"]}]})");
    t.write("version.txt", "01.00.00.00");
    t.write("broken.json", "{ this is not json");

    const auto table = load_model_subseries(t.dir.string());
    REQUIRE(table.size() == 2);
    CHECK(resolve_model_subseries("O1C2-V2", table) == "O1C2");
    CHECK(resolve_model_subseries("O1C2-V3", table) == "O1C2");
    CHECK(resolve_model_subseries("O1D-V2", table) == "O1D");
    // A parent code is not its own sub-series; the file lookup handles it.
    CHECK(resolve_model_subseries("O1C2", table) == "");
    CHECK(resolve_model_subseries("O1S-V2", table) == "");
    CHECK(resolve_model_subseries("", table) == "");
}

TEST_CASE("A missing printers folder yields an empty table", "[DeviceModelCode]")
{
    const auto table = load_model_subseries((boost::filesystem::temp_directory_path() / "no-such-printers-dir-xyz").string());
    CHECK(table.empty());
    CHECK(resolve_model_subseries("O1C2-V2", table) == "");
}
