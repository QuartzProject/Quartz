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
        std::size_t bytes = std::max<std::size_t>(cap * l->stride(), sizeof(Slot));
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

ir::TypeId Sandbox::waitType(std::uint32_t task) const {
    return task < tasks_.size() && tasks_[task].live ? tasks_[task].waitType : ir::kVoid;
}

} // namespace quartz::vm
