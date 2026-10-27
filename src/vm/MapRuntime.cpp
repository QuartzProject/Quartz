#include "vm/Sandbox.hpp"

#include <cstring>

namespace quartz::vm {

using ir::TypeId;
using ir::TypeKind;

namespace {

inline std::uint64_t mix64(std::uint64_t x) {
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
}

std::uint64_t combine(std::uint64_t h, std::uint64_t part) { return mix64(h + 0x9e3779b97f4a7c15ULL + part); }

inline std::size_t entrySlots(const Map* m) { return static_cast<std::size_t>(m->keySlots) + m->valueSlots; }

} // namespace

Map* Sandbox::newMap(TypeId mapType) {
    const ir::TypeTable& types = program_->module->types;
    const ir::TypeInfo& info = types.info(mapType);
    auto* m = static_cast<Map*>(cur_->allocate(sizeof(Map)));
    if (m == nullptr) {
        fail(ir::TrapCode::OutOfMemory);
        return nullptr;
    }
    std::memset(m, 0, sizeof(Map));
    m->keyType = info.key;
    m->valueType = info.elem;
    m->valueSlots = static_cast<std::uint16_t>(program_->layouts->slots(info.elem));
    m->keySlots = static_cast<std::uint16_t>(program_->layouts->slots(info.key));
    TypeKind kk = types.kind(info.key);
    m->keyKind = info.key == ir::kStr ? kMapKeyStr
                 : kk == TypeKind::Struct || kk == TypeKind::Tuple || kk == TypeKind::Enum || kk == TypeKind::List
                     ? kMapKeyFields
                     : kMapKeyRaw;
    m->keyHasRefs = types.hasRefs(info.key);
    m->valueHasRefs = types.hasRefs(info.elem);
    m->valuePtrNullable = types.isRefKind(info.elem) || types.kind(info.elem) == TypeKind::Handle;
    m->owner = Owner::Arena;
    m->level = static_cast<std::uint16_t>(curLevel_);
    return m;
}

namespace {

// A list inside a key (a boxed recursive payload, or a list key) waits here, so a deep key never recurses natively.
struct KeyFrame {
    const List* a;
    const List* b;
    std::uint32_t next;
};

} // namespace

// Equal values hash equal: both walk the same fields, only the payload of the variant the tag names, and the
// elements of lists in the same order.
std::uint64_t Sandbox::valueHash(TypeId type, const Slot* v) const {
    std::vector<KeyFrame> lists;
    std::uint64_t h = 0;
    auto walk = [&](auto& self, TypeId t, const Slot* at) -> void {
        const ir::TypeInfo& info = program_->module->types.info(t);
        const Layouts& lay = *program_->layouts;
        switch (info.kind) {
        case TypeKind::Str:
            h = combine(h, strHash(static_cast<Str*>(at->p)));
            return;
        case TypeKind::Struct:
        case TypeKind::Tuple: {
            h = combine(h, info.kind == TypeKind::Tuple ? 1 : 0);
            std::size_t n = info.kind == TypeKind::Tuple ? info.elems.size() : info.fields.size();
            for (std::uint32_t i = 0; i < n; ++i) {
                self(self, info.kind == TypeKind::Tuple ? info.elems[i] : info.fields[i].type,
                     at + lay.fieldOffset(t, i));
            }
            return;
        }
        case TypeKind::Enum: {
            h = combine(h, combine(2, at[0].u));
            if (at[0].u < info.variants.size()) {
                std::uint32_t off = 1;
                for (TypeId p : info.variants[static_cast<std::size_t>(at[0].u)].payload) {
                    self(self, p, at + off);
                    off += lay.slots(p);
                }
            }
            return;
        }
        case TypeKind::List: {
            const auto* l = static_cast<const List*>(at->p);
            h = combine(h, combine(3, l->len));
            if (l->len > 0) {
                lists.push_back({l, nullptr, 0});
            }
            return;
        }
        default:
            h = combine(h, mix64(at->u ^ hashKey().k0));
            return;
        }
    };
    walk(walk, type, v);
    while (!lists.empty()) {
        KeyFrame& f = lists.back();
        const List* l = f.a;
        std::uint32_t i = f.next++;
        if (f.next >= l->len) {
            lists.pop_back();
        }
        if (l->packed) {
            h = combine(h, l->bytes()[i]);
        } else {
            walk(walk, l->elemType, l->data + static_cast<std::size_t>(i) * l->elemSlots);
        }
    }
    return h;
}

bool Sandbox::valueEq(TypeId type, const Slot* a, const Slot* b) const {
    std::vector<KeyFrame> lists;
    auto walk = [&](auto& self, TypeId t, const Slot* x, const Slot* y) -> bool {
        const ir::TypeInfo& info = program_->module->types.info(t);
        const Layouts& lay = *program_->layouts;
        switch (info.kind) {
        case TypeKind::Str:
            return strEq(static_cast<const Str*>(x->p), static_cast<const Str*>(y->p));
        case TypeKind::Struct:
        case TypeKind::Tuple: {
            std::size_t n = info.kind == TypeKind::Tuple ? info.elems.size() : info.fields.size();
            for (std::uint32_t i = 0; i < n; ++i) {
                std::uint32_t off = lay.fieldOffset(t, i);
                if (!self(self, info.kind == TypeKind::Tuple ? info.elems[i] : info.fields[i].type, x + off, y + off)) {
                    return false;
                }
            }
            return true;
        }
        case TypeKind::Enum: {
            if (x[0].u != y[0].u || x[0].u >= info.variants.size()) {
                return x[0].u == y[0].u;
            }
            std::uint32_t off = 1;
            for (TypeId p : info.variants[static_cast<std::size_t>(x[0].u)].payload) {
                if (!self(self, p, x + off, y + off)) {
                    return false;
                }
                off += lay.slots(p);
            }
            return true;
        }
        case TypeKind::List: {
            const auto* la = static_cast<const List*>(x->p);
            const auto* lb = static_cast<const List*>(y->p);
            if (la == lb) {
                return true;
            }
            if (la->len != lb->len) {
                return false;
            }
            if (la->len > 0) {
                lists.push_back({la, lb, 0});
            }
            return true;
        }
        default:
            return x->u == y->u;
        }
    };
    if (!walk(walk, type, a, b)) {
        return false;
    }
    while (!lists.empty()) {
        KeyFrame& f = lists.back();
        const List* la = f.a;
        const List* lb = f.b;
        std::uint32_t i = f.next++;
        if (f.next >= la->len) {
            lists.pop_back();
        }
        bool same = la->packed ? la->bytes()[i] == lb->bytes()[i]
                               : walk(walk, la->elemType, la->data + static_cast<std::size_t>(i) * la->elemSlots,
                                      lb->data + static_cast<std::size_t>(i) * lb->elemSlots);
        if (!same) {
            return false;
        }
    }
    return true;
}

std::uint64_t Sandbox::keyHash(const Map* m, const Slot* key) const {
    if (m->keyKind == kMapKeyRaw) {
        return mix64(key->u ^ hashKey().k0) | 2;
    }
    return (m->keyKind == kMapKeyStr ? strHash(static_cast<Str*>(key->p)) : valueHash(m->keyType, key)) | 2;
}

namespace {

template <class Same>
Slot* findWith(const Map* m, std::uint64_t h, Same&& same) {
    std::uint32_t mask = m->cap - 1;
    std::size_t es = entrySlots(m);
    for (std::uint32_t i = static_cast<std::uint32_t>(h) & mask;; i = (i + 1) & mask) {
        std::uint64_t tag = m->hashes[i];
        if (tag == 0) {
            return nullptr;
        }
        Slot* e = m->entries + i * es;
        if (tag == h && same(e)) {
            return e;
        }
    }
}

} // namespace

// Buckets work as before insertion order existed; `order` lists the buckets in insertion order beside them.
Slot* Sandbox::mapFind(const Map* m, const Slot* key) const {
    if (m->cap == 0) {
        return nullptr;
    }
    if (m->keyKind == kMapKeyRaw) {
        std::uint64_t k = key->u;
        return findWith(m, mix64(k ^ hashKey().k0) | 2, [k](const Slot* e) { return e->u == k; });
    }
    if (m->keyKind == kMapKeyStr) {
        auto* k = static_cast<Str*>(key->p);
        return findWith(m, strHash(k) | 2, [k](const Slot* e) { return strEq(static_cast<const Str*>(e->p), k); });
    }
    return findWith(m, valueHash(m->keyType, key) | 2, [&](const Slot* e) { return valueEq(m->keyType, e, key); });
}

// Cursors carry the map's version, so adding or removing a key mid-loop traps instead of skipping entries.
std::int64_t Sandbox::mapNext(const Map* m, std::int64_t cursor) {
    std::uint64_t stamp = static_cast<std::uint64_t>(m->version & 0x7FFFFFFF) << 32;
    std::uint32_t pos = 0;
    if (cursor >= 0) {
        if ((static_cast<std::uint64_t>(cursor) & ~0xFFFFFFFFULL) != stamp) {
            fail(ir::TrapCode::Panic, "map changed during iteration");
            return -2;
        }
        pos = static_cast<std::uint32_t>(cursor) + 1;
    }
    while (pos < m->ordered && m->order[pos] == 0) {
        ++pos;
    }
    return pos < m->ordered ? static_cast<std::int64_t>(stamp | pos) : -1;
}

bool Sandbox::mapEntry(const Map* m, std::int64_t cursor, bool value, Slot* dst) {
    std::uint64_t stamp = static_cast<std::uint64_t>(m->version & 0x7FFFFFFF) << 32;
    auto pos = static_cast<std::uint32_t>(cursor);
    if (cursor < 0 || (static_cast<std::uint64_t>(cursor) & ~0xFFFFFFFFULL) != stamp || pos >= m->ordered ||
        m->order[pos] == 0) {
        return fail(ir::TrapCode::Panic, "map changed during iteration");
    }
    const Slot* e = m->entries + (m->order[pos] - 1) * entrySlots(m);
    if (value) {
        std::memcpy(dst, e + m->keySlots, m->valueSlots * sizeof(Slot));
    } else {
        std::memcpy(dst, e, m->keySlots * sizeof(Slot));
    }
    return true;
}

// Drops removed entries from `order`; only an insertion calls it, and that already ends any `for` over the map.
void Sandbox::compactOrder(Map* m) {
    std::uint32_t n = 0;
    for (std::uint32_t p = 0; p < m->ordered; ++p) {
        if (std::uint32_t b = m->order[p]; b != 0) {
            m->order[n] = b;
            m->place[b - 1] = n++;
        }
    }
    m->ordered = n;
}

bool Sandbox::growMap(Map* m) {
    std::uint32_t cap = m->cap == 0 ? 8 : m->cap * 2;
    if (cap > (1u << 26)) {
        return fail(ir::TrapCode::OutOfMemory, "map too large");
    }
    return growMapTo(m, cap);
}

bool Sandbox::growMapTo(Map* m, std::uint32_t cap) {
    std::size_t es = entrySlots(m);
    InvocationArena* region = regionFor(m->owner, m->level);
    InvocationArena* savedCur = cur_;
    if (m->owner == Owner::Arena) {
        cur_ = region;
    }
    auto* hashes = static_cast<std::uint64_t*>(allocFor(m->owner, sizeof(std::uint64_t) * cap));
    auto* entries = static_cast<Slot*>(allocFor(m->owner, sizeof(Slot) * es * cap));
    auto* order = static_cast<std::uint32_t*>(allocFor(m->owner, sizeof(std::uint32_t) * cap * 2));
    cur_ = savedCur;
    if (hashes == nullptr || entries == nullptr || order == nullptr) {
        return false;
    }
    std::memset(hashes, 0, sizeof(std::uint64_t) * cap);
    std::uint32_t* place = order + cap;
    std::uint32_t n = 0;
    for (std::uint32_t p = 0; p < m->ordered; ++p) {
        std::uint32_t b = m->order[p];
        if (b == 0) {
            continue;
        }
        std::uint64_t tag = m->hashes[b - 1];
        std::uint32_t j = static_cast<std::uint32_t>(tag) & (cap - 1);
        while (hashes[j] != 0) {
            j = (j + 1) & (cap - 1);
        }
        hashes[j] = tag;
        std::memcpy(entries + j * es, m->entries + (b - 1) * es, es * sizeof(Slot));
        order[n] = j + 1;
        place[j] = n++;
    }
    if (m->owner == Owner::State) {
        heap_.release(m->hashes);
        heap_.release(m->entries);
        heap_.release(m->order);
    }
    m->hashes = hashes;
    m->entries = entries;
    m->order = order;
    m->place = place;
    m->cap = cap;
    m->used = n;
    m->ordered = n;
    return true;
}

bool Sandbox::mapSet(Map* m, const Slot* key, const Slot* value) {
    bool state = m->owner == Owner::State;
    bool promote = !state && m->owner == Owner::Arena && m->level < curLevel_;
    std::uint32_t ks = m->keySlots;
    if (Slot* e = mapFind(m, key)) {
        if (state && m->valueHasRefs) {
            return storeState(m->valueType, value, e + ks);
        }
        std::memcpy(e + ks, value, m->valueSlots * sizeof(Slot));
        return !(promote && m->valueHasRefs) || promoteInto(m->valueType, e + ks, m->level);
    }
    if ((static_cast<std::uint64_t>(m->used) + 1) * 4 > static_cast<std::uint64_t>(m->cap) * 3 && !growMap(m)) {
        return false;
    }
    if (m->ordered == m->cap) {
        compactOrder(m);
    }
    std::uint64_t h = keyHash(m, key);
    std::uint32_t mask = m->cap - 1;
    std::uint32_t i = static_cast<std::uint32_t>(h) & mask;
    while (m->hashes[i] >= 2) {
        i = (i + 1) & mask;
    }
    if (m->hashes[i] == 0) {
        ++m->used;
    }
    Slot* e = m->entries + i * entrySlots(m);
    if (state && m->keyHasRefs) {
        if (!copyIntoState(m->keyType, key, e)) {
            return false;
        }
    } else {
        std::memcpy(e, key, ks * sizeof(Slot));
    }
    if (state && m->valueHasRefs) {
        if (!copyIntoState(m->valueType, value, e + ks)) {
            return false;
        }
    } else {
        std::memcpy(e + ks, value, m->valueSlots * sizeof(Slot));
    }
    if (promote && ((m->keyHasRefs && !promoteInto(m->keyType, e, m->level)) ||
                    (m->valueHasRefs && !promoteInto(m->valueType, e + ks, m->level)))) {
        return false;
    }
    m->hashes[i] = h;
    m->order[m->ordered] = i + 1;
    m->place[i] = m->ordered++;
    ++m->count;
    ++m->version;
    return true;
}

bool Sandbox::mapRemove(Map* m, const Slot* key) {
    Slot* e = mapFind(m, key);
    if (e == nullptr) {
        return false;
    }
    auto i = static_cast<std::uint32_t>((e - m->entries) / entrySlots(m));
    if (m->owner == Owner::State) {
        if (m->keyHasRefs) {
            releaseState(m->keyType, e);
        }
        if (m->valueHasRefs) {
            releaseState(m->valueType, e + m->keySlots);
        }
    }
    m->hashes[i] = 1;
    m->order[m->place[i]] = 0;
    --m->count;
    ++m->version;
    return true;
}

} // namespace quartz::vm
