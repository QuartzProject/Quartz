#include "vm/Sandbox.hpp"
#include "vm/Walk.hpp"

#include <algorithm>
#include <cstring>

namespace quartz::vm {

using ir::TypeId;
using ir::TypeKind;

namespace {

// Frames kept between walks without charging the quota; a deeper walk charges its stack and frees it at the end.
constexpr std::size_t kWalkKept = 64;

} // namespace

bool Sandbox::walkGrow(const WalkFrame& frame, bool force) {
    std::size_t cap = walk_.capacity();
    std::size_t want = std::max<std::size_t>(16, cap * 2);
    std::size_t charge = (std::max(want, kWalkKept) - std::max(cap, kWalkKept)) * sizeof(WalkFrame);
    if (charge != 0 && !quota_.take(charge)) {
        if (!force) {
            return fail(ir::TrapCode::OutOfMemory, "value nested too deeply for the memory limit");
        }
        quota_.used += charge; // releasing cannot fail; the stack is given back when the walk ends
    }
    walkCharged_ += charge;
    walk_.reserve(want);
    walk_.push_back(frame);
    return true;
}

void Sandbox::walkShrink() {
    quota_.give(walkCharged_);
    walkCharged_ = 0;
    std::vector<WalkFrame>().swap(walk_);
}

// Takes the next value of the top frame; a frame is popped before its last value, so a chain keeps the stack flat.
template <class Visit>
bool Sandbox::walkStep(Visit&& visit) {
    WalkFrame& f = walk_.back();
    switch (f.kind) {
    case kElements: {
        const Slot* s = f.src;
        Slot* d = f.dst;
        TypeId t = f.type;
        f.src += f.stride;
        f.dst += f.stride;
        if (++f.next >= f.end) {
            walk_.pop_back();
        }
        return visit(t, s, d);
    }
    case kEntries: {
        const auto* m = static_cast<const Map*>(f.from);
        auto* out = static_cast<Map*>(f.to);
        f.next = liveFrom(m, f.next, f.end);
        if (f.next >= f.end) {
            walk_.pop_back();
            return true;
        }
        std::size_t es = entrySlots(m);
        const Slot* s = m->entries + static_cast<std::size_t>(m->order[f.next] - 1) * es;
        Slot* d =
            out == m ? const_cast<Slot*>(s) : out->entries + static_cast<std::size_t>(out->order[f.at++] - 1) * es;
        f.next = liveFrom(m, f.next + 1, f.end);
        if (f.next >= f.end) {
            walk_.pop_back();
        }
        std::uint32_t ks = m->keySlots;
        return (!m->keyHasRefs || visit(m->keyType, s, d)) && (!m->valueHasRefs || visit(m->valueType, s + ks, d + ks));
    }
    case kCaptures: {
        const auto& fn = *static_cast<const ir::Function*>(f.from);
        TypeId t = fn.regs[f.next];
        const Slot* s = f.src;
        Slot* d = f.dst;
        std::uint32_t n = program_->layouts->slots(t);
        f.src += n;
        f.dst += n;
        if (++f.next >= f.end) {
            walk_.pop_back();
        }
        return visit(t, s, d);
    }
    default:
        walk_.pop_back();
        return fail(ir::TrapCode::Unreachable, "bad walk frame");
    }
}

// Every entry of a frame at once: for keys and values that hold no container, so the copy cannot nest.
template <class Visit>
bool Sandbox::entriesInline(const WalkFrame& f, Visit&& visit) {
    const auto* m = static_cast<const Map*>(f.from);
    auto* out = static_cast<Map*>(f.to);
    std::size_t es = entrySlots(m);
    std::uint32_t ks = m->keySlots;
    std::uint32_t at = 0;
    for (std::uint32_t p = 0; p < m->ordered; ++p) {
        if (m->order[p] == 0) {
            continue;
        }
        const Slot* s = m->entries + static_cast<std::size_t>(m->order[p] - 1) * es;
        Slot* d = out == m ? const_cast<Slot*>(s) : out->entries + static_cast<std::size_t>(out->order[at++] - 1) * es;
        if ((m->keyHasRefs && !visit(m->keyType, s, d)) || (m->valueHasRefs && !visit(m->valueType, s + ks, d + ks))) {
            return false;
        }
    }
    return true;
}

bool Sandbox::copyElements(const WalkFrame& f, Copy mode, std::uint32_t level) {
    if (program_->layouts->holdsContainers(f.type)) {
        return walkPush(f);
    }
    for (std::uint32_t i = 0; i < f.end; ++i) {
        std::size_t off = static_cast<std::size_t>(i) * f.stride;
        if (!copyNode(f.type, f.src + off, f.dst + off, mode, level)) {
            return false;
        }
    }
    return true;
}

bool Sandbox::copyEntries(const WalkFrame& f, Copy mode, std::uint32_t level) {
    const auto* m = static_cast<const Map*>(f.from);
    const Layouts& lay = *program_->layouts;
    if (lay.holdsContainers(m->keyType) || lay.holdsContainers(m->valueType)) {
        return walkPush(f);
    }
    return entriesInline(f, [&](TypeId t, const Slot* s, Slot* d) { return copyNode(t, s, d, mode, level); });
}

inline bool Sandbox::copyStrSlot(const Slot* src, Slot* dst, Copy mode, std::uint32_t level) {
    const auto* s = static_cast<const Str*>(src->p);
    if (s == nullptr || s->owner == Owner::Static || (mode == Copy::StateOnly && s->owner != Owner::State) ||
        (mode == Copy::NewerThan && (s->owner != Owner::Arena || s->level <= level))) {
        dst->p = src->p;
        return true;
    }
    Str* out = allocStr(s->len);
    if (out == nullptr) {
        return false;
    }
    std::memcpy(out->data, s->data, s->len);
    out->hash = s->hash;
    dst->p = out;
    return true;
}

bool Sandbox::copyToArena(TypeId type, const Slot* src, Slot* dst, Copy mode, std::uint32_t level) {
    if (type == ir::kStr || type == ir::kBytes) {
        return copyStrSlot(src, dst, mode, level); // the common relocation at a `wait`, without a call
    }
    if (!program_->layouts->holdsContainers(type)) {
        return copyNode(type, src, dst, mode, level);
    }
    std::size_t base = walk_.size();
    bool ok = copyNode(type, src, dst, mode, level);
    while (ok && walk_.size() > base) {
        ok = walkStep([&](TypeId t, const Slot* s, Slot* d) { return copyNode(t, s, d, mode, level); });
    }
    walkEnd(base);
    return ok;
}

// Copies the inline part of a value (bounded by kMaxTypeNesting) and queues the contents of what it points to.
bool Sandbox::copyNode(TypeId type, const Slot* src, Slot* dst, Copy mode, std::uint32_t level) {
    const ir::TypeTable& types = program_->module->types;
    const Layouts& lay = *program_->layouts;
    const ir::TypeInfo& info = types.info(type);
    switch (info.kind) {
    case TypeKind::Str:
    case TypeKind::Bytes:
        return copyStrSlot(src, dst, mode, level);
    case TypeKind::List: {
        auto* l = static_cast<List*>(src->p);
        if (l == nullptr || (mode == Copy::NewerThan && (l->owner != Owner::Arena || l->level <= level))) {
            dst->p = l;
            return true;
        }
        WalkFrame f{.type = l->elemType, .end = l->len, .stride = l->elemSlots, .kind = kElements};
        if (mode == Copy::StateOnly && l->owner != Owner::State) {
            dst->p = l;
            f.src = f.dst = l->data;
            return !l->elemHasRefs || l->len == 0 || copyElements(f, mode, level);
        }
        List* out = newList(l->elemType);
        if (out == nullptr || (l->len > 0 && !growList(out, l->len, true))) {
            return false;
        }
        dst->p = out;
        if (!l->elemHasRefs) {
            if (l->len > 0) {
                std::memcpy(out->data, l->data, static_cast<std::size_t>(l->len) * l->stride());
            }
            out->len = l->len;
            return true;
        }
        out->len = l->len;
        f.src = l->data;
        f.dst = out->data;
        return l->len == 0 || copyElements(f, mode, level);
    }
    case TypeKind::Map: {
        auto* m = static_cast<Map*>(src->p);
        if (m == nullptr || (mode == Copy::NewerThan && (m->owner != Owner::Arena || m->level <= level))) {
            dst->p = m;
            return true;
        }
        bool refs = m->keyHasRefs || m->valueHasRefs;
        WalkFrame f{.from = m, .to = m, .end = m->ordered, .kind = kEntries};
        if (mode == Copy::StateOnly && m->owner != Owner::State) {
            dst->p = m;
            return !refs || m->count == 0 || copyEntries(f, mode, level);
        }
        Map* out = newMap(type);
        if (out == nullptr || !mapCopyEntries(out, m, false)) {
            return false;
        }
        dst->p = out;
        f.to = out;
        return !refs || m->count == 0 || copyEntries(f, mode, level);
    }
    case TypeKind::Fn: {
        auto* c = static_cast<Closure*>(src->p);
        if (c == nullptr || c->owner != Owner::Arena || (mode == Copy::NewerThan && c->level <= level)) {
            dst->p = src->p;
            return true;
        }
        const ir::Function& f = program_->module->functions[c->fn];
        // Closures never live in a state, so for StateOnly only their captures can hold views and they move in place.
        Closure* out = mode == Copy::StateOnly ? c : newClosure(c->fn, c->captures, c->slots, c->caps);
        if (out == nullptr) {
            return false;
        }
        dst->p = out;
        if (c->captures == 0) {
            return true;
        }
        return walkPush(WalkFrame{.src = c->caps,
                                  .dst = out->caps,
                                  .from = &f,
                                  .next = f.paramCount - c->captures,
                                  .end = f.paramCount,
                                  .kind = kCaptures});
    }
    case TypeKind::Nullable:
        if (lay.nullableIsPointer(type)) {
            if (src->p == nullptr) {
                dst->p = nullptr;
                return true;
            }
            return copyNode(info.elem, src, dst, mode, level);
        }
        dst[0] = src[0];
        return src[0].u == 0 || copyNode(info.elem, src + 1, dst + 1, mode, level);
    case TypeKind::Struct:
    case TypeKind::Tuple: {
        std::size_t count = info.kind == TypeKind::Struct ? info.fields.size() : info.elems.size();
        for (std::uint32_t i = 0; i < count; ++i) {
            TypeId ft = info.kind == TypeKind::Struct ? info.fields[i].type : info.elems[i];
            std::uint32_t off = lay.fieldOffset(type, i);
            if (!copyNode(ft, src + off, dst + off, mode, level)) {
                return false;
            }
        }
        return true;
    }
    case TypeKind::Enum: {
        dst[0] = src[0];
        auto tag = static_cast<std::size_t>(src[0].u);
        if (tag >= info.variants.size()) {
            return true;
        }
        std::uint32_t off = 1;
        for (TypeId p : info.variants[tag].payload) {
            if (!copyNode(p, src + off, dst + off, mode, level)) {
                return false;
            }
            off += lay.slots(p);
        }
        return true;
    }
    default:
        if (src != dst) {
            std::memcpy(dst, src, lay.slots(type) * sizeof(Slot));
        }
        return true;
    }
}

// Fills a new, empty `out` with `m`'s entries in the same order, reusing their hashes; with `zeroRefs` the slots
// that hold references start zeroed, ready for a deep copy that may stop halfway and still be released.
bool Sandbox::mapCopyEntries(Map* out, const Map* m, bool zeroRefs) {
    if (m->count == 0) {
        return true;
    }
    std::uint64_t cap = 8;
    while (static_cast<std::uint64_t>(m->count) * 4 > cap * 3) {
        cap *= 2;
    }
    if (cap > (1u << 26)) {
        return fail(ir::TrapCode::OutOfMemory, "map too large");
    }
    if (!growMapTo(out, static_cast<std::uint32_t>(cap))) {
        return false;
    }
    std::size_t es = entrySlots(m);
    std::uint32_t ks = m->keySlots;
    std::uint32_t mask = out->cap - 1;
    for (std::uint32_t p = 0; p < m->ordered; ++p) {
        std::uint32_t b = m->order[p];
        if (b == 0) {
            continue;
        }
        std::uint64_t tag = m->hashes[b - 1];
        std::uint32_t i = static_cast<std::uint32_t>(tag) & mask;
        while (out->hashes[i] != 0) {
            i = (i + 1) & mask;
        }
        out->hashes[i] = tag;
        const Slot* s = m->entries + (b - 1) * es;
        Slot* d = out->entries + i * es;
        if (zeroRefs && m->keyHasRefs) {
            std::memset(d, 0, ks * sizeof(Slot));
        } else {
            std::memcpy(d, s, ks * sizeof(Slot));
        }
        if (zeroRefs && m->valueHasRefs) {
            std::memset(d + ks, 0, m->valueSlots * sizeof(Slot));
        } else {
            std::memcpy(d + ks, s + ks, m->valueSlots * sizeof(Slot));
        }
        out->order[out->ordered] = i + 1;
        out->place[i] = out->ordered++;
    }
    out->count = m->count;
    out->used = m->count;
    ++out->version;
    return true;
}

inline bool Sandbox::stateStr(const Slot* src, Slot* dst) {
    const auto* s = static_cast<const Str*>(src->p);
    if (s == nullptr || s->owner == Owner::Static) {
        dst->p = src->p; // lives as long as the program, which the sandbox keeps alive
        return true;
    }
    auto* out = static_cast<Str*>(heap_.allocate(kStrHeader + s->len + 1));
    if (out == nullptr) {
        return fail(ir::TrapCode::OutOfMemory);
    }
    out->len = s->len;
    out->hash = s->hash;
    out->owner = Owner::State;
    std::memcpy(out->data, s->data, s->len + 1);
    dst->p = out;
    return true;
}

// `dst` starts zeroed when the caller releases it after a failure (storeState); the copy keeps that invariant.
bool Sandbox::copyIntoState(TypeId type, const Slot* src, Slot* dst) {
    if (!program_->layouts->holdsContainers(type)) {
        return stateNode(type, src, dst);
    }
    std::size_t base = walk_.size();
    bool ok = stateNode(type, src, dst);
    while (ok && walk_.size() > base) {
        ok = walkStep([&](TypeId t, const Slot* s, Slot* d) { return stateNode(t, s, d); });
    }
    walkEnd(base);
    return ok;
}

// Destinations start zeroed, so a copy that runs out of memory halfway can still be released.
bool Sandbox::stateNode(TypeId type, const Slot* src, Slot* dst) {
    const ir::TypeTable& types = program_->module->types;
    const Layouts& lay = *program_->layouts;
    const ir::TypeInfo& info = types.info(type);
    switch (info.kind) {
    case TypeKind::Str:
    case TypeKind::Bytes:
        return stateStr(src, dst);
    case TypeKind::List: {
        const auto* l = static_cast<const List*>(src->p);
        if (l == nullptr) {
            dst->p = nullptr;
            return true;
        }
        auto* out = static_cast<List*>(heap_.allocate(sizeof(List)));
        if (out == nullptr) {
            return fail(ir::TrapCode::OutOfMemory);
        }
        *out = *l;
        out->owner = Owner::State;
        out->data = nullptr;
        out->cap = 0;
        out->len = 0;
        dst->p = out;
        if (l->len == 0) {
            return true;
        }
        if (!growList(out, l->len, true)) {
            return false;
        }
        if (!l->elemHasRefs) {
            std::memcpy(out->data, l->data, static_cast<std::size_t>(l->len) * l->stride());
            out->len = l->len;
            return true;
        }
        if (!lay.holdsContainers(l->elemType)) {
            // Counted as they are copied: a failure leaves a list that releases only what it holds.
            bool str = l->elemType == ir::kStr || l->elemType == ir::kBytes;
            for (std::uint32_t i = 0; i < l->len; ++i) {
                std::size_t off = static_cast<std::size_t>(i) * l->elemSlots;
                if (!(str ? stateStr(l->data + off, out->data + off)
                          : stateNode(l->elemType, l->data + off, out->data + off))) {
                    return false;
                }
                out->len = i + 1;
            }
            return true;
        }
        std::memset(out->data, 0, static_cast<std::size_t>(l->len) * l->elemSlots * sizeof(Slot));
        out->len = l->len;
        return walkPush(WalkFrame{.src = l->data,
                                  .dst = out->data,
                                  .type = l->elemType,
                                  .end = l->len,
                                  .stride = l->elemSlots,
                                  .kind = kElements});
    }
    case TypeKind::Map: {
        const auto* m = static_cast<const Map*>(src->p);
        if (m == nullptr) {
            dst->p = nullptr;
            return true;
        }
        auto* out = static_cast<Map*>(heap_.allocate(sizeof(Map)));
        if (out == nullptr) {
            return fail(ir::TrapCode::OutOfMemory);
        }
        *out = *m;
        out->owner = Owner::State;
        out->hashes = nullptr;
        out->entries = nullptr;
        out->order = nullptr;
        out->place = nullptr;
        out->cap = 0;
        out->count = 0;
        out->used = 0;
        out->ordered = 0;
        dst->p = out;
        if (!mapCopyEntries(out, m, true)) {
            return false;
        }
        bool refs = m->keyHasRefs || m->valueHasRefs;
        WalkFrame f{.from = m, .to = out, .end = m->ordered, .kind = kEntries};
        if (!refs || m->count == 0) {
            return true;
        }
        if (lay.holdsContainers(m->keyType) || lay.holdsContainers(m->valueType)) {
            return walkPush(f);
        }
        return entriesInline(f, [&](TypeId t, const Slot* s, Slot* d) { return stateNode(t, s, d); });
    }
    case TypeKind::Nullable:
        if (lay.nullableIsPointer(type)) {
            if (src->p == nullptr) {
                dst->p = nullptr;
                return true;
            }
            return stateNode(info.elem, src, dst);
        }
        dst[0] = src[0];
        return src[0].u == 0 || stateNode(info.elem, src + 1, dst + 1);
    case TypeKind::Struct:
    case TypeKind::Tuple: {
        std::size_t count = info.kind == TypeKind::Struct ? info.fields.size() : info.elems.size();
        for (std::uint32_t i = 0; i < count; ++i) {
            TypeId ft = info.kind == TypeKind::Struct ? info.fields[i].type : info.elems[i];
            std::uint32_t off = lay.fieldOffset(type, i);
            if (!stateNode(ft, src + off, dst + off)) {
                return false;
            }
        }
        return true;
    }
    case TypeKind::Enum: {
        dst[0] = src[0];
        auto tag = static_cast<std::size_t>(src[0].u);
        if (tag >= info.variants.size()) {
            return true;
        }
        std::uint32_t off = 1;
        for (TypeId p : info.variants[tag].payload) {
            if (!stateNode(p, src + off, dst + off)) {
                return false;
            }
            off += lay.slots(p);
        }
        return true;
    }
    case TypeKind::State:
    case TypeKind::Fn:
        return fail(ir::TrapCode::Unreachable, "this value cannot be stored in state");
    default:
        if (src != dst) {
            std::memcpy(dst, src, lay.slots(type) * sizeof(Slot));
        }
        return true;
    }
}

void Sandbox::releaseState(TypeId type, Slot* slots) {
    if (!program_->layouts->holdsContainers(type)) {
        releaseNode(type, slots);
        return;
    }
    std::size_t base = walk_.size();
    releaseNode(type, slots);
    while (walk_.size() > base) {
        WalkFrame& f = walk_.back();
        if (f.kind == kReleaseList) {
            auto* l = static_cast<List*>(f.to);
            Slot* e = f.dst;
            TypeId t = f.type;
            f.dst += f.stride;
            bool last = ++f.next >= f.end;
            if (last) {
                walk_.pop_back();
            }
            releaseNode(t, e);
            if (last) {
                heap_.release(l->data);
                heap_.release(l);
            }
            continue;
        }
        auto* m = static_cast<Map*>(f.to);
        f.next = liveFrom(m, f.next, f.end);
        Slot* e =
            f.next < f.end ? m->entries + static_cast<std::size_t>(m->order[f.next] - 1) * entrySlots(m) : nullptr;
        f.next = e != nullptr ? liveFrom(m, f.next + 1, f.end) : f.end;
        bool last = f.next >= f.end;
        if (last) {
            walk_.pop_back();
        }
        if (e != nullptr && m->keyHasRefs) {
            releaseNode(m->keyType, e);
        }
        if (e != nullptr && m->valueHasRefs) {
            releaseNode(m->valueType, e + m->keySlots);
        }
        if (last) {
            heap_.release(m->hashes);
            heap_.release(m->entries);
            heap_.release(m->order);
            heap_.release(m);
        }
    }
    walkEnd(base);
}

void Sandbox::releaseNode(TypeId type, Slot* slots) {
    const ir::TypeTable& types = program_->module->types;
    const Layouts& lay = *program_->layouts;
    const ir::TypeInfo& info = types.info(type);
    switch (info.kind) {
    case TypeKind::Str:
    case TypeKind::Bytes: {
        auto* s = static_cast<Str*>(slots->p);
        if (s != nullptr && s->owner == Owner::State) {
            heap_.release(s);
        }
        return;
    }
    case TypeKind::List: {
        auto* l = static_cast<List*>(slots->p);
        if (l == nullptr || l->owner != Owner::State) {
            return;
        }
        if (l->elemHasRefs && l->len > 0 && (l->elemType == ir::kStr || l->elemType == ir::kBytes)) {
            for (std::uint32_t i = 0; i < l->len; ++i) {
                auto* e = static_cast<Str*>(l->data[i].p);
                if (e != nullptr && e->owner == Owner::State) {
                    heap_.release(e);
                }
            }
        } else if (l->elemHasRefs && l->len > 0 && !lay.holdsContainers(l->elemType)) {
            for (std::uint32_t i = 0; i < l->len; ++i) {
                releaseNode(l->elemType, l->data + static_cast<std::size_t>(i) * l->elemSlots);
            }
        } else if (l->elemHasRefs && l->len > 0) {
            walkPush(WalkFrame{.dst = l->data,
                               .to = l,
                               .type = l->elemType,
                               .end = l->len,
                               .stride = l->elemSlots,
                               .kind = kReleaseList},
                     true);
            return;
        }
        heap_.release(l->data);
        heap_.release(l);
        return;
    }
    case TypeKind::Map: {
        auto* m = static_cast<Map*>(slots->p);
        if (m == nullptr || m->owner != Owner::State) {
            return;
        }
        if ((m->keyHasRefs || m->valueHasRefs) && m->count > 0) {
            walkPush(WalkFrame{.to = m, .end = m->ordered, .kind = kReleaseMap}, true);
            return;
        }
        heap_.release(m->hashes);
        heap_.release(m->entries);
        heap_.release(m->order);
        heap_.release(m);
        return;
    }
    case TypeKind::Nullable:
        if (lay.nullableIsPointer(type)) {
            if (slots->p != nullptr) {
                releaseNode(info.elem, slots);
            }
        } else if (slots[0].u != 0) {
            releaseNode(info.elem, slots + 1);
        }
        return;
    case TypeKind::Struct:
    case TypeKind::Tuple: {
        std::size_t count = info.kind == TypeKind::Struct ? info.fields.size() : info.elems.size();
        for (std::uint32_t i = 0; i < count; ++i) {
            TypeId ft = info.kind == TypeKind::Struct ? info.fields[i].type : info.elems[i];
            if (lay.hasRefs(ft)) {
                releaseNode(ft, slots + lay.fieldOffset(type, i));
            }
        }
        return;
    }
    case TypeKind::Enum: {
        auto tag = static_cast<std::size_t>(slots[0].u);
        if (tag >= info.variants.size()) {
            return;
        }
        std::uint32_t off = 1;
        for (TypeId p : info.variants[tag].payload) {
            releaseNode(p, slots + off);
            off += lay.slots(p);
        }
        return;
    }
    default:
        return;
    }
}

} // namespace quartz::vm
