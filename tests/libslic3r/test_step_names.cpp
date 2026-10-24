#include <catch2/catch.hpp>

#include "libslic3r/Model.hpp"
#include "libslic3r/Format/STEP.hpp"
#include "libslic3r/Format/STEPExport.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>

#include <algorithm>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

using namespace Slic3r;

namespace {

// Per-process unique temp name, removed on scope exit.
struct StepTempFile
{
    boost::filesystem::path path;
    explicit StepTempFile(const std::string &name)
        : path(boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("edge_stepname_%%%%-%%%%-%%%%"))
    {
        boost::filesystem::create_directories(path);
        path /= name;
    }
    ~StepTempFile()
    {
        boost::system::error_code ec;
        boost::filesystem::remove(path, ec);
        boost::filesystem::remove(path.parent_path(), ec);
    }
    std::string str() const { return path.string(); }
};

std::string read_file(const std::string &path)
{
    boost::nowide::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void write_file(const std::string &path, const std::string &content)
{
    boost::nowide::ofstream out(path, std::ios::binary);
    out << content;
}

void replace_all(std::string &text, const std::string &from, const std::string &to)
{
    for (size_t pos = text.find(from); pos != std::string::npos; pos = text.find(from, pos + to.size()))
        text.replace(pos, from.size(), to);
}

// A two-part assembly written by our own STEP exporter, whose ASCII placeholder part names are then
// patched to raw UTF-8, which is how most CAD exporters write non-ASCII names (rather than \X2\ escapes).
void write_utf8_named_step(const std::string &path)
{
    Model        model;
    ModelObject *object = model.add_object();
    object->name        = "assembly";
    object->add_volume(make_cube(20., 10., 5.), ModelVolumeType::MODEL_PART, false)->name = "ZZPIECE";
    ModelVolume *second = object->add_volume(make_cube(5., 5., 5.), ModelVolumeType::MODEL_PART, false);
    second->name        = "ZZGEHAEUSE";
    second->set_offset(Vec3d(30., 0., 0.));
    object->add_instance();

    StepExportReport report;
    REQUIRE(store_step(path, model, {}, report));
    REQUIRE(report.parts == 2);

    std::string text = read_file(path);
    REQUIRE(text.find("ZZPIECE") != std::string::npos);
    replace_all(text, "ZZPIECE", "pi\xC3\xA8" "ce");
    replace_all(text, "ZZGEHAEUSE", "Geh\xC3\xA4use");
    write_file(path, text);
}

std::vector<std::string> volume_names(const Model &model)
{
    std::vector<std::string> names;
    for (const ModelObject *object : model.objects)
        for (const ModelVolume *volume : object->volumes)
            names.push_back(volume->name);
    return names;
}

bool has_name(const std::vector<std::string> &names, const std::string &name)
{
    return std::find(names.begin(), names.end(), name) != names.end();
}

} // namespace

TEST_CASE("isUtf8 recognises two, three and four byte sequences", "[Step][StepNames]")
{
    CHECK(StepPreProcessor::isUtf8("\xC3\xA9"));         // U+00E9
    CHECK(StepPreProcessor::isUtf8("\xE4\xB8\xAD"));     // U+4E2D
    CHECK(StepPreProcessor::isUtf8("\xF0\x9F\x94\xA9")); // U+1F529
    CHECK(StepPreProcessor::isUtf8("bracket"));
    CHECK_FALSE(StepPreProcessor::isUtf8("\x81\x30"));   // 0x81 is not a lead byte
    CHECK_FALSE(StepPreProcessor::isUtf8("\xC3"));       // truncated sequence
}

TEST_CASE("STEP part names with accented characters keep their name on import", "[Step][StepNames]")
{
    StepTempFile step("accented.step");
    write_utf8_named_step(step.str());

    // The exact B-rep reader, used by the STEP exporter ...
    std::vector<NamedSolid> plain, split;
    REQUIRE(read_step_named_shapes(step.str().c_str(), plain, split));
    std::vector<std::string> shape_names;
    for (const NamedSolid &ns : plain)
        shape_names.push_back(ns.name);
    CHECK(has_name(shape_names, "pi\xC3\xA8" "ce"));
    CHECK(has_name(shape_names, "Geh\xC3\xA4use"));

    // ... and the importer the GUI uses.
    Model model;
    bool  cancelled = false;
    REQUIRE(load_step(step.str().c_str(), &model, cancelled));
    REQUIRE(model.objects.size() == 1);
    const std::vector<std::string> names = volume_names(model);
    CHECK(has_name(names, "pi\xC3\xA8" "ce"));
    CHECK(has_name(names, "Geh\xC3\xA4use"));
}
