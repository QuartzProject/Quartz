#include "vm/Sandbox.hpp"
#include "vm/Tier.hpp"

#include <cmath>
#include <format>
#include <iterator>

namespace quartz::vm {

namespace {

Sandbox& sb(NativeCtx* c) { return *c->sandbox; }

void where(NativeCtx* c, std::uint32_t fn, std::uint32_t line) {
    c->trapFunction = fn;
    c->trapLine = line;
}

void qzTrap(NativeCtx* c, std::uint32_t code, std::uint32_t fn, std::uint32_t line) {
    if (code != kTrapAlreadySet) {
        sb(c).fail(static_cast<ir::TrapCode>(code));
    }
    where(c, fn, line);
}

void qzPanic(NativeCtx* c, const Str* msg, std::uint32_t fn, std::uint32_t line) {
    sb(c).fail(ir::TrapCode::Panic, std::string(msg->view()));
    where(c, fn, line);
}

void qzIndexTrap(NativeCtx* c, std::int64_t index, std::int64_t len, std::uint32_t fn, std::uint32_t line) {
    sb(c).fail(ir::TrapCode::IndexOutOfBounds, std::format("index {} of a list of {}", index, len));
    where(c, fn, line);
}

std::uint32_t qzCallHost(NativeCtx* c, std::uint32_t import, const Slot* args, Slot* result) {
    return sb(c).callHost(import, args, result) ? 1 : 0;
}

Str* qzConcat(NativeCtx* c, const Slot* parts, std::uint32_t n) { return sb(c).concat(parts, n); }

Str* qzSlice(NativeCtx* c, const Str* s, std::int64_t b, std::int64_t e) { return sb(c).sliceStr(s, b, e); }

Str* qzCase(NativeCtx* c, const Str* s, std::uint32_t upper) { return sb(c).caseMap(s, upper != 0); }

Str* qzFromI64(NativeCtx* c, std::int64_t v) { return sb(c).fromI64(v); }

Str* qzFromU64(NativeCtx* c, std::uint64_t v) { return sb(c).fromU64(v); }

Str* qzFromF64(NativeCtx* c, double v) { return sb(c).fromF64(v); }

Str* qzFromBool(NativeCtx* c, std::uint32_t v) { return sb(c).newStr(v != 0 ? "true" : "false"); }

Str* qzFromChar(NativeCtx* c, std::uint64_t v) { return sb(c).fromChar(static_cast<std::uint32_t>(v)); }

std::uint32_t qzStrEq(const Str* a, const Str* b) {
    if (a == nullptr || b == nullptr) {
        return a == b ? 1 : 0;
    }
    return strEq(a, b) ? 1 : 0;
}

std::int32_t qzStrCmp(const Str* a, const Str* b) {
    std::uint32_t n = a->len < b->len ? a->len : b->len;
    int c = std::memcmp(a->data, b->data, n);
    if (c != 0) {
        return c;
    }
    return a->len < b->len ? -1 : a->len > b->len ? 1 : 0;
}

std::uint32_t qzStrContains(const Str* a, const Str* b) {
    return a->view().find(b->view()) != std::string_view::npos ? 1 : 0;
}

std::int64_t qzStrFind(const Str* a, const Str* b) {
    std::size_t at = a->view().find(b->view());
    return at == std::string_view::npos ? -1 : static_cast<std::int64_t>(at);
}

std::uint32_t qzStrStarts(const Str* a, const Str* b) { return a->view().starts_with(b->view()) ? 1 : 0; }

std::uint32_t qzStrEnds(const Str* a, const Str* b) { return a->view().ends_with(b->view()) ? 1 : 0; }

List* qzListNew(NativeCtx* c, std::uint32_t elem) { return sb(c).newList(elem); }

List* qzListFilled(NativeCtx* c, std::int64_t n, const Slot* v, std::uint32_t elem) {
    return sb(c).filledList(n, v, elem);
}

std::uint32_t qzListPush(NativeCtx* c, List* l, const Slot* v) { return sb(c).listPush(l, v) ? 1 : 0; }

std::uint32_t qzListSet(NativeCtx* c, List* l, std::int64_t i, const Slot* v) { return sb(c).listSet(l, i, v) ? 1 : 0; }

void qzListPop(NativeCtx* c, List* l, Slot* dst) { sb(c).listPop(l, dst); }

void qzListClear(NativeCtx* c, List* l) { sb(c).listClear(l); }

Str* qzJoin(NativeCtx* c, const List* l, const Str* sep) { return sb(c).join(l, sep); }

Map* qzMapNew(NativeCtx* c, std::uint32_t type) { return sb(c).newMap(type); }

Slot* qzMapFind(NativeCtx* c, const Map* m, const Slot* key) { return sb(c).mapFind(m, key); }

std::uint32_t qzMapSet(NativeCtx* c, Map* m, const Slot* key, const Slot* value) {
    return sb(c).mapSet(m, key, value) ? 1 : 0;
}

std::uint32_t qzMapRemove(NativeCtx* c, Map* m, const Slot* key) { return sb(c).mapRemove(m, key) ? 1 : 0; }

std::uint32_t qzStateStore(NativeCtx* c, std::uint32_t type, const Slot* src, Slot* dst) {
    return sb(c).storeState(type, src, dst) ? 1 : 0;
}

double qzPow(double a, double b) { return std::pow(a, b); }

double qzSin(double a) { return std::sin(a); }

double qzCos(double a) { return std::cos(a); }

template <class F>
void* addr(F* f) {
    return reinterpret_cast<void*>(f);
}

const NativeSymbol kSymbols[] = {
    {"qz_trap", addr(qzTrap)},
    {"qz_panic", addr(qzPanic)},
    {"qz_index_trap", addr(qzIndexTrap)},
    {"qz_call_host", addr(qzCallHost)},
    {"qz_concat", addr(qzConcat)},
    {"qz_slice", addr(qzSlice)},
    {"qz_case", addr(qzCase)},
    {"qz_from_i64", addr(qzFromI64)},
    {"qz_from_u64", addr(qzFromU64)},
    {"qz_from_f64", addr(qzFromF64)},
    {"qz_from_bool", addr(qzFromBool)},
    {"qz_from_char", addr(qzFromChar)},
    {"qz_str_eq", addr(qzStrEq)},
    {"qz_str_cmp", addr(qzStrCmp)},
    {"qz_str_contains", addr(qzStrContains)},
    {"qz_str_find", addr(qzStrFind)},
    {"qz_str_starts", addr(qzStrStarts)},
    {"qz_str_ends", addr(qzStrEnds)},
    {"qz_list_new", addr(qzListNew)},
    {"qz_list_filled", addr(qzListFilled)},
    {"qz_list_push", addr(qzListPush)},
    {"qz_list_set", addr(qzListSet)},
    {"qz_list_pop", addr(qzListPop)},
    {"qz_list_clear", addr(qzListClear)},
    {"qz_join", addr(qzJoin)},
    {"qz_map_new", addr(qzMapNew)},
    {"qz_map_find", addr(qzMapFind)},
    {"qz_map_set", addr(qzMapSet)},
    {"qz_map_remove", addr(qzMapRemove)},
    {"qz_state_store", addr(qzStateStore)},
    {"qz_pow", addr(qzPow)},
    {"qz_sin", addr(qzSin)},
    {"qz_cos", addr(qzCos)},
};

} // namespace

std::span<const NativeSymbol> nativeRuntimeSymbols() { return kSymbols; }

} // namespace quartz::vm
