#include "vm/Heap.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string_view>

namespace quartz::vm {

const HashKey& hashKey() {
    static const HashKey kKey = [] {
        std::random_device rd;
        auto word = [&] { return (static_cast<std::uint64_t>(rd()) << 32) ^ rd(); };
        return HashKey{word(), word()};
    }();
    return kKey;
}

namespace {

inline std::uint64_t rotl(std::uint64_t x, int b) { return (x << b) | (x >> (64 - b)); }

inline void sipRound(std::uint64_t& v0, std::uint64_t& v1, std::uint64_t& v2, std::uint64_t& v3) {
    v0 += v1;
    v1 = rotl(v1, 13);
    v1 ^= v0;
    v0 = rotl(v0, 32);
    v2 += v3;
    v3 = rotl(v3, 16);
    v3 ^= v2;
    v0 += v3;
    v3 = rotl(v3, 21);
    v3 ^= v0;
    v2 += v1;
    v1 = rotl(v1, 17);
    v1 ^= v2;
    v2 = rotl(v2, 32);
}

} // namespace

std::uint32_t hashBytes(std::string_view s) {
    const HashKey& k = hashKey();
    std::uint64_t v0 = k.k0 ^ 0x736f6d6570736575ULL;
    std::uint64_t v1 = k.k1 ^ 0x646f72616e646f6dULL;
    std::uint64_t v2 = k.k0 ^ 0x6c7967656e657261ULL;
    std::uint64_t v3 = k.k1 ^ 0x7465646279746573ULL;
    std::size_t n = s.size();
    std::size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        std::uint64_t m = 0;
        std::memcpy(&m, s.data() + i, 8);
        v3 ^= m;
        sipRound(v0, v1, v2, v3);
        v0 ^= m;
    }
    std::uint64_t last = static_cast<std::uint64_t>(n) << 56;
    for (std::size_t j = 0; i + j < n; ++j) {
        last |= static_cast<std::uint64_t>(static_cast<unsigned char>(s[i + j])) << (8 * j);
    }
    v3 ^= last;
    sipRound(v0, v1, v2, v3);
    v0 ^= last;
    v2 ^= 0xFF;
    sipRound(v0, v1, v2, v3);
    sipRound(v0, v1, v2, v3);
    sipRound(v0, v1, v2, v3);
    std::uint64_t h = v0 ^ v1 ^ v2 ^ v3;
    auto r = static_cast<std::uint32_t>(h ^ (h >> 32));
    return r == 0 ? 1 : r;
}

bool InvocationArena::grow(std::size_t atLeast) {
    std::size_t size = std::max(atLeast, kChunk);
    if (!quota_.take(size)) {
        return false;
    }
    auto* base = static_cast<std::byte*>(std::malloc(size)); // NOLINT(cppcoreguidelines-no-malloc)
    if (base == nullptr) {
        quota_.give(size);
        return false;
    }
    chunks_.push_back({base, size});
    cursor_ = base;
    end_ = base + size;
    return true;
}

void InvocationArena::resetSlow() {
    for (std::size_t i = 1; i < chunks_.size(); ++i) {
        quota_.give(chunks_[i].size);
        std::free(chunks_[i].base); // NOLINT(cppcoreguidelines-no-malloc)
    }
    chunks_.resize(1);
    if (chunks_[0].size != kChunk) {
        quota_.give(chunks_[0].size);
        std::free(chunks_[0].base); // NOLINT(cppcoreguidelines-no-malloc)
        chunks_.clear();
        cursor_ = end_ = nullptr;
        return;
    }
    cursor_ = chunks_[0].base;
    end_ = cursor_ + chunks_[0].size;
}

void InvocationArena::releaseAll() {
    for (Chunk& c : chunks_) {
        quota_.give(c.size);
        std::free(c.base); // NOLINT(cppcoreguidelines-no-malloc)
    }
    chunks_.clear();
    cursor_ = end_ = nullptr;
}

namespace {

constexpr std::size_t kHeader = 16;
constexpr std::size_t kLarge = ~std::size_t{0};

} // namespace

// Block header: the size class, or kLarge followed by the byte count charged to the quota.
void* StateHeap::allocateSmall(std::size_t cls) {
    if (free_[cls] != nullptr) {
        void* p = free_[cls];
        free_[cls] = *static_cast<void**>(p);
        return p;
    }
    std::size_t bytes = (cls + 1) * kGranule + kHeader;
    if (slabCursor_ == nullptr || static_cast<std::size_t>(slabEnd_ - slabCursor_) < bytes) {
        if (!quota_.take(kSlab)) {
            return nullptr;
        }
        auto* slab = static_cast<std::byte*>(std::malloc(kSlab)); // NOLINT(cppcoreguidelines-no-malloc)
        if (slab == nullptr) {
            quota_.give(kSlab);
            return nullptr;
        }
        slabs_.push_back(slab);
        slabCursor_ = slab;
        slabEnd_ = slab + kSlab;
    }
    std::byte* base = slabCursor_;
    slabCursor_ += bytes;
    *reinterpret_cast<std::size_t*>(base) = cls;
    return base + kHeader;
}

void* StateHeap::allocate(std::size_t size) {
    if (size <= kMaxSmall && kMaxSmall > 0) {
        return allocateSmall(size == 0 ? 0 : (size - 1) / kGranule);
    }
    std::size_t total = size + kHeader;
    if (!quota_.take(total)) {
        return nullptr;
    }
    auto* base = static_cast<std::byte*>(std::malloc(total)); // NOLINT(cppcoreguidelines-no-malloc)
    if (base == nullptr) {
        quota_.give(total);
        return nullptr;
    }
    reinterpret_cast<std::size_t*>(base)[0] = kLarge;
    reinterpret_cast<std::size_t*>(base)[1] = total;
    return base + kHeader;
}

void StateHeap::release(void* p) {
    if (p != nullptr) {
        deferred_.push_back(p);
    }
}

void StateHeap::flushSlow() {
    for (void* p : deferred_) {
        auto* base = static_cast<std::byte*>(p) - kHeader;
        std::size_t cls = reinterpret_cast<std::size_t*>(base)[0];
        if (cls != kLarge) {
            *static_cast<void**>(p) = free_[cls];
            free_[cls] = p;
            continue;
        }
        quota_.give(reinterpret_cast<std::size_t*>(base)[1]);
        std::free(base); // NOLINT(cppcoreguidelines-no-malloc)
    }
    deferred_.clear();
}

StateHeap::~StateHeap() {
    flush();
    for (std::byte* slab : slabs_) {
        quota_.give(kSlab);
        std::free(slab); // NOLINT(cppcoreguidelines-no-malloc)
    }
}

} // namespace quartz::vm
