// File > Export > Export Preset Bundle > "Process presets (.zip)": the row model behind the table,
// the selection -> zip entries mapping, and a round trip through PresetBundle::import_presets
// (libslic3r/ProcessPresetExport).
#include <catch2/catch.hpp>

#include <algorithm>
#include <chrono>
#include <memory>
#include <set>
#include <thread>

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>
#include <miniz.h>

#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/ProcessPresetExport.hpp"
#include "libslic3r/Utils.hpp"

using namespace Slic3r;
namespace fs = boost::filesystem;

namespace {

// A directory under the system temp dir, removed (best effort) at the end.
class ScratchDir
{
public:
    explicit ScratchDir(const std::string &name)
        : m_path(fs::temp_directory_path() / fs::unique_path("snorca_exportproc_" + name + "_%%%%-%%%%-%%%%"))
    {
        fs::create_directories(m_path);
    }
    ~ScratchDir()
    {
        for (int attempt = 0; attempt < 5; ++attempt) {
            if (attempt > 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(50 << attempt));
            boost::system::error_code ec;
            fs::remove_all(m_path, ec);
            if (!ec && !fs::exists(m_path, ec))
                return;
        }
    }
    ScratchDir(const ScratchDir &) = delete;
    ScratchDir &operator=(const ScratchDir &) = delete;
    const fs::path &path() const { return m_path; }

private:
    fs::path m_path;
};

// Points libslic3r's data dir somewhere else until the end of the scope.
class DataDirGuard
{
public:
    explicit DataDirGuard(const fs::path &p) : m_saved(data_dir()) { set_data_dir(p.string()); }
    ~DataDirGuard() { set_data_dir(m_saved); }
    DataDirGuard(const DataDirGuard &) = delete;
    DataDirGuard &operator=(const DataDirGuard &) = delete;

private:
    std::string m_saved;
};

std::string profiles_dir() { return (fs::path(TEST_DATA_DIR) / ".." / ".." / "resources" / "profiles").string(); }

// The shipped Snapmaker vendor profiles plus Orca's filament library, loaded the way the application
// loads system presets.
std::unique_ptr<PresetBundle> load_system_bundle()
{
    PresetBundle library;
    library.load_vendor_configs_from_json(profiles_dir(), PresetBundle::ORCA_FILAMENT_LIBRARY, PresetBundle::LoadSystem,
                                          ForwardCompatibilitySubstitutionRule::EnableSilent);
    auto bundle = std::make_unique<PresetBundle>();
    bundle->load_vendor_configs_from_json(profiles_dir(), "Snapmaker", PresetBundle::LoadSystem, ForwardCompatibilitySubstitutionRule::EnableSilent,
                                          &library);
    return bundle;
}

std::vector<const Preset *> listed_system_printers(const PresetBundle &bundle)
{
    std::vector<const Preset *> out;
    for (const Preset &p : bundle.printers.get_presets())
        if (p.is_system && p.is_visible && !p.is_default && bundle.printers.get_preset_base(p) == &p)
            out.push_back(&p);
    return out;
}

const Preset &first_system_process(const PresetBundle &bundle)
{
    for (const Preset &p : bundle.prints.get_presets())
        if (p.is_system && !p.is_default)
            return p;
    FAIL("no system process preset");
    return bundle.prints.get_presets().front();
}

// A user process preset the way "Save as" makes one: the parent's config, its name as inherits, the
// given compatible printers and layer height; the file is written under `user_dir`.
Preset &add_user_process(PresetBundle &bundle, const fs::path &user_dir, const std::string &name, const Preset &parent,
                         const std::vector<std::string> &compatible, double layer_height)
{
    DynamicPrintConfig config = parent.config;
    config.set_key_value("inherits", new ConfigOptionString(parent.name));
    config.option<ConfigOptionStrings>("compatible_printers", true)->values = compatible;
    config.set_key_value("compatible_printers_condition", new ConfigOptionString(""));
    config.option<ConfigOptionFloat>("layer_height", true)->value = layer_height;
    Preset &p = bundle.prints.load_preset((user_dir / "process" / (name + ".json")).string(), name, config, false);
    REQUIRE(p.save(nullptr));
    return p;
}

std::string read_bytes(const fs::path &path)
{
    boost::nowide::ifstream in(path.string(), std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// name -> bytes of every entry of the zip, plus the entry order.
std::vector<std::pair<std::string, std::string>> read_zip(const fs::path &zip_path)
{
    std::vector<std::pair<std::string, std::string>> out;
    mz_zip_archive zip;
    mz_zip_zero_struct(&zip);
    REQUIRE(mz_zip_reader_init_file(&zip, zip_path.string().c_str(), 0));
    for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&zip); ++i) {
        mz_zip_archive_file_stat st;
        REQUIRE(mz_zip_reader_file_stat(&zip, i, &st));
        size_t size = 0;
        void  *data = mz_zip_reader_extract_to_heap(&zip, i, &size, 0);
        REQUIRE(data != nullptr);
        out.emplace_back(st.m_filename, std::string(static_cast<const char *>(data), size));
        mz_free(data);
    }
    mz_zip_reader_end(&zip);
    return out;
}

// The export as it was before the table: for each ticked printer, its list of user process presets,
// a preset already added (same name) skipped, an empty file path skipped; entry "<name>.json".
std::vector<PresetZipEntry> legacy_entries(const ProcessPresetsByPrinter &by_printer, const std::vector<std::string> &ticked_printers)
{
    std::vector<PresetZipEntry> out;
    std::set<std::string>       seen;
    for (const std::string &printer : ticked_printers) {
        auto it = by_printer.find(printer);
        if (it == by_printer.end())
            continue;
        for (const Preset *preset : it->second) {
            if (!seen.insert(preset->name).second)
                continue;
            const std::string path = fs::path(preset->file).make_preferred().string();
            if (path.empty())
                continue;
            out.push_back({ preset->name + ".json", path });
        }
    }
    return out;
}

std::vector<std::pair<std::string, std::string>> sorted_pairs(const std::vector<PresetZipEntry> &entries)
{
    std::vector<std::pair<std::string, std::string>> out;
    for (const PresetZipEntry &e : entries)
        out.emplace_back(e.name, e.path);
    std::sort(out.begin(), out.end());
    return out;
}

// A model without a PresetBundle, for the pure mapping tests.
ProcessExportModel hand_model(const std::vector<std::pair<std::string, std::string>> &name_file)
{
    ProcessExportModel m;
    for (const auto &nf : name_file) {
        ProcessExportRow r;
        r.id   = m.rows.size();
        r.name = nf.first;
        r.file = nf.second;
        m.rows.push_back(r);
    }
    return m;
}

struct Fixture
{
    ScratchDir                   data { "data" };
    DataDirGuard                 guard { data.path() };
    std::unique_ptr<PresetBundle> bundle;
    fs::path                     user_dir;
    std::string                  p1, p2, user_printer;
    std::vector<std::string>     system_printers;      // every listed system printer
    Preset                      *a = nullptr, *b = nullptr, *c = nullptr, *d = nullptr, *embedded = nullptr;
    std::string                  system_process;

