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

    int m_status_range = 0;
    Plater *m_plater;

    // The user's own edge margins, applied on top of arrange's shrink. Kept here rather than in
    // Arrange.cpp so plain Arrange is untouched.
    Points apply_user_bed_margins(const Points &bedpts, const arrangement::ArrangeParams &p) const;

public:

    void prepare();
    void process(Ctl &ctl) override;

    FillBedJob();
    explicit FillBedJob(const FillBedSettings &settings);

    void set_settings(const FillBedSettings &settings) { m_settings = settings; }

    int status_range() const
    {
        return m_status_range;
    }

    void finalize(bool canceled, std::exception_ptr &e) override;
};

}} // namespace Slic3r::GUI

#endif // FILLBEDJOB_HPP
