#include "ShowcaseWebP.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <queue>
#include <utility>

namespace Slic3r {
namespace Showcase {

namespace {

// --- bits, LSB first (RFC 9649 section 3) -------------------------------------------------------

class BitWriter
{
public:
    void put(uint32_t bits, int nbits)
    {
        if (nbits <= 0)
            return;
        m_acc |= uint64_t(bits & ((nbits >= 32) ? 0xFFFFFFFFu : ((1u << nbits) - 1u))) << m_n;
        m_n += nbits;
        while (m_n >= 8) {
            m_out.push_back(uint8_t(m_acc & 0xFF));
            m_acc >>= 8;
            m_n -= 8;
        }
    }
    std::vector<uint8_t> finish()
    {
        if (m_n > 0)
            m_out.push_back(uint8_t(m_acc & 0xFF));
        m_acc = 0;
        m_n   = 0;
        return std::move(m_out);
    }

private:
    std::vector<uint8_t> m_out;
    uint64_t             m_acc = 0;
    int                  m_n   = 0;
};

// --- prefix codes ---------------------------------------------------------------------------------

constexpr int NUM_LENGTH_CODES   = 24;
constexpr int NUM_DISTANCE_CODES = 40;
constexpr int GREEN_ALPHABET     = 256 + NUM_LENGTH_CODES;   // no colour cache
constexpr int MAX_LENGTH         = 4096;
constexpr int CODE_LENGTH_CODES  = 19;
const int     CODE_LENGTH_ORDER[CODE_LENGTH_CODES] = { 17, 18, 0, 1, 2, 3, 4, 5, 16, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };

// Huffman code lengths of at most `max_len` bits. Counts are flattened until the tree fits, which
// ends at worst in a balanced tree (always within the limit for these alphabets).
std::vector<uint8_t> huffman_lengths(std::vector<uint32_t> counts, int max_len)
{
    const size_t n = counts.size();
    std::vector<uint8_t> lengths(n, 0);
    for (;;) {
        struct Node { uint64_t weight; int left, right; };
        std::vector<Node> nodes;
        using Item = std::pair<uint64_t, int>;   // weight, node index (index breaks ties deterministically)
        std::priority_queue<Item, std::vector<Item>, std::greater<Item>> q;
        for (size_t s = 0; s < n; ++s)
            if (counts[s] > 0) {
                nodes.push_back({ counts[s], -1 - int(s), 0 });
                q.push({ counts[s], int(nodes.size()) - 1 });
            }
        if (nodes.size() < 2)
            return lengths;   // the callers never ask for fewer than two symbols
        while (q.size() > 1) {
            const Item a = q.top(); q.pop();
            const Item b = q.top(); q.pop();
            nodes.push_back({ a.first + b.first, a.second, b.second });
            q.push({ a.first + b.first, int(nodes.size()) - 1 });
        }
        // Depths, iteratively from the root.
        std::fill(lengths.begin(), lengths.end(), uint8_t(0));
        int max_depth = 0;
        std::vector<std::pair<int, int>> stack{ { q.top().second, 0 } };
        while (!stack.empty()) {
            const auto [idx, depth] = stack.back();
            stack.pop_back();
            const Node &nd = nodes[size_t(idx)];
            if (nd.left < 0) {
                lengths[size_t(-1 - nd.left)] = uint8_t(std::min(depth, 255));
                max_depth = std::max(max_depth, depth);
            } else {
                stack.push_back({ nd.left, depth + 1 });
                stack.push_back({ nd.right, depth + 1 });
            }
        }
        if (max_depth <= max_len)
            return lengths;
        for (uint32_t &c : counts)
            if (c > 0)
                c = (c + 1) / 2;
    }
}

struct PrefixCode
{
    std::vector<uint8_t>  lengths;
    std::vector<uint16_t> codes;   // bit-reversed, ready to be written LSB first