    Fixture()
    {
        bundle   = load_system_bundle();
        user_dir = data.path() / PRESET_USER_DIR / "default";
        const auto printers = listed_system_printers(*bundle);
        REQUIRE(printers.size() >= 3);
        for (const Preset *p : printers)
            system_printers.push_back(p->name);
        p1 = system_printers[0];
        p2 = system_printers[1];

        // A user printer derived from p1.
        DynamicPrintConfig pc = bundle->printers.find_preset(p1)->config;
        pc.set_key_value("inherits", new ConfigOptionString(p1));
        user_printer = "ET user printer";
        Preset &up   = bundle->printers.load_preset((user_dir / "machine" / (user_printer + ".json")).string(), user_printer, pc, false);
        up.is_visible = true;

        const Preset &parent = first_system_process(*bundle);
        system_process       = parent.name;
        a = &add_user_process(*bundle, user_dir, "ET A only p1", parent, { p1 }, 0.2);
        b = &add_user_process(*bundle, user_dir, "ET B only p2", parent, { p2 }, 0.16);
        c = &add_user_process(*bundle, user_dir, "et C every printer", parent, {}, 0.12);
        d = &add_user_process(*bundle, user_dir, "ET D user printer only", parent, { user_printer }, 0.28);
        embedded = &add_user_process(*bundle, user_dir, "ET E embedded", parent, { p1 }, 0.3);
        embedded->is_project_embedded = true;
    }

