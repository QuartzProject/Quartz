#include "vm/Builtins.hpp"
#include "vm/Math.hpp"
#include "vm/Sandbox.hpp"
#include "vm/Tier.hpp"

#include <bit>
#include <cmath>
#include <cstring>
#include <format>
#include <iterator>

#if defined(__SSE2__)
#include <emmintrin.h>
#endif

namespace quartz::vm {

/// What the compiled code's callbacks need from the sandbox beyond its public runtime.
struct NativeRuntimeAccess {
    static std::int64_t& fuel(Sandbox& s) { return s.fuel_; }

    static std::uint32_t level(const Sandbox& s) { return s.curLevel_; }

    static bool persist(Sandbox& s, ir::TypeId type, Slot* value, std::uint32_t levels) {
        return s.curLevel_ < levels || s.promoteInto(type, value, s.curLevel_ - levels);
    }

    static void wait(Sandbox& s, std::uint32_t kind, std::uint64_t handle, ir::TypeId type) {
        s.suspendKind_ = static_cast<TaskOutcome::Kind>(kind + 1);
        s.awaited_.u = handle;
        s.suspendType_ = type;
        s.suspendDst_ = type == ir::kVoid ? 0xFFFF : 0;
    }

    // Views into state memory must not outlive the invocation (D27, D35): the record gets task arena copies.
    static bool save(Sandbox& s, NativeCtx* c, const Slot* rec, std::uint32_t n, std::uint32_t state,
                     const std::uint32_t* reloc, std::uint32_t count) {
        std::vector<Slot>& v = *s.nativeSaved_;
        v.resize(c->saveTop);
        std::size_t at = v.size();
        v.insert(v.end(), rec, rec + n);
        Slot meta;
        meta.u = static_cast<std::uint64_t>(state) << 32 | n;
        v.push_back(meta);
        c->saveTop = static_cast<std::uint32_t>(v.size());
        InvocationArena* cur = s.cur_;
        std::uint32_t level = s.curLevel_;
        s.cur_ = s.regions_[0];
        s.curLevel_ = 0;
        bool ok = true;
        for (std::uint32_t i = 0; ok && i < count; ++i) {
            Slot* p = v.data() + at + reloc[2 * i];
            ok = s.copyToArena(reloc[2 * i + 1], p, p, Sandbox::Copy::StateOnly);
        }
        s.cur_ = cur;
        s.curLevel_ = level;
        return ok;
    }

    static Slot* restore(Sandbox& s, NativeCtx* c) {
        std::vector<Slot>& v = *s.nativeSaved_;
        c->resumeState = 0xFFFFFFFF;
        if (c->saveTop == 0 || c->saveTop > v.size()) {
            return v.data();
        }
        std::uint64_t meta = v[c->saveTop - 1].u;
        auto n = static_cast<std::uint32_t>(meta);
        if (n >= c->saveTop) {
            c->saveTop = 0;
            return v.data();
        }
        c->saveTop -= n + 1;
        c->resumeState = static_cast<std::uint32_t>(meta >> 32);
        return v.data() + c->saveTop;
    }
};

namespace {

Sandbox& sb(NativeCtx* c) { return *c->sandbox; }

void where(NativeCtx* c, std::uint32_t fn, std::uint32_t line) {
    c->trapFunction = fn;
    c->trapLine = line;
}

// A call that failed in a callee: the caller adds itself to the trace; a suspension is not a failure.
void qzTrace(NativeCtx* c, std::uint32_t, std::uint32_t fn, std::uint32_t line) {
    if (c->suspending == 0) {
        sb(c).addCaller(fn, line);
    }
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
    std::int64_t& fuel = NativeRuntimeAccess::fuel(sb(c));
    fuel = c->fuel;
    bool ok = sb(c).callHost(import, args, result);
    c->fuel = fuel;
    return ok ? 1 : 0;
}

std::uint32_t qzScopeIn(NativeCtx* c) {
    bool ok = sb(c).scopeIn();
    c->scopeLevel = NativeRuntimeAccess::level(sb(c));
    return ok ? 1 : 0;
}

void qzScopeOut(NativeCtx* c) {
    sb(c).scopeOut();
    c->scopeLevel = NativeRuntimeAccess::level(sb(c));
}

std::uint32_t qzPersist(NativeCtx* c, std::uint32_t type, Slot* value, std::uint32_t levels) {
    return NativeRuntimeAccess::persist(sb(c), type, value, levels) ? 1 : 0;
}

// Keeps the sandbox's fuel and the compiled code's in step around a runtime call that may charge (D34).
class FuelSync {
public:
    explicit FuelSync(NativeCtx* c) : c_(c) { NativeRuntimeAccess::fuel(sb(c)) = c->fuel; }

    FuelSync(const FuelSync&) = delete;
    FuelSync& operator=(const FuelSync&) = delete;

