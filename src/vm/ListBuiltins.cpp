#include "vm/Builtins.hpp"
#include "vm/Sandbox.hpp"

#include <algorithm>
#include <bit>
#include <format>

namespace quartz::vm {

namespace {

List* listOf(const Slot& s) { return static_cast<List*>(s.p); }

std::uint8_t* at(const List* l, std::uint64_t i) { return l->bytes() + i * l->stride(); }

bool outOfRange(Sandbox& sb, std::string_view what, std::int64_t i, std::uint32_t len) {
    return sb.fail(ir::TrapCode::IndexOutOfBounds, std::format("{} {} of a list of {}", what, i, len));
}

// Moves the last element of `l` to position `i`, shifting the ones after `i` up.
void moveLastTo(List* l, std::uint32_t i) {
    std::size_t stride = l->stride();
    std::uint8_t* first = at(l, i);
    std::uint8_t* last = at(l, l->len - 1);
    constexpr std::size_t kSmall = 64;
    if (stride <= kSmall) {
        std::uint8_t tmp[kSmall];
        std::memcpy(tmp, last, stride);
        std::memmove(first + stride, first, static_cast<std::size_t>(last - first));
        std::memcpy(first, tmp, stride);
    } else {
        std::rotate(first, last, last + stride);
    }
}

// IEEE totalOrder on the bits, except that every NaN sorts last and equal to the others.
std::uint64_t floatKey(double d) {
    if (d != d) {
        return ~std::uint64_t{0};
    }
    auto b = std::bit_cast<std::uint64_t>(d);
    return (b >> 63) != 0 ? ~b : b | 0x8000000000000000ULL;
}

std::uint64_t floatKey32(float f) {
    if (f != f) {
        return ~std::uint64_t{0};
    }
    auto b = std::bit_cast<std::uint32_t>(f);
    return (b >> 31) != 0 ? ~b & 0xFFFFFFFFu : b | 0x80000000u;
}

int compareStr(const Str* a, const Str* b) {
    std::uint32_t n = std::min(a->len, b->len);
    int c = std::memcmp(a->data, b->data, n);
    if (c != 0) {
        return c;
    }
    return a->len < b->len ? -1 : a->len > b->len ? 1 : 0;
}

std::uint64_t log2Ceil(std::uint64_t n) { return n < 2 ? 1 : static_cast<std::uint64_t>(std::bit_width(n - 1)); }

} // namespace

// Charged for the bytes moved; the value is pushed first, so state copies and scope promotion follow push().
bool Builtins::insert(Sandbox& sb, const Slot* a, Slot*) {
    List* l = listOf(a[0]);
    std::int64_t i = a[1].i;
    if (i < 0 || i > l->len) {
        return outOfRange(sb, "insert at", i, l->len);
    }
    if (!chargeBytes(sb, (l->len - static_cast<std::uint64_t>(i)) * l->stride()) || !sb.listPush(l, a + 2)) {
        return false;
    }
    moveLastTo(l, static_cast<std::uint32_t>(i));
    return true;
}

bool Builtins::removeAt(Sandbox& sb, const Slot* a, Slot* out) {
    List* l = listOf(a[0]);
    std::int64_t i = a[1].i;
    if (i < 0 || i >= l->len) {
        return outOfRange(sb, "remove at", i, l->len);
    }
    auto index = static_cast<std::uint32_t>(i);
    std::size_t stride = l->stride();
    if (!chargeBytes(sb, (l->len - index) * stride)) {
        return false;
    }
    std::uint8_t* p = at(l, index);
    if (l->packed) {
        out[0].u = *p;
    } else {
        std::memcpy(out, p, stride);
        if (l->owner == Owner::State && l->elemHasRefs) {
            sb.releaseState(l->elemType, reinterpret_cast<Slot*>(p));
        }
    }
    std::memmove(p, p + stride, (l->len - index - 1) * stride);
    --l->len;
    return true;
}

namespace {

// Index of the first element equal to `v` under `==`, or -1; `bytes` collects what string comparisons read.
std::int64_t find(const List* l, const Slot* v, std::uint64_t& bytes) {
    if (l->packed) {
        const void* hit = std::memchr(l->bytes(), static_cast<int>(v->u & 1), l->len);
        return hit == nullptr ? -1 : static_cast<const std::uint8_t*>(hit) - l->bytes();
    }
    const Slot* d = l->data;
    if (l->elemType == ir::kF64) {
        for (std::uint32_t i = 0; i < l->len; ++i) {
            if (d[i].f == v->f) {
                return i;
            }
        }
        return -1;
    }
    if (l->elemType == ir::kF32) {
        for (std::uint32_t i = 0; i < l->len; ++i) {
            if (d[i].f32 == v->f32) {
                return i;
            }
        }
        return -1;
    }
    if (l->elemType == ir::kStr) {
        const auto* s = static_cast<const Str*>(v->p);
        for (std::uint32_t i = 0; i < l->len; ++i) {
            const auto* e = static_cast<const Str*>(d[i].p);
            if (e->len == s->len) {
                bytes += s->len;
                if (strEq(e, s)) {
                    return i;
                }
            }
        }
        return -1;
    }
    for (std::uint32_t i = 0; i < l->len; ++i) {
        if (d[i].u == v->u) {
            return i;
        }
    }
    return -1;
}

} // namespace

bool Builtins::contains(Sandbox& sb, const Slot* a, Slot* out) {
    const List* l = listOf(a[0]);
    std::uint64_t bytes = 0;
    std::int64_t i = find(l, a + 1, bytes);
    out[0].u = i >= 0 ? 1 : 0;
    std::uint64_t scanned = (i >= 0 ? static_cast<std::uint64_t>(i) + 1 : l->len) * l->stride();
    return chargeBytes(sb, scanned + bytes);
}

bool Builtins::indexOf(Sandbox& sb, const Slot* a, Slot* out) {
    const List* l = listOf(a[0]);
    std::uint64_t bytes = 0;
    std::int64_t i = find(l, a + 1, bytes);
    out[0].u = i >= 0 ? 1 : 0;
    out[1].i = i >= 0 ? i : 0;
    std::uint64_t scanned = (i >= 0 ? static_cast<std::uint64_t>(i) + 1 : l->len) * l->stride();
    return chargeBytes(sb, scanned + bytes);
}

bool Builtins::reverse(Sandbox& sb, const Slot* a, Slot*) {
    List* l = listOf(a[0]);
    if (!chargeBytes(sb, static_cast<std::uint64_t>(l->len) * l->stride())) {
        return false;
    }
    if (l->packed) {
        std::reverse(l->bytes(), l->bytes() + l->len);
    } else if (l->elemSlots == 1) {
        std::reverse(l->data, l->data + l->len);
    } else {
        for (std::uint32_t i = 0, j = l->len; i + 1 < j; ++i, --j) {
            std::swap_ranges(l->data + std::size_t{i} * l->elemSlots, l->data + std::size_t{i + 1} * l->elemSlots,
                             l->data + std::size_t{j - 1} * l->elemSlots);
        }
    }
    return true;
}

// Negative bounds count from the end, as in Python; what remains must satisfy 0 <= begin <= end <= len.
bool Builtins::slice(Sandbox& sb, const Slot* a, Slot* out) {
    const List* l = listOf(a[0]);
    std::int64_t b = a[1].i;
    std::int64_t e = a[2].i;
    std::int64_t len = l->len;
    std::int64_t from = b < 0 ? b + len : b;
    std::int64_t to = e < 0 ? e + len : e;
    out[0].p = nullptr;
    if (from < 0 || to < from || to > len) {
        return sb.fail(ir::TrapCode::IndexOutOfBounds, std::format("slice [{}, {}) of a list of {}", b, e, len));
    }
    auto n = static_cast<std::uint32_t>(to - from);
    if (!chargeBytes(sb, std::uint64_t{n} * l->stride())) {
        return false;
    }
    List* r = sb.newList(l->elemType);
    if (r == nullptr || (n > 0 && !sb.growList(r, n))) {
        return false;
    }
    if (n > 0) {
        std::memcpy(r->data, at(l, static_cast<std::uint64_t>(from)), std::size_t{n} * l->stride());
    }
    r->len = n;
    out[0].p = r;
    return true;
}

// `xs.insertAll(i, xs)` works: the room is made first, so the elements read are the original ones.
bool Builtins::insertAll(Sandbox& sb, const Slot* a, Slot*) {
    List* l = listOf(a[0]);
    std::int64_t i = a[1].i;
    const List* other = listOf(a[2]);
    if (i < 0 || i > l->len) {
        return outOfRange(sb, "insert at", i, l->len);
    }
    std::uint32_t n = other->len;
    std::uint32_t len0 = l->len;
    if (n == 0) {
        return true;
    }
    if (std::uint64_t{len0} + n > 0x7FFFFFFFu) {
        return sb.fail(ir::TrapCode::OutOfMemory, "list too large");
    }
    std::uint64_t moved = (std::uint64_t{len0} - static_cast<std::uint64_t>(i) + n) * l->stride();
    if (!chargeBytes(sb, moved) || (l->cap < len0 + n && !sb.growList(l, len0 + n))) {
        return false;
    }
    for (std::uint32_t k = 0; k < n; ++k) {
        Slot one{};
        const Slot* v = &one;
        if (other->packed) {
            one.u = other->bytes()[k];
        } else {
            v = other->data + std::size_t{k} * other->elemSlots;
        }
        if (!sb.listPush(l, v)) {
            for (std::uint32_t j = len0; l->owner == Owner::State && l->elemHasRefs && j < l->len; ++j) {
                sb.releaseState(l->elemType, l->data + std::size_t{j} * l->elemSlots);
            }
            l->len = len0;
            return false;
        }
    }
    std::rotate(at(l, static_cast<std::uint64_t>(i)), at(l, len0), at(l, std::uint64_t{len0} + n));
    return true;
}

// Stable; floats in IEEE totalOrder (-0.0 before 0.0) with every NaN last; str by bytes.
bool Builtins::sort(Sandbox& sb, const Slot* a, Slot*) {
    List* l = listOf(a[0]);
    std::uint64_t n = l->len;
    if (n < 2) {
        return true;
    }
    std::uint64_t bytes = n * log2Ceil(n) * 8;
    if (l->elemType == ir::kStr) {
        for (std::uint64_t i = 0; i < n; ++i) {
            bytes += static_cast<const Str*>(l->data[i].p)->len * log2Ceil(n);
        }
    }
    if (!chargeBytes(sb, bytes)) {
        return false;
    }
    if (l->packed) {
        auto ones = static_cast<std::size_t>(std::count(l->bytes(), l->bytes() + n, std::uint8_t{1}));
        std::memset(l->bytes(), 0, n - ones);
        std::memset(l->bytes() + (n - ones), 1, ones);
        return true;
    }
    Slot* first = l->data;
    Slot* last = l->data + n;
    const ir::TypeTable& types = sb.program().module->types;
    ir::TypeId t = l->elemType;
    if (t == ir::kF64) {
        std::stable_sort(first, last, [](Slot x, Slot y) { return floatKey(x.f) < floatKey(y.f); });
    } else if (t == ir::kF32) {
        std::stable_sort(first, last, [](Slot x, Slot y) { return floatKey32(x.f32) < floatKey32(y.f32); });
    } else if (t == ir::kStr) {
        std::stable_sort(first, last, [](Slot x, Slot y) {
            return compareStr(static_cast<const Str*>(x.p), static_cast<const Str*>(y.p)) < 0;
        });
    } else if (types.isInteger(t) && types.isSigned(t)) {
        std::stable_sort(first, last, [](Slot x, Slot y) { return x.i < y.i; });
    } else {
        std::stable_sort(first, last, [](Slot x, Slot y) { return x.u < y.u; });
    }
    return true;
}

} // namespace quartz::vm