    const ProcessExportRow *row(const ProcessExportModel &m, const std::string &name) const
    {
        for (const ProcessExportRow &r : m.rows)
            if (r.name == name)
                return &r;
        return nullptr;
    }
};

} // namespace

TEST_CASE("process export model: one row per user process preset", "[ProcessPresetExport]")
{
    Fixture f;
    const ProcessExportModel model = build_process_export_model(*f.bundle);

    // The three listed presets plus the one compatible with the user printer only; no system
    // preset, no project-embedded preset.
    REQUIRE(model.rows.size() == 4);
    CHECK(f.row(model, "ET E embedded") == nullptr);
    CHECK(f.row(model, f.system_process) == nullptr);

    // Ordered by name, case-insensitively, ids are the positions.
    std::vector<std::string> names;
    for (size_t i = 0; i < model.rows.size(); ++i) {
        CHECK(model.rows[i].id == i);
        names.push_back(model.rows[i].name);
    }
    CHECK(names == std::vector<std::string>{ "ET A only p1", "ET B only p2", "et C every printer", "ET D user printer only" });
}

TEST_CASE("process export model: columns", "[ProcessPresetExport]")
{
    Fixture f;
    const ProcessExportModel model = build_process_export_model(*f.bundle);

    const ProcessExportRow *a = f.row(model, "ET A only p1");
    REQUIRE(a != nullptr);
    CHECK(a->printers == std::vector<std::string>{ f.p1 });
    CHECK_FALSE(a->all_printers);
    CHECK(a->inherits == f.system_process);
    CHECK(a->layer_height == Approx(0.2));
    CHECK(a->file == f.a->file);
    CHECK(a->mtime > 0);

    const ProcessExportRow *b = f.row(model, "ET B only p2");
    REQUIRE(b != nullptr);
    CHECK(b->printers == std::vector<std::string>{ f.p2 });
    CHECK(b->layer_height == Approx(0.16));

    // Compatible with everything: listed under every system printer, flagged as such.
    const ProcessExportRow *c = f.row(model, "et C every printer");
    REQUIRE(c != nullptr);
    CHECK(c->all_printers);
    std::vector<std::string> sys = f.system_printers;
    std::sort(sys.begin(), sys.end());
    CHECK(c->printers == sys);

    // Compatible with the user's own printer only: listed under it, so it can still be exported.
    const ProcessExportRow *d = f.row(model, "ET D user printer only");
    REQUIRE(d != nullptr);
    CHECK(d->printers == std::vector<std::string>{ f.user_printer });
    CHECK_FALSE(d->all_printers);

    // The printer filter's values: every printer a row is listed under, sorted, no repeats.
    CHECK(std::is_sorted(model.printers.begin(), model.printers.end()));
    CHECK(std::set<std::string>(model.printers.begin(), model.printers.end()).size() == model.printers.size());
    CHECK(std::count(model.printers.begin(), model.printers.end(), f.p1) == 1);
    CHECK(std::count(model.printers.begin(), model.printers.end(), f.p2) == 1);
    CHECK(std::count(model.printers.begin(), model.printers.end(), f.user_printer) == 1);
    CHECK(model.printers.size() == f.system_printers.size() + 1);
}

