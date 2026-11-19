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
    std::uint32_t count;
    std::uint32_t cap; // power of two
    std::uint16_t keySlots;
    std::uint16_t valueSlots;
    std::uint8_t keyKind;
    bool keyHasRefs;
    bool valueHasRefs;
    bool valuePtrNullable; // Map.get returns a single pointer slot instead of flag plus payload
    std::uint32_t used;    // live + tombstones
    std::uint32_t keyType;
    std::uint32_t valueType;
    Owner owner;
    std::uint16_t level;
    std::uint32_t* order;  // bucket + 1 of each inserted entry in insertion order, 0 once removed
    std::uint32_t* place;  // per bucket: its position in `order` (allocated with it)
    std::uint32_t ordered; // positions of `order` in use
    std::uint32_t version; // changes when a key is added or removed: a `for` over the map checks it
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

// Up to 16 bytes compared inline with two overlapping loads, which stay inside both ranges; longer ones go to memcmp.
inline bool bytesEq(const char* a, const char* b, std::size_t n) {
    if (n >= 8 && n <= 16) {
        std::uint64_t a0 = 0;
        std::uint64_t b0 = 0;
        std::uint64_t a1 = 0;
        std::uint64_t b1 = 0;
        std::memcpy(&a0, a, 8);
        std::memcpy(&b0, b, 8);
        std::memcpy(&a1, a + n - 8, 8);
        std::memcpy(&b1, b + n - 8, 8);
        return ((a0 ^ b0) | (a1 ^ b1)) == 0;
    }
    if (n >= 4 && n < 8) {
        std::uint32_t a0 = 0;
        std::uint32_t b0 = 0;
        std::uint32_t a1 = 0;
        std::uint32_t b1 = 0;
        std::memcpy(&a0, a, 4);
        std::memcpy(&b0, b, 4);
        std::memcpy(&a1, a + n - 4, 4);
        std::memcpy(&b1, b + n - 4, 4);
        return ((a0 ^ b0) | (a1 ^ b1)) == 0;
    }
    if (n < 4) {
        return n == 0 || (a[0] == b[0] && a[n / 2] == b[n / 2] && a[n - 1] == b[n - 1]);
    }
    return std::memcmp(a, b, n) == 0;
}

// Short copies inline with the same overlapping loads; the libc call costs more than the copy up to 16 bytes.
inline void copyBytes(char* dst, const char* src, std::size_t n) {
    if (n >= 8 && n <= 16) {
        std::uint64_t x = 0;
        std::uint64_t y = 0;
        std::memcpy(&x, src, 8);
        std::memcpy(&y, src + n - 8, 8);
        std::memcpy(dst, &x, 8);
        std::memcpy(dst + n - 8, &y, 8);
    } else if (n >= 4 && n < 8) {
        std::uint32_t x = 0;
        std::uint32_t y = 0;
        std::memcpy(&x, src, 4);
        std::memcpy(&y, src + n - 4, 4);
        std::memcpy(dst, &x, 4);
        std::memcpy(dst + n - 4, &y, 4);
    } else if (n < 4) {
        if (n != 0) {
            char a = src[0];
            char b = src[n / 2];
            char c = src[n - 1];
            dst[0] = a;
            dst[n / 2] = b;
            dst[n - 1] = c;
        }
    } else {
        std::memcpy(dst, src, n);
    }
}

inline bool strEq(const Str* a, const Str* b) {
    return a == b || (a->len == b->len && bytesEq(a->data, b->data, a->len));
}

} // namespace quartz::vm
