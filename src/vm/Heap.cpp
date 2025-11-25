#include "vm/Heap.hpp"

#include <algorithm>
#include <cstdlib>

namespace quartz::vm {

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

void InvocationArena::reset() {
    if (chunks_.empty()) {
        return;
    }
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

} // namespace

void* StateHeap::allocate(std::size_t size) {
    std::size_t total = size + kHeader;
    if (!quota_.take(total)) {
        return nullptr;
    }
    auto* base = static_cast<std::byte*>(std::malloc(total)); // NOLINT(cppcoreguidelines-no-malloc)
    if (base == nullptr) {
        quota_.give(total);
        return nullptr;
    }
    *reinterpret_cast<std::size_t*>(base) = total;
    return base + kHeader;
}

void StateHeap::release(void* p) {
    if (p != nullptr) {
        deferred_.push_back(p);
    }
}

void StateHeap::flush() {
    for (void* p : deferred_) {
        auto* base = static_cast<std::byte*>(p) - kHeader;
        quota_.give(*reinterpret_cast<std::size_t*>(base));
        std::free(base); // NOLINT(cppcoreguidelines-no-malloc)
    }
    deferred_.clear();
}

StateHeap::~StateHeap() { flush(); }

} // namespace quartz::vm
