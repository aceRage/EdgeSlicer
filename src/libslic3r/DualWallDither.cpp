#include "DualWallDither.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <numeric>
#include <sstream>

namespace Slic3r {

namespace {

using Rgb = std::array<double, 3>;

double srgb_to_linear(double c)
{
    c = std::clamp(c, 0., 1.);
    return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

double linear_to_srgb(double c)
{
    c = std::clamp(c, 0., 1.);
    return c <= 0.0031308 ? c * 12.92 : 1.055 * std::pow(c, 1. / 2.4) - 0.055;
}

std::array<float, 3> linear_to_oklab(const Rgb &c)
{
    const double l = std::cbrt(0.4122214708 * c[0] + 0.5363325363 * c[1] + 0.0514459929 * c[2]);
    const double m = std::cbrt(0.2119034982 * c[0] + 0.6806995451 * c[1] + 0.1073969566 * c[2]);
    const double s = std::cbrt(0.0883024619 * c[0] + 0.2817188376 * c[1] + 0.6299787005 * c[2]);
    return { float(0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s),
             float(1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s),
             float(0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s) };
}

double oklab_dist2(const std::array<float, 3> &a, const std::array<float, 3> &b)
{
    const double dl = a[0] - b[0], da = a[1] - b[1], db = a[2] - b[2];
    return dl * dl + da * da + db * db;
}

// Per filament: linear colour and the two-way transmission of the outer and of the inner wall.
struct Prepared
{
    Rgb    linear;
    double pass_outer; // T^2 through an outer wall
    double pass_inner; // T^2 through an inner wall
};

std::vector<Prepared> prepare(const std::vector<DwdFilament> &filaments, const DwdModelParams &model)
{
    std::vector<Prepared> out;
    out.reserve(filaments.size());
    for (const DwdFilament &f : filaments) {
        const double to = dwd_transmission(f.td_mm, model.outer_width_mm, model.k);
        const double ti = dwd_transmission(f.td_mm, model.inner_width_mm, model.k);
        out.push_back({ { srgb_to_linear(f.srgb[0]), srgb_to_linear(f.srgb[1]), srgb_to_linear(f.srgb[2]) }, to * to, ti * ti });
    }
    return out;
}

Rgb predict_linear(const std::vector<Prepared> &fils, const std::vector<uint8_t> &outer, const std::vector<uint8_t> &inner, uint8_t backing)
{
    Rgb sum { 0., 0., 0. };
    if (outer.empty() || inner.empty() || backing >= fils.size())
        return sum;
    const size_t period = std::lcm(outer.size(), inner.size());
    for (size_t layer = 0; layer < period; ++layer) {
        const uint8_t o = outer[layer % outer.size()];
        const uint8_t i = inner[layer % inner.size()];
        if (o >= fils.size() || i >= fils.size())
            return { 0., 0., 0. };
        const Prepared &fo = fils[o], &fi = fils[i], &fb = fils[backing];
        for (int c = 0; c < 3; ++c) {
            const double behind = fi.linear[c] * (1. - fi.pass_inner) + fi.pass_inner * fb.linear[c];
            sum[c] += fo.linear[c] * (1. - fo.pass_outer) + fo.pass_outer * behind;
        }
    }
    for (double &c : sum)
        c /= double(period);
    return sum;
}

// Longest run of one value, the sequence taken as repeating.
int max_cyclic_run(const std::vector<uint8_t> &seq)
{
    const size_t n = seq.size();
    if (std::all_of(seq.begin(), seq.end(), [&](uint8_t v) { return v == seq.front(); }))
        return int(n);
    int best = 1;
    for (size_t start = 0; start < n; ++start) {
        int run = 1;
        while (run < int(n) && seq[(start + run) % n] == seq[start])
            ++run;
        best = std::max(best, run);
    }
    return best;
}

// Shortest sequence that repeats to the same thing ("1212" -> "12").
std::vector<uint8_t> minimal_period(const std::vector<uint8_t> &seq)
{
    const size_t n = seq.size();
    for (size_t p = 1; p < n; ++p) {
        if (n % p != 0)
            continue;
        bool same = true;
        for (size_t i = p; i < n && same; ++i)
            same = seq[i] == seq[i - p];
        if (same)
            return std::vector<uint8_t>(seq.begin(), seq.begin() + p);
    }
    return seq;
}

// Every way to split `total` layers over n filaments.
void compositions(size_t n, int total, std::vector<int> &current, std::vector<std::vector<int>> &out)
{
    if (current.size() + 1 == n) {
        current.push_back(total);
        out.push_back(current);
        current.pop_back();
        return;
    }
    for (int c = total; c >= 0; --c) {
        current.push_back(c);
        compositions(n, total - c, current, out);
        current.pop_back();
    }
}

// The counts laid out as evenly as possible: each filament's layers spread over the period.
std::vector<uint8_t> spread(const std::vector<int> &counts)
{
    const int total = std::accumulate(counts.begin(), counts.end(), 0);
    // Slot k of filament f wants position (k + 0.5) * total / count; sort by that.
    std::vector<std::pair<double, uint8_t>> slots;
    for (size_t f = 0; f < counts.size(); ++f)
        for (int k = 0; k < counts[f]; ++k)
            slots.emplace_back((k + 0.5) * total / counts[f], uint8_t(f));
    std::stable_sort(slots.begin(), slots.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
    std::vector<uint8_t> seq;
    seq.reserve(slots.size());
    for (const auto &s : slots)
        seq.push_back(s.second);
    return seq;
}

// The densest order of a 4-layer mix: the permutation with the shortest longest run.
std::vector<uint8_t> densest(const std::vector<int> &counts)
{
    std::vector<uint8_t> seq;
    for (size_t f = 0; f < counts.size(); ++f)
        seq.insert(seq.end(), counts[f], uint8_t(f));
    std::vector<uint8_t> best = seq;
    int best_run = max_cyclic_run(seq);
    while (std::next_permutation(seq.begin(), seq.end())) {
        const int run = max_cyclic_run(seq);
        if (run < best_run) {
            best_run = run;
            best = seq;
        }
    }
    return best;
}

int distinct_filaments(const std::vector<uint8_t> &outer, const std::vector<uint8_t> &inner, uint8_t backing)
{
    std::vector<uint8_t> all = outer;
    all.insert(all.end(), inner.begin(), inner.end());
    all.push_back(backing);
    std::sort(all.begin(), all.end());
    return int(std::unique(all.begin(), all.end()) - all.begin());
}

} // namespace

double dwd_transmission(double td_mm, double width_mm, double k)
{
    if (td_mm <= 0. || width_mm < 0. || k < 0.)
        return 0.;
    return std::clamp(std::exp(-k * width_mm / td_mm), 0., 1.);
}

std::array<float, 3> dwd_predict(const std::vector<DwdFilament> &filaments, const std::vector<uint8_t> &outer,
                                 const std::vector<uint8_t> &inner, uint8_t backing, const DwdModelParams &model)
{
    const Rgb lin = predict_linear(prepare(filaments, model), outer, inner, backing);
    return { float(linear_to_srgb(lin[0])), float(linear_to_srgb(lin[1])), float(linear_to_srgb(lin[2])) };
}

std::vector<std::vector<uint8_t>> dwd_outer_sequences(size_t n_filaments, bool relaxed)
{
    std::vector<std::vector<uint8_t>> out;
    if (n_filaments == 0)
        return out;
    std::vector<std::vector<int>> mixes;
    std::vector<int> current;
    compositions(n_filaments, 4, current, mixes);
    for (const std::vector<int> &counts : mixes) {
        const int largest = *std::max_element(counts.begin(), counts.end());
        // 4:0 is a single filament; otherwise no filament may take 3 of the 4 layers unless relaxed.
        if (largest == 3 && !relaxed)
            continue;
        out.push_back(minimal_period(densest(counts)));
    }
    return out;
}

std::vector<std::vector<uint8_t>> dwd_inner_sequences(size_t n_filaments)
{
    std::vector<std::vector<uint8_t>> out;
    if (n_filaments == 0)
        return out;
    std::vector<std::vector<int>> mixes;
    std::vector<int> current;
    compositions(n_filaments, 8, current, mixes);
    out.reserve(mixes.size());
    for (const std::vector<int> &counts : mixes)
        out.push_back(minimal_period(spread(counts)));
    return out;
}

std::vector<DwdPair> dwd_build_table(const std::vector<DwdFilament> &filaments, uint8_t backing,
                                     const DwdModelParams &model, const DwdTableParams &params)
{
    std::vector<DwdPair> table;
    const size_t n = filaments.size();
    if (n < 2 || n > 9 || backing >= n || params.max_pairs == 0)
        return table;

    const std::vector<Prepared> fils = prepare(filaments, model);
    const size_t n_seq = params.sequence_filaments == 0 ? n : std::min(n, params.sequence_filaments);
    auto       outers = dwd_outer_sequences(n_seq, params.relaxed_outer);
    const auto inners = dwd_inner_sequences(n_seq);
    if (outers.empty() || inners.empty())
        return table;

    // Denser outer walls first (shorter runs, then the shorter period): when two outer sequences
    // have the same colour, the first one in the table wins the match.
    std::stable_sort(outers.begin(), outers.end(), [](const std::vector<uint8_t> &a, const std::vector<uint8_t> &b) {
        const int run_a = a.size() == 1 ? 1 : max_cyclic_run(a), run_b = b.size() == 1 ? 1 : max_cyclic_run(b);
        return run_a != run_b ? run_a < run_b : a.size() < b.size();
    });

    // Every outer sequence keeps its share of the table: the inner sequences that give it distinct
    // colours. Preference when two give the same colour: fewer filaments on the layer (fewer tool
    // changes), then the shorter inner period. stable_sort keeps enumeration order for the rest,
    // so the table is deterministic.
    struct Candidate
    {
        Rgb                  lin;
        std::array<float, 3> oklab;
        size_t               inner;
        int                  used;
    };
    const size_t share = std::max<size_t>(1, params.max_pairs / outers.size());
    std::vector<Candidate> candidates;
    std::vector<size_t>    kept;
    for (const std::vector<uint8_t> &outer : outers) {
        Rgb outer_lin { 0., 0., 0. };
        for (uint8_t f : outer)
            for (int ch = 0; ch < 3; ++ch)
                outer_lin[ch] += fils[f].linear[ch] / double(outer.size());

        candidates.clear();
        candidates.reserve(inners.size());
        for (size_t i = 0; i < inners.size(); ++i) {
            const Rgb lin = predict_linear(fils, outer, inners[i], backing);
            candidates.push_back({ lin, linear_to_oklab(lin), i, distinct_filaments(outer, inners[i], backing) });
        }
        std::stable_sort(candidates.begin(), candidates.end(), [&](const Candidate &a, const Candidate &b) {
            if (a.used != b.used)
                return a.used < b.used;
            return inners[a.inner].size() < inners[b.inner].size();
        });

        double delta = std::max(params.min_delta, 1e-4);
        for (;;) {
            kept.clear();
            const double delta2 = delta * delta;
            for (size_t idx = 0; idx < candidates.size() && kept.size() <= share; ++idx) {
                bool distinct = true;
                for (size_t k : kept)
                    if (oklab_dist2(candidates[idx].oklab, candidates[k].oklab) < delta2) {
                        distinct = false;
                        break;
                    }
                if (distinct)
                    kept.push_back(idx);
            }
            if (kept.size() <= share)
                break;
            delta *= 1.15;
        }
        for (size_t k : kept) {
            const Candidate &cand = candidates[k];
            DwdPair pair;
            pair.outer   = outer;
            pair.inner   = inners[cand.inner];
            pair.backing = backing;
            pair.srgb  = { float(linear_to_srgb(cand.lin[0])), float(linear_to_srgb(cand.lin[1])), float(linear_to_srgb(cand.lin[2])) };
            pair.oklab = cand.oklab;
            pair.outer_srgb  = { float(linear_to_srgb(outer_lin[0])), float(linear_to_srgb(outer_lin[1])), float(linear_to_srgb(outer_lin[2])) };
            pair.outer_oklab = linear_to_oklab(outer_lin);
            pair.filaments_used = cand.used;
            table.push_back(std::move(pair));
        }
    }
    return table;
}

std::shared_ptr<const std::vector<DwdPair>> dwd_cached_table(const std::vector<DwdFilament> &filaments, uint8_t backing,
                                                             const DwdModelParams &model, const DwdTableParams &params)
{
    std::ostringstream key;
    key << int(backing) << '|' << model.k << '|' << model.outer_width_mm << '|' << model.inner_width_mm << '|'
        << params.relaxed_outer << '|' << params.max_pairs << '|' << params.min_delta << '|' << params.sequence_filaments;
    for (const DwdFilament &f : filaments)
        key << '|' << f.srgb[0] << ',' << f.srgb[1] << ',' << f.srgb[2] << ',' << f.td_mm;

    static std::mutex                                                                     mutex;
    static std::map<std::string, std::shared_ptr<const std::vector<DwdPair>>> cache;
    std::lock_guard<std::mutex> lock(mutex);
    auto it = cache.find(key.str());
    if (it != cache.end())
        return it->second;
    // A handful of filament sets per session at most; keep the cache from growing without bound anyway.
    if (cache.size() >= 32)
        cache.clear();
    auto table = std::make_shared<const std::vector<DwdPair>>(dwd_build_table(filaments, backing, model, params));
    cache.emplace(key.str(), table);
    return table;
}

int dwd_nearest(const std::vector<DwdPair> &table, const std::array<float, 3> &target_srgb)
{
    if (table.empty())
        return -1;
    const std::array<float, 3> target = dwd_srgb_to_oklab(target_srgb);
    // The outer wall first: the mix whose own colour is nearest.
    double outer_d = oklab_dist2(table.front().outer_oklab, target);
    for (size_t i = 1; i < table.size(); ++i)
        outer_d = std::min(outer_d, oklab_dist2(table[i].outer_oklab, target));
    // Then, of that outer wall's pairs, the nearest predicted colour.
    int    best = -1;
    double best_d = 0.;
    for (size_t i = 0; i < table.size(); ++i) {
        if (oklab_dist2(table[i].outer_oklab, target) > outer_d + 1e-12)
            continue;
        if (best >= 0 && table[i].outer != table[size_t(best)].outer)
            continue; // an outer sequence of the same colour further down: the first one wins
        const double d = oklab_dist2(table[i].oklab, target);
        if (best < 0 || d < best_d) {
            best_d = d;
            best = int(i);
        }
    }
    return best;
}

uint8_t dwd_default_backing(const std::vector<DwdFilament> &filaments)
{
    uint8_t best = 0;
    float   best_l = -1.f;
    for (size_t i = 0; i < filaments.size() && i < 256; ++i) {
        const float l = dwd_srgb_to_oklab(filaments[i].srgb)[0];
        if (l > best_l) {
            best_l = l;
            best = uint8_t(i);
        }
    }
    return best;
}

std::string dwd_pattern(const DwdPair &pair, const std::vector<int> &physical_ids, int component_a, int component_b)
{
    auto token = [&](uint8_t filament) -> char {
        if (filament >= physical_ids.size())
            return 0;
        const int id = physical_ids[filament];
        if (id == component_a)
            return '1';
        if (id == component_b)
            return '2';
        return id >= 3 && id <= 9 ? char('0' + id) : 0;
    };
    std::string out;
    auto append = [&](const std::vector<uint8_t> &seq) {
        for (uint8_t f : seq) {
            const char t = token(f);
            if (t == 0)
                return false;
            out.push_back(t);
        }
        return !seq.empty();
    };
    if (!append(pair.outer))
        return {};
    out.push_back(',');
    if (!append(pair.inner))
        return {};
    out.push_back(',');
    if (!append({ pair.backing }))
        return {};
    return out;
}

std::array<float, 3> dwd_srgb_to_oklab(const std::array<float, 3> &srgb)
{
    return linear_to_oklab({ srgb_to_linear(srgb[0]), srgb_to_linear(srgb[1]), srgb_to_linear(srgb[2]) });
}

} // namespace Slic3r
