#include "PaintWire.hpp"


#include <algorithm>
#include <cstring>
#include <map>

namespace Slic3r {

namespace {

// Walks one tree in pre-order. Fills `bits` (when given) with TriangleSelector::serialize()'s
// layout and marks `used`. Returns false with `error` on a bad tree.
struct TreeReader
{
    const std::vector<uint8_t> &bytes;
    size_t                      pos = 0;
    std::vector<bool>          *bits;
    std::vector<bool>          &used;
    std::string                &error;

    // The state of a node that is a leaf, or -1 for a split node. Used for the normal-form check.
    int node(int depth)
    {
        if (pos >= bytes.size()) {
            error = "tree is truncated";
            return -2;
        }
        const uint8_t b = bytes[pos++];
        if (b != PAINT_WIRE_SPLIT) {
            write_leaf(b);
            return b;
        }
        if (depth >= PAINT_WIRE_MAX_DEPTH) {
            error = "tree is deeper than " + std::to_string(PAINT_WIRE_MAX_DEPTH);
            return -2;
        }
        // A three-side split with special side 0 is the 4-way midpoint split; serialize() writes the
        // children last to first, so they are read into a buffer and emitted in reverse.
        std::vector<bool> children[4];
        int               states[4];
        for (int i = 0; i < 4; ++i) {
            std::vector<bool> *outer = bits;
            bits                     = outer ? &children[i] : nullptr;
            states[i]                = node(depth + 1);
            bits                     = outer;
            if (states[i] == -2)
                return -2;
        }
        if (states[0] >= 0 && states[0] == states[1] && states[0] == states[2] && states[0] == states[3]) {
            error = "tree is not in normal form (a split with four equal leaves)";
            return -2;
        }
        if (bits) {
            bits->insert(bits->end(), {true, true, false, false});
            for (int i = 3; i >= 0; --i)
                bits->insert(bits->end(), children[i].begin(), children[i].end());
        }
        return -1;
    }

