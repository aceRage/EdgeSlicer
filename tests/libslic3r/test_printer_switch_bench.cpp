// DIAGNOSTIC ONLY (branch diag/printer-switch-timing, not for merge).
//
// Headless timing of the PresetBundle half of a printer switch (Plater combo -> Tab::select_preset),
// on the bundled profiles of the vendors the owner has installed (BBL, Custom, Elegoo, Flashforge,
// OrcaFilamentLibrary, Snapmaker) plus Voron, and a synthetic user-preset set about the owner's
// size (600 filament, 140 process user presets). Hidden test: run it with
//   libslic3r_tests "[bench_printer_switch]"
// It never touches %APPDATA%: the datadir is a scratch folder under the temp directory.
//
// Steps timed per switch, the way Tab::select_preset / Sidebar / Plater run them for a printer:
//   capture_carry   PresetBundle::capture_print_settings_carry (FFF -> FFF)
//   compat_checks   is_compatible_with_printer for the current print + filament
//   select          printers.select_preset_by_name
//   update_compat   update_compatible(Always, Always)
//   update_sel      update_selections(app_config)      (remember_printer_config)
//   carry_apply     select_print_carry_target + apply_print_settings_carry
//   full_config     PresetBundle::full_config()          (x1, on_select_preset)
//   cfg_diff        full_config.diff(previous)           (Plater::on_config_change)
//   printer_type    Preset::get_printer_type x6          (has_cali_lines/get_default_bed_type, per on_presets_changed)
//   bbl_checks      is_bbl_vendor + use_bbl_network x6   (each copies the printer config)
//   export_sel      export_selections(app_config) x5     (every Sidebar::update_presets)
//   cfg_copy        printer config copy x6               (update_all_preset_comboboxes / update_presets)

#include <catch2/catch.hpp>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <boost/filesystem.hpp>

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"

using namespace Slic3r;
namespace fs = boost::filesystem;

namespace {

using clk = std::chrono::steady_clock;

double ms_since(clk::time_point t0) { return std::chrono::duration<double, std::milli>(clk::now() - t0).count(); }

void copy_vendor_json(const fs::path &profiles, const fs::path &system_dir, const std::string &vendor)
{
    fs::copy_file(profiles / (vendor + ".json"), system_dir / (vendor + ".json"), fs::copy_option::overwrite_if_exists);
    const fs::path src = profiles / vendor;
    if (!fs::exists(src))
        return;
    for (fs::recursive_directory_iterator it(src), end; it != end; ++it) {
        if (!fs::is_regular_file(it->path()) || it->path().extension() != ".json")
            continue;
        const fs::path rel = fs::relative(it->path(), profiles);
        fs::create_directories((system_dir / rel).parent_path());
        fs::copy_file(it->path(), system_dir / rel, fs::copy_option::overwrite_if_exists);
    }
}

void write_user(const fs::path &dir, const char *type_dir, const char *type, const std::string &name, const std::string &inherits)
{
    const fs::path file = dir / type_dir / (name + ".json");
    fs::create_directories(file.parent_path());
    std::ofstream(file.string()) << R"({"type":")" << type << R"(","name":")" << name
                                 << R"(","from":"User","version":"1.0.0","inherits":")" << inherits << R"("})";
}

struct Stats
{
    std::map<std::string, std::vector<double>> samples;
    std::vector<std::string>                   order;
    void add(const std::string &k, double v)
    {
        if (samples.find(k) == samples.end())
            order.push_back(k);
        samples[k].push_back(v);
    }
};

double median(std::vector<double> v)
{
    if (v.empty())
        return 0.;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

} // namespace

