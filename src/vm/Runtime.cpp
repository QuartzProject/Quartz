#include "vm/Sandbox.hpp"

#include "vm/Builtins.hpp"
#include "vm/ReprC.hpp"

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

namespace {

std::string place(std::string_view file, std::uint32_t line) {
    if (line == 0) {
        return "";
    }
    return file.empty() ? std::format(" at line {}", line) : std::format(" at {}:{}", file, line);
}

} // namespace

std::string Trap::describe() const {
    std::string where = function.empty() ? "" : std::format(" in '{}'", function);
    where += place(file, line);
    std::string out = std::format("trap: {}{}{}", ir::trapName(code), message.empty() ? "" : ": " + message, where);
    for (const Frame& f : callers) {
        out += std::format("\n  called from '{}'{}", f.function, place(f.file, f.line));
    }
    return out;
}

bool Sandbox::fail(ir::TrapCode code, std::string message) {
    if (!trapped_) {
        trapped_ = true;
        trap_.code = code;
        trap_.message = std::move(message);
        trap_.callers.clear();
    }
    return false;
}

InvocationArena* Sandbox::regionFor(Owner owner, std::uint32_t level) {
    (void)owner;
    return level < regions_.size() ? regions_[level] : cur_;
}

void Sandbox::resetRegions(InvocationArena* base) {
    if (scopeArenas_.empty() && regions_.size() == 1 && regions_[0] == base) {
        cur_ = base;
        curLevel_ = 0;
        return;
    }
    for (auto& a : scopeArenas_) {
        a->reset();
        spare_.push_back(std::move(a));
    }
    scopeArenas_.clear();
    regions_.assign(1, base);
    cur_ = base;
    curLevel_ = 0;
}

std::vector<std::unique_ptr<InvocationArena>> Sandbox::takeScopes() {
    std::vector<std::unique_ptr<InvocationArena>> out = std::move(scopeArenas_);
    scopeArenas_.clear();
    return out;
}

bool Sandbox::scopeIn() {
    if (regions_.size() >= 250) {
        return fail(ir::TrapCode::StackOverflow, "too many nested scopes");
    }
    std::unique_ptr<InvocationArena> a;
    if (!spare_.empty()) {
        a = std::move(spare_.back());
        spare_.pop_back();
    } else {
        a = std::make_unique<InvocationArena>(quota_);
    }
    regions_.push_back(a.get());
    scopeArenas_.push_back(std::move(a));
    cur_ = regions_.back();
    curLevel_ = static_cast<std::uint32_t>(regions_.size() - 1);
    return true;
}

void Sandbox::scopeOut() {
    if (scopeArenas_.empty()) {
        return;
    }
    scopeArenas_.back()->reset();
    spare_.push_back(std::move(scopeArenas_.back()));
    scopeArenas_.pop_back();
    regions_.pop_back();
    cur_ = regions_.back();
    curLevel_ = static_cast<std::uint32_t>(regions_.size() - 1);
}

bool Sandbox::promoteInto(TypeId type, Slot* slot, std::uint32_t level) {
    if (level >= curLevel_) {
        return true;
    }
    InvocationArena* savedCur = cur_;
    std::uint32_t savedLevel = curLevel_;
    cur_ = regions_[level];
    curLevel_ = level;
    bool ok = copyToArena(type, slot, slot, Copy::NewerThan, level);
    cur_ = savedCur;
    curLevel_ = savedLevel;
    return ok;
}

void* Sandbox::allocFor(Owner owner, std::size_t bytes) {
    void* p = owner == Owner::State ? heap_.allocate(bytes) : cur_->allocate(bytes);
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
    auto* s = static_cast<Str*>(cur_->allocate(kStrHeader + len + 1));
    if (s == nullptr) {
        fail(ir::TrapCode::OutOfMemory);
        return nullptr;
    }
    s->len = static_cast<std::uint32_t>(len);
    s->hash = 0;
    s->owner = Owner::Arena;
    s->level = static_cast<std::uint8_t>(curLevel_);
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
        h->level = 0;
        if (!text.empty()) {
            std::memcpy(h->data, text.data(), text.size());
        }
        h->data[text.size()] = '\0';
        return h;
    }
    Str* s = allocStr(text.size());
    if (s != nullptr && !text.empty()) {
        std::memcpy(s->data, text.data(), text.size());
    }
    return s;
}

Str* HostCall::string(std::string_view text) { return sandbox.newStr(text); }

