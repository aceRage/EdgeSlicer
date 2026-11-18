#ifndef ARRANGEJOB_HPP
#define ARRANGEJOB_HPP


#include <optional>

#include "Job.hpp"
#include "libslic3r/Arrange.hpp"

namespace Slic3r {

class ModelInstance;

namespace GUI {

class Plater;

class ArrangeJob : public Job
{
    using ArrangePolygon = arrangement::ArrangePolygon;
    using ArrangePolygons = arrangement::ArrangePolygons;

    //BBS: add locked logic
    ArrangePolygons m_selected, m_unselected, m_unprintable, m_locked;
    ArrangePolygons m_reach_regions; // EdgeSlicer: one-nozzle strips for params.excluded_regions (fallback only)
    ExtruderAreas   m_reach_areas;   // EdgeSlicer: nozzle areas when "Avoid nozzle reach zones" is on
    std::vector<ModelInstance*> m_unarranged;
    std::map<int, ArrangePolygons> m_selected_groups;   // groups of selected items for sequential printing
    std::vector<int> m_uncompatible_plates;  // plate indices with different printing sequence than global

    arrangement::ArrangeParams params;
    int current_plate_index = 0;
    Polygon bed_poly;
    Plater *m_plater;

    // BBS: add flag for whether on current part plate
    bool only_on_partplate{false};

    // EdgeSlicer: place only the loose instances (prepare_loose), everything else fixed.
    bool m_loose_only{false};

    // clear m_selected and m_unselected, reserve space for next usage
    void clear_input();

    // Prepare the selected and unselected items separately. If nothing is
    // selected, behaves as if everything would be selected.
    void prepare_selected();

    void prepare_all();

    // EdgeSlicer: the instances on no plate, or over the edge of their (unlocked) plate, are the
    // ones to place; every instance wholly on a plate is a fixed item and stays where it is.
    void prepare_loose();

    //BBS:prepare the items from current selected partplate
    void prepare_partplate();
    void prepare_wipe_tower();

    ArrangePolygon prepare_arrange_polygon(void* instance);

protected:

    void check_unprintable();

public:

    void prepare();

    void process(Ctl &ctl) override;

    // loose_only: place the loose instances into free space (new plates when none has room) and
    // move nothing else - the remote hub's "Place loose objects". Otherwise the prepare state
    // decides, as before (Arrange all, or the current plate).
    explicit ArrangeJob(bool loose_only = false);

    int status_range() const
    {
        // ensure finalize() is called after all operations in process() is finished.
        return int(m_selected.size() + m_unprintable.size() + 1);
    }

    void finalize(bool canceled, std::exception_ptr &e) override;
};

std::optional<arrangement::ArrangePolygon> get_wipe_tower_arrangepoly(const Plater &);

// The gap between logical beds in the x axis expressed in ratio of
// the current bed width.
static const constexpr double LOGICAL_BED_GAP = 1. / 5.;

//BBS: add sudoku-style strides for x and y
// Stride between logical beds
double bed_stride_x(const Plater* plater);
double bed_stride_y(const Plater* plater);

arrangement::ArrangeParams init_arrange_params(Plater *p);

// EdgeSlicer "Avoid nozzle reach zones": the printer's nozzle areas when the arrange option is on,
// empty (nothing avoided) when it is off or the printer has no one-nozzle strip. Main thread only.
// The arrangement clips the bed to what every nozzle reaches (arrangement::clip_bed_to_nozzle_reach);
// only when that cannot be done are the strips placed as fixed items (nozzle_reach_fallback_blockers).
ExtruderAreas nozzle_reach_areas(Plater *p);
// The strips as fixed items on beds 0 .. num_beds - 1, or nothing when `bed` can be clipped instead.
arrangement::ArrangePolygons nozzle_reach_fallback_blockers(const ExtruderAreas &areas, const Points &bed, int num_beds, coord_t inflation = 0);

}} // namespace Slic3r::GUI

#endif // ARRANGEJOB_HPP
