#include "vm/Sandbox.hpp"

#include <algorithm>
#include <cstring>

namespace quartz::vm {

namespace {

// Inside a host function values go where the program's own would; outside, to the host arena `newStr` uses.
InvocationArena& hostArena(bool inCall, InvocationArena* cur, InvocationArena (&host)[2], int which) {
    return inCall ? *cur : host[which ^ 1];
}

} // namespace

List* Sandbox::hostList(ir::TypeId listType) {
    const ir::TypeTable& types = program_->module->types;
    if (listType >= types.size() || types.kind(listType) != ir::TypeKind::List) {
        return nullptr;
    }
    ir::TypeId elem = types.info(listType).elem;
    auto* l = static_cast<List*>(hostArena(inCall_, cur_, hostArenas_, hostArena_).allocate(sizeof(List)));
    if (l == nullptr) {
        return nullptr;
    }
    l->data = nullptr;
    l->len = 0;
    l->cap = 0;
    l->elemType = elem;
    l->elemSlots = static_cast<std::uint16_t>(program_->layouts->slots(elem));
    l->packed = elem == ir::kBool;
    l->elemHasRefs = types.hasRefs(elem);
    l->owner = Owner::Arena;
    l->level = static_cast<std::uint16_t>(inCall_ ? curLevel_ : 0);
    return l;
}

bool Sandbox::hostListPush(List* l, std::span<const Slot> element) {
    if (l == nullptr || element.size() != l->elemSlots) {
        return false;
    }
    if (l->len == l->cap) {
        std::uint64_t cap = l->cap == 0 ? 4 : static_cast<std::uint64_t>(l->cap) * 2;
        if (cap * l->stride() > (1ULL << 31)) {
            return false;
        }
        std::size_t bytes = std::max<std::size_t>(static_cast<std::size_t>(cap * l->stride()), sizeof(Slot));
        auto* data = static_cast<Slot*>(hostArena(inCall_, cur_, hostArenas_, hostArena_).allocate(bytes));
        if (data == nullptr) {
            return false;
        }
        if (l->len > 0) {
            std::memcpy(data, l->data, static_cast<std::size_t>(l->len) * l->stride());
        }
        l->data = data;
        l->cap = static_cast<std::uint32_t>(cap);
    }
    if (l->packed) {
        l->bytes()[l->len] = element[0].u != 0 ? 1 : 0;
    } else {
        std::memcpy(l->data + static_cast<std::size_t>(l->len) * l->elemSlots, element.data(),
                    element.size() * sizeof(Slot));
    }
    ++l->len;
    return true;
}

// Exactly `n` elements, left for the caller to fill, in the arena `hostListPush` would use.
bool Sandbox::hostListResize(List* l, std::uint32_t n) {
    std::uint64_t bytes = static_cast<std::uint64_t>(n) * l->stride();
    if (bytes > (1ULL << 31)) {
        return false;
    }
    auto* data =
        static_cast<Slot*>(hostArena(inCall_, cur_, hostArenas_, hostArena_)
                               .allocate(std::max<std::size_t>(static_cast<std::size_t>(bytes), sizeof(Slot))));
    if (data == nullptr) {
        return false;
    }
    l->data = data;
    l->cap = n;
    l->len = n;
    return true;
}

ir::TypeId Sandbox::waitType(std::uint32_t task) const {
    return task < tasks_.size() && tasks_[task].live ? tasks_[task].waitType : ir::kVoid;
}

// Outside a call the map grows where `newStr` strings live (region 0 is the host arena); failing leaves no trap.
Map* Sandbox::hostMap(ir::TypeId mapType) {
    const ir::TypeTable& types = program_->module->types;
    if (mapType >= types.size() || types.kind(mapType) != ir::TypeKind::Map) {
        return nullptr;
    }
    bool wasTrapped = trapped_;
    if (!inCall_) {
        resetRegions(&hostArenas_[hostArena_ ^ 1]);
    }
    Map* m = newMap(mapType);
    if (!inCall_) {
        resetRegions(&arena_);
    }
    trapped_ = wasTrapped;
    return m;
}

bool Sandbox::hostMapSet(Map* m, std::span<const Slot> key, std::span<const Slot> value) {
    if (m == nullptr || m->owner != Owner::Arena || key.size() != m->keySlots || value.size() != m->valueSlots) {
        return false;
    }
    bool wasTrapped = trapped_;
    if (!inCall_) {
        resetRegions(&hostArenas_[hostArena_ ^ 1]);
    }
    bool ok = mapSet(m, key.data(), value.data());
    if (!inCall_) {
        resetRegions(&arena_);
    }
    trapped_ = wasTrapped;
    return ok;
}

bool Sandbox::hostMapEntry(const Map* m, std::size_t index, std::span<Slot> key, std::span<Slot> value) {
    if (m == nullptr || index >= m->count || key.size() != m->keySlots || value.size() != m->valueSlots) {
        return false;
    }
    auto at = static_cast<std::uint32_t>(index);
    std::uint32_t pos = at;
    if (m->ordered != m->count) {
        MapCursor& c = mapCursor_;
        bool next = c.map == m && c.version == m->version && c.index + 1 == at;
        pos = next ? c.pos + 1 : 0;
        for (std::uint32_t seen = next ? at : 0; pos < m->ordered; ++pos) {
            if (m->order[pos] != 0 && seen++ == at) {
                break;
            }
        }
        if (pos >= m->ordered) {
            return false;
        }
        c = {m, m->version, at, pos};
    }
    const Slot* e = m->entries + static_cast<std::size_t>(m->order[pos] - 1) * (m->keySlots + m->valueSlots);
    std::copy_n(e, m->keySlots, key.begin());
    std::copy_n(e + m->keySlots, m->valueSlots, value.begin());
    return true;
}

const Slot* Sandbox::hostMapGet(const Map* m, std::span<const Slot> key) const {
    if (m == nullptr || key.size() != m->keySlots) {
        return nullptr;
    }
    const Slot* e = mapFind(m, key.data());
    return e != nullptr ? e + m->keySlots : nullptr;
}

} // namespace quartz::vm