// `@repr(C)` structs reach the host as one slot holding a pointer to a C struct (D66).
bool Sandbox::callHostC(std::uint32_t import, const Slot* args, Slot* result) {
    const ir::HostImport& imp = program_->module->imports[import];
    const ir::TypeTable& types = program_->module->types;
    const Layouts& lay = *program_->layouts;
    std::vector<Slot> cargs;
    const Slot* in = args;
    for (TypeId p : imp.params) {
        if (types.info(p).reprC) {
            CLayout cl = cLayout(types, p);
            auto* buf = static_cast<std::uint8_t*>(cur_->allocate(cl.size));
            if (buf == nullptr) {
                return fail(ir::TrapCode::OutOfMemory);
            }
            std::memset(buf, 0, cl.size);
            toC(types, lay, p, in, buf);
            Slot s{};
            s.p = buf;
            cargs.push_back(s);
        } else {
            cargs.insert(cargs.end(), in, in + lay.slots(p));
        }
        in += lay.slots(p);
    }
    std::uint8_t* cresult = nullptr;
    if (types.info(imp.result).reprC) {
        CLayout cl = cLayout(types, imp.result);
        cresult = static_cast<std::uint8_t*>(cur_->allocate(cl.size));
        if (cresult == nullptr) {
            return fail(ir::TrapCode::OutOfMemory);
        }
        std::memset(cresult, 0, cl.size);
    }
    HostCall hc{*this, cargs.data(), {}, {}, cresult != nullptr ? nullptr : result};
    if (cresult != nullptr) {
        hc.result.p = cresult;
    }
    const Binding& bnd = bindings_[import];
    if (!bnd.fn(hc, bnd.user)) {
        return fail(ir::TrapCode::HostError, std::move(hc.error));
    }
    if (trapped_) {
        return false;
    }
    if (cresult != nullptr) {
        fromC(types, lay, imp.result, cresult, result);
    } else if (result != nullptr && program_->importResultSlots[import] == 1) {
        *result = hc.result;
    }
    return true;
}

bool HostCall::charge(std::int64_t units) {
    sandbox.fuel_ -= units;
    if (sandbox.fuel_ < -sandbox.limits_.fuelPerCall || (!sandbox.taskMode_ && sandbox.fuel_ < 0)) {
        return sandbox.fail(ir::TrapCode::OutOfFuel);
    }
    return true;
}

bool Sandbox::chargeFuel(std::int64_t units) {
    fuel_ -= units;
    if (fuel_ < -limits_.fuelPerCall || (!taskMode_ && fuel_ < 0)) {
        return fail(ir::TrapCode::OutOfFuel);
    }
    return true;
}