TEST_CASE("Printer switch: PresetBundle cost per step", "[.][bench_printer_switch]")
{
    const fs::path profiles = fs::path(TEST_DATA_DIR) / ".." / ".." / "resources" / "profiles";
    const fs::path scratch  = fs::temp_directory_path() / fs::unique_path("printer_switch_bench_%%%%-%%%%");
    const fs::path sys      = scratch / "system";
    fs::create_directories(sys);
    for (const char *v : { "OrcaFilamentLibrary", "BBL", "Custom", "Elegoo", "Flashforge", "Snapmaker", "Voron" })
        copy_vendor_json(profiles, sys, v);

    // Synthetic user presets, about the owner's count (user/default: ~690 filament, 141 process).
    const fs::path user = scratch / "user" / "default";
    const std::vector<std::string> fil_parents = { "Bambu PLA Basic @BBL X1C", "Bambu PLA Basic @BBL H2D", "Bambu PETG HF @BBL X1C",
                                                   "Generic PLA @U1 0.4 nozzle" };
    const std::vector<std::string> proc_parents = { "0.20mm Standard @BBL X1C", "0.20mm Standard @BBL H2D",
                                                    "0.08mm Standard @Snapmaker U1 (0.4 nozzle)" };
    for (int i = 0; i < 600; ++i)
        write_user(user, "filament", "filament", "Bench user filament " + std::to_string(i), fil_parents[i % fil_parents.size()]);
    for (int i = 0; i < 140; ++i)
        write_user(user, "process", "process", "Bench user process " + std::to_string(i), proc_parents[i % proc_parents.size()]);

    const std::string saved_data_dir = data_dir();
    set_data_dir(scratch.string());

    std::ostringstream out;
    {
        PresetBundle bundle;
        AppConfig    app_config;
        auto t_load = clk::now();
        bundle.load_presets(app_config, ForwardCompatibilitySubstitutionRule::EnableSilent);
        const double load_ms = ms_since(t_load);

        size_t n_print = 0, n_fil = 0, n_printer = 0;
        for (const Preset &p : bundle.prints) (void)p, ++n_print;
        for (const Preset &p : bundle.filaments) (void)p, ++n_fil;
        for (const Preset &p : bundle.printers) (void)p, ++n_printer;
        out << "load_presets ms=" << std::fixed << std::setprecision(1) << load_ms << " presets: printers=" << n_printer
            << " prints=" << n_print << " filaments=" << n_fil << " vendors=" << bundle.vendors.size() << "\n";

        const std::vector<std::string> cycle = { "Bambu Lab X1 Carbon 0.4 nozzle", "Snapmaker U1 (0.4 nozzle)", "Bambu Lab H2D 0.4 nozzle",
                                                 "Voron 2.4 350 0.4 nozzle" };
        const std::vector<std::string> short_name = { "X1C", "U1", "H2D", "Voron" };
        for (const std::string &p : cycle)
            REQUIRE(bundle.printers.find_preset(p, false) != nullptr);

        REQUIRE(bundle.printers.select_preset_by_name(cycle[0], true));
        bundle.update_compatible(PresetSelectCompatibleType::Always, PresetSelectCompatibleType::Always);
        DynamicPrintConfig prev_full = bundle.full_config();

        Stats  per_step;  // key "step"
        Stats  per_total; // key "A->B"
        const int rounds = 5;
        for (int r = 0; r < rounds; ++r) {
            for (size_t k = 1; k <= cycle.size(); ++k) {
                const size_t from = (k - 1) % cycle.size(), to = k % cycle.size();
                const std::string trans = short_name[from] + "->" + short_name[to];
                const auto t_all = clk::now();
                auto t = clk::now();

                PrintSettingsCarry carry = bundle.capture_print_settings_carry();
                per_step.add("capture_carry", ms_since(t));

                t = clk::now();
                const Preset &new_printer = *bundle.printers.find_preset(cycle[to], true);
                const PresetWithVendorProfile new_pv = bundle.printers.get_preset_with_vendor_profile(new_printer);
                bool c1 = is_compatible_with_printer(bundle.prints.get_edited_preset_with_vendor_profile(), new_pv);
                bool c2 = is_compatible_with_printer(bundle.filaments.get_edited_preset_with_vendor_profile(), new_pv);
                (void)c1; (void)c2;
                per_step.add("compat_checks", ms_since(t));

                t = clk::now();
                bundle.printers.select_preset_by_name(cycle[to], false);
                per_step.add("select", ms_since(t));

                t = clk::now();
                bundle.update_compatible(PresetSelectCompatibleType::Always, PresetSelectCompatibleType::Always);
                per_step.add("update_compat", ms_since(t));

                t = clk::now();
                std::vector<std::string> keep_filaments = bundle.filament_presets;
                bundle.update_selections(app_config);
                bundle.filament_presets = keep_filaments;
                per_step.add("update_sel", ms_since(t));

                t = clk::now();
                bundle.select_print_carry_target(carry);
                bundle.apply_print_settings_carry(carry);
                per_step.add("carry_apply", ms_since(t));

                t = clk::now();
                DynamicPrintConfig full = bundle.full_config();
                per_step.add("full_config", ms_since(t));

                t = clk::now();
                t_config_option_keys diff = prev_full.diff(full);
                per_step.add("cfg_diff", ms_since(t));
                prev_full = std::move(full);

                t = clk::now();
                for (int i = 0; i < 6; ++i) {
                    std::string pt = bundle.printers.get_edited_preset().get_printer_type(&bundle);
                    (void)pt;
                }
                per_step.add("printer_type_x6", ms_since(t));

                t = clk::now();
                for (int i = 0; i < 6; ++i) {
                    bool a = bundle.is_bbl_vendor();
                    bool b = bundle.use_bbl_network();
                    (void)a; (void)b;
                }
                per_step.add("bbl_checks_x6", ms_since(t));

                t = clk::now();
                for (int i = 0; i < 5; ++i)
                    bundle.export_selections(app_config);
                per_step.add("export_sel_x5", ms_since(t));

                t = clk::now();
                for (int i = 0; i < 6; ++i) {
                    DynamicPrintConfig copy = bundle.printers.get_edited_preset().config;
                    (void)copy;
                }
                per_step.add("printer_cfg_copy_x6", ms_since(t));

                per_total.add(trans, ms_since(t_all));
                if (r == 0)
                    out << "  " << trans << ": config diff keys=" << diff.size() << "\n";
            }
        }

        out << "median ms per step over " << rounds << " rounds x " << cycle.size() << " switches:\n";
        for (const std::string &k : per_step.order)
            out << "  " << std::left << std::setw(22) << k << std::right << std::setw(9) << median(per_step.samples[k]) << "\n";
        out << "median ms per switch (sum of the steps above):\n";
        for (const std::string &k : per_total.order)
            out << "  " << std::left << std::setw(22) << k << std::right << std::setw(9) << median(per_total.samples[k]) << "\n";
    }

    set_data_dir(saved_data_dir);
    boost::system::error_code ec;
    fs::remove_all(scratch, ec);

    const fs::path report = fs::temp_directory_path() / "printer_switch_bench_result.txt";
    std::ofstream(report.string()) << out.str();
    std::cout << "[printer_switch_bench]\n" << out.str() << "report: " << report.string() << std::endl;
    WARN(out.str());
    SUCCEED();
}