    void build(const std::vector<uint8_t> &lens)
    {
        lengths = lens;
        codes.assign(lens.size(), 0);
        int bl_count[16] = { 0 };
        for (uint8_t l : lens)
            if (l > 0)
                ++bl_count[l];
        int next_code[16] = { 0 };
        int code          = 0;
        for (int bits = 1; bits < 16; ++bits) {
            code            = (code + bl_count[bits - 1]) << 1;
            next_code[bits] = code;
        }
        for (size_t s = 0; s < lens.size(); ++s) {
            const int l = lens[s];
            if (l == 0)
                continue;
            int c = next_code[l]++, r = 0;
            for (int i = 0; i < l; ++i) {
                r = (r << 1) | (c & 1);
                c >>= 1;
            }
            codes[s] = uint16_t(r);
        }
    }
    void write(BitWriter &bw, int symbol) const
    {
        const size_t s = size_t(symbol);
        if (s < lengths.size() && lengths[s] > 0)
            bw.put(codes[s], lengths[s]);
    }
};

// Writes the code for a histogram and returns it (section 3.7.2.1).
PrefixCode write_prefix_code(BitWriter &bw, std::vector<uint32_t> counts)
{
    PrefixCode code;
    std::vector<int> used;
    for (size_t s = 0; s < counts.size(); ++s)
        if (counts[s] > 0)
            used.push_back(int(s));

    // Simple code: up to two 8-bit symbols. One symbol costs no bits at all.
    if (used.size() <= 2 && std::all_of(used.begin(), used.end(), [](int s) { return s < 256; })) {
        if (used.empty())
            used.push_back(0);
        bw.put(1, 1);
        bw.put(uint32_t(used.size() - 1), 1);
        const bool eight_bits = used[0] > 1;
        bw.put(eight_bits ? 1 : 0, 1);
        bw.put(uint32_t(used[0]), eight_bits ? 8 : 1);
        if (used.size() == 2)
            bw.put(uint32_t(used[1]), 8);
        std::vector<uint8_t> lens(counts.size(), 0);
        if (used.size() == 2) {
            lens[size_t(used[0])] = 1;
            lens[size_t(used[1])] = 1;
        }
        code.build(lens);
        return code;
    }

    // Normal code. A complete code needs two symbols: give a lone one a partner.
    if (used.size() < 2)
        counts[used.empty() || used[0] != 0 ? 0 : 1] = 1;
    const std::vector<uint8_t> lens = huffman_lengths(counts, 15);
    code.build(lens);

    // The lengths as code length symbols: 0..15 literal, 17 / 18 runs of zeros.
    struct Token { int symbol; int extra; int extra_bits; };
    std::vector<Token> tokens;
    for (size_t i = 0; i < lens.size();) {
        if (lens[i] != 0) {
            tokens.push_back({ lens[i], 0, 0 });
            ++i;
            continue;
        }
        size_t run = 0;
        while (i + run < lens.size() && lens[i + run] == 0)
            ++run;
        size_t left = run;
        while (left > 0) {
            if (left >= 11) {
                const size_t r = std::min<size_t>(left, 138);
                tokens.push_back({ 18, int(r - 11), 7 });
                left -= r;
            } else if (left >= 3) {
                tokens.push_back({ 17, int(left - 3), 3 });
                left = 0;
            } else {
                tokens.push_back({ 0, 0, 0 });
                --left;
            }
        }
        i += run;
    }
    std::vector<uint32_t> cl_counts(CODE_LENGTH_CODES, 0);
    for (const Token &t : tokens)
        ++cl_counts[size_t(t.symbol)];
    if (std::count_if(cl_counts.begin(), cl_counts.end(), [](uint32_t c) { return c > 0; }) < 2)
        cl_counts[cl_counts[0] > 0 ? 1 : 0] = 1;
    const std::vector<uint8_t> cl_lens = huffman_lengths(cl_counts, 7);
    PrefixCode cl_code;
    cl_code.build(cl_lens);

    int num_code_lengths = CODE_LENGTH_CODES;
    while (num_code_lengths > 4 && cl_lens[size_t(CODE_LENGTH_ORDER[num_code_lengths - 1])] == 0)
        --num_code_lengths;
    bw.put(0, 1);
    bw.put(uint32_t(num_code_lengths - 4), 4);
    for (int i = 0; i < num_code_lengths; ++i)
        bw.put(cl_lens[size_t(CODE_LENGTH_ORDER[i])], 3);
    bw.put(0, 1);   // max_symbol = the alphabet size
    for (const Token &t : tokens) {
        cl_code.write(bw, t.symbol);
        bw.put(uint32_t(t.extra), t.extra_bits);
    }
    return code;
}

// --- LZ77 values (section 3.6.2.1) -----------------------------------------------------------------

struct PrefixValue { int code; int extra_bits; uint32_t extra; };

PrefixValue prefix_encode(int value)   // value >= 1
{
    const int v = value - 1;
    if (v < 4)
        return { v, 0, 0 };
    int high = 0;
    while ((v >> (high + 1)) != 0)
        ++high;
    const int second     = (v >> (high - 1)) & 1;
    const int extra_bits = high - 1;
    return { 2 * high + second, extra_bits, uint32_t(v & ((1 << extra_bits) - 1)) };
}

// One literal pixel or one back reference.
struct Token
{
    uint32_t argb   = 0;
    int      length = 0;   // 0 = literal
    int      dist   = 0;   // pixels back
};

std::vector<Token> lz77(const std::vector<uint32_t> &px, int width)
{
    const size_t n = px.size();
    std::vector<Token> out;
    out.reserve(n / 2 + 16);
    // static: MSVC will not use a local constexpr inside a lambda without capturing it.
    static constexpr int  HASH_BITS  = 18;
    static constexpr int  CHAIN      = 12;
    // The largest distance code (39) reaches about a million; the plain distance is that minus 120.
    static constexpr size_t WINDOW   = (size_t(1) << 20) - 256;
    std::vector<int32_t> head(size_t(1) << HASH_BITS, -1);
    std::vector<int32_t> prev(n, -1);
    auto hash = [&px, n](size_t i) -> uint32_t {
        const uint32_t a = px[i], b = i + 1 < n ? px[i + 1] : 0u;
        return ((a * 0x9E3779B1u) ^ (b * 0x85EBCA77u) ^ (b >> 15)) >> (32 - HASH_BITS);
    };
    auto insert = [&](size_t i) {
        if (i + 1 >= n)
            return;
        const uint32_t h = hash(i);
        prev[i]          = head[h];
        head[h]          = int32_t(i);
    };
    auto match_len = [&px, n](size_t i, size_t j) {
        const size_t limit = std::min<size_t>(MAX_LENGTH, n - i);
        size_t       l     = 0;
        while (l < limit && px[i + l] == px[j + l])
            ++l;
        return l;
    };

    for (size_t i = 0; i < n;) {
        size_t best_len = 0, best_dist = 0;
        auto   consider = [&](size_t j) {
            if (j >= i || i - j > WINDOW)
                return;
            const size_t l = match_len(i, j);
            if (l > best_len) {
                best_len  = l;
                best_dist = i - j;
            }
        };
        if (i >= 1)
            consider(i - 1);
        if (size_t(width) <= i)
            consider(i - size_t(width));
        if (i + 1 < n) {
            int32_t j = head[hash(i)];
            for (int k = 0; k < CHAIN && j >= 0 && best_len < size_t(MAX_LENGTH); ++k, j = prev[size_t(j)])
                consider(size_t(j));
        }
        if (best_len >= 3) {
            Token t;
            t.length = int(best_len);
            t.dist   = int(best_dist);
            out.push_back(t);
            for (size_t k = 0; k < best_len; ++k)
                insert(i + k);
            i += best_len;
        } else {
            Token t;
            t.argb = px[i];
            out.push_back(t);
            insert(i);
            ++i;
        }
    }
    return out;
}

// An entropy-coded image (the predictor modes or the pixels): colour cache bit, the meta prefix
// bit for the main image only, five prefix codes, the data.
void write_image(BitWriter &bw, const std::vector<uint32_t> &px, int width, bool is_main)
{
    bw.put(0, 1);   // no colour cache
    if (is_main)
        bw.put(0, 1);   // one prefix code group for the whole image

    const std::vector<Token> tokens = lz77(px, width);
    std::vector<uint32_t> green(GREEN_ALPHABET, 0), red(256, 0), blue(256, 0), alpha(256, 0), dist(NUM_DISTANCE_CODES, 0);
    for (const Token &t : tokens) {
        if (t.length == 0) {
            ++green[(t.argb >> 8) & 0xFF];
            ++red[(t.argb >> 16) & 0xFF];
            ++blue[t.argb & 0xFF];
            ++alpha[t.argb >> 24];
        } else {
            ++green[size_t(256 + prefix_encode(t.length).code)];
            ++dist[size_t(prefix_encode(t.dist + 120).code)];
        }
    }
    const PrefixCode cg = write_prefix_code(bw, green);
    const PrefixCode cr = write_prefix_code(bw, red);
    const PrefixCode cb = write_prefix_code(bw, blue);
    const PrefixCode ca = write_prefix_code(bw, alpha);
    const PrefixCode cd = write_prefix_code(bw, dist);
    for (const Token &t : tokens) {
        if (t.length == 0) {
            cg.write(bw, int((t.argb >> 8) & 0xFF));
            cr.write(bw, int((t.argb >> 16) & 0xFF));
            cb.write(bw, int(t.argb & 0xFF));
            ca.write(bw, int(t.argb >> 24));
        } else {
            const PrefixValue l = prefix_encode(t.length);
            cg.write(bw, 256 + l.code);
            bw.put(l.extra, l.extra_bits);
            const PrefixValue d = prefix_encode(t.dist + 120);   // codes above 120 are plain distances
            cd.write(bw, d.code);
            bw.put(d.extra, d.extra_bits);
        }
    }
}

// --- the predictor transform (section 4.1) ---------------------------------------------------------

inline uint32_t ch(uint32_t p, int s) { return (p >> s) & 0xFF; }

inline uint32_t average2(uint32_t a, uint32_t b)
{
    return (((a ^ b) & 0xFEFEFEFEu) >> 1) + (a & b);
}

uint32_t select_pred(uint32_t L, uint32_t T, uint32_t TL)
{
    int pl = 0, pt = 0;
    for (int s = 0; s < 32; s += 8) {
        const int p = int(ch(L, s)) + int(ch(T, s)) - int(ch(TL, s));
        pl += std::abs(p - int(ch(L, s)));
        pt += std::abs(p - int(ch(T, s)));
    }
    return pl < pt ? L : T;
}

inline uint32_t clamp255(int v) { return uint32_t(std::clamp(v, 0, 255)); }

uint32_t clamp_add_subtract_full(uint32_t a, uint32_t b, uint32_t c)
{
    uint32_t out = 0;
    for (int s = 0; s < 32; s += 8)
        out |= clamp255(int(ch(a, s)) + int(ch(b, s)) - int(ch(c, s))) << s;
    return out;
}

uint32_t clamp_add_subtract_half(uint32_t a, uint32_t b)
{
    uint32_t out = 0;
    for (int s = 0; s < 32; s += 8)
        out |= clamp255(int(ch(a, s)) + (int(ch(a, s)) - int(ch(b, s))) / 2) << s;
    return out;
}

// The modes tried per block. None of them reads the top-right pixel, whose rule on the last column
// differs from the others.
constexpr int PREDICTOR_MODES[] = { 1, 2, 7, 11, 12, 13 };

uint32_t predict(int mode, uint32_t L, uint32_t T, uint32_t TL)
{
    switch (mode) {
    case 1: return L;
    case 2: return T;
    case 7: return average2(L, T);
    case 11: return select_pred(L, T, TL);
    case 12: return clamp_add_subtract_full(L, T, TL);
    case 13: return clamp_add_subtract_half(average2(L, T), TL);
    default: return 0xFF000000u;
    }
}

inline uint32_t sub_pixels(uint32_t a, uint32_t b)
{
    uint32_t out = 0;
    for (int s = 0; s < 32; s += 8)
        out |= ((ch(a, s) - ch(b, s)) & 0xFF) << s;
    return out;
}

inline int residual_cost(uint32_t r)
{
    int c = 0;
    for (int s = 0; s < 32; s += 8) {
        const int v = int(ch(r, s));
        c += std::min(v, 256 - v);
    }
    return c;
}

} // namespace

std::vector<uint8_t> encode_webp_lossless(const std::vector<uint8_t> &rgba, int width, int height)
{
    if (width <= 0 || height <= 0 || width > 16384 || height > 16384 || rgba.size() < size_t(width) * size_t(height) * 4)
        return {};
    const size_t n = size_t(width) * size_t(height);

    // ARGB, then subtract green (section 4.2).
    std::vector<uint32_t> px(n);
    bool has_alpha = false;
    for (size_t i = 0; i < n; ++i) {
        const uint32_t r = rgba[i * 4], g = rgba[i * 4 + 1], b = rgba[i * 4 + 2], a = rgba[i * 4 + 3];
        has_alpha |= a != 255;
        px[i] = (a << 24) | (((r - g) & 0xFF) << 16) | (g << 8) | ((b - g) & 0xFF);
    }

    // Predictor modes per block, chosen by the smallest residuals.
    static constexpr int SIZE_BITS = 5;
    const int     bw_blocks = (width + (1 << SIZE_BITS) - 1) >> SIZE_BITS;
    const int     bh_blocks = (height + (1 << SIZE_BITS) - 1) >> SIZE_BITS;
    std::vector<uint32_t> modes(size_t(bw_blocks) * size_t(bh_blocks), 0xFF000000u | (11u << 8));
    for (int by = 0; by < bh_blocks; ++by)
        for (int bx = 0; bx < bw_blocks; ++bx) {
            long best_cost = -1;
            int  best_mode = 11;
            for (int mode : PREDICTOR_MODES) {
                long cost = 0;
                for (int y = std::max(1, by << SIZE_BITS); y < std::min(height, (by + 1) << SIZE_BITS); ++y)
                    for (int x = std::max(1, bx << SIZE_BITS); x < std::min(width, (bx + 1) << SIZE_BITS); ++x) {
                        const size_t i = size_t(y) * size_t(width) + size_t(x);
                        cost += residual_cost(sub_pixels(px[i], predict(mode, px[i - 1], px[i - size_t(width)], px[i - size_t(width) - 1])));
                    }
                if (best_cost < 0 || cost < best_cost) {
                    best_cost = cost;
                    best_mode = mode;
                }
            }
            modes[size_t(by) * size_t(bw_blocks) + size_t(bx)] = 0xFF000000u | (uint32_t(best_mode) << 8);
        }
    std::vector<uint32_t> residuals(n);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            const size_t i = size_t(y) * size_t(width) + size_t(x);
            uint32_t     pred;
            if (x == 0 && y == 0)
                pred = 0xFF000000u;
            else if (y == 0)
                pred = px[i - 1];
            else if (x == 0)
                pred = px[i - size_t(width)];
            else
                pred = predict(int((modes[size_t(y >> SIZE_BITS) * size_t(bw_blocks) + size_t(x >> SIZE_BITS)] >> 8) & 0xFF), px[i - 1],
                               px[i - size_t(width)], px[i - size_t(width) - 1]);
            residuals[i] = sub_pixels(px[i], pred);
        }
    px.clear();
    px.shrink_to_fit();

