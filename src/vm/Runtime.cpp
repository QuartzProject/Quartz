#include "vm/Sandbox.hpp"

#include <algorithm>
#include <charconv>
#include <format>

namespace quartz::vm {

using ir::TypeId;
using ir::TypeKind;

namespace {

std::uint64_t mix64(std::uint64_t x) {
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
}

} // namespace

std::string Trap::describe() const {
    std::string where = function.empty() ? "" : std::format(" in '{}'", function);
    if (line != 0) {
        where += std::format(" at line {}", line);
    }
    return std::format("trap: {}{}{}", ir::trapName(code), message.empty() ? "" : ": " + message, where);
}

bool Sandbox::fail(ir::TrapCode code, std::string message) {
    if (!trapped_) {
        trapped_ = true;
        trap_.code = code;
        trap_.message = std::move(message);
    }
    return false;
}

void* Sandbox::allocFor(Owner owner, std::size_t bytes) {
    void* p = owner == Owner::State ? heap_.allocate(bytes) : arena_.allocate(bytes);
    if (p == nullptr) {
        fail(ir::TrapCode::OutOfMemory);
    }
    return p;
}

Str* Sandbox::allocStr(std::size_t len) {
    if (len > 0x7FFFFFFF) {
        fail(ir::TrapCode::OutOfMemory, "string too long");
        return nullptr;
    }
    auto* s = static_cast<Str*>(arena_.allocate(kStrHeader + len + 1));
    if (s == nullptr) {
        fail(ir::TrapCode::OutOfMemory);
        return nullptr;
    }
    s->len = static_cast<std::uint32_t>(len);
    s->hash = 0;
    s->owner = Owner::Arena;
    s->data[len] = '\0';
    return s;
}

Str* Sandbox::newStr(std::string_view text) {
    if (!inCall_) {
        auto* h = static_cast<Str*>(hostArenas_[hostArena_ ^ 1].allocate(kStrHeader + text.size() + 1));
        if (h == nullptr) {
            return nullptr;
        }
        h->len = static_cast<std::uint32_t>(text.size());
        h->hash = 0;
        h->owner = Owner::Arena;
        std::memcpy(h->data, text.data(), text.size());
        h->data[text.size()] = '\0';
        return h;
    }
    Str* s = allocStr(text.size());
    if (s != nullptr) {
        std::memcpy(s->data, text.data(), text.size());
    }
    return s;
}

Str* HostCall::string(std::string_view text) { return sandbox.newStr(text); }

Str* Sandbox::concat(const Slot* parts, std::uint32_t count) {
    std::size_t total = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        total += static_cast<const Str*>(parts[i].p)->len;
    }
    Str* s = allocStr(total);
    if (s == nullptr) {
        return nullptr;
    }
    char* out = s->data;
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto* p = static_cast<const Str*>(parts[i].p);
        std::memcpy(out, p->data, p->len);
        out += p->len;
    }
    return s;
}

Str* Sandbox::sliceStr(const Str* s, std::int64_t begin, std::int64_t end) {
    auto isBoundary = [&](std::int64_t i) {
        return i == s->len || (static_cast<unsigned char>(s->data[i]) & 0xC0) != 0x80;
    };
    if (begin < 0 || end < begin || end > s->len || !isBoundary(begin) || !isBoundary(end)) {
        fail(ir::TrapCode::IndexOutOfBounds, std::format("slice [{}, {}) of a string of {} bytes", begin, end, s->len));
        return nullptr;
    }
    return newStr(std::string_view(s->data + begin, static_cast<std::size_t>(end - begin)));
}

Str* Sandbox::caseMap(const Str* s, bool upper) {
    Str* out = allocStr(s->len);
    if (out == nullptr) {
        return nullptr;
    }
    for (std::uint32_t i = 0; i < s->len; ++i) {
        char c = s->data[i];
        if (upper && c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 32);
        } else if (!upper && c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c + 32);
        }
        out->data[i] = c;
    }
    return out;
}

Str* Sandbox::fromI64(std::int64_t v) {
    char buf[24];
    auto r = std::to_chars(buf, buf + sizeof buf, v);
    return newStr(std::string_view(buf, static_cast<std::size_t>(r.ptr - buf)));
}

