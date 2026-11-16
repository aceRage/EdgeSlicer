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

// P4 (items 2, 3): arranges groups of objects one after the other, each later group on plates of its
// own: the main parts first (into free space on the plates, then new ones, as before), then the loose
// pieces (when asked), then the dowels. Each step is the "place loose objects" arrange; while it runs,
// the groups still to come are set not to print (a loose object set not to print stays where it is)
// and, for a later group, every plate that holds anything is locked (a locked plate takes nothing new).
// Both are put back after each step and when an arrange is canceled. Kept alive by the jobs' callbacks.
class SplitArrangeChain : public std::enable_shared_from_this<SplitArrangeChain>
{
public:
    SplitArrangeChain(Plater *plater, std::vector<std::vector<ObjectID>> groups);
    // UI thread: arranges the next group with anything in it, then queues the step after it.
    void run_step();

private:
    ModelObject *find(const ObjectID &id) const;
    void         release();

    Plater                                  *m_plater{ nullptr };
    std::vector<std::vector<ObjectID>>       m_groups;
    size_t                                   m_step{ 0 };
    std::vector<std::pair<ObjectID, size_t>> m_held;    // instances set not to print (object, instance)
    std::vector<int>                         m_locked;  // plates locked by this chain
};

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_SplitToFitJob_hpp_