    BitWriter bw;
    bw.put(0x2F, 8);   // VP8L signature
    bw.put(uint32_t(width - 1), 14);
    bw.put(uint32_t(height - 1), 14);
    bw.put(has_alpha ? 1 : 0, 1);
    bw.put(0, 3);      // version
    // Transforms in the order they were applied; the decoder undoes them backwards.
    bw.put(1, 1);
    bw.put(2, 2);      // SUBTRACT_GREEN
    bw.put(1, 1);
    bw.put(0, 2);      // PREDICTOR
    bw.put(SIZE_BITS - 2, 3);
    write_image(bw, modes, bw_blocks, false);
    bw.put(0, 1);      // no more transforms
    write_image(bw, residuals, width, true);
    const std::vector<uint8_t> vp8l = bw.finish();

    std::vector<uint8_t> out;
    out.reserve(vp8l.size() + 21);
    auto u32 = [&out](uint32_t v) {
        for (int i = 0; i < 4; ++i)
            out.push_back(uint8_t((v >> (8 * i)) & 0xFF));
    };
    const uint32_t chunk = uint32_t(vp8l.size());
    const uint32_t pad   = chunk & 1u;
    out.insert(out.end(), { 'R', 'I', 'F', 'F' });
    u32(4 + 8 + chunk + pad);
    out.insert(out.end(), { 'W', 'E', 'B', 'P', 'V', 'P', '8', 'L' });
    u32(chunk);
    out.insert(out.end(), vp8l.begin(), vp8l.end());
    if (pad)
        out.push_back(0);
    return out;
}

} // namespace Showcase
} // namespace Slic3r