Str* Sandbox::fromU64(std::uint64_t v) {
    char buf[24];
    auto r = std::to_chars(buf, buf + sizeof buf, v);
    return newStr(std::string_view(buf, static_cast<std::size_t>(r.ptr - buf)));
}

Str* Sandbox::fromF64(double v) {
    char buf[40];
    auto r = std::to_chars(buf, buf + sizeof buf - 3, v);
    std::string_view text(buf, static_cast<std::size_t>(r.ptr - buf));
    if (text.find_first_of(".einn") == std::string_view::npos) {
        *r.ptr++ = '.';
        *r.ptr++ = '0';
        text = std::string_view(buf, static_cast<std::size_t>(r.ptr - buf));
    }
    return newStr(text);
}

Str* Sandbox::fromChar(std::uint32_t cp) {
    char buf[4];
    std::size_t n = 0;
    if (cp < 0x80) {
        buf[n++] = static_cast<char>(cp);
    } else if (cp < 0x800) {
        buf[n++] = static_cast<char>(0xC0 | (cp >> 6));
        buf[n++] = static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        buf[n++] = static_cast<char>(0xE0 | (cp >> 12));
        buf[n++] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        buf[n++] = static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        buf[n++] = static_cast<char>(0xF0 | ((cp >> 18) & 0x07));
        buf[n++] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        buf[n++] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        buf[n++] = static_cast<char>(0x80 | (cp & 0x3F));
    }
    return newStr(std::string_view(buf, n));
}

List* Sandbox::newList(TypeId elem) {
    auto* l = static_cast<List*>(arena_.allocate(sizeof(List)));
    if (l == nullptr) {
        fail(ir::TrapCode::OutOfMemory);
        return nullptr;
    }
    l->data = nullptr;
    l->len = 0;
    l->cap = 0;
    l->elemType = elem;
    l->elemSlots = static_cast<std::uint16_t>(program_->layouts->slots(elem));
    l->elemHasRefs = program_->module->types.hasRefs(elem);
    l->owner = Owner::Arena;
    return l;
}

bool Sandbox::growList(List* l, std::uint32_t need) {
    std::uint64_t cap = l->cap == 0 ? 4 : static_cast<std::uint64_t>(l->cap) * 2;
    cap = std::max<std::uint64_t>(cap, need);
    if (cap * l->elemSlots > (1ULL << 28)) {
        return fail(ir::TrapCode::OutOfMemory, "list too large");
    }
    auto* data = static_cast<Slot*>(allocFor(l->owner, cap * l->elemSlots * sizeof(Slot)));
    if (data == nullptr) {
        return false;
    }
    if (l->len > 0) {
        std::memcpy(data, l->data, static_cast<std::size_t>(l->len) * l->elemSlots * sizeof(Slot));
    }
    if (l->owner == Owner::State) {
        heap_.release(l->data);
    }
    l->data = data;
    l->cap = static_cast<std::uint32_t>(cap);
    return true;
}

List* Sandbox::filledList(std::int64_t n, const Slot* value, TypeId elem) {
    if (n < 0 || n > (1LL << 28)) {
        fail(ir::TrapCode::OutOfMemory, std::format("cannot create a list of {} elements", n));
        return nullptr;
    }
    List* l = newList(elem);
    if (l == nullptr || (n > 0 && !growList(l, static_cast<std::uint32_t>(n)))) {
        return nullptr;
    }
    std::uint16_t es = l->elemSlots;
    if (es == 1) {
        std::fill_n(l->data, n, *value);
    } else {
        for (std::int64_t i = 0; i < n; ++i) {
            std::memcpy(l->data + i * es, value, es * sizeof(Slot));
        }
    }
    l->len = static_cast<std::uint32_t>(n);
    return l;
}

bool Sandbox::listPush(List* l, const Slot* value) {
    if (l->len == l->cap && !growList(l, l->len + 1)) {
        return false;
    }
    Slot* dst = l->data + static_cast<std::size_t>(l->len) * l->elemSlots;
    if (l->owner == Owner::State && l->elemHasRefs) {
        if (!copyIntoState(l->elemType, value, dst)) {
            return false;
        }
    } else {
        std::memcpy(dst, value, l->elemSlots * sizeof(Slot));
    }
    ++l->len;
    return true;
}

