// GLGizmoBase's docked gizmo panels (Cut, Assembly) are pinned to an exact rect and re-measure
// their own contents every frame (ImGuiWindow::ContentSizeIdeal) to decide how wide that rect
// should be. The owner reported the Cut panel's docked width visibly jittering between two values,
// every frame, specifically when the panel reopened already docked after being used once in the
// session; undocking, resizing and redocking made it settle down again.
//
// The previous logic (GLGizmoBase::GizmoImguiEnd()) adopted ANY measurement that differed from the
// remembered width by more than 0.5px, on a single frame. That has no defence against a one-frame
// transient - sub-pixel rounding in the window's own (non-integer) position, a hover/tooltip that
// briefly touched the content bounds, or the first measurement taken right after reopening docked,
// before layout has fully settled - being read as a real width change: it gets adopted, one extra
// frame is requested to redraw at it, and if the very next ordinary frame's measurement drifts back
// towards the old width, the same thing happens in reverse. Two close values then take turns being
// "the" width every single frame for as long as the panel stays open - exactly the reported jitter.
//
// DockWidthSettle (src/slic3r/GUI/Gizmos/DockWidthSettle.hpp) is the extracted, ImGui-free decision
// logic: a candidate must be measured on two consecutive frames before it replaces the committed
// width. These are the hand-checkable numbers behind that behaviour.

#include <catch2/catch.hpp>

#include "slic3r/GUI/Gizmos/DockWidthSettle.hpp"

namespace {
constexpr float kThreshold = 0.5f;
}

TEST_CASE("DockWidthSettle: first measurement is committed immediately", "[DockWidthSettle]")
{
    DockWidthSettle settle;
    REQUIRE(settle.committed_width == 0.f);

    // A fresh panel (nothing committed yet) takes its first reading outright,
    // same as before: no extra frame is spent sitting at a placeholder width.
    CHECK(settle.update(320.f, kThreshold) == true);
    CHECK(settle.committed_width == 320.f);
    CHECK(settle.has_pending == false);
}

TEST_CASE("DockWidthSettle: a reading within the threshold just confirms, no churn", "[DockWidthSettle]")
{
    DockWidthSettle settle;
    settle.update(320.f, kThreshold);

    CHECK(settle.update(320.2f, kThreshold) == false);
    CHECK(settle.committed_width == 320.f);
    CHECK(settle.update(319.7f, kThreshold) == false);
    CHECK(settle.committed_width == 320.f);
}

TEST_CASE("DockWidthSettle: this is the reported bug - two widths alternating every frame", "[DockWidthSettle]")
{
    DockWidthSettle settle;
    settle.update(320.f, kThreshold); // committed: 320 (the "correct" width undocking/redocking finds)

    // Frame 2: a transient reading of 340 - e.g. sub-pixel rounding on reopen.
    // The old code would have adopted 340 right here. DockWidthSettle holds it
    // as a candidate instead and changes nothing yet.
    CHECK(settle.update(340.f, kThreshold) == false);
    CHECK(settle.committed_width == 320.f);

    // Frame 3: an ordinary frame reads back 320 - the transient did not repeat.
    // The old code would now flip back to 320, having drawn one frame at 340:
    // that flip, repeated every frame, IS the jitter the owner saw.
    // DockWidthSettle drops the uncorroborated candidate and stays put.
    CHECK(settle.update(320.f, kThreshold) == false);
    CHECK(settle.committed_width == 320.f);

    // Frames 4/5/6: same 340/320 alternation - nothing ever gets confirmed
    // because no candidate is ever repeated on the very next frame, so the
    // committed width never moves. No visible jitter.
    CHECK(settle.update(340.f, kThreshold) == false);
    CHECK(settle.committed_width == 320.f);
    CHECK(settle.update(320.f, kThreshold) == false);
    CHECK(settle.committed_width == 320.f);
}

TEST_CASE("DockWidthSettle: a genuine content-width change still lands, one frame later", "[DockWidthSettle]")
{
    DockWidthSettle settle;
    settle.update(320.f, kThreshold);

    // The user switches cut mode; the panel's contents really are wider now,
    // and stay wider on every following frame (no alternation).
    CHECK(settle.update(400.f, kThreshold) == false); // frame 1 of the new width: held as a candidate
    CHECK(settle.committed_width == 320.f);

    CHECK(settle.update(400.f, kThreshold) == true); // frame 2: confirmed, now adopted
    CHECK(settle.committed_width == 400.f);
    CHECK(settle.has_pending == false);
}

TEST_CASE("DockWidthSettle: reset_pending forgets a candidate without touching the committed width", "[DockWidthSettle]")
{
    DockWidthSettle settle;
    settle.update(320.f, kThreshold);
    settle.update(340.f, kThreshold); // a candidate is now pending
    CHECK(settle.has_pending == true);

    // GLGizmoBase calls this when the panel (re)opens, the dock state toggles,
    // or a frame renders only the title row - the candidate describes a layout
    // that no longer applies and must not get confirmed against what follows.
    settle.reset_pending();
    CHECK(settle.has_pending == false);
    CHECK(settle.committed_width == 320.f); // unaffected - this is the "last expanded width"

    // The next candidate needs its own two-frame confirmation from scratch.
    CHECK(settle.update(340.f, kThreshold) == false);
    CHECK(settle.committed_width == 320.f);
    CHECK(settle.update(340.f, kThreshold) == true);
    CHECK(settle.committed_width == 340.f);
}

TEST_CASE("DockWidthSettle: two different transient candidates in a row do not compound into a false confirm", "[DockWidthSettle]")
{
    DockWidthSettle settle;
    settle.update(320.f, kThreshold);

    CHECK(settle.update(340.f, kThreshold) == false); // candidate A = 340
    CHECK(settle.update(360.f, kThreshold) == false); // candidate B = 360, disagrees with A - replaces it, not confirmed
    CHECK(settle.committed_width == 320.f);
    CHECK(settle.update(320.f, kThreshold) == false); // back to the committed width - nothing pending survives
    CHECK(settle.committed_width == 320.f);
}
