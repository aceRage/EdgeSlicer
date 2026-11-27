#pragma once
// Paint Split R2 "Responsive gizmo" (tests/design_paint_split.md 8.2 E4-E6): the gizmo's region and its Show cut,
// computed off the UI thread, cancellable, coarse first and then fine. The fine claims are computed with the one-pass
// parameters (paint_split_pass_params), keyed (paint_split_claim_key), and handed to Apply, which then does no field
// work for them. No GUI dependencies, so the worker's cancel and generation rules are tested headless.
#include "PaintSplit.hpp"
#include "TriangleMesh.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace Slic3r {

struct PaintSplitPreviewInput
{
    uint64_t                                generation{ 0 };
    std::shared_ptr<const TriangleMesh>     mesh;                          // the masked part's own mesh (its object frame)
    Transform3d                             to_cut{ Transform3d::Identity() }; // the part into the cut frame
    TriangleSelector::TriangleSplittingData mask;
    PaintSplitRegionParams                  region_params;
    std::shared_ptr<const PaintSplitRegion> region;     // already computed for these inputs (cut frame): skips that stage
    std::shared_ptr<const PaintSplitPart>   part;       // likewise: the part in the cut frame, prepared
    bool                                    claims{ false }; // Show cut: the coarse, then the fine claims and cut surfaces
    PaintSplitSkinParams                    skin;       // the fine claims' parameters (Apply's)
};

struct PaintSplitPreviewResult
{
    enum class Stage { Region, Coarse, Fine };
    uint64_t                                            generation{ 0 };
    Stage                                               stage{ Stage::Region };
    std::shared_ptr<const PaintSplitRegion>             region; // cut frame
    std::shared_ptr<const PaintSplitPart>               part;
    std::vector<int>                                    comps;    // the components one Apply cuts, in pass order
    std::vector<std::shared_ptr<const PaintSplitClaim>> claims;   // per comps entry; Fine: keyed, for Apply
    std::vector<indexed_triangle_set>                   surfaces; // per comps entry: the cut surface inside the part
    std::vector<std::string>                            log;      // Fine: one time line per region
    double                                              seconds{ 0. };
};

// The coarse preview's parameters: a voxel of at least 1.2 mm (twice the fine one), one near-skin round, two root
// steps and no seam snap. It shows within about a second where the cut will run.
PaintSplitSkinParams paint_split_coarse_params(const PaintSplitSkinParams& fine);
// The components one Apply cuts: every component that can be split, in order.
std::vector<int> paint_split_pass_components(const PaintSplitRegion& region);

// One region / Show cut computation at a time, on its own thread. start() cancels and joins the one under way, so a
// result of an older generation is never stored after a newer start(). `notify` runs on the worker thread each time
// a result is stored and when the thread ends (the gizmo posts to the UI thread from it).
class PaintSplitPreviewWorker
{
public:
    using Notify = std::function<void()>;
    explicit PaintSplitPreviewWorker(Notify notify = {});
    ~PaintSplitPreviewWorker();
    PaintSplitPreviewWorker(const PaintSplitPreviewWorker&)            = delete;
    PaintSplitPreviewWorker& operator=(const PaintSplitPreviewWorker&) = delete;

    void start(PaintSplitPreviewInput input);
    // Stops and joins the computation under way and drops every stored result.
    void cancel();
    // Asks the computation under way to stop, without waiting: nothing more is stored for it, and `notify` runs when
    // its thread ends (the gizmo then starts the newest request; start() joins at once).
    void request_cancel();
    // Joins the computation under way (tests; the gizmo never blocks on a running one).
    void wait();
    bool running() const { return m_running.load(); }
    // The results stored since the last take(), oldest first.
    std::vector<PaintSplitPreviewResult> take();

private:
    void run(PaintSplitPreviewInput input, std::shared_ptr<std::atomic<bool>> cancel);
    void store(PaintSplitPreviewResult&& r, const std::atomic<bool>& cancel);

    Notify                               m_notify;
    std::thread                          m_thread;
    std::shared_ptr<std::atomic<bool>>   m_cancel;
    std::atomic<bool>                    m_running{ false };
    std::mutex                           m_mutex;
    std::vector<PaintSplitPreviewResult> m_results;
};

} // namespace Slic3r
