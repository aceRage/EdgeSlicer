// Airbrush P0 (tests/design_airbrush_paint.md, section 5): how paint density costs.
//
// Hidden benchmark. For three densities of ordinary MMU paint (about 10k / 100k / 1M painted
// subtriangles on a 40 mm sphere, 1 mm specks at 30% coverage, written with the Image Fill
// encoder), it times:
//   * TriangleSelector deserialize / serialize (what loading a project and every paint stroke cost),
//   * FacetsAnnotation::get_facets() per state: the CPU half of a painted GLVolume's render-buffer
//     build (3DScene.cpp, GLVolume::render's mmuseg_models); the GL upload is not measurable here,
//   * the 3MF: store time, load time and file size,
//   * slicing: Print::process() of the painted sphere against the same sphere unpainted, and
//     multi_material_segmentation_by_painting() alone on the sliced object (median of 3).
// Run:  fff_print_tests "[AirbrushBench]"
#include <catch2/catch.hpp>

#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/ImageFill.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/MultiMaterialSegmentation.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Semver.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/filesystem.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

using namespace Slic3r;

namespace {

using clk = std::chrono::steady_clock;
double ms_since(clk::time_point t0) { return std::chrono::duration<double, std::milli>(clk::now() - t0).count(); }

DynamicPrintConfig two_filament_config()
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(2);
    config.set_num_filaments(2);
    config.option<ConfigOptionFloats>("filament_diameter")->values = { 1.75, 1.75 };
    config.option<ConfigOptionStrings>("filament_colour")->values  = { "#FFFFFF", "#3A7BD5" };
    config.option<ConfigOptionFloats>("nozzle_diameter")->values   = { 0.4, 0.4 };
    config.option<ConfigOptionBool>("single_extruder_multi_material")->value = true;
    config.option<ConfigOptionBool>("enable_prime_tower")->value             = false;
    config.set_deserialize_strict({ { "layer_height", "0.12" }, { "initial_layer_print_height", "0.2" } });
    return config;
}

// 1 mm specks at 30% coverage: a hash of the 1 mm cube cell a leaf's centroid falls in.
int speck_state(const Vec3f &p)
{
    const int32_t x = int32_t(std::floor(p.x())), y = int32_t(std::floor(p.y())), z = int32_t(std::floor(p.z()));
    uint32_t      h = uint32_t(x) * 73856093u ^ uint32_t(y) * 19349663u ^ uint32_t(z) * 83492791u;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return (h % 1000u) < 300u ? 2 : 0;
}

struct Density
{
    const char *name;
    size_t      facets;   // sphere facets; depth 2 gives 16 leaves each
};

} // namespace

