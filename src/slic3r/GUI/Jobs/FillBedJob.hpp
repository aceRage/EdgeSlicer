#ifndef FILLBEDJOB_HPP
#define FILLBEDJOB_HPP

#include "ArrangeJob.hpp"

#include "slic3r/GUI/FillBedDialog.hpp"

namespace Slic3r { namespace GUI {

class Plater;

class FillBedJob : public Job
{
    int     m_object_idx = -1;

    using ArrangePolygon  = arrangement::ArrangePolygon;
    using ArrangePolygons = arrangement::ArrangePolygons;

    ArrangePolygons m_selected;
    ArrangePolygons m_unselected;
    //BBS: add partplate related logic
    ArrangePolygons m_locked;;

    Points m_bedpts;

    arrangement::ArrangeParams params;

    // What the user asked for in FillBedDialog. Defaults reproduce the pre-dialog behaviour:
    // gap 0 means "auto" (init_arrange_params' arrange spacing, then the brim rule), and both
    // margins 0 leave the bed exactly where get_shrink_bedpts puts it.
    FillBedSettings m_settings;

    // The Grid layout's placements, computed in prepare() (the geometry it needs is all there)
    // and consumed by process(). Empty on the Compact path unless the estimate blew past
    // COUNT_CAP, in which case m_fallback_to_grid is set and this is used instead.
    std::vector<fill_bed::GridCell> m_grid_cells;
    bool m_fallback_to_grid = false;
    // Set on the throwaway job grid_copies_for() runs for the dialog's live label: prepare()
    // then computes the grid but changes nothing the user can see - no plate re-selection, no
    // clone list.
    bool m_estimate_only = false;

    int m_status_range = 0;
    Plater *m_plater;

    // The user's own edge margins, applied on top of arrange's shrink. Kept here rather than in
    // Arrange.cpp so plain Arrange is untouched.
    Points apply_user_bed_margins(const Points &bedpts, const arrangement::ArrangeParams &p) const;

    // Everything already on this plate that a grid cell must not land on: the exclusion regions
    // and the wipe tower (both already in m_unselected as virtual objects) and every object
    // staying put. Each carries its own clearance, so the copy-to-copy gap never leaks into an
    // exclusion region's margin.
    // `p` must already have been through update_arrange_params(); the obstacle clearances come
    // from update_unselected_items_inflation(), which prepare() has not run yet, so this runs it
    // on a throwaway copy rather than disturbing the items process() will pack.
    std::vector<fill_bed::GridObstacle> grid_obstacles(const arrangement::ArrangeParams &p) const;

    // Place as many copies of the template as the Grid layout fits, writing the result straight
    // into m_selected. Returns how many landed on the plate. Replaces the old ad-hoc "more than
    // 100 items" bounding-box branch, which ignored the gap, the margins, rotation and every
    // object on the plate.
    size_t run_grid_layout();

public:

    void prepare();
    void process(Ctl &ctl) override;

    FillBedJob();
    explicit FillBedJob(const FillBedSettings &settings);

    void set_settings(const FillBedSettings &settings) { m_settings = settings; }

    // The exact number of copies the Grid layout would place for `settings`, for the dialog's
    // live label. Runs prepare()-equivalent geometry on the main thread and packs nothing.
    static int grid_copies_for(Plater *plater, const FillBedSettings &settings);

    int status_range() const
    {
        return m_status_range;
    }

    void finalize(bool canceled, std::exception_ptr &e) override;
};

}} // namespace Slic3r::GUI

#endif // FILLBEDJOB_HPP
