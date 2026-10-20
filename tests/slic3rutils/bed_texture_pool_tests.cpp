// Bed-type textures kept across a bed re-layout (src/slic3r/GUI/BedTexturePool.hpp, used by
// PartPlateList::load_bedtype_textures).
//
// Every printer switch that changes the bed size (and every set_shapes / extruder-area change)
// used to delete and re-rasterise all bed-type SVGs at 2048 px. The rasters only depend on the
// file (and the raster size, checked by the caller); the re-layout only moves and scales them.
// These tests run the same keep / hand back / drain steps load_bedtype_textures runs, on layouts
// with the real file names, with a fake texture that counts rasterisations.

#include <catch2/catch.hpp>

#include "slic3r/GUI/BedTexturePool.hpp"

#include <set>
#include <string>
#include <vector>

using namespace Slic3r::GUI;

namespace {

struct FakeTexture
{
    std::string file;
};

struct Part
{
    std::string  filename;
    FakeTexture* texture = nullptr;
};

using Layout = std::vector<std::vector<Part>>; // per bed type, its parts

// Single-nozzle layout (init_bed_type_info): left + bottom art per plate type; btPCT shares the
// Cool Plate bottom art with btPC.
Layout single_nozzle()
{
    return { { { "bbl_bed_st_left.svg" }, { "bbl_bed_st_bottom.svg" } },
             { { "bbl_bed_pc_left.svg" }, { "bbl_bed_pc_bottom.svg" } },
             { { "orca_bed_pct_left.svg" }, { "bbl_bed_pc_bottom.svg" } },
             { { "bbl_bed_ep_left.svg" }, { "bbl_bed_ep_bottom.svg" } },
             { { "bbl_bed_pei_left.svg" }, { "bbl_bed_pei_bottom.svg" } },
             { { "bbl_bed_pte_left.svg" }, { "bbl_bed_pte_bottom.svg" } } };
}

// Dual-nozzle layout (H2D / H2C): the name along the back edge and a short front-left tab.
Layout dual_nozzle()
{
    return { { { "bbl_bed_st_middle.svg" }, { "bbl_bed_st_left_bottom.svg" } },
             { { "bbl_bed_pc_middle.svg" }, { "bbl_bed_pc_left_bottom.svg" } },
             { { "bbl_bed_pc_middle.svg" }, { "bbl_bed_pc_left_bottom.svg" } },
             { { "bbl_bed_ep_middle.svg" }, { "bbl_bed_ep_left_bottom.svg" } },
             { { "bbl_bed_pei_middle.svg" }, { "bbl_bed_pei_left_bottom.svg" } },
             { { "bbl_bed_pte_middle.svg" }, { "bbl_bed_pte_left_bottom.svg" } } };
}

struct Counts
{
    int reused = 0, rasterised = 0, deleted = 0;
};

// What load_bedtype_textures does: keep the old textures, lay out again, hand back by file name,
// rasterise the rest, delete what is left over.
Counts relayout(Layout& current, Layout next, bool same_raster_size = true)
{
    Counts                       c;
    BedTexturePool<FakeTexture>  kept;
    for (auto& type : current)
        for (auto& p : type) {
            if (p.texture != nullptr && same_raster_size) {
                kept.put(p.filename, p.texture);
                p.texture = nullptr;
            }
            delete p.texture; // init_bed_type_info() resets whatever was not kept
            p.texture = nullptr;
        }
    for (auto& type : next)
        for (auto& p : type) {
            if (FakeTexture* t = kept.take(p.filename)) {
                p.texture = t;
                ++c.reused;
            } else {
                p.texture = new FakeTexture{ p.filename };
                ++c.rasterised;
            }
        }
    for (FakeTexture* t : kept.drain()) {
        delete t;
        ++c.deleted;
    }
    current = std::move(next);
    return c;
}

void free_all(Layout& l)
{
    for (auto& type : l)
        for (auto& p : type) {
            delete p.texture;
            p.texture = nullptr;
        }
}

// Every part shows its own file, and no texture object is shared by two parts.
void check_layout(const Layout& l)
{
    std::set<const FakeTexture*> seen;
    for (const auto& type : l)
        for (const auto& p : type) {
            REQUIRE(p.texture != nullptr);
            CHECK(p.texture->file == p.filename);
            CHECK(seen.insert(p.texture).second);
        }
}

} // namespace

TEST_CASE("the pool hands textures back by file name, one per part", "[BedTexturePool]")
{
    BedTexturePool<FakeTexture> pool;
    FakeTexture a{ "a.svg" }, b1{ "b.svg" }, b2{ "b.svg" };
    pool.put("a.svg", &a);
    pool.put("b.svg", &b1);
    pool.put("b.svg", &b2);
    pool.put("c.svg", nullptr); // nothing to keep
    CHECK(pool.size() == 3);

    CHECK(pool.take("a.svg") == &a);
    CHECK(pool.take("a.svg") == nullptr); // handed out once
    FakeTexture* first  = pool.take("b.svg");
    FakeTexture* second = pool.take("b.svg");
    CHECK(first != nullptr);
    CHECK(second != nullptr);
    CHECK(first != second);
    CHECK(pool.take("c.svg") == nullptr);
    CHECK(pool.empty());
    CHECK(pool.drain().empty());
}

TEST_CASE("a bed-size-only switch rasterises nothing", "[BedTexturePool]")
{
    Layout l;
    Counts first = relayout(l, single_nozzle()); // startup
    CHECK(first.rasterised == 12);
    check_layout(l);

    // X1C (256 x 256) -> U1 (270 x 270) -> P1S: same art, other placement.
    for (int i = 0; i < 3; ++i) {
        INFO("re-layout " << i);
        Counts c = relayout(l, single_nozzle());
        CHECK(c.rasterised == 0);
        CHECK(c.reused == 12);
        CHECK(c.deleted == 0);
        check_layout(l);
    }
    free_all(l);
}

TEST_CASE("switching to and from a dual-nozzle bed swaps only the art that differs", "[BedTexturePool]")
{
    Layout l;
    relayout(l, single_nozzle());

    // X1C -> H2D: every plate type shows other files on a dual-nozzle bed.
    Counts to_dual = relayout(l, dual_nozzle());
    CHECK(to_dual.reused == 0);
    CHECK(to_dual.rasterised == 12);
    CHECK(to_dual.deleted == 12);
    check_layout(l);

    // H2D -> H2C: same dual art.
    Counts dual_again = relayout(l, dual_nozzle());
    CHECK(dual_again.rasterised == 0);
    CHECK(dual_again.reused == 12);
    check_layout(l);

    // A P2S-style machine art file replaces one bottom tab: only that one is rasterised.
    Layout p2s       = single_nozzle();
    p2s[4][1].filename = "bbl_bed_pei_bottom_n.svg";
    relayout(l, single_nozzle());
    Counts to_p2s = relayout(l, p2s);
    CHECK(to_p2s.rasterised == 1);
    CHECK(to_p2s.reused == 11);
    CHECK(to_p2s.deleted == 1);
    check_layout(l);
    free_all(l);
}

TEST_CASE("another raster size reloads everything", "[BedTexturePool]")
{
    Layout l;
    relayout(l, single_nozzle());
    Counts c = relayout(l, single_nozzle(), /*same_raster_size*/ false);
    CHECK(c.reused == 0);
    CHECK(c.rasterised == 12);
    check_layout(l);
    free_all(l);
}
