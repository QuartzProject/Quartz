#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace quartz::vm {

/// One interpreter register or field slot.
union Slot {
    std::int64_t i;
    std::uint64_t u;
    double f;
    float f32;
    void* p;
};
static_assert(sizeof(Slot) == 8);

enum class Owner : std::uint8_t { Arena, State, Static };

/// Immutable UTF-8 string; `hash` is computed lazily (0 means not yet).
struct Str {
    std::uint32_t len;
    std::uint32_t hash;
    Owner owner;
    char data[1];

    [[nodiscard]] std::string_view view() const { return {data, len}; }
};

inline constexpr std::size_t kStrHeader = offsetof(Str, data);

struct List {
    Slot* data;
    std::uint32_t len;
    std::uint32_t cap;
    std::uint32_t elemType; // ir::TypeId of the element
    std::uint16_t elemSlots;
    bool elemHasRefs;
    Owner owner;
};

struct Map {
    std::uint64_t* hashes; // 0 empty, 1 tombstone, otherwise hash | 2
    Slot* entries;         // per entry: key slot then value slots
    std::uint32_t count;
    std::uint32_t used;    // live + tombstones
    std::uint32_t cap;     // power of two
    std::uint32_t keyType;
    std::uint32_t valueType;
    std::uint16_t valueSlots;
    bool strKeys;
    bool valueHasRefs;
    bool valuePtrNullable; // Map.get returns a single pointer slot instead of flag plus payload
    Owner owner;
};

inline std::uint32_t hashBytes(std::string_view s) {
    std::uint64_t h = 0xcbf29ce484222325ULL;
    for (char c : s) {
        h ^= static_cast<unsigned char>(c);
        h *= 0x100000001b3ULL;
    }
    auto r = static_cast<std::uint32_t>(h ^ (h >> 32));
    return r == 0 ? 1 : r;
}

inline std::uint32_t strHash(Str* s) {
    if (s->hash == 0) {
        s->hash = hashBytes(s->view());
    }
    return s->hash;
}

inline bool strEq(const Str* a, const Str* b) {
    return a == b || (a->len == b->len && std::memcmp(a->data, b->data, a->len) == 0);
}

} // namespace quartz::vm