    ~FuelSync() { c_->fuel = NativeRuntimeAccess::fuel(sb(c_)); }

private:
    NativeCtx* c_;
};

Str* qzConcat(NativeCtx* c, const Slot* parts, std::uint32_t n) {
    FuelSync sync(c);
    return sb(c).concat(parts, n);
}

Str* qzSlice(NativeCtx* c, const Str* s, std::int64_t b, std::int64_t e) {
    FuelSync sync(c);
    return sb(c).sliceStr(s, b, e);
}

Str* qzCase(NativeCtx* c, const Str* s, std::uint32_t upper) {
    FuelSync sync(c);
    return sb(c).caseMap(s, upper != 0);
}

std::uint32_t qzCharge(NativeCtx* c, std::int64_t units) {
    FuelSync sync(c);
    return sb(c).chargeFuel(units) ? 1 : 0;
}

Str* qzFromI64(NativeCtx* c, std::int64_t v) { return sb(c).fromI64(v); }

Str* qzFromU64(NativeCtx* c, std::uint64_t v) { return sb(c).fromU64(v); }

Str* qzFromF64(NativeCtx* c, double v) { return sb(c).fromF64(v); }

Str* qzFromBool(NativeCtx* c, std::uint32_t v) { return sb(c).copyStr(v != 0 ? "true" : "false"); }

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

} // namespace

#if defined(__SSE2__)
// Candidates in [skip, 16) of the block at `base` whose first and last bytes match; -1 when none matches fully.
[[gnu::always_inline]] inline std::int64_t scanBlock(const char* base, const char* n, std::size_t nn, __m128i first,
                                                     __m128i last, std::uint32_t keep) {
    __m128i a = _mm_loadu_si128(reinterpret_cast<const __m128i*>(base));
    __m128i b = _mm_loadu_si128(reinterpret_cast<const __m128i*>(base + nn - 1));
    auto mask =
        static_cast<std::uint32_t>(_mm_movemask_epi8(_mm_and_si128(_mm_cmpeq_epi8(a, first), _mm_cmpeq_epi8(b, last))));
    mask &= keep;
    while (mask != 0) {
        auto bit = static_cast<std::size_t>(std::countr_zero(mask));
        if (std::memcmp(base + bit + 1, n + 1, nn - 2) == 0) {
            return static_cast<std::int64_t>(bit);
        }
        mask &= mask - 1;
    }
    return -1;
}

// First and last needle byte compared 16 positions at a time; same answer as std::string_view::find.
std::int64_t findBytes(const char* h, std::size_t hn, const char* n, std::size_t nn) {
    if (nn == 0) {
        return 0;
    }
    if (nn > hn) {
        return -1;
    }
    if (nn == 1) {
        const void* at = std::memchr(h, n[0], hn);
        return at == nullptr ? -1 : static_cast<const char*>(at) - h;
    }
    const __m128i first = _mm_set1_epi8(n[0]);
    const __m128i last = _mm_set1_epi8(n[nn - 1]);
    std::size_t candidates = hn - nn + 1;
    if (candidates < 16) {
        // Too short for one block: pad a copy so no load leaves the string.
        constexpr std::size_t kPad = 64;
        if (nn > kPad - 16) {
            std::size_t at = std::string_view(h, hn).find(std::string_view(n, nn));
            return at == std::string_view::npos ? -1 : static_cast<std::int64_t>(at);
        }
        char padded[kPad] = {};
        std::memcpy(padded, h, hn);
        return scanBlock(padded, n, nn, first, last, (1u << candidates) - 1);
    }
    std::size_t i = 0;
    for (; i + 16 <= candidates; i += 16) {
        if (std::int64_t r = scanBlock(h + i, n, nn, first, last, 0xFFFFu); r >= 0) {
            return static_cast<std::int64_t>(i) + r;
        }
    }
    if (i == candidates) {
        return -1;
    }
    // The last block overlaps the previous one; positions already checked are masked out.
    std::size_t start = candidates - 16;
    std::int64_t r = scanBlock(h + start, n, nn, first, last, 0xFFFFu << (i - start));
    return r < 0 ? -1 : static_cast<std::int64_t>(start) + r;
}
#else
std::int64_t findBytes(const char* h, std::size_t hn, const char* n, std::size_t nn) {
    std::size_t at = std::string_view(h, hn).find(std::string_view(n, nn));
    return at == std::string_view::npos ? -1 : static_cast<std::int64_t>(at);
}
#endif

namespace {

std::uint32_t qzStrContains(const Str* a, const Str* b) {
    return findBytes(a->data, a->len, b->data, b->len) >= 0 ? 1 : 0;
}

std::int64_t qzStrFind(const Str* a, const Str* b) { return findBytes(a->data, a->len, b->data, b->len); }

std::uint32_t qzStrStarts(const Str* a, const Str* b) { return a->view().starts_with(b->view()) ? 1 : 0; }

std::uint32_t qzStrEnds(const Str* a, const Str* b) { return a->view().ends_with(b->view()) ? 1 : 0; }

List* qzListNew(NativeCtx* c, std::uint32_t elem) { return sb(c).newList(elem); }

List* qzListFilled(NativeCtx* c, std::int64_t n, const Slot* v, std::uint32_t elem) {
    FuelSync sync(c);
    return sb(c).filledList(n, v, elem);
}

std::uint32_t qzListPush(NativeCtx* c, List* l, const Slot* v) { return sb(c).listPush(l, v) ? 1 : 0; }

std::uint32_t qzListSet(NativeCtx* c, List* l, std::int64_t i, const Slot* v) { return sb(c).listSet(l, i, v) ? 1 : 0; }

void qzListPop(NativeCtx* c, List* l, Slot* dst) { sb(c).listPop(l, dst); }

void qzListClear(NativeCtx* c, List* l) { sb(c).listClear(l); }

Str* qzJoin(NativeCtx* c, const List* l, const Str* sep) {
    FuelSync sync(c);
    return sb(c).join(l, sep);
}

Map* qzMapNew(NativeCtx* c, std::uint32_t type) { return sb(c).newMap(type); }

Slot* qzMapFind(NativeCtx* c, const Map* m, const Slot* key) { return sb(c).mapFind(m, key); }

std::uint32_t qzMapSet(NativeCtx* c, Map* m, const Slot* key, const Slot* value) {
    return sb(c).mapSet(m, key, value) ? 1 : 0;
}

std::uint32_t qzMapRemove(NativeCtx* c, Map* m, const Slot* key) { return sb(c).mapRemove(m, key) ? 1 : 0; }

std::int64_t qzMapNext(NativeCtx* c, const Map* m, std::int64_t cursor) { return sb(c).mapNext(m, cursor); }

std::uint32_t qzMapEntry(NativeCtx* c, const Map* m, std::int64_t cursor, std::uint32_t value, Slot* dst) {
    return sb(c).mapEntry(m, cursor, value != 0, dst) ? 1 : 0;
}

Str* qzFormat(NativeCtx* c, const Slot* v, std::uint32_t kind, std::int64_t spec) {
    return sb(c).formatValue(*v, kind, spec);
}

std::uint32_t qzStrDecode(NativeCtx* c, const Str* s, std::int64_t at, Slot* dst) {
    return sb(c).decodeChar(s, at, dst) ? 1 : 0;
}

std::uint32_t qzStateStore(NativeCtx* c, std::uint32_t type, const Slot* src, Slot* dst) {
    return sb(c).storeState(type, src, dst) ? 1 : 0;
}

void qzTaskWait(NativeCtx* c, std::uint32_t kind, std::uint64_t handle, std::uint32_t type) {
    NativeRuntimeAccess::wait(sb(c), kind, handle, type);
    c->suspending = 1;
}

void qzTaskSave(NativeCtx* c, const Slot* rec, std::uint32_t n, std::uint32_t state, const std::uint32_t* reloc,
                std::uint32_t count) {
    if (!NativeRuntimeAccess::save(sb(c), c, rec, n, state, reloc, count)) {
        c->suspending = 0;
    }
}

Slot* qzTaskRestore(NativeCtx* c) { return NativeRuntimeAccess::restore(sb(c), c); }

Closure* qzClosure(NativeCtx* c, std::uint32_t fn, std::uint32_t shape, const Slot* values) {
    return sb(c).newClosure(fn, shape >> 16, shape & 0xFFFF, values);
}

double qzPow(double a, double b) { return math::pow(a, b); }

double qzSin(double a) { return math::sin(a); }

double qzCos(double a) { return math::cos(a); }

std::uint32_t qzBuiltin(NativeCtx* c, std::uint32_t index, const Slot* args, Slot* out) {
    FuelSync sync(c);
    return Builtins::run(sb(c), index, args, out) ? 1 : 0;
}

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
    {"qz_scope_in", addr(qzScopeIn)},
    {"qz_scope_out", addr(qzScopeOut)},
    {"qz_persist", addr(qzPersist)},
    {"qz_pow", addr(qzPow)},
    {"qz_sin", addr(qzSin)},
    {"qz_cos", addr(qzCos)},
    {"qz_task_wait", addr(qzTaskWait)},
    {"qz_task_save", addr(qzTaskSave)},
    {"qz_task_restore", addr(qzTaskRestore)},
    {"qz_closure", addr(qzClosure)},
    {"qz_builtin", addr(qzBuiltin)},
    {"qz_tan", addr(math::tan)},
    {"qz_asin", addr(math::asin)},
    {"qz_acos", addr(math::acos)},
    {"qz_atan", addr(math::atan)},
    {"qz_exp", addr(math::exp)},
    {"qz_log", addr(math::log)},
    {"qz_log10", addr(math::log10)},
    {"qz_atan2", addr(math::atan2)},
    {"qz_charge", addr(qzCharge)},
    {"qz_trace", addr(qzTrace)},
    {"qz_map_next", addr(qzMapNext)},
    {"qz_map_entry", addr(qzMapEntry)},
    {"qz_str_decode", addr(qzStrDecode)},
    {"qz_format", addr(qzFormat)},
};

} // namespace

std::span<const NativeSymbol> nativeRuntimeSymbols() { return kSymbols; }

} // namespace quartz::vm
