#ifndef slic3r_DockWidthSettle_hpp_
#define slic3r_DockWidthSettle_hpp_

#include <cmath>

// Pure, ImGui/wx-free decision logic for GLGizmoBase's docked-panel width
// measurement (see GLGizmoBase::GizmoImguiEnd()).
//
// A docked auto-sizing panel (Cut, Assembly) is pinned to an exact rect, so it
// cannot report its own wanted width the way a floating AlwaysAutoResize window
// does - GLGizmoBase re-measures the panel's CONTENTS every frame instead
// (ImGuiWindow::ContentSizeIdeal) and remembers the result as the docked width.
// Adopting a new measurement the instant it differs from the remembered width
// by more than a rounding epsilon is fragile: a single transient frame - a
// sub-pixel rounding difference from the window's own (non-integer) position,
// a tooltip or hover state that briefly touched the content bounds, the first
// post-reopen measurement before layout has fully settled - reads as a "new"
// width, gets adopted immediately, and calls for another frame to redraw at
// it. If the very next frame's measurement drifts back, the same thing happens
// in reverse, and the panel jitters between the two widths every frame instead
// of ever settling.
//
// DockWidthSettle requires a candidate width to be confirmed on a SECOND
// consecutive frame - not just measured once - before it replaces the
// remembered width. A transient single-frame reading is thereby ignored; a
// genuine content-width change (the user switched cut mode, opened connector
// editing, ...) still lands within two frames, same as before.
struct DockWidthSettle
{
    // The width most recently adopted as "the" docked width. 0 means nothing
    // has been adopted yet.
    float committed_width{ 0.f };
    // A candidate measurement seen on the previous frame, not yet confirmed.
    // NaN (via has_pending) means there is no candidate pending.
    float pending_width{ 0.f };
    bool  has_pending{ false };

    // Forget any in-flight candidate without touching the committed width.
    // Call this whenever the panel (re)opens, changes dock state, or the body
    // was not rendered this frame (title-row-only measurements must never
    // become a candidate) - a stale candidate from before the reset could
    // otherwise get confirmed against an unrelated later frame.
    void reset_pending()
    {
        has_pending = false;
        pending_width = 0.f;
    }

    // Feed this frame's raw measurement. Returns true when `committed_width`
    // changed as a result (the caller should ask for one more frame so the
    // corrected width is applied without waiting for other input).
    //
    // `measured`   - this frame's ContentSizeIdeal-derived width.
    // `threshold`  - minimum difference from committed_width worth reacting to
    //                (the base's existing 0.5f epsilon).
    // `settle_tolerance` - how close two consecutive candidates must be to
    //                each other to count as "the same" measurement (as
    //                opposed to two different transient values that happen to
    //                both differ from committed_width). Defaults to the same
    //                epsilon as `threshold`.
    bool update(float measured, float threshold, float settle_tolerance = -1.f)
    {
        if (settle_tolerance < 0.f)
            settle_tolerance = threshold;

        if (committed_width <= 0.f) {
            // Nothing committed yet (fresh panel): take the first measurement
            // outright so the panel does not sit at a placeholder width for an
            // extra frame it does not need to.
            committed_width = measured;
            reset_pending();
            return true;
        }

        if (std::fabs(measured - committed_width) <= threshold) {
            // Confirms the width already in effect; no pending candidate
            // survives a frame that agrees with the committed width.
            reset_pending();
            return false;
        }

        if (has_pending && std::fabs(measured - pending_width) <= settle_tolerance) {
            // Same candidate two frames running: it is a real change, not a
            // one-frame transient. Adopt it.
            committed_width = measured;
            reset_pending();
            return true;
        }

        // First frame proposing this candidate (or it disagrees with the
        // previous candidate) - hold it for confirmation, change nothing yet.
        pending_width = measured;
        has_pending = true;
        return false;
    }
};

#endif // slic3r_DockWidthSettle_hpp_
