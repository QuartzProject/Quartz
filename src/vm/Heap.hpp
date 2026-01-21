#pragma once

#include "vm/Value.hpp"

#include <cstddef>
#include <memory>
#include <vector>

namespace quartz::vm {

/// Memory accounting shared by the arena and the state heap of one sandbox.
struct Quota {
    std::size_t limit = 64u << 20;
    std::size_t used = 0;

    bool take(std::size_t n) {
        if (n > limit - used) {
            return false;
        }
        used += n;
        return true;
    }

    void give(std::size_t n) { used -= n < used ? n : used; }
};

/// Per-invocation bump allocator; everything is released at once by reset().
class InvocationArena {
public:
    explicit InvocationArena(Quota& quota) : quota_(quota) {}

    InvocationArena(const InvocationArena&) = delete;
    InvocationArena& operator=(const InvocationArena&) = delete;
    ~InvocationArena() { releaseAll(); }

    void* allocate(std::size_t size) {
        size = (size + 7) & ~std::size_t{7};
        if (static_cast<std::size_t>(end_ - cursor_) < size) {
            if (!grow(size)) {
                return nullptr;
            }
        }
        void* p = cursor_;
        cursor_ += size;
        return p;
    }

    /// Keeps the first chunk for reuse; returns the rest to the system.
    void reset() {
        if (chunks_.size() == 1 && chunks_[0].size == kChunk) {
            cursor_ = chunks_[0].base;
            end_ = cursor_ + kChunk;
            return;
        }
        if (!chunks_.empty()) {
            resetSlow();
        }
    }

private:
    bool grow(std::size_t atLeast);
    void releaseAll();
    void resetSlow();

    struct Chunk {
        std::byte* base;
        std::size_t size;
    };

    static constexpr std::size_t kChunk = 64 * 1024;
    Quota& quota_;
    std::vector<Chunk> chunks_;
    std::byte* cursor_ = nullptr;
    std::byte* end_ = nullptr;
};

/// Heap for `state` values; frees are deferred to the end of the invocation so views stay valid (D27).
/// Blocks up to kMaxSmall bytes come from per-size-class free lists over slabs (D47).
class StateHeap {
public:
    explicit StateHeap(Quota& quota) : quota_(quota) {}

    StateHeap(const StateHeap&) = delete;
    StateHeap& operator=(const StateHeap&) = delete;
    ~StateHeap();

    void* allocate(std::size_t size);
    void release(void* p);
    void flush() {
        if (!deferred_.empty()) {
            flushSlow();
        }
    }

private:
    static constexpr std::size_t kGranule = 16;
#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define QUARTZ_HEAP_ASAN 1
#endif
#endif
#ifdef QUARTZ_HEAP_ASAN
    static constexpr std::size_t kMaxSmall = 0; // every block from malloc so ASan sees use after free
#else
    static constexpr std::size_t kMaxSmall = 512;
#endif
    static constexpr std::size_t kClasses = kMaxSmall / kGranule + 1;
    static constexpr std::size_t kSlab = 64 * 1024;

    void* allocateSmall(std::size_t cls);
    void flushSlow();

    Quota& quota_;
    std::vector<void*> deferred_;
    void* free_[kClasses] = {};
    std::vector<std::byte*> slabs_;
    std::byte* slabCursor_ = nullptr;
    std::byte* slabEnd_ = nullptr;
};

} // namespace quartz::vm
