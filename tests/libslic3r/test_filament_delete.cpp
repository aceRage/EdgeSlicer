// Deleting and merging filaments (fix/filament-delete-flush-and-merge).

#include <catch2/catch.hpp>

#include "libslic3r/CustomGCode.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <string>

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

    // Deleted one after another: four filaments, the first, then the new first (the old second).
    {
        PresetBundle bundle;
        bundle.set_num_filaments(4, std::string("#FF0000"));
        DynamicPrintConfig &project = bundle.project_config;
        REQUIRE(project.option<ConfigOptionFloats>("flush_volumes_matrix")->values.size() == 16);
        project.option<ConfigOptionFloats>("flush_volumes_matrix")->values = matrix({ 1, 2, 3, 4 });
        project.option<ConfigOptionFloats>("flush_volumes_vector")->values = vector_of({ 1, 2, 3, 4 });

        bundle.update_num_filaments(0);
        CHECK(project.option<ConfigOptionFloats>("flush_volumes_matrix")->values == matrix({ 2, 3, 4 }));
        bundle.update_num_filaments(0);
        CHECK(bundle.filament_presets.size() == 2);
        CHECK(project.option<ConfigOptionFloats>("flush_volumes_matrix")->values == matrix({ 3, 4 }));
        CHECK(project.option<ConfigOptionFloats>("flush_volumes_vector")->values == vector_of({ 3, 4 }));
    }
}

// Plater::on_filaments_delete() -> CustomGCode::update_tool_changes_when_delete_filament(). When a
// filament was merged into another, std::remove_if moved the other items forward and left the
// filament changes to the merged filament in an unspecified tail: only the first slot of that tail
// got the target, the tail stayed in the list, and that slot was then numbered down a second time.
TEST_CASE("Merging a filament retargets every layer filament change to it once", "[FilamentDelete][CustomGCode]")
{
    using CustomGCode::Item;
    using CustomGCode::ToolChange;
    using CustomGCode::ColorChange;
    auto items = [](std::initializer_list<std::pair<double, int>> list) {
        CustomGCode::Info info;
        for (const auto &[z, extruder] : list)
            info.gcodes.push_back(Item{ z, ToolChange, extruder, "", "" });
        return info;
    };
    // "print_z:filament" of each item, in order.
    auto extruders = [](const CustomGCode::Info &info) {
        std::string out;
        for (const Item &item : info.gcodes)
            out += (out.empty() ? "" : " ") + std::to_string(int(item.print_z)) + ":" + std::to_string(item.extruder);
        return out;
    };

    // Four filaments; filament 2 (0-based 1) merges into filament 4, which is filament 3 (0-based 2)
    // after the deletion.
    CustomGCode::Info info = items({ { 1., 2 }, { 2., 3 }, { 3., 2 }, { 4., 4 }, { 5., 1 }, { 6., 2 } });
    info.gcodes.push_back(Item{ 7., ColorChange, 2, "#00FF00", "" }); // not a filament change: kept as is
    CustomGCode::update_tool_changes_when_delete_filament(info, 1, 2);
    CHECK(extruders(info) == "1:3 2:2 3:3 4:3 5:1 6:3 7:2");

    // Merged into a filament before it: filament 3 (0-based 2) into filament 1.
    info = items({ { 1., 3 }, { 2., 4 }, { 3., 3 }, { 4., 2 } });
    CustomGCode::update_tool_changes_when_delete_filament(info, 2, 0);
    CHECK(extruders(info) == "1:1 2:3 3:1 4:2");

    // Deleted, not merged: its filament changes go, the later filaments are numbered down.
    info = items({ { 1., 2 }, { 2., 3 }, { 3., 2 }, { 4., 1 } });
    CustomGCode::update_tool_changes_when_delete_filament(info, 1, -1);
    CHECK(extruders(info) == "2:2 4:1");
}
