#include "std/Deflate.hpp"

#include <algorithm>
#include <array>
#include <queue>

namespace quartz::stdlib {

namespace {

constexpr std::size_t kWindow = 32768;
constexpr std::size_t kHashBits = 15;
constexpr std::size_t kMaxMatch = 258;
constexpr std::size_t kBlockSymbols = 16384;

struct Symbol {
    std::uint16_t litlen; // literal byte, or 257.. length code
    std::uint16_t dist;   // 0 for literals
    std::uint16_t len;    // match length
};

class BitWriter {
public:
    void put(std::uint32_t value, int bits) {
        buf_ |= static_cast<std::uint64_t>(value) << count_;
        count_ += bits;
        while (count_ >= 8) {
            out.push_back(static_cast<std::uint8_t>(buf_));
            buf_ >>= 8;
            count_ -= 8;
        }
    }
    void putCode(std::uint32_t code, int len) {
        std::uint32_t rev = 0;
        for (int i = 0; i < len; ++i) {
            rev = (rev << 1) | ((code >> i) & 1);
        }
        put(rev, len);
    }
    void align() {
        if (count_ > 0) {
            put(0, 8 - count_);
        }
    }
    std::vector<std::uint8_t> out;

private:
    std::uint64_t buf_ = 0;
    int count_ = 0;
};

int lengthCode(std::size_t len, int& extraBits, int& extra) {
    static constexpr std::array<std::uint16_t, 29> kBase = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                                            31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
    static constexpr std::array<std::uint8_t, 29> kExtra = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                                           2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    int i = 28;
    while (kBase[static_cast<std::size_t>(i)] > len) {
        --i;
    }
    extraBits = kExtra[static_cast<std::size_t>(i)];
    extra = static_cast<int>(len - kBase[static_cast<std::size_t>(i)]);
    return 257 + i;
}

int distCode(std::size_t d, int& extraBits, int& extra) {
    static constexpr std::array<std::uint16_t, 30> kBase = {1,    2,    3,    4,    5,    7,     9,     13,    17,  25,
                                                            33,   49,   65,   97,   129,  193,   257,   385,   513, 769,
                                                            1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
    static constexpr std::array<std::uint8_t, 30> kExtra = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                                           6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
    int i = 29;
    while (kBase[static_cast<std::size_t>(i)] > d) {
        --i;
    }
    extraBits = kExtra[static_cast<std::size_t>(i)];
    extra = static_cast<int>(d - kBase[static_cast<std::size_t>(i)]);
    return i;
}

// Huffman code lengths limited to `limit` bits: halve the frequencies until the tree fits.
std::vector<std::uint8_t> codeLengths(std::vector<std::uint32_t> freq, int limit) {
    std::size_t n = freq.size();
    std::vector<std::uint8_t> lengths(n, 0);
    while (true) {
        std::vector<int> used;
        for (std::size_t i = 0; i < n; ++i) {
            if (freq[i] > 0) {
                used.push_back(static_cast<int>(i));
            }
        }
        std::fill(lengths.begin(), lengths.end(), 0);
        if (used.empty()) {
            return lengths;
        }
        if (used.size() == 1) {
            lengths[static_cast<std::size_t>(used[0])] = 1;
            return lengths;
        }
        struct Node {
            std::uint64_t weight;
            int left;
            int right;
        };
        std::vector<Node> nodes;
        using Item = std::pair<std::uint64_t, int>;
        std::priority_queue<Item, std::vector<Item>, std::greater<>> heap;
        for (int s : used) {
            nodes.push_back({freq[static_cast<std::size_t>(s)], -1 - s, 0});
            heap.emplace(nodes.back().weight, static_cast<int>(nodes.size() - 1));
        }
        while (heap.size() > 1) {
            auto [wa, a] = heap.top();
            heap.pop();
            auto [wb, b] = heap.top();
            heap.pop();
            nodes.push_back({wa + wb, a, b});
            heap.emplace(wa + wb, static_cast<int>(nodes.size() - 1));
        }
        int maxLen = 0;
        std::vector<std::pair<int, int>> stack{{heap.top().second, 0}};
        while (!stack.empty()) {
            auto [idx, depth] = stack.back();
            stack.pop_back();
            const Node& nd = nodes[static_cast<std::size_t>(idx)];
            if (nd.left < 0) {
                lengths[static_cast<std::size_t>(-1 - nd.left)] = static_cast<std::uint8_t>(depth);
                maxLen = std::max(maxLen, depth);
                continue;
            }
            stack.emplace_back(nd.left, depth + 1);
            stack.emplace_back(nd.right, depth + 1);
        }
        if (maxLen <= limit) {
            return lengths;
        }
        for (std::uint32_t& f : freq) {
            if (f > 0) {
                f = (f + 1) / 2;
            }
        }
    }
}

std::vector<std::uint32_t> canonicalCodes(const std::vector<std::uint8_t>& lengths) {
    std::array<std::uint32_t, 16> count{};
    for (std::uint8_t l : lengths) {
        ++count[l];
    }
    count[0] = 0;
    std::array<std::uint32_t, 16> next{};
    std::uint32_t code = 0;
    for (int bits = 1; bits < 16; ++bits) {
        code = (code + count[static_cast<std::size_t>(bits - 1)]) << 1;
        next[static_cast<std::size_t>(bits)] = code;
    }
    std::vector<std::uint32_t> codes(lengths.size(), 0);
    for (std::size_t i = 0; i < lengths.size(); ++i) {
        if (lengths[i] != 0) {
            codes[i] = next[lengths[i]]++;
        }
    }
    return codes;
}

void writeBlock(BitWriter& w, const std::vector<Symbol>& syms, bool last) {
    std::vector<std::uint32_t> litFreq(286, 0);
    std::vector<std::uint32_t> distFreq(30, 0);
    for (const Symbol& s : syms) {
        ++litFreq[s.litlen];
        if (s.dist != 0) {
            int eb = 0;
            int ex = 0;
            ++distFreq[static_cast<std::size_t>(distCode(s.dist, eb, ex))];
        }
    }
    litFreq[256] = 1;
    if (std::all_of(distFreq.begin(), distFreq.end(), [](std::uint32_t f) { return f == 0; })) {
        distFreq[0] = 1;
    }
    std::vector<std::uint8_t> litLen = codeLengths(litFreq, 15);
    std::vector<std::uint8_t> distLen = codeLengths(distFreq, 15);
    std::size_t nlit = 286;
    while (nlit > 257 && litLen[nlit - 1] == 0) {
        --nlit;
    }
    std::size_t ndist = 30;
    while (ndist > 1 && distLen[ndist - 1] == 0) {
        --ndist;
    }
    std::vector<std::uint8_t> all(litLen.begin(), litLen.begin() + static_cast<std::ptrdiff_t>(nlit));
    all.insert(all.end(), distLen.begin(), distLen.begin() + static_cast<std::ptrdiff_t>(ndist));
    // Run-length encode the code lengths with symbols 16 (repeat), 17 and 18 (zeros).
    struct Rle {
        std::uint8_t sym;
        std::uint8_t extra;
    };
    std::vector<Rle> rle;
    for (std::size_t i = 0; i < all.size();) {
        std::size_t run = 1;
        while (i + run < all.size() && all[i + run] == all[i]) {
            ++run;
        }
        if (all[i] == 0 && run >= 3) {
            std::size_t take = std::min<std::size_t>(run, 138);
            rle.push_back(take >= 11 ? Rle{18, static_cast<std::uint8_t>(take - 11)} : Rle{17, static_cast<std::uint8_t>(take - 3)});
            i += take;
        } else if (all[i] != 0 && run >= 4) {
            rle.push_back({all[i], 0});
            std::size_t take = std::min<std::size_t>(run - 1, 6);
            rle.push_back({16, static_cast<std::uint8_t>(take - 3)});
            i += 1 + take;
        } else {
            rle.push_back({all[i], 0});
            ++i;
        }
    }
    std::vector<std::uint32_t> clFreq(19, 0);
    for (const Rle& r : rle) {
        ++clFreq[r.sym];
    }
    std::vector<std::uint8_t> clLen = codeLengths(clFreq, 7);
    static constexpr std::array<std::uint8_t, 19> kOrder = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    std::size_t ncode = 19;
    while (ncode > 4 && clLen[kOrder[ncode - 1]] == 0) {
        --ncode;
    }
    std::vector<std::uint32_t> litCodes = canonicalCodes(litLen);
    std::vector<std::uint32_t> distCodes = canonicalCodes(distLen);
    std::vector<std::uint32_t> clCodes = canonicalCodes(clLen);
    w.put(last ? 1 : 0, 1);
    w.put(2, 2);
    w.put(static_cast<std::uint32_t>(nlit - 257), 5);
    w.put(static_cast<std::uint32_t>(ndist - 1), 5);
    w.put(static_cast<std::uint32_t>(ncode - 4), 4);
    for (std::size_t i = 0; i < ncode; ++i) {
        w.put(clLen[kOrder[i]], 3);
    }
    for (const Rle& r : rle) {
        w.putCode(clCodes[r.sym], clLen[r.sym]);
        if (r.sym == 16) {
            w.put(r.extra, 2);
        } else if (r.sym == 17) {
            w.put(r.extra, 3);
        } else if (r.sym == 18) {
            w.put(r.extra, 7);
        }
    }
    for (const Symbol& s : syms) {
        w.putCode(litCodes[s.litlen], litLen[s.litlen]);
        if (s.dist == 0) {
            continue;
        }
        int eb = 0;
        int ex = 0;
        lengthCode(s.len, eb, ex);
        w.put(static_cast<std::uint32_t>(ex), eb);
        int dc = distCode(s.dist, eb, ex);
        w.putCode(distCodes[static_cast<std::size_t>(dc)], distLen[static_cast<std::size_t>(dc)]);
        w.put(static_cast<std::uint32_t>(ex), eb);
    }
    w.putCode(litCodes[256], litLen[256]);
}

} // namespace

std::vector<std::uint8_t> deflate(std::span<const std::uint8_t> in, int level) {
    BitWriter w;
    if (level <= 0) {
        std::size_t pos = 0;
        do {
            std::size_t len = std::min<std::size_t>(in.size() - pos, 65535);
            bool last = pos + len == in.size();
            w.put(last ? 1 : 0, 1);
            w.put(0, 2);
            w.align();
            w.put(static_cast<std::uint32_t>(len), 16);
            w.put(static_cast<std::uint32_t>(len ^ 0xFFFF), 16);
            w.out.insert(w.out.end(), in.begin() + static_cast<std::ptrdiff_t>(pos),
                         in.begin() + static_cast<std::ptrdiff_t>(pos + len));
            pos += len;
        } while (pos < in.size());
        return std::move(w.out);
    }
    std::size_t maxChain = std::size_t{4} << std::min(level, 9);
    std::vector<std::int32_t> head(std::size_t{1} << kHashBits, -1);
    std::vector<std::int32_t> prev(kWindow, -1);
    auto hash = [&](std::size_t i) {
        std::uint32_t v = in[i] | (in[i + 1] << 8) | (in[i + 2] << 16);
        return (v * 2654435761U) >> (32 - kHashBits);
    };
    auto insert = [&](std::size_t i) {
        if (i + 2 < in.size()) {
            std::uint32_t h = hash(i);
            prev[i % kWindow] = head[h];
            head[h] = static_cast<std::int32_t>(i);
        }
    };
    std::vector<Symbol> block;
    block.reserve(kBlockSymbols);
    std::size_t i = 0;
    while (i < in.size()) {
        std::size_t bestLen = 0;
        std::size_t bestDist = 0;
        if (i + 2 < in.size()) {
            std::int32_t cand = head[hash(i)];
            std::size_t chain = maxChain;
            std::size_t limit = std::min(kMaxMatch, in.size() - i);
            while (cand >= 0 && chain-- > 0 && i - static_cast<std::size_t>(cand) <= kWindow - 1) {
                auto c = static_cast<std::size_t>(cand);
                if (in[c + bestLen] == in[i + bestLen]) {
                    std::size_t len = 0;
                    while (len < limit && in[c + len] == in[i + len]) {
                        ++len;
                    }
                    if (len > bestLen) {
                        bestLen = len;
                        bestDist = i - c;
                        if (len == limit) {
                            break;
                        }
                    }
                }
                std::int32_t next = prev[c % kWindow];
                if (next >= cand) {
                    break;
                }
                cand = next;
            }
        }
        if (bestLen >= 3) {
            int eb = 0;
            int ex = 0;
            block.push_back({static_cast<std::uint16_t>(lengthCode(bestLen, eb, ex)), static_cast<std::uint16_t>(bestDist),
                             static_cast<std::uint16_t>(bestLen)});
            for (std::size_t k = 0; k < bestLen; ++k) {
                insert(i + k);
            }
            i += bestLen;
        } else {
            block.push_back({in[i], 0, 0});
            insert(i);
            ++i;
        }
        if (block.size() >= kBlockSymbols) {
            writeBlock(w, block, i >= in.size());
            block.clear();
        }
    }
    if (!block.empty() || in.empty()) {
        writeBlock(w, block, true);
    }
    w.align();
    return std::move(w.out);
}

} // namespace quartz::stdlib