bool Sandbox::listSet(List* l, std::int64_t index, const Slot* value) {
    if (static_cast<std::uint64_t>(index) >= l->len) {
        return fail(ir::TrapCode::IndexOutOfBounds, std::format("index {} of a list of {}", index, l->len));
    }
    Slot* dst = l->data + static_cast<std::size_t>(index) * l->elemSlots;
    if (l->owner == Owner::State && l->elemHasRefs) {
        return storeState(l->elemType, value, dst);
    }
    std::memcpy(dst, value, l->elemSlots * sizeof(Slot));
    return true;
}

void Sandbox::listPop(List* l, Slot* dst) {
    const ir::TypeTable& types = program_->module->types;
    bool ptr = types.isRefKind(l->elemType) || types.kind(l->elemType) == TypeKind::Handle;
    if (l->len == 0) {
        dst[0].u = 0;
        return;
    }
    --l->len;
    Slot* src = l->data + static_cast<std::size_t>(l->len) * l->elemSlots;
    if (ptr) {
        dst[0] = src[0];
    } else {
        dst[0].u = 1;
        std::memcpy(dst + 1, src, l->elemSlots * sizeof(Slot));
    }
    if (l->owner == Owner::State && l->elemHasRefs) {
        releaseState(l->elemType, src);
    }
}

void Sandbox::listClear(List* l) {
    if (l->owner == Owner::State && l->elemHasRefs) {
        for (std::uint32_t i = 0; i < l->len; ++i) {
            releaseState(l->elemType, l->data + static_cast<std::size_t>(i) * l->elemSlots);
        }
    }
    l->len = 0;
}

Str* Sandbox::join(const List* l, const Str* sep) {
    std::size_t total = l->len > 0 ? static_cast<std::size_t>(l->len - 1) * sep->len : 0;
    for (std::uint32_t i = 0; i < l->len; ++i) {
        total += static_cast<const Str*>(l->data[i].p)->len;
    }
    Str* s = allocStr(total);
    if (s == nullptr) {
        return nullptr;
    }
    char* out = s->data;
    for (std::uint32_t i = 0; i < l->len; ++i) {
        if (i > 0) {
            std::memcpy(out, sep->data, sep->len);
            out += sep->len;
        }
        const auto* p = static_cast<const Str*>(l->data[i].p);
        std::memcpy(out, p->data, p->len);
        out += p->len;
    }
    return s;
}

Map* Sandbox::newMap(TypeId mapType) {
    const ir::TypeInfo& info = program_->module->types.info(mapType);
    auto* m = static_cast<Map*>(arena_.allocate(sizeof(Map)));
    if (m == nullptr) {
        fail(ir::TrapCode::OutOfMemory);
        return nullptr;
    }
    std::memset(m, 0, sizeof(Map));
    m->keyType = info.key;
    m->valueType = info.elem;
    m->valueSlots = static_cast<std::uint16_t>(program_->layouts->slots(info.elem));
    m->strKeys = info.key == ir::kStr;
    m->valueHasRefs = program_->module->types.hasRefs(info.elem);
    m->valuePtrNullable = program_->module->types.isRefKind(info.elem) ||
                          program_->module->types.kind(info.elem) == TypeKind::Handle;
    m->owner = Owner::Arena;
    return m;
}

namespace {

std::uint64_t keyHash(const Map* m, const Slot* key) {
    std::uint64_t h = m->strKeys ? strHash(static_cast<Str*>(key->p)) : mix64(key->u);
    return h | 2;
}

bool keyEq(const Map* m, const Slot* a, const Slot* b) {
    return m->strKeys ? strEq(static_cast<const Str*>(a->p), static_cast<const Str*>(b->p)) : a->u == b->u;
}

} // namespace

Slot* Sandbox::mapFind(const Map* m, const Slot* key) const {
    if (m->cap == 0) {
        return nullptr;
    }
    std::uint64_t h = keyHash(m, key);
    std::uint32_t mask = m->cap - 1;
    std::uint32_t es = 1u + m->valueSlots;
    for (std::uint32_t i = static_cast<std::uint32_t>(h) & mask;; i = (i + 1) & mask) {
        std::uint64_t tag = m->hashes[i];
        if (tag == 0) {
            return nullptr;
        }
        Slot* e = m->entries + static_cast<std::size_t>(i) * es;
        if (tag == h && keyEq(m, e, key)) {
            return e;
        }
    }
}