    void write_leaf(int state)
    {
        if (size_t(state) < used.size())
            used[size_t(state)] = true;
        if (!bits)
            return;
        bits->push_back(false);
        bits->push_back(false);
        if (state >= 3) {
            bits->push_back(true);
            bits->push_back(true);
            int m = state - 3;
            for (; m >= 15; m -= 15)
                bits->insert(bits->end(), {true, true, true, true});
            for (int i = 0; i < 4; ++i)
                bits->push_back((m & (1 << i)) != 0);
        } else {
            bits->push_back((state & 0b01) != 0);
            bits->push_back((state & 0b10) != 0);
        }
    }
};

bool read_tree(const std::vector<uint8_t> &bytes, std::vector<bool> *bits, std::vector<bool> &used, std::string &error)
{
    TreeReader r{bytes, 0, bits, used, error};
    if (r.node(0) == -2)
        return false;
    if (r.pos != bytes.size()) {
        error = "tree has trailing bytes";
        return false;
    }
    return true;
}

} // namespace

bool paint_wire_check_tree(const std::vector<uint8_t> &bytes, std::string &error)
{
    std::vector<bool> used(size_t(EnforcerBlockerType::ExtruderMax) + 1, false);
    return read_tree(bytes, nullptr, used, error);
}

bool paint_wire_apply(size_t                                         n_original_triangles,
                      const std::vector<PaintWireTree>              &trees,
                      const TriangleSelector::TriangleSplittingData &existing,
                      TriangleSelector::TriangleSplittingData       &out,
                      std::string                                   &error)
{
    out = TriangleSelector::TriangleSplittingData();
    for (size_t k = 0; k < trees.size(); ++k) {
        const int t = trees[k].first;
        if (t < 0 || size_t(t) >= n_original_triangles) {
            error = "triangle " + std::to_string(t) + " is out of range";
            return false;
        }
        if (k > 0 && t <= trees[k - 1].first) {
            error = "triangles are not ascending and unique";
            return false;
        }
    }

    // Where each triangle's subtree sits in `existing` (serialize() writes them in ascending order).
    std::map<int, std::pair<size_t, size_t>> carry;
    const auto &tts = existing.triangles_to_split;
    for (size_t k = 0; k < tts.size(); ++k) {
        const size_t begin = size_t(tts[k].bitstream_start_idx);
        const size_t end   = k + 1 < tts.size() ? size_t(tts[k + 1].bitstream_start_idx) : existing.bitstream.size();
        if (begin <= end && end <= existing.bitstream.size())
            carry.emplace(tts[k].triangle_idx, std::make_pair(begin, end));
    }

    TriangleSelector::TriangleSplittingData data;
    std::vector<bool>                       listed(n_original_triangles, false);
    for (const PaintWireTree &tree : trees)
        listed[size_t(tree.first)] = true;

    auto next_tree = trees.begin();
    for (size_t t = 0; t < n_original_triangles; ++t) {
        if (!listed[t]) {
            auto it = carry.find(int(t));
            if (it == carry.end())
                continue;
            data.triangles_to_split.emplace_back(int(t), int(data.bitstream.size()));
            data.bitstream.insert(data.bitstream.end(), existing.bitstream.begin() + it->second.first,
                                  existing.bitstream.begin() + it->second.second);
            continue;
        }
        const std::vector<uint8_t> &bytes = (next_tree++)->second;
        // serialize() omits a triangle that is one unpainted leaf; so does the wire's "clear".
        if (bytes.size() == 1 && bytes[0] == 0)
            continue;
        std::vector<bool> bits;
        if (!read_tree(bytes, &bits, data.used_states, error)) {
            error = "triangle " + std::to_string(t) + ": " + error;
            return false;
        }
        data.triangles_to_split.emplace_back(int(t), int(data.bitstream.size()));
        data.bitstream.insert(data.bitstream.end(), bits.begin(), bits.end());
    }
    // used_states covers the carried triangles too: recompute it from the whole stream.
    data.reset_used_states();
    if (!data.triangles_to_split.empty() && !data.update_used_states(0)) {
        error = "the merged painting does not decode";
        return false;
    }
    out = std::move(data);
    return true;
}

std::vector<PaintWireDisplay> paint_wire_display(const TriangleMesh                            &mesh,
                                                 const TriangleSelector::TriangleSplittingData &painting)
{
    std::vector<PaintWireDisplay> out;
    TriangleSelector              selector(mesh);
    selector.deserialize(painting);
    for (size_t s = 1; s < painting.used_states.size(); ++s) {
        if (!painting.used_states[s])
            continue;
        std::vector<int>           source;
        const indexed_triangle_set its = selector.get_facets_strict(EnforcerBlockerType(s), &source);
        if (its.indices.empty())
            continue;
        PaintWireDisplay d;
        d.state = int(s);
        d.vertices.reserve(its.indices.size() * 3);
        for (const stl_triangle_vertex_indices &f : its.indices)
            for (int i = 0; i < 3; ++i)
                d.vertices.push_back(its.vertices[size_t(f[i])]);
        d.source = std::move(source);
        out.push_back(std::move(d));
    }
    return out;
}

std::vector<bool> paint_wire_triangle_bits(const TriangleSelector::TriangleSplittingData &data, int triangle)
{
    // serialize() writes the triangles in ascending order, each subtree right after the previous one.
    const auto &tts = data.triangles_to_split;
    auto it = std::lower_bound(tts.begin(), tts.end(), triangle,
                               [](const TriangleSelector::TriangleBitStreamMapping &m, int t) { return m.triangle_idx < t; });
    if (it == tts.end() || it->triangle_idx != triangle)
        return {};
    const size_t begin = size_t(it->bitstream_start_idx);
    const size_t end   = it + 1 != tts.end() ? size_t((it + 1)->bitstream_start_idx) : data.bitstream.size();
    if (begin > end || end > data.bitstream.size())
        return {};
    return std::vector<bool>(data.bitstream.begin() + begin, data.bitstream.begin() + end);
}

int paint_wire_max_state(const std::vector<PaintWireTree> &trees, std::string &error)
{
    std::vector<bool> used(size_t(EnforcerBlockerType::ExtruderMax) + 1, false);
    for (const PaintWireTree &tree : trees)
        if (!read_tree(tree.second, nullptr, used, error)) {
            error = "triangle " + std::to_string(tree.first) + ": " + error;
            return -1;
        }
    for (int s = int(used.size()) - 1; s > 0; --s)
        if (used[size_t(s)])
            return s;
    return 0;
}

} // namespace Slic3r
