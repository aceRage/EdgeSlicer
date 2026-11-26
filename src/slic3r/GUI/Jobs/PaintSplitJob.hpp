#ifndef slic3r_GUI_PaintSplitJob_hpp_
#define slic3r_GUI_PaintSplitJob_hpp_

// Paint Split, phase 2: the Apply of the gizmo (tests/design_paint_split.md 3.5).
//
// process() runs Cut::perform_with_cutter_solid() off the UI thread on a COPY of the object taken when
// the job was made, once per selected region (one piece each, D8), each next cut on the body the last
// one left. A region with several edges is cut once per edge; the rest pieces become parts (Keep as parts)
// or objects of their own. finalize() (UI thread) re-finds the source object by id and replaces it with the results
// under ONE undo snapshot, so one Ctrl+Z brings the uncut object back.

#include "Job.hpp"

#include "libslic3r/Model.hpp"
#include "libslic3r/ObjectID.hpp"
#include "libslic3r/PaintSplit.hpp"

#include <memory>
#include <string>
#include <vector>

namespace Slic3r { namespace GUI {

class Plater;

struct PaintSplitApplyPiece
{
    int                          component{ 0 };
    std::vector<PaintSplitPlane> planes;   // one per loop of the component, in the cut frame (the instance without its offset)
    std::vector<indexed_triangle_set> cutters; // Follow paint: one closed cutter per loop, cut frame (wins over planes)
    std::vector<Vec3d>           body_hints; // the later regions' selected skin: the rest piece holding it stays the body
    std::string          label;            // "Region 1", or the picked filament's name
    int                  filament{ 0 };    // 1-based extruder for the piece (D6), 0 = inherit
};

struct PaintSplitApplyPlan
{
    int                               instance{ 0 };
    int                               mask_volume_idx{ -1 }; // index into the object's volumes
    PaintSplitRegion                  region;                // in the cut frame
    std::vector<PaintSplitApplyPiece> pieces;
    bool                              keep_as_parts{ false };
    bool                              cut_only_painted_part{ false }; // D19 (c)
};

class PaintSplitJob : public Job
{
public:
    // `object` is copied here, on the UI thread.
    PaintSplitJob(Plater *plater, const ModelObject &object, PaintSplitApplyPlan &&plan);

    void process(Ctl &ctl) override;
    void finalize(bool canceled, std::exception_ptr &eptr) override;

private:
    Plater             *m_plater{ nullptr };
    ObjectID            m_object_id;
    std::string         m_name;
    Model               m_source;  // holds the copy, then the results
    PaintSplitApplyPlan m_plan;

    ModelObjectPtrs     m_results; // owned by m_source
    PaintSplitError     m_error{ PaintSplitError::None };
    std::vector<PaintSplitWarning> m_warnings;
    int                 m_returned{ 0 };
    int                 m_attached{ 0 };
    int                 m_bad_loop{ -1 };
    int                 m_rest_count{ 0 };
    std::string         m_bad_label;
};

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_PaintSplitJob_hpp_
