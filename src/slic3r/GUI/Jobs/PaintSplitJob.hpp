#ifndef slic3r_GUI_PaintSplitJob_hpp_
#define slic3r_GUI_PaintSplitJob_hpp_

// Paint Split: the Apply of the gizmo (tests/design_paint_split.md 3.5, R2 8.2 E7).
//
// process() runs ONE Cut::perform_paint_split_one_pass() off the UI thread on a COPY of the object taken when the
// job was made: every selected region at once (one piece each, D8), the rest of the part as the body plus rest pieces
// (parts with Keep as parts, objects of their own otherwise). The claims Show cut computed come along and are reused
// when they still match. finalize() (UI thread) re-finds the source object by id and replaces it with the results
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
    int         component{ 0 };
    std::string label;          // "Region 1", or the picked filament's name
    int         filament{ 0 };  // 1-based extruder for the piece (D6), 0 = inherit
    // Show cut's fine claim for this piece (R2 E4), or null: reused when its key matches what the cut computes.
    std::shared_ptr<const PaintSplitClaim> claim;
};

struct PaintSplitApplyPlan
{
    int                               instance{ 0 };
    int                               mask_volume_idx{ -1 }; // index into the object's volumes
    PaintSplitRegion                  region;                // in the cut frame
    std::vector<PaintSplitApplyPiece> pieces;                // in pass order
    PaintSplitSkinParams              skin;
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

    // The last Apply's Output Log lines (design 8.1 step 0: a total and one time line per region), shown by the
    // gizmo when it opens again. UI thread only.
    static const std::vector<std::string> &last_log();

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
    int                 m_bad_loop{ -1 };
    int                 m_rest_count{ 0 };
    std::string         m_bad_label;
    std::vector<std::string> m_log; // per region: "Region n: field x s, cut y s, paint z s"
    double              m_total_s{ 0. };
};

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_PaintSplitJob_hpp_