TEST_CASE("Airbrush P0 benchmark: paint density vs serialisation, 3MF, render data and segmentation", "[.][Benchmark][AirbrushBench]")
{
    const int depth = 2;
    // ~5 painted of 16 leaves per facet with 1 mm specks at 30%, so these land near 10k / 100k / 1M painted.
    const Density densities[] = { { "10k", 2100 }, { "100k", 21000 }, { "1M", 210000 } };

    const boost::filesystem::path tmp = boost::filesystem::temp_directory_path() / ("airbrush_bench_" + std::to_string(get_current_pid()));
    boost::filesystem::create_directories(tmp);
    set_temporary_dir(tmp.string());

    std::ostringstream out;
    out << "density | facets | leaves encoded | leaves painted | encode ms | deserialize ms | serialize ms | get_facets ms | "
           "3MF store ms | 3MF load ms | 3MF bytes | process ms (painted) | process ms (unpainted) | segmentation ms (median of 3)\n";

    for (const Density &d : densities) {
        const double               fa  = 2. * PI / std::sqrt(double(d.facets));
        const indexed_triangle_set its = its_make_sphere(20., fa);
        const size_t               n   = its.indices.size();
        const size_t               per = size_t(1) << (2 * depth);

        // Paint.
        auto t0 = clk::now();
        std::vector<int>           states(n * per, 0);
        std::vector<ImageFillLeaf> leaves;
        for (size_t t = 0; t < n; ++t) {
            leaves.clear();
            const auto &f = its.indices[t];
            image_fill_subdivide(its.vertices[f[0]], its.vertices[f[1]], its.vertices[f[2]], depth, leaves);
            for (size_t l = 0; l < per; ++l)
                states[t * per + l] = speck_state(leaves[l].centroid() + Vec3f(20.f, 20.f, 20.f));
        }
        const TriangleSelector::TriangleSplittingData data = image_fill_encode(n, depth, states);
        const double encode_ms = ms_since(t0);
        size_t encoded = 0, painted = 0;
        for (size_t t = 0; t < n; ++t) {
            size_t p = 0;
            for (size_t l = 0; l < per; ++l) p += states[t * per + l] != 0;
            if (p) { encoded += per; painted += p; }
        }

        Model        model;
        ModelObject *mo = model.add_object();
        mo->name        = std::string("sphere_") + d.name;
        ModelVolume *mv = mo->add_volume(TriangleMesh(its));
        mo->add_instance();
        mo->center_around_origin();
        mo->instances.front()->set_offset(Vec3d(100., 100., 0.));
        mo->ensure_on_bed();

        // Deserialize / serialize.
        TriangleSelector sel(mv->mesh());
        t0 = clk::now();
        sel.deserialize(data, true);
        const double deser_ms = ms_since(t0);
        t0 = clk::now();
        TriangleSelector::TriangleSplittingData back = sel.serialize();
        const double ser_ms = ms_since(t0);
        CHECK(back.bitstream.size() == data.bitstream.size());
        mv->mmu_segmentation_facets.set(sel);

        // The CPU half of the painted GLVolume's render data.
        t0 = clk::now();
        std::vector<indexed_triangle_set> per_colour;
        mv->mmu_segmentation_facets.get_facets(*mv, per_colour);
        const double facets_ms = ms_since(t0);

        // 3MF.
        DynamicPrintConfig  cfg  = two_filament_config();
        const std::string   path = (tmp / (std::string("bench_") + d.name + ".3mf")).string();
        StoreParams         sp;
        sp.path     = path;
        sp.model    = &model;
        sp.config   = &cfg;
        sp.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SkipAuxiliary;
        t0 = clk::now();
        REQUIRE(store_bbs_3mf(sp));
        const double store_ms = ms_since(t0);
        const auto   bytes    = boost::filesystem::file_size(path);
        double       load_ms  = 0.;
        {
            Model                     lm;
            DynamicPrintConfig        lc;
            ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::EnableSilent };
            PlateDataPtrs             plates;
            std::vector<Preset *>     presets;
            bool                      is_bbl = false;
            Semver                    ver;
            t0 = clk::now();
            REQUIRE(load_bbs_3mf(path.c_str(), &lc, &ctxt, &lm, &plates, &presets, &is_bbl, &ver, nullptr,
                                 LoadStrategy::LoadModel | LoadStrategy::AddDefaultInstances | LoadStrategy::Silence));
            load_ms = ms_since(t0);
            release_PlateData_list(plates);
            for (Preset *pr : presets) delete pr;
            CHECK(lm.objects.front()->volumes.front()->mmu_segmentation_facets.get_data() == mv->mmu_segmentation_facets.get_data());
        }

        // Slicing: painted vs unpainted, then the segmentation alone.
        double process_painted_ms = 0., process_plain_ms = 0., seg_ms = 0.;
        {
            Print print;
            print.apply(model, cfg);
            print.set_status_silent();
            t0 = clk::now();
            print.process();
            process_painted_ms = ms_since(t0);
            std::vector<double> runs;
            for (int r = 0; r < 3; ++r) {
                t0 = clk::now();
                const auto seg = multi_material_segmentation_by_painting(*print.objects().front(), []() {});
                runs.push_back(ms_since(t0));
                CHECK(!seg.empty());
            }
            std::sort(runs.begin(), runs.end());
            seg_ms = runs[1];
        }
        {
            Model plain;
            plain.add_object(*mo);
            plain.objects.front()->volumes.front()->mmu_segmentation_facets.reset();
            Print print;
            print.apply(plain, cfg);
            print.set_status_silent();
            t0 = clk::now();
            print.process();
            process_plain_ms = ms_since(t0);
        }

        out << d.name << " | " << n << " | " << encoded << " | " << painted << " | " << encode_ms << " | " << deser_ms << " | "
            << ser_ms << " | " << facets_ms << " | " << store_ms << " | " << load_ms << " | " << bytes << " | "
            << process_painted_ms << " | " << process_plain_ms << " | " << seg_ms << "\n";
        boost::filesystem::remove(path);
    }
    WARN(out.str());
    boost::system::error_code ec;
    boost::filesystem::remove_all(tmp, ec);
}
