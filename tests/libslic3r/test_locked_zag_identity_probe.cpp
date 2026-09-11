// Locked Zag byte-identity probe.
//
// This file is NOT a feature test - it is the instrument for the before/after G-code identity check
// in docs/superpowers/specs/2026-09-10-locked-zag-skin-skeleton.md section 4.2. It slices two
// fixed configurations (one Locked Zag, one plain Grid) and prints a stable digest of every
// extrusion the slicer produced, so the SAME file compiled into a pre-change build and a
// post-change build can be diffed line for line.
//
// It uses no configuration key that the pre-change tree lacks, on purpose: drop it into the
// baseline worktree unchanged, build, run with "[LZProbe]", and compare the two outputs.
//
// Run: libslic3r_tests.exe "[LZProbe]" -s   (the digest goes to stdout via WARN)

#include <catch2/catch.hpp>

#include <cmath>
#include <iostream>
#include <sstream>
#include <string>

#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"

using namespace Slic3r;

namespace {

void probe_collect(const ExtrusionEntity *entity, std::ostringstream &out)
{
    if (const auto *collection = dynamic_cast<const ExtrusionEntityCollection *>(entity)) {
        for (const ExtrusionEntity *child : collection->entities)
            probe_collect(child, out);
        return;
    }
    if (const auto *path = dynamic_cast<const ExtrusionPath *>(entity)) {
        out << int(path->role()) << ':' << int(std::lround(path->width * 1000.)) << ':'
            << int(std::lround(path->height * 1000.));
        for (const Point &p : path->polyline.points)
            out << ' ' << p.x() << ',' << p.y();
        out << '\n';
        return;
    }
    if (const auto *loop = dynamic_cast<const ExtrusionLoop *>(entity)) {
        for (const ExtrusionPath &path : loop->paths)
            probe_collect(&path, out);
        return;
    }
    if (const auto *multi = dynamic_cast<const ExtrusionMultiPath *>(entity)) {
        for (const ExtrusionPath &path : multi->paths)
            probe_collect(&path, out);
    }
}

std::string probe_digest(const DynamicPrintConfig &config)
{
    Model        model;
    ModelObject *object = model.add_object();
    object->name        = "lz-probe-cube.stl";
    object->add_volume(make_cube(20., 20., 20.));
    object->add_instance();
    object->ensure_on_bed();

    Print print;
    print.set_status_silent();
    print.apply(model, config);
    REQUIRE(print.objects().size() == 1);
    print.process();

    const PrintObject *po = print.objects().front();
    std::ostringstream out;
    out << "layers=" << po->layer_count() << '\n';
    for (size_t i = 0; i < size_t(po->layer_count()); ++ i) {
        const Layer *layer = po->get_layer(int(i));
        out << "L" << i << " z=" << int(std::lround(layer->print_z * 1000.)) << '\n';
        for (const LayerRegion *region : layer->regions()) {
            probe_collect(&region->perimeters, out);
            probe_collect(&region->fills, out);
        }
    }
    return out.str();
}

// Fold the digest into a hex checksum so the comparison is one short line per case, and print the
// length too so a truncation cannot hide behind a collision.
std::string checksum(const std::string &s)
{
    std::uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ull;
    }
    std::ostringstream out;
    out << std::hex << h << std::dec << " len=" << s.size();
    return out.str();
}

DynamicPrintConfig probe_base()
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "sparse_infill_density",      "15%" },
        { "skin_infill_density",        "40%" },
        { "skeleton_infill_density",    "10%" },
        { "skin_infill_depth",          "2" },
        { "infill_lock_depth",          "1" },
        { "skin_infill_line_width",     "0.45" },
        { "skeleton_infill_line_width", "0.6" },
        { "top_shell_layers",           "1" },
        { "bottom_shell_layers",        "1" },
        { "wall_loops",                 "2" },
        { "layer_height",               "0.2" },
    });
    return config;
}

} // namespace

TEST_CASE("LZProbe: locked zag at profile defaults", "[LZProbe]")
{
    DynamicPrintConfig config = probe_base();
    config.set_deserialize_strict({ { "sparse_infill_pattern", "lockedzag" } });
    const std::string digest = probe_digest(config);
    std::cout << "LZPROBE lockedzag " << checksum(digest) << std::endl;
    CHECK(! digest.empty());
}

TEST_CASE("LZProbe: plain grid sparse infill", "[LZProbe]")
{
    DynamicPrintConfig config = probe_base();
    config.set_deserialize_strict({ { "sparse_infill_pattern", "grid" } });
    const std::string digest = probe_digest(config);
    std::cout << "LZPROBE grid " << checksum(digest) << std::endl;
    CHECK(! digest.empty());
}

TEST_CASE("LZProbe: crosshatch (the shipped sparse default)", "[LZProbe]")
{
    DynamicPrintConfig config = probe_base();
    config.set_deserialize_strict({ { "sparse_infill_pattern", "crosshatch" } });
    const std::string digest = probe_digest(config);
    std::cout << "LZPROBE crosshatch " << checksum(digest) << std::endl;
    CHECK(! digest.empty());
}
