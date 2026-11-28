// Paint Split R2 (tests/design_paint_split.md 8.2 E4-E6): see PaintSplitPreview.hpp.
#include "PaintSplitPreview.hpp"

#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>

#include <boost/log/trivial.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>

namespace Slic3r {

namespace {
using PspClock = std::chrono::steady_clock;
double psp_secs(PspClock::time_point t0) { return std::chrono::duration<double>(PspClock::now() - t0).count(); }
} // namespace

PaintSplitSkinParams paint_split_coarse_params(const PaintSplitSkinParams& fine)
{
    PaintSplitSkinParams p = fine;
    p.voxel                = std::max(1.2, 2. * (fine.voxel > 0. ? fine.voxel : 0.6));
    p.skin_refine          = std::min(fine.skin_refine, 1);
    p.root_iterations      = std::min(fine.root_iterations, 2);
    return p;
}

std::vector<int> paint_split_pass_components(const PaintSplitRegion& region)
{
    std::vector<int> out;
    for (size_t c = 0; c < region.num_components(); ++c)
        if (region.component_ok(int(c)))
            out.push_back(int(c));
    return out;
}

PaintSplitPreviewWorker::PaintSplitPreviewWorker(Notify notify) : m_notify(std::move(notify)) {}

PaintSplitPreviewWorker::~PaintSplitPreviewWorker() { cancel(); }

void PaintSplitPreviewWorker::start(PaintSplitPreviewInput input)
{
    if (m_cancel)
        m_cancel->store(true);
    if (m_thread.joinable())
        m_thread.join();
    {
        // What an older computation stored is stale now.
        std::lock_guard<std::mutex> lk(m_mutex);
        m_results.clear();
    }
    m_cancel = std::make_shared<std::atomic<bool>>(false);
    m_running.store(true);
    std::shared_ptr<std::atomic<bool>> cancel = m_cancel;
    m_thread = std::thread([this, input = std::move(input), cancel]() mutable {
        try {
            run(std::move(input), cancel);
        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(error) << "Paint split preview failed: " << e.what();
        }
        m_running.store(false);
        if (m_notify)
            m_notify();
    });
}

void PaintSplitPreviewWorker::request_cancel()
{
    if (m_cancel)
        m_cancel->store(true);
}

void PaintSplitPreviewWorker::cancel()
{
    if (m_cancel)
        m_cancel->store(true);
    if (m_thread.joinable())
        m_thread.join();
    m_running.store(false);
    std::lock_guard<std::mutex> lk(m_mutex);
    m_results.clear();
}

void PaintSplitPreviewWorker::wait()
{
    if (m_thread.joinable())
        m_thread.join();
}

std::vector<PaintSplitPreviewResult> PaintSplitPreviewWorker::take()
{
    std::lock_guard<std::mutex> lk(m_mutex);
    std::vector<PaintSplitPreviewResult> out;
    out.swap(m_results);
    return out;
}

void PaintSplitPreviewWorker::store(PaintSplitPreviewResult&& r, const std::atomic<bool>& cancel)
{
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (cancel.load())
            return;
        m_results.push_back(std::move(r));
    }
    if (m_notify)
        m_notify();
}

void PaintSplitPreviewWorker::run(PaintSplitPreviewInput in, std::shared_ptr<std::atomic<bool>> cancel)
{
    const auto t0 = PspClock::now();
    // ---- The region and the prepared part.
    std::shared_ptr<const PaintSplitRegion> region = in.region;
    if (!region) {
        if (!in.mesh)
            return;
        auto r = std::make_shared<PaintSplitRegion>(paint_split_region(in.mesh->its, in.mask, in.region_params));
        if (r->error == PaintSplitError::None)
            paint_split_transform(*r, in.to_cut);
        region = r;
    }
    if (cancel->load())
        return;
    std::shared_ptr<const PaintSplitPart> part = in.part;
    if (!part && region->error == PaintSplitError::None && in.mesh) {
        TriangleMesh cut(*in.mesh);
        cut.transform(in.to_cut, true);
        part = std::make_shared<const PaintSplitPart>(paint_split_prepare_part(cut.its));
    }
    if (cancel->load())
        return;
    const std::vector<int> comps = region->error == PaintSplitError::None ? paint_split_pass_components(*region) : std::vector<int>();
    {
        PaintSplitPreviewResult r;
        r.generation = in.generation;
        r.stage      = PaintSplitPreviewResult::Stage::Region;
        r.region     = region;
        r.part       = part;
        r.comps      = comps;
        r.seconds    = psp_secs(t0);
        store(std::move(r), *cancel);
    }
    if (!in.claims || !part || !part->ok || comps.empty() || cancel->load())
        return;

    // ---- Show cut: coarse, then fine (E6). Each claim with the earlier components excluded, as Apply cuts them.
    for (const bool fine : { false, true }) {
        const auto           t1   = PspClock::now();
        PaintSplitSkinParams base = fine ? in.skin : paint_split_coarse_params(in.skin);
        base.cancel               = cancel.get();
        std::vector<std::shared_ptr<const PaintSplitClaim>> claims(comps.size());
        std::vector<double>                                 secs(comps.size(), 0.);
        tbb::parallel_for(tbb::blocked_range<size_t>(0, comps.size(), 1), [&](const tbb::blocked_range<size_t>& rg) {
            for (size_t k = rg.begin(); k < rg.end(); ++k) {
                const auto t = PspClock::now();
                // The fine claim's key must be Apply's: the cancel pointer is not part of it.
                claims[k] = std::make_shared<const PaintSplitClaim>(
                    paint_split_nearest_cutter(*part, *region, comps[k], paint_split_pass_params(base, comps, k)));
                secs[k]   = psp_secs(t);
            }
        });
        if (cancel->load())
            return;
        std::vector<const PaintSplitClaim*> raw;
        for (const auto& c : claims)
            raw.push_back(c.get());
        const auto              t2 = PspClock::now();
        PaintSplitPreviewResult r;
        r.surfaces = paint_split_claim_cut_surfaces(part->mesh, raw, cancel.get());
        if (cancel->load())
            return;
        r.generation = in.generation;
        r.stage      = fine ? PaintSplitPreviewResult::Stage::Fine : PaintSplitPreviewResult::Stage::Coarse;
        r.region     = region;
        r.part       = part;
        r.comps      = comps;
        r.claims     = std::move(claims);
        r.seconds    = psp_secs(t1);
        if (fine) {
            char buf[256];
            for (size_t k = 0; k < comps.size(); ++k) {
                const PaintSplitClaimStats& s = r.claims[k]->stats;
                std::snprintf(buf, sizeof(buf), "Region %d: %.1f s (field %.1f s, mesh %.1f s, smooth %.1f s), voxel %.2f mm, %.3f mm at the skin",
                              comps[k] + 1, secs[k], s.setup_s + s.field_s, s.mesh_s, s.smooth_s, s.voxel, s.near_skin_voxel);
                r.log.emplace_back(buf);
            }
            std::snprintf(buf, sizeof(buf), "Cut surfaces: %.1f s", psp_secs(t2));
            r.log.emplace_back(buf);
        }
        BOOST_LOG_TRIVIAL(info) << "Paint split preview: " << (fine ? "fine" : "coarse") << " claims of " << comps.size() << " region(s) in "
                                << r.seconds << " s";
        store(std::move(r), *cancel);
    }
}

} // namespace Slic3r
