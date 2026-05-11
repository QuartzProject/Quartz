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
    std::uint8_t level; // arena region (`scope` nesting) it lives in
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
    std::uint16_t level;
    bool packed; // `[bool]`: one byte per element (D38)

    [[nodiscard]] std::size_t stride() const { return packed ? 1 : elemSlots * sizeof(Slot); }
    [[nodiscard]] std::uint8_t* bytes() const { return reinterpret_cast<std::uint8_t*>(data); }
};

inline constexpr std::uint8_t kMapKeyRaw = 0;    // one slot compared bit for bit
inline constexpr std::uint8_t kMapKeyStr = 1;    // a `str`, compared by content
inline constexpr std::uint8_t kMapKeyFields = 2; // a struct, tuple or enum, hashed and compared field by field

/// Open addressing over buckets; `order` keeps the buckets in insertion order, which is the iteration order.
struct Map {
    std::uint64_t* hashes; // per bucket: 0 empty, 1 tombstone, otherwise hash | 2
    Slot* entries;         // per bucket: key slots then value slots
    std::uint32_t* order;  // bucket + 1 of each inserted entry in insertion order, 0 once removed
    std::uint32_t* place;  // per bucket: its position in `order` (allocated with it)
    std::uint32_t count;
    std::uint32_t used;    // live + tombstones
    std::uint32_t cap;     // power of two
    std::uint32_t ordered; // positions of `order` in use
    std::uint32_t keyType;
    std::uint32_t valueType;
    std::uint32_t version; // changes when a key is added or removed: a `for` over the map checks it
    std::uint16_t valueSlots;
    std::uint16_t keySlots;
    std::uint8_t keyKind;
    bool keyHasRefs;
    bool valueHasRefs;
    bool valuePtrNullable; // Map.get returns a single pointer slot instead of flag plus payload
    Owner owner;
    std::uint16_t level;
};

/// Calls `f` with each live entry (key slots then value slots) in insertion order.
template <class F>
void forEachEntry(const Map* m, F&& f) {
    std::size_t es = static_cast<std::size_t>(m->keySlots) + m->valueSlots;
    for (std::uint32_t p = 0; p < m->ordered; ++p) {
        if (std::uint32_t b = m->order[p]; b != 0) {
            f(m->entries + (b - 1) * es);
        }
    }
}

/// A function value: the function and the values it captured, which fill its last parameters.
struct Closure {
    std::uint32_t fn;
    std::uint16_t captures; // captured parameters
    std::uint16_t slots;    // slots they span
    Owner owner;
    std::uint8_t level;
    std::uint8_t pad[6];
    Slot caps[1];
};

inline constexpr std::size_t kClosureHeader = offsetof(Closure, caps);
static_assert(kClosureHeader == 16);

struct HashKey {
    std::uint64_t k0;
    std::uint64_t k1;
};

/// Secret per process (D56): map hashes cannot be predicted, so keys cannot be chosen to collide.
const HashKey& hashKey();

/// SipHash-1-3 of `s` under the process key, folded to 32 bits (never 0, which marks "not computed").
std::uint32_t hashBytes(std::string_view s);

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