bool Sandbox::growMap(Map* m) {
    std::uint32_t cap = m->cap == 0 ? 8 : m->cap * 2;
    if (cap > (1u << 26)) {
        return fail(ir::TrapCode::OutOfMemory, "map too large");
    }
    std::uint32_t es = 1u + m->valueSlots;
    auto* hashes = static_cast<std::uint64_t*>(allocFor(m->owner, sizeof(std::uint64_t) * cap));
    auto* entries = static_cast<Slot*>(allocFor(m->owner, sizeof(Slot) * es * cap));
    if (hashes == nullptr || entries == nullptr) {
        return false;
    }
    std::memset(hashes, 0, sizeof(std::uint64_t) * cap);
    for (std::uint32_t i = 0; i < m->cap; ++i) {
        std::uint64_t tag = m->hashes[i];
        if (tag < 2) {
            continue;
        }
        std::uint32_t j = static_cast<std::uint32_t>(tag) & (cap - 1);
        while (hashes[j] != 0) {
            j = (j + 1) & (cap - 1);
        }
        hashes[j] = tag;
        std::memcpy(entries + static_cast<std::size_t>(j) * es, m->entries + static_cast<std::size_t>(i) * es,
                    es * sizeof(Slot));
    }
    if (m->owner == Owner::State) {
        heap_.release(m->hashes);
        heap_.release(m->entries);
    }
    m->hashes = hashes;
    m->entries = entries;
    m->cap = cap;
    m->used = m->count;
    return true;
}