TEST_CASE("process export entries: selection -> zip entries", "[ProcessPresetExport]")
{
    const ProcessExportModel model = hand_model({ { "B", "/p/B.json" }, { "A", "/p/A.json" }, { "C", "" }, { "A2", "/p/A2.json" } });

    SECTION("nothing selected, nothing exported")
    {
        CHECK(process_export_entries(model, {}).empty());
    }
    SECTION("entries follow the row order, not the click order; repeated and unknown ids are ignored")
    {
        const auto e = process_export_entries(model, { 3, 0, 0, 99 });
        REQUIRE(e.size() == 2);
        CHECK(e[0].name == "B.json");
        CHECK(e[1].name == "A2.json");
        CHECK(fs::path(e[0].path).filename() == "B.json");
    }
    SECTION("a preset without a file is skipped and reported")
    {
        std::vector<std::string> skipped;
        const auto e = process_export_entries(model, { 1, 2 }, &skipped);
        REQUIRE(e.size() == 1);
        CHECK(e[0].name == "A.json");
        REQUIRE(skipped.size() == 1);
        CHECK(skipped[0].find("C") == 0);
    }
    SECTION("two presets that would share a zip entry: the first wins, the second is reported")
    {
        const ProcessExportModel dup = hand_model({ { "X", "/p/X1.json" }, { "X", "/q/X2.json" } });
        std::vector<std::string> skipped;
        const auto e = process_export_entries(dup, { 0, 1 }, &skipped);
        REQUIRE(e.size() == 1);
        CHECK(fs::path(e[0].path).filename() == "X1.json");
        CHECK(skipped.size() == 1);
    }
}

TEST_CASE("process export rows json: what the table page receives", "[ProcessPresetExport]")
{
    ProcessExportModel m = hand_model({ { "A", "/p/A.json" }, { "B", "/p/B.json" } });
    m.rows[0].printers      = { "P1", "P2" };
    m.rows[0].inherits      = "0.20mm Standard";
    m.rows[0].layer_height  = 0.2;
    m.rows[0].mtime         = 1790000000;
    m.rows[1].all_printers  = true; // no layer height, no mtime, no parent
    m.printers              = { "P1", "P2" };

    const nlohmann::json j = process_export_rows_json(m, { 1 });
    REQUIRE(j["rows"].is_array());
    REQUIRE(j["rows"].size() == 2);
    const auto &a = j["rows"][0];
    CHECK(a["id"] == 0);
    CHECK(a["name"] == "A");
    CHECK(a["printers"] == nlohmann::json::array({ "P1", "P2" }));
    CHECK(a["all"] == false);
    CHECK(a["inherits"] == "0.20mm Standard");
    CHECK(a["lh"].get<double>() == Approx(0.2));
    CHECK(a["mtime"].get<std::int64_t>() == 1790000000);
    const auto &b = j["rows"][1];
    CHECK(b["all"] == true);
    CHECK(b["inherits"] == "");
    CHECK(b["lh"].is_null());      // the page shows a dash
    CHECK(b["mtime"].is_null());
    CHECK(j["printers"] == nlohmann::json::array({ "P1", "P2" }));
    CHECK(j["selected"] == nlohmann::json::array({ 1 }));
    // Non-ASCII names survive the round trip through the text the page is sent.
    std::string name = "Pr";
    for (int byte : { 0xC3, 0xA9 }) name += char(byte);                     // e-acute
    name += "set ";
    for (int byte : { 0xE2, 0x80, 0x93, 0xE6, 0xB5, 0x8B }) name += char(byte); // en dash, a CJK character
    ProcessExportModel u = hand_model({ { name, "/p/u.json" } });
    CHECK(nlohmann::json::parse(process_export_rows_json(u, {}).dump(-1, ' ', true))["rows"][0]["name"] == u.rows[0].name);
}

TEST_CASE("process export: all presets of one printer = what the per-printer export wrote", "[ProcessPresetExport]")
{
    Fixture f;
    const ProcessPresetsByPrinter by_printer = collect_user_process_presets(*f.bundle);
    const ProcessExportModel      model      = build_process_export_model(by_printer, f.bundle->printers);

    for (const std::string &printer : { f.p1, f.p2, f.system_printers.back() }) {
        INFO("printer " << printer);
        // The old dialog's list for this printer ...
        const auto expected = sorted_pairs(legacy_entries(by_printer, { printer }));
        REQUIRE_FALSE(expected.empty());
        // ... against ticking every row that is listed under it.
        std::vector<size_t> ids;
        for (const ProcessExportRow &r : model.rows)
            if (std::find(r.printers.begin(), r.printers.end(), printer) != r.printers.end())
                ids.push_back(r.id);
        CHECK(sorted_pairs(process_export_entries(model, ids)) == expected);
    }

    // Two printers ticked in the old dialog = the union, one entry per preset.
    {
        const auto expected = sorted_pairs(legacy_entries(by_printer, { f.p1, f.p2 }));
        std::vector<size_t> ids;
        for (const ProcessExportRow &r : model.rows)
            for (const std::string &p : r.printers)
                if (p == f.p1 || p == f.p2) {
                    ids.push_back(r.id);
                    break;
                }
        CHECK(sorted_pairs(process_export_entries(model, ids)) == expected);
    }
}

