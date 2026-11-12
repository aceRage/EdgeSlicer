// Deleting and merging filaments (fix/filament-delete-flush-and-merge).

#include <catch2/catch.hpp>

#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"

using namespace Slic3r;

// PresetBundle::update_num_filaments() -> update_multi_material_filament_presets(): the flush
// matrix kept its first n - 1 rows and columns and the flush vector lost its last pair, whichever
// filament was deleted. Deleting filament 2 of 3 left filament 3 with filament 2's volumes.
TEST_CASE("Deleting a filament removes its own flushing volumes", "[FilamentDelete][Config]")
{
    // Row = from, column = to; entry ij = 10 * (i + 1) + (j + 1), so every value says where it is.
    auto matrix = [](const std::vector<int> &ids) {
        std::vector<double> m;
        for (int i : ids)
            for (int j : ids)
                m.push_back(i == j ? 0. : 10. * i + j);
        return m;
    };
    auto vector_of = [](const std::vector<int> &ids) {
        std::vector<double> v;
        for (int i : ids) {
            v.push_back(100. + i); // unload
            v.push_back(200. + i); // load
        }
        return v;
    };

    for (size_t to_delete : { size_t(0), size_t(1), size_t(2) }) {
        PresetBundle bundle;
        bundle.set_num_filaments(3, std::string("#FF0000"));
        DynamicPrintConfig &project = bundle.project_config;
        REQUIRE(project.option<ConfigOptionFloats>("flush_volumes_matrix")->values.size() == 9);
        project.option<ConfigOptionFloats>("flush_volumes_matrix")->values = matrix({ 1, 2, 3 });
        project.option<ConfigOptionFloats>("flush_volumes_vector")->values = vector_of({ 1, 2, 3 });

        bundle.update_num_filaments(unsigned(to_delete));

        std::vector<int> kept;
        for (int id : { 1, 2, 3 })
            if (id != int(to_delete) + 1)
                kept.push_back(id);
        INFO("deleted filament " << to_delete + 1);
        CHECK(bundle.filament_presets.size() == 2);
        CHECK(project.option<ConfigOptionFloats>("flush_volumes_matrix")->values == matrix(kept));
        CHECK(project.option<ConfigOptionFloats>("flush_volumes_vector")->values == vector_of(kept));
    }
}
