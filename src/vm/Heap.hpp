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
    void reset();

private:
    bool grow(std::size_t atLeast);
    void releaseAll();

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
class StateHeap {
public:
    explicit StateHeap(Quota& quota) : quota_(quota) {}

    StateHeap(const StateHeap&) = delete;
    StateHeap& operator=(const StateHeap&) = delete;
    ~StateHeap();

    void* allocate(std::size_t size);
    void release(void* p);
    void flush();

private:
    Quota& quota_;
    std::vector<void*> deferred_;
};

} // namespace quartz::vm
