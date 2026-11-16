#ifndef slic3r_GUI_SplitToFitJob_hpp_
#define slic3r_GUI_SplitToFitJob_hpp_

// Split to Fit, phase 1: the Apply of the gizmo (tests/design_split_to_build_volume.md, 4.1 and 4.2).
//
// process() runs split_apply() off the UI thread on a COPY of the object taken when the job was made,
// so nothing it reads can be edited under it. finalize() (UI thread) re-finds the source object by
// id, replaces it with the parts under ONE undo snapshot, and then arranges the new parts over as
// many plates as needed (D12) without a snapshot of its own, so one Ctrl+Z brings the uncut object back.

#include "Job.hpp"

#include "libslic3r/Model.hpp"
#include "libslic3r/ObjectID.hpp"
#include "libslic3r/SplitToVolume.hpp"

#include <memory>
#include <string>

namespace Slic3r { namespace GUI {

class Plater;

class SplitToFitJob : public Job
{
public:
    // `object` is copied here, on the UI thread.
    SplitToFitJob(Plater *plater, const ModelObject &object, int instance, const SplitPlan &plan);

    void process(Ctl &ctl) override;
    void finalize(bool canceled, std::exception_ptr &eptr) override;

private:
    Plater          *m_plater{ nullptr };
    ObjectID         m_object_id;
    std::string      m_name;
    Model            m_source;      // holds the copy
    int              m_instance{ 0 };
    SplitPlan        m_plan;
    SplitApplyResult m_result;
};

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_SplitToFitJob_hpp_