Str* Sandbox::concat(const Slot* parts, std::uint32_t count) {
    std::size_t total = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        total += static_cast<const Str*>(parts[i].p)->len;
    }
    if (!Builtins::chargeBytes(*this, kFuelWriteFactor * total)) {
        return nullptr;
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

// Negative bounds count from the end, as in Python; both must then fall on character boundaries.
Str* Sandbox::sliceStr(const Str* s, std::int64_t begin, std::int64_t end) {
    auto isBoundary = [&](std::int64_t i) {
        return i == s->len || (static_cast<unsigned char>(s->data[i]) & 0xC0) != 0x80;
    };
    std::int64_t from = begin < 0 ? begin + s->len : begin;
    std::int64_t to = end < 0 ? end + s->len : end;
    if (from < 0 || to < from || to > s->len || !isBoundary(from) || !isBoundary(to)) {
        fail(ir::TrapCode::IndexOutOfBounds, std::format("slice [{}, {}) of a string of {} bytes", begin, end, s->len));
        return nullptr;
    }
    if (!Builtins::chargeBytes(*this, kFuelWriteFactor * static_cast<std::uint64_t>(to - from))) {
        return nullptr;
    }
    return newStr(std::string_view(s->data + from, static_cast<std::size_t>(to - from)));
}

Str* Sandbox::caseMap(const Str* s, bool upper) {
    if (!Builtins::chargeBytes(*this, (1 + kFuelWriteFactor) * s->len)) {
        return nullptr;
    }
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
    if (v != v) {
        return newStr("nan"); // the sign of a NaN differs between CPUs (0.0 / 0.0 is -nan on x86, nan on arm64)
    }
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
    auto* l = static_cast<List*>(cur_->allocate(sizeof(List)));
    if (l == nullptr) {
        fail(ir::TrapCode::OutOfMemory);
        return nullptr;
    }
    l->data = nullptr;
    l->len = 0;
    l->cap = 0;
    l->elemType = elem;
    l->elemSlots = static_cast<std::uint16_t>(program_->layouts->slots(elem));
    l->packed = elem == ir::kBool;
    l->elemHasRefs = program_->module->types.hasRefs(elem);
    l->owner = Owner::Arena;
    l->level = static_cast<std::uint16_t>(curLevel_);
    return l;
}

bool Sandbox::growList(List* l, std::uint32_t need) {
    std::uint64_t cap = l->cap == 0 ? 4 : static_cast<std::uint64_t>(l->cap) * 2;
    cap = std::max<std::uint64_t>(cap, need);
    if (cap * l->stride() > (1ULL << 31)) {
        return fail(ir::TrapCode::OutOfMemory, "list too large");
    }
    std::size_t bytes = std::max<std::size_t>(cap * l->stride(), sizeof(Slot));
    auto* data = static_cast<Slot*>(l->owner == Owner::State ? allocFor(l->owner, bytes)
                                                              : regionFor(l->owner, l->level)->allocate(bytes));
    if (data == nullptr && l->owner != Owner::State) {
        return fail(ir::TrapCode::OutOfMemory);
    }
    if (data == nullptr) {
        return false;
    }
    if (l->len > 0) {
        std::memcpy(data, l->data, static_cast<std::size_t>(l->len) * l->stride());
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
    std::uint64_t stride = elem == ir::kBool ? 1 : program_->layouts->slots(elem) * sizeof(Slot);
    if (!Builtins::chargeBytes(*this, kFuelWriteFactor * static_cast<std::uint64_t>(n) * stride)) {
        return nullptr;
    }
    List* l = newList(elem);
    if (l == nullptr || (n > 0 && !growList(l, static_cast<std::uint32_t>(n)))) {
        return nullptr;
    }
    std::uint16_t es = l->elemSlots;
    if (l->packed) {
        std::memset(l->data, static_cast<int>(value->u & 1), static_cast<std::size_t>(n));
    } else if (es == 1) {
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
    if (l->packed) {
        l->bytes()[l->len++] = static_cast<std::uint8_t>(value->u & 1);
        return true;
    }
    Slot* dst = l->data + static_cast<std::size_t>(l->len) * l->elemSlots;
    if (l->owner == Owner::State && l->elemHasRefs) {
        if (!copyIntoState(l->elemType, value, dst)) {
            return false;
        }
    } else {
        std::memcpy(dst, value, l->elemSlots * sizeof(Slot));
        if (l->elemHasRefs && l->level < curLevel_ && l->owner == Owner::Arena &&
            !promoteInto(l->elemType, dst, l->level)) {
            return false;
        }
    }
    ++l->len;
    return true;
}

bool Sandbox::listSet(List* l, std::int64_t index, const Slot* value) {
    if (static_cast<std::uint64_t>(index) >= l->len) {
        return fail(ir::TrapCode::IndexOutOfBounds, std::format("index {} of a list of {}", index, l->len));
    }
    if (l->packed) {
        l->bytes()[index] = static_cast<std::uint8_t>(value->u & 1);
        return true;
    }
    Slot* dst = l->data + static_cast<std::size_t>(index) * l->elemSlots;
    if (l->owner == Owner::State && l->elemHasRefs) {
        return storeState(l->elemType, value, dst);
    }
    std::memcpy(dst, value, l->elemSlots * sizeof(Slot));
    if (l->elemHasRefs && l->level < curLevel_ && l->owner == Owner::Arena) {
        return promoteInto(l->elemType, dst, l->level);
    }
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
    if (l->packed) {
        dst[0].u = 1;
        dst[1].u = l->bytes()[l->len];
        return;
    }
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
    if (!Builtins::chargeBytes(*this, kFuelWriteFactor * total)) {
        return nullptr;
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
    auto* m = static_cast<Map*>(cur_->allocate(sizeof(Map)));
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
    m->level = static_cast<std::uint16_t>(curLevel_);
    return m;
}

namespace {

std::uint64_t keyHash(const Map* m, const Slot* key) {
    std::uint64_t h = m->strKeys ? strHash(static_cast<Str*>(key->p)) : mix64(key->u ^ hashKey().k0);
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
    InvocationArena* region = regionFor(m->owner, m->level);
    InvocationArena* savedCur = cur_;
    if (m->owner == Owner::Arena) {
        cur_ = region;
    }
    auto* hashes = static_cast<std::uint64_t*>(allocFor(m->owner, sizeof(std::uint64_t) * cap));
    auto* entries = static_cast<Slot*>(allocFor(m->owner, sizeof(Slot) * es * cap));
    cur_ = savedCur;
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
    bool promote = !state && m->owner == Owner::Arena && m->level < curLevel_;
    if (Slot* e = mapFind(m, key)) {
        if (state && m->valueHasRefs) {
            return storeState(m->valueType, value, e + 1);
        }
        std::memcpy(e + 1, value, m->valueSlots * sizeof(Slot));
        return !(promote && m->valueHasRefs) || promoteInto(m->valueType, e + 1, m->level);
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
    if (promote && ((m->strKeys && !promoteInto(ir::kStr, e, m->level)) ||
                    (m->valueHasRefs && !promoteInto(m->valueType, e + 1, m->level)))) {
        return false;
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
    case TypeKind::Str:
    case TypeKind::Bytes: {
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
        if (!l->elemHasRefs) {
            if (l->len > 0) {
                std::memcpy(out->data, l->data, static_cast<std::size_t>(l->len) * l->stride());
            }
            out->len = l->len;
            return true;
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
