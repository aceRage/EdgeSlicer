#include <catch2/catch.hpp>

#include "slic3r/GUI/DualNozzleLayout.hpp"

using namespace Slic3r::GUI::DualNozzleLayout;

namespace {

// What DualNozzleSliceDialog::relayout measures, modelled in DIP and scaled like FromDIP:
// one row = max(chip 26, arrow 24, combo ~24) + 6 gap; body borders 12 top + 12 bottom;
// chrome ~ 330 DIP (intro, printer row, status, tip, buttons, card titles).
struct Probe
{
    double scale;
    int    display_h; // client-area height, px
    int px(int dip) const { return int(dip * scale + 0.5); }
    int natural(int rows) const { return rows == 0 ? px(40) : rows * px(26 + 6) + px(24); }
    int chrome() const { return px(330); }
    CardBodies run(int left_rows, int right_rows) const
    {
        return card_bodies(natural(left_rows), natural(right_rows), chrome(), max_dialog_height(display_h), px(60));
    }
};

} // namespace

TEST_CASE("Card bodies keep every row at its natural height until the cap, then scroll", "[DualNozzleLayout]")
{
    for (const Probe p : { Probe{ 1.0, 1040 }, Probe{ 1.5, 1400 }, Probe{ 1.5, 1040 } }) {
        const int cap = max_dialog_height(p.display_h);
        for (int rows : { 1, 4, 8, 16 }) {
            // One side with `rows` filaments, the other with one: both bodies get the taller height.
            const CardBodies b = p.run(rows, 1);
            INFO("scale " << p.scale << ", display " << p.display_h << ", rows " << rows);
            if (!b.scroll) {
                CHECK(b.height == std::max(p.natural(rows), p.px(60)));
                CHECK(b.height + p.chrome() <= cap);
            } else {
                // Never taller than the cap allows, never shorter than an empty card.
                CHECK(b.height + p.chrome() == cap);
                CHECK(b.height < p.natural(rows));
                CHECK(b.height >= p.px(60));
            }
            // Symmetric: the swap button sits between two equally tall cards either way round.
            CHECK(p.run(1, rows).height == b.height);
        }
    }

    // 100 %, 1040 px client area: 1, 4 and 8 rows fit (8 rows = 280 px body), 16 rows scroll.
    const Probe p100{ 1.0, 1040 };
    CHECK_FALSE(p100.run(1, 1).scroll);
    CHECK_FALSE(p100.run(4, 1).scroll);
    CHECK_FALSE(p100.run(8, 1).scroll);
    CHECK(p100.run(16, 1).scroll);
    CHECK(p100.run(8, 1).height == 8 * 32 + 24);

    // 150 % on a 1440p screen (1400 px client area): 8 rows fit, 16 scroll.
    const Probe p150{ 1.5, 1400 };
    CHECK_FALSE(p150.run(8, 8).scroll);
    CHECK(p150.run(16, 4).scroll);
    // 150 % on a 1080p screen: the rows are 1.5x taller in the same pixels, so 8 rows already scroll.
    CHECK(Probe{ 1.5, 1040 }.run(8, 1).scroll);
    CHECK_FALSE(Probe{ 1.5, 1040 }.run(4, 1).scroll);

    // An empty side keeps a usable drop area.
    CHECK(p100.run(0, 0).height == 60);

    // A tiny screen never produces a body below the empty-card height.
    CHECK(card_bodies(900, 30, 700, 600, 60).height == 60);
    CHECK(card_bodies(900, 30, 700, 600, 60).scroll);
}