bool Sandbox::mapSet(Map* m, const Slot* key, const Slot* value) {
    bool state = m->owner == Owner::State;
    if (Slot* e = mapFind(m, key)) {
        if (state && m->valueHasRefs) {
            return storeState(m->valueType, value, e + 1);
        }
        std::memcpy(e + 1, value, m->valueSlots * sizeof(Slot));
        return true;
    }
    if ((static_cast<std::uint64_t>(m->used) + 1) * 4 > static_cast<std::uint64_t>(m->cap) * 3 && !growMap(m)) {
        return false;
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
    Slot* e = m->entries + static_cast<std::size_t>(i) * (1u + m->valueSlots);
    if (state && m->strKeys) {
        if (!copyIntoState(ir::kStr, key, e)) {
            return false;
        }
    } else {
        e[0] = *key;
    }
    if (state && m->valueHasRefs) {
        if (!copyIntoState(m->valueType, value, e + 1)) {
            return false;
        }
    } else {
        std::memcpy(e + 1, value, m->valueSlots * sizeof(Slot));
    }
    m->hashes[i] = h;
    ++m->count;
    return true;
}

bool Sandbox::mapRemove(Map* m, const Slot* key) {
    Slot* e = mapFind(m, key);
    if (e == nullptr) {
        return false;
    }
    auto i = static_cast<std::uint32_t>((e - m->entries) / (1 + m->valueSlots));
    if (m->owner == Owner::State) {
        if (m->strKeys) {
            releaseState(ir::kStr, e);
        }
        if (m->valueHasRefs) {
            releaseState(m->valueType, e + 1);
        }
    }
    m->hashes[i] = 1;
    --m->count;
    return true;
}

bool Sandbox::storeState(TypeId type, const Slot* src, Slot* dst) {
    releaseState(type, dst);
    return copyIntoState(type, src, dst);
}

bool Sandbox::copyIntoState(TypeId type, const Slot* src, Slot* dst) {
    const ir::TypeTable& types = program_->module->types;
    const Layouts& lay = *program_->layouts;
    const ir::TypeInfo& info = types.info(type);
    switch (info.kind) {
    case TypeKind::Str: {
        const auto* s = static_cast<const Str*>(src->p);
        if (s->owner == Owner::Static) {
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
    case TypeKind::List: {
        const auto* l = static_cast<const List*>(src->p);
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
        if (l->len > 0 && !growList(out, l->len)) {
            return false;
        }
        for (std::uint32_t i = 0; i < l->len; ++i) {
            std::size_t off = static_cast<std::size_t>(i) * l->elemSlots;
            if (l->elemHasRefs) {
                if (!copyIntoState(l->elemType, l->data + off, out->data + off)) {
                    out->len = i;
                    return false;
                }
            } else {
                std::memcpy(out->data + off, l->data + off, l->elemSlots * sizeof(Slot));
            }
            out->len = i + 1;
        }
        return true;
    }
    case TypeKind::Map: {
        const auto* m = static_cast<const Map*>(src->p);
        auto* out = static_cast<Map*>(heap_.allocate(sizeof(Map)));
        if (out == nullptr) {
            return fail(ir::TrapCode::OutOfMemory);
        }
        *out = *m;
        out->owner = Owner::State;
        out->hashes = nullptr;
        out->entries = nullptr;
        out->cap = 0;
        out->count = 0;
        out->used = 0;
        dst->p = out;
        std::uint32_t es = 1u + m->valueSlots;
        for (std::uint32_t i = 0; i < m->cap; ++i) {
            if (m->hashes[i] >= 2) {
                const Slot* e = m->entries + static_cast<std::size_t>(i) * es;
                if (!mapSet(out, e, e + 1)) {
                    return false;
                }
            }
        }
        return true;
    }
    case TypeKind::Nullable: {
        if (lay.nullableIsPointer(type)) {
            if (src->p == nullptr) {
                dst->p = nullptr;
                return true;
            }
            return copyIntoState(info.elem, src, dst);
        }
        dst[0] = src[0];
        if (src[0].u == 0) {
            return true;
        }
        return copyIntoState(info.elem, src + 1, dst + 1);
    }
    case TypeKind::Struct:
    case TypeKind::Tuple: {
        std::size_t count = info.kind == TypeKind::Struct ? info.fields.size() : info.elems.size();
        for (std::uint32_t i = 0; i < count; ++i) {
            TypeId ft = info.kind == TypeKind::Struct ? info.fields[i].type : info.elems[i];
            std::uint32_t off = lay.fieldOffset(type, i);
            if (!copyIntoState(ft, src + off, dst + off)) {
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
            if (!copyIntoState(p, src + off, dst + off)) {
                return false;
            }
            off += lay.slots(p);
        }
        return true;
    }
    case TypeKind::State:
    case TypeKind::Fn: return fail(ir::TrapCode::Unreachable, "this value cannot be stored in state");
    default: std::memcpy(dst, src, lay.slots(type) * sizeof(Slot)); return true;
    }
}

void Sandbox::releaseState(TypeId type, Slot* slots) {
    const ir::TypeTable& types = program_->module->types;
    const Layouts& lay = *program_->layouts;
    const ir::TypeInfo& info = types.info(type);
    switch (info.kind) {
    case TypeKind::Str: {
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
        listClear(l);
        heap_.release(l->data);
        heap_.release(l);
        return;
    }
    case TypeKind::Map: {
        auto* m = static_cast<Map*>(slots->p);
        if (m == nullptr || m->owner != Owner::State) {
            return;
        }
        std::uint32_t es = 1u + m->valueSlots;
        for (std::uint32_t i = 0; i < m->cap; ++i) {
            if (m->hashes[i] >= 2) {
                Slot* e = m->entries + static_cast<std::size_t>(i) * es;
                if (m->strKeys) {
                    releaseState(ir::kStr, e);
                }
                if (m->valueHasRefs) {
                    releaseState(m->valueType, e + 1);
                }
            }
        }
        heap_.release(m->hashes);
        heap_.release(m->entries);
        heap_.release(m);
        return;
    }
    case TypeKind::Nullable:
        if (lay.nullableIsPointer(type)) {
            if (slots->p != nullptr) {
                releaseState(info.elem, slots);
            }
        } else if (slots[0].u != 0) {
            releaseState(info.elem, slots + 1);
        }
        return;
    case TypeKind::Struct:
    case TypeKind::Tuple: {
        std::size_t count = info.kind == TypeKind::Struct ? info.fields.size() : info.elems.size();
        for (std::uint32_t i = 0; i < count; ++i) {
            TypeId ft = info.kind == TypeKind::Struct ? info.fields[i].type : info.elems[i];
            if (types.hasRefs(ft)) {
                releaseState(ft, slots + lay.fieldOffset(type, i));
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
            releaseState(p, slots + off);
            off += lay.slots(p);
        }
        return;
    }
    default: return;
    }
}

} // namespace quartz::vm