TEST_CASE("process export: the zip has the old layout and imports back exactly", "[ProcessPresetExport]")
{
    Fixture f;
    const ProcessPresetsByPrinter by_printer = collect_user_process_presets(*f.bundle);
    const ProcessExportModel      model      = build_process_export_model(by_printer, f.bundle->printers);
    ScratchDir                    out("out");

    // Rows of two printers, plus the one for the user printer: A, B and D, but not C.
    std::vector<size_t> ids;
    for (const char *n : { "ET A only p1", "ET B only p2", "ET D user printer only" })
        ids.push_back(f.row(model, n)->id);
    const auto entries = process_export_entries(model, ids);
    REQUIRE(entries.size() == 3);

    const fs::path zip = out.path() / "Process presets.zip";
    REQUIRE(write_presets_zip(zip.string(), entries) == PresetZipResult::Ok);

    // Layout: flat, "<name>.json", content = the preset's own file, byte for byte.
    const auto content = read_zip(zip);
    REQUIRE(content.size() == 3);
    for (const auto &[name, bytes] : content) {
        INFO(name);
        CHECK(name.find('/') == std::string::npos);
        CHECK(name.find('\\') == std::string::npos);
        CHECK(name.size() > 5);
        CHECK(name.substr(name.size() - 5) == ".json");
        const Preset *p = f.bundle->prints.find_preset(name.substr(0, name.size() - 5), false);
        REQUIRE(p != nullptr);
        CHECK(bytes == read_bytes(p->file));
    }

    // The same bytes the old export would have produced for the same presets.
    {
        const fs::path old_zip = out.path() / "legacy.zip";
        std::vector<PresetZipEntry> old_entries;
        for (const char *n : { "ET A only p1", "ET B only p2", "ET D user printer only" })
            old_entries.push_back({ std::string(n) + ".json", f.bundle->prints.find_preset(n, false)->file });
        REQUIRE(write_presets_zip(old_zip.string(), old_entries) == PresetZipResult::Ok);
        auto a = read_zip(zip), b = read_zip(old_zip);
        std::sort(a.begin(), a.end());
        std::sort(b.begin(), b.end());
        CHECK(a == b);
    }

    // Import into a scratch data dir with the same system presets: exactly those presets arrive.
    ScratchDir dest("dest");
    {
        DataDirGuard guard(dest.path());
        auto target = load_system_bundle();
        target->update_user_presets_directory("default"); // where an import saves the presets
        const size_t user_before = std::count_if(target->prints.get_presets().begin(), target->prints.get_presets().end(),
                                                 [](const Preset &p) { return !p.is_system && !p.is_default; });
        CHECK(user_before == 0);

        std::vector<std::string> files{ zip.string() };
        target->import_presets(files, [](const std::string &) { return 1; }, ForwardCompatibilitySubstitutionRule::Enable);
        CHECK(files.size() == 3);

        std::set<std::string> arrived;
        for (const Preset &p : target->prints.get_presets())
            if (!p.is_system && !p.is_default)
                arrived.insert(p.name);
        CHECK(arrived == std::set<std::string>{ "ET A only p1", "ET B only p2", "ET D user printer only" });

        const Preset *b = target->prints.find_preset("ET B only p2", false);
        REQUIRE(b != nullptr);
        CHECK(b->inherits() == f.system_process);
        CHECK(b->config.opt_float("layer_height") == Approx(0.16));
        // And the files landed in the data dir's user folder.
        CHECK(fs::exists(dest.path() / PRESET_USER_DIR / "default" / "process" / "ET B only p2.json"));
    }
}
