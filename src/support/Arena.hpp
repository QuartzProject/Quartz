#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

namespace quartz {

/// Chunked bump allocator for objects that die together; only trivially destructible types.
class Arena {
public:
    explicit Arena(std::size_t chunkSize = 64 * 1024) : chunkSize_(chunkSize) {}

    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;
    Arena(Arena&&) noexcept = default;
    Arena& operator=(Arena&&) noexcept = default;
    ~Arena() = default;

    void* allocate(std::size_t size, std::size_t align) {
        auto p = reinterpret_cast<std::uintptr_t>(cursor_);
        std::uintptr_t aligned = (p + align - 1) & ~(static_cast<std::uintptr_t>(align) - 1);
        if (cursor_ == nullptr || aligned + size > reinterpret_cast<std::uintptr_t>(end_)) {
            grow(size + align);
            p = reinterpret_cast<std::uintptr_t>(cursor_);
            aligned = (p + align - 1) & ~(static_cast<std::uintptr_t>(align) - 1);
        }
        cursor_ = reinterpret_cast<std::byte*>(aligned + size);
        bytesUsed_ += size;
        return reinterpret_cast<void*>(aligned);
    }

    template <class T, class... Args>
    T* make(Args&&... args) {
        static_assert(std::is_trivially_destructible_v<T>, "arena objects are never destroyed");
        return ::new (allocate(sizeof(T), alignof(T))) T{std::forward<Args>(args)...};
    }

    template <class T>
    std::span<T> copy(std::span<const T> items) {
        static_assert(std::is_trivially_copyable_v<T>);
        if (items.empty()) {
            return {};
        }
        auto* out = static_cast<T*>(allocate(sizeof(T) * items.size(), alignof(T)));
        std::memcpy(out, items.data(), sizeof(T) * items.size());
        return {out, items.size()};
    }

    template <class T>
    std::span<T> copy(const std::vector<T>& items) {
        return copy(std::span<const T>(items));
    }

    std::string_view copy(std::string_view text) {
        if (text.empty()) {
            return {};
        }
        auto* out = static_cast<char*>(allocate(text.size(), 1));
        std::memcpy(out, text.data(), text.size());
        return {out, text.size()};
    }

    [[nodiscard]] std::size_t bytesUsed() const { return bytesUsed_; }

private:
    void grow(std::size_t atLeast) {
        std::size_t size = atLeast > chunkSize_ ? atLeast : chunkSize_;
        chunks_.push_back(std::make_unique<std::byte[]>(size));
        cursor_ = chunks_.back().get();
        end_ = cursor_ + size;
    }

    std::size_t chunkSize_;
    std::vector<std::unique_ptr<std::byte[]>> chunks_;
    std::byte* cursor_ = nullptr;
    std::byte* end_ = nullptr;
    std::size_t bytesUsed_ = 0;
};

} // namespace quartz
