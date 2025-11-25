#include "vm/Sandbox.hpp"

#include <cmath>
#include <limits>
#include <string_view>

#pragma clang diagnostic ignored "-Wgnu-label-as-value"

#ifndef QUARTZ_SWITCH_DISPATCH
#define QUARTZ_SWITCH_DISPATCH 0
#endif

namespace quartz::vm {

namespace {

Slot saturate(double d, ir::TypeId t) {
    Slot out{};
    if (std::isnan(d)) {
        out.u = 0;
        return out;
    }
    auto clampSigned = [&](double lo, double hi, std::int64_t loV, std::int64_t hiV) {
        out.i = d <= lo ? loV : d >= hi ? hiV : static_cast<std::int64_t>(d);
    };
    auto clampUnsigned = [&](double hi, std::uint64_t hiV) {
        out.u = d <= 0.0 ? 0 : d >= hi ? hiV : static_cast<std::uint64_t>(d);
    };
    switch (t) {
    case ir::kI8: clampSigned(-128.0, 127.0, -128, 127); break;
    case ir::kI16: clampSigned(-32768.0, 32767.0, -32768, 32767); break;
    case ir::kI32: clampSigned(-2147483648.0, 2147483647.0, INT32_MIN, INT32_MAX); break;
    case ir::kU8: clampUnsigned(255.0, 255); break;
    case ir::kU16: clampUnsigned(65535.0, 65535); break;
    case ir::kU32: clampUnsigned(4294967295.0, 4294967295ULL); break;
    case ir::kU64: clampUnsigned(18446744073709551616.0, UINT64_MAX); break;
    default: clampSigned(-9223372036854775808.0, 9223372036854775808.0, INT64_MIN, INT64_MAX); break;
    }
    return out;
}

Slot f32Slot(float v) {
    Slot s{};
    s.u = 0;
    s.f32 = v;
    return s;
}

double javaMin(double a, double b) {
    if (std::isnan(a) || std::isnan(b)) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    if (a == 0.0 && b == 0.0) {
        return std::signbit(a) ? a : b;
    }
    return a < b ? a : b;
}

double javaMax(double a, double b) {
    if (std::isnan(a) || std::isnan(b)) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    if (a == 0.0 && b == 0.0) {
        return std::signbit(a) ? b : a;
    }
    return a > b ? a : b;
}

int strCompare(const Str* a, const Str* b) {
    std::uint32_t n = a->len < b->len ? a->len : b->len;
    int c = std::memcmp(a->data, b->data, n);
    if (c != 0) {
        return c;
    }
    return a->len < b->len ? -1 : a->len > b->len ? 1 : 0;
}

bool strEqNullable(const Str* a, const Str* b) {
    if (a == nullptr || b == nullptr) {
        return a == b;
    }
    return strEq(a, b);
}

} // namespace

bool Sandbox::callHost(std::uint32_t import, const Slot* args, Slot* result) {
    const Binding& bnd = bindings_[import];
    HostCall hc{*this, args, {}, {}};
    if (!bnd.fn(hc, bnd.user)) {
        return fail(ir::TrapCode::HostError, std::move(hc.error));
    }
    if (trapped_) {
        return false;
    }
    if (result != nullptr) {
        *result = hc.result;
    }
    return true;
}

bool Sandbox::run(std::uint32_t function, Slot* base) {
    const Program& P = *program_;
    const Slot* K = P.constants.data();
    const CompiledFunction* fn = &P.functions[function];
    const Ins* pc = fn->code.data();
    Slot* R = base;
    Slot* const stackEnd = stack_.data() + stack_.size();
    const std::size_t entryDepth = calls_.size();
    const std::size_t maxDepth = entryDepth + limits_.maxDepth;
    std::int64_t fuel = fuel_;
    Ins ins{};

    if (R + fn->frameSlots > stackEnd) {
        fail(ir::TrapCode::StackOverflow);
        return false;
    }

#define TRAP(code, msg)                                                                                                \
    do {                                                                                                               \
        fail(code, msg);                                                                                               \
        goto trap;                                                                                                     \
    } while (0)
#define CHECK(ok)                                                                                                      \
    do {                                                                                                               \
        if (!(ok)) {                                                                                                   \
            goto trap;                                                                                                 \
        }                                                                                                              \
    } while (0)
#define WORD() (static_cast<std::uint32_t>((pc++)->bc()))
#define RA R[ins.a]
#define RB R[ins.b]
#define RC R[ins.c]
#define STR(x) static_cast<Str*>((x).p)
#define LIST(x) static_cast<List*>((x).p)
#define MAP(x) static_cast<Map*>((x).p)

#if QUARTZ_SWITCH_DISPATCH
#define CASE(name) case Opc::name:
#define NEXT() continue
    for (;;) {
        ins = *pc++;
        switch (ins.op) {
#else
    static const void* const kLabels[] = {
#define QUARTZ_OPC_LABEL(name) &&L_##name,
        QUARTZ_OPCODES(QUARTZ_OPC_LABEL)
#undef QUARTZ_OPC_LABEL
    };
#define CASE(name) L_##name:
#define NEXT()                                                                                                         \
    do {                                                                                                               \
        ins = *pc++;                                                                                                   \
        goto* kLabels[static_cast<std::size_t>(ins.op)];                                                               \
    } while (0)
    NEXT();
    {
        {
#endif
            CASE(Nop) NEXT();
            CASE(Mov) RA = RB;
            NEXT();
            CASE(MovN) std::memmove(&RA, &RB, ins.c * sizeof(Slot));
            NEXT();
            CASE(LoadK) RA = K[ins.bc()];
            NEXT();
            CASE(LoadI) RA.i = ins.bc();
            NEXT();
            CASE(Jmp) pc += ins.bc();
            NEXT();
            CASE(Loop) if (--fuel < 0) { TRAP(ir::TrapCode::OutOfFuel, {}); }
            pc += ins.bc();
            NEXT();
            CASE(JmpT) if (RA.u != 0) { pc += ins.bc(); }
            NEXT();
            CASE(JmpF) if (RA.u == 0) { pc += ins.bc(); }
            NEXT();
            CASE(LoopT) if (RA.u != 0) {
                if (--fuel < 0) {
                    TRAP(ir::TrapCode::OutOfFuel, {});
                }
                pc += ins.bc();
            }
            NEXT();
            CASE(AddI) RA.u = RB.u + RC.u;
            NEXT();
            CASE(SubI) RA.u = RB.u - RC.u;
            NEXT();
            CASE(MulI) RA.u = RB.u * RC.u;
            NEXT();
            CASE(DivI) {
                std::int64_t d = RC.i;
                if (d == 0) {
                    TRAP(ir::TrapCode::DivideByZero, {});
                }
                RA.i = (d == -1) ? static_cast<std::int64_t>(0 - RB.u) : RB.i / d;
                NEXT();
            }
            CASE(RemI) {
                std::int64_t d = RC.i;
                if (d == 0) {
                    TRAP(ir::TrapCode::DivideByZero, {});
                }
                RA.i = (d == -1) ? 0 : RB.i % d;
                NEXT();
            }
            CASE(DivU) {
                if (RC.u == 0) {
                    TRAP(ir::TrapCode::DivideByZero, {});
                }
                RA.u = RB.u / RC.u;
                NEXT();
            }
            CASE(RemU) {
                if (RC.u == 0) {
                    TRAP(ir::TrapCode::DivideByZero, {});
                }
                RA.u = RB.u % RC.u;
                NEXT();
            }
            CASE(AndI) RA.u = RB.u & RC.u;
            NEXT();
            CASE(OrI) RA.u = RB.u | RC.u;
            NEXT();
            CASE(XorI) RA.u = RB.u ^ RC.u;
            NEXT();
            CASE(ShlI) RA.u = RB.u << (RC.u & ins.pad);
            NEXT();
            CASE(ShrI) RA.i = RB.i >> (RC.u & ins.pad);
            NEXT();
            CASE(ShrU) RA.u = RB.u >> (RC.u & ins.pad);
            NEXT();
            CASE(NegI) RA.u = 0 - RB.u;
            NEXT();
            CASE(NotB) RA.u = RB.u ^ 1;
            NEXT();
            CASE(BNotI) RA.u = ~RB.u;
            NEXT();
            CASE(AddF) RA.f = RB.f + RC.f;
            NEXT();
            CASE(SubF) RA.f = RB.f - RC.f;
            NEXT();
            CASE(MulF) RA.f = RB.f * RC.f;
            NEXT();
            CASE(DivF) RA.f = RB.f / RC.f;
            NEXT();
            CASE(RemF) RA.f = std::fmod(RB.f, RC.f);
            NEXT();
            CASE(NegF) RA.f = -RB.f;
            NEXT();
            CASE(AddF32) RA = f32Slot(RB.f32 + RC.f32);
            NEXT();
            CASE(SubF32) RA = f32Slot(RB.f32 - RC.f32);
            NEXT();
            CASE(MulF32) RA = f32Slot(RB.f32 * RC.f32);
            NEXT();
            CASE(DivF32) RA = f32Slot(RB.f32 / RC.f32);
            NEXT();
            CASE(RemF32) RA = f32Slot(std::fmod(RB.f32, RC.f32));
            NEXT();
            CASE(NegF32) RA = f32Slot(-RB.f32);
            NEXT();
            CASE(EqI) RA.u = RB.u == RC.u;
            NEXT();
            CASE(NeI) RA.u = RB.u != RC.u;
            NEXT();
            CASE(LtI) RA.u = RB.i < RC.i;
            NEXT();
            CASE(LeI) RA.u = RB.i <= RC.i;
            NEXT();
            CASE(LtU) RA.u = RB.u < RC.u;
            NEXT();
            CASE(LeU) RA.u = RB.u <= RC.u;
            NEXT();
            CASE(EqF) RA.u = RB.f == RC.f;
            NEXT();
            CASE(NeF) RA.u = RB.f != RC.f;
            NEXT();
            CASE(LtF) RA.u = RB.f < RC.f;
            NEXT();
            CASE(LeF) RA.u = RB.f <= RC.f;
            NEXT();
            CASE(EqF32) RA.u = RB.f32 == RC.f32;
            NEXT();
            CASE(NeF32) RA.u = RB.f32 != RC.f32;
            NEXT();
            CASE(LtF32) RA.u = RB.f32 < RC.f32;
            NEXT();
            CASE(LeF32) RA.u = RB.f32 <= RC.f32;
            NEXT();
            CASE(EqS) RA.u = strEqNullable(STR(RB), STR(RC));
            NEXT();
            CASE(NeS) RA.u = !strEqNullable(STR(RB), STR(RC));
            NEXT();
            CASE(LtS) RA.u = strCompare(STR(RB), STR(RC)) < 0;
            NEXT();
            CASE(LeS) RA.u = strCompare(STR(RB), STR(RC)) <= 0;
            NEXT();
            CASE(EqN) {
                std::uint32_t n = WORD();
                const Slot* x = &RB;
                const Slot* y = &RC;
                bool eq = x[0].u == y[0].u;
                if (eq && x[0].u != 0) {
                    if (ins.pad == 1) {
                        eq = x[1].f == y[1].f;
                    } else if (ins.pad == 2) {
                        eq = x[1].f32 == y[1].f32;
                    } else {
                        eq = std::memcmp(x + 1, y + 1, (n - 1) * sizeof(Slot)) == 0;
                    }
                }
                RA.u = eq;
                NEXT();
            }
            CASE(Sext8) RA.i = static_cast<std::int8_t>(RB.u);
            NEXT();
            CASE(Sext16) RA.i = static_cast<std::int16_t>(RB.u);
            NEXT();
            CASE(Sext32) RA.i = static_cast<std::int32_t>(RB.u);
            NEXT();
            CASE(Zext8) RA.u = static_cast<std::uint8_t>(RB.u);
            NEXT();
            CASE(Zext16) RA.u = static_cast<std::uint16_t>(RB.u);
            NEXT();
            CASE(Zext32) RA.u = static_cast<std::uint32_t>(RB.u);
            NEXT();
            CASE(I2F) RA.f = static_cast<double>(RB.i);
            NEXT();
            CASE(U2F) RA.f = static_cast<double>(RB.u);
            NEXT();
            CASE(F2I) RA = saturate(RB.f, ins.pad);
            NEXT();
            CASE(F2U) RA = saturate(RB.f, ins.pad);
            NEXT();
            CASE(I2F32) RA = f32Slot(static_cast<float>(RB.i));
            NEXT();
            CASE(U2F32) RA = f32Slot(static_cast<float>(RB.u));
            NEXT();
            CASE(F322I) RA = saturate(static_cast<double>(RB.f32), ins.pad);
            NEXT();
            CASE(F322U) RA = saturate(static_cast<double>(RB.f32), ins.pad);
            NEXT();
            CASE(F2F32) RA = f32Slot(static_cast<float>(RB.f));
            NEXT();
            CASE(F322F) RA.f = static_cast<double>(RB.f32);
            NEXT();
            CASE(ToBool) RA.u = RB.u != 0;
            NEXT();
            CASE(IsNullP) RA.u = RB.p == nullptr;
            NEXT();
            CASE(IsNullF) RA.u = RB.u == 0;
            NEXT();
            CASE(UnwrapP) if (RB.p == nullptr) { TRAP(ir::TrapCode::NullUnwrap, {}); }
            RA = RB;
            NEXT();
            CASE(UnwrapF) if (RB.u == 0) { TRAP(ir::TrapCode::NullUnwrap, {}); }
            std::memmove(&RA, &RB + 1, ins.c * sizeof(Slot));
            NEXT();
            CASE(WrapF) std::memmove(&RA + 1, &RB, ins.c * sizeof(Slot));
            RA.u = 1;
            NEXT();
            CASE(NullP) RA.p = nullptr;
            NEXT();
            CASE(NullF) RA.u = 0;
            NEXT();
            CASE(Call) {
                const CompiledFunction* callee = &P.functions[ins.b];
                Slot* nb = R + ins.c;
                if (nb + callee->frameSlots > stackEnd || calls_.size() >= maxDepth) {
                    TRAP(ir::TrapCode::StackOverflow, {});
                }
                if (--fuel < 0) {
                    TRAP(ir::TrapCode::OutOfFuel, {});
                }
                calls_.push_back({fn, pc, R, ins.a});
                fn = callee;
                pc = fn->code.data();
                R = nb;
                NEXT();
            }
            CASE(CallHost) {
                fuel_ = fuel;
                CHECK(callHost(ins.b, R + ins.c, P.importResultSlots[ins.b] != 0 ? &RA : nullptr));
                NEXT();
            }
            CASE(Ret) {
                Slot* src = &RA;
                std::uint32_t n = ins.b;
                if (calls_.size() == entryDepth) {
                    std::memmove(base, src, n * sizeof(Slot));
                    fuel_ = fuel;
                    return true;
                }
                CallRecord r = calls_.back();
                calls_.pop_back();
                std::memmove(r.base + r.dst, src, n * sizeof(Slot));
                fn = r.fn;
                pc = r.pc;
                R = r.base;
                NEXT();
            }
            CASE(Ret0) {
                if (calls_.size() == entryDepth) {
                    fuel_ = fuel;
                    return true;
                }
                CallRecord r = calls_.back();
                calls_.pop_back();
                fn = r.fn;
                pc = r.pc;
                R = r.base;
                NEXT();
            }
            CASE(Trap) TRAP(static_cast<ir::TrapCode>(ins.a), {});
            CASE(SGet) RA = static_cast<Slot*>(RB.p)[ins.c];
            NEXT();
            CASE(SGetN) {
                std::uint32_t n = WORD();
                std::memcpy(&RA, static_cast<Slot*>(RB.p) + ins.c, n * sizeof(Slot));
                NEXT();
            }
            CASE(SSet) static_cast<Slot*>(RA.p)[ins.c] = RB;
            NEXT();
            CASE(SSetR) {
                ir::TypeId t = WORD();
                CHECK(storeState(t, &RB, static_cast<Slot*>(RA.p) + ins.c));
                NEXT();
            }
            CASE(ListNew) {
                ir::TypeId t = WORD();
                List* l = newList(t);
                CHECK(l != nullptr);
                RA.p = l;
                NEXT();
            }
            CASE(ListFilled) {
                ir::TypeId t = WORD();
                List* l = filledList(RB.i, &RC, t);
                CHECK(l != nullptr);
                RA.p = l;
                NEXT();
            }
            CASE(ListLen) RA.i = LIST(RB)->len;
            NEXT();
            CASE(ListGet) {
                const List* l = LIST(RB);
                if (RC.u >= l->len) {
                    TRAP(ir::TrapCode::IndexOutOfBounds, {});
                }
                RA = l->data[RC.u];
                NEXT();
            }
            CASE(ListGetN) {
                const List* l = LIST(RB);
                if (RC.u >= l->len) {
                    TRAP(ir::TrapCode::IndexOutOfBounds, {});
                }
                std::memcpy(&RA, l->data + RC.u * l->elemSlots, l->elemSlots * sizeof(Slot));
                NEXT();
            }
            CASE(ListSet) {
                List* l = LIST(RA);
                if (RB.u >= l->len) {
                    TRAP(ir::TrapCode::IndexOutOfBounds, {});
                }
                l->data[RB.u] = RC;
                NEXT();
            }
            CASE(ListSetN) {
                List* l = LIST(RA);
                if (l->elemSlots == 1 && !(l->owner == Owner::State && l->elemHasRefs)) {
                    if (RB.u >= l->len) {
                        TRAP(ir::TrapCode::IndexOutOfBounds, {});
                    }
                    l->data[RB.u] = RC;
                } else {
                    CHECK(listSet(l, RB.i, &RC));
                }
                NEXT();
            }
            CASE(ListPush) CHECK(listPush(LIST(RA), &RB));
            NEXT();
            CASE(ListPop) listPop(LIST(RB), &RA);
            NEXT();
            CASE(ListClear) listClear(LIST(RA));
            NEXT();
            CASE(ListJoin) {
                Str* s = join(LIST(RB), STR(RC));
                CHECK(s != nullptr);
                RA.p = s;
                NEXT();
            }
            CASE(MapNew) {
                ir::TypeId t = WORD();
                Map* m = newMap(t);
                CHECK(m != nullptr);
                RA.p = m;
                NEXT();
            }
            CASE(MapLen) RA.i = MAP(RB)->count;
            NEXT();
            CASE(MapGet) {
                const Map* m = MAP(RB);
                const Slot* e = mapFind(m, &RC);
                if (m->valuePtrNullable) {
                    RA.p = e != nullptr ? e[1].p : nullptr;
                } else if (e == nullptr) {
                    RA.u = 0;
                } else {
                    RA.u = 1;
                    std::memcpy(&RA + 1, e + 1, m->valueSlots * sizeof(Slot));
                }
                NEXT();
            }
            CASE(MapSet) CHECK(mapSet(MAP(RA), &RB, &RC));
            NEXT();
            CASE(MapHas) RA.u = mapFind(MAP(RB), &RC) != nullptr;
            NEXT();
            CASE(MapRemove) RA.u = mapRemove(MAP(RB), &RC);
            NEXT();
            CASE(StrLen) RA.i = STR(RB)->len;
            NEXT();
            CASE(StrConcat) {
                Str* s = concat(&RB, ins.c);
                CHECK(s != nullptr);
                RA.p = s;
                NEXT();
            }
            CASE(StrSlice) {
                Str* s = sliceStr(STR(RB), (&RC)[0].i, (&RC)[1].i);
                CHECK(s != nullptr);
                RA.p = s;
                NEXT();
            }
            CASE(StrLower) {
                Str* s = caseMap(STR(RB), false);
                CHECK(s != nullptr);
                RA.p = s;
                NEXT();
            }
            CASE(StrUpper) {
                Str* s = caseMap(STR(RB), true);
                CHECK(s != nullptr);
                RA.p = s;
                NEXT();
            }
            CASE(StrContains) RA.u = STR(RB)->view().find(STR(RC)->view()) != std::string_view::npos;
            NEXT();
            CASE(StrFind) {
                std::size_t at = STR(RB)->view().find(STR(RC)->view());
                Slot* d = &RA;
                d[0].u = at != std::string_view::npos;
                d[1].i = at != std::string_view::npos ? static_cast<std::int64_t>(at) : 0;
                NEXT();
            }
            CASE(StrStarts) RA.u = STR(RB)->view().starts_with(STR(RC)->view());
            NEXT();
            CASE(StrEnds) RA.u = STR(RB)->view().ends_with(STR(RC)->view());
            NEXT();
            CASE(StrByteAt) {
                const Str* s = STR(RB);
                if (RC.u >= s->len) {
                    TRAP(ir::TrapCode::IndexOutOfBounds, {});
                }
                RA.i = static_cast<unsigned char>(s->data[RC.u]);
                NEXT();
            }
            CASE(StrFromI) {
                Str* s = fromI64(RB.i);
                CHECK(s != nullptr);
                RA.p = s;
                NEXT();
            }
            CASE(StrFromU) {
                Str* s = fromU64(RB.u);
                CHECK(s != nullptr);
                RA.p = s;
                NEXT();
            }
            CASE(StrFromF) {
                Str* s = fromF64(RB.f);
                CHECK(s != nullptr);
                RA.p = s;
                NEXT();
            }
            CASE(StrFromB) {
                Str* s = newStr(RB.u != 0 ? "true" : "false");
                CHECK(s != nullptr);
                RA.p = s;
                NEXT();
            }
            CASE(StrFromC) {
                Str* s = fromChar(static_cast<std::uint32_t>(RB.u));
                CHECK(s != nullptr);
                RA.p = s;
                NEXT();
            }
            CASE(Sqrt) RA.f = std::sqrt(RB.f);
            NEXT();
            CASE(Floor) RA.f = std::floor(RB.f);
            NEXT();
            CASE(Ceil) RA.f = std::ceil(RB.f);
            NEXT();
            CASE(AbsF) RA.f = std::fabs(RB.f);
            NEXT();
            CASE(AbsI) RA.u = RB.i < 0 ? 0 - RB.u : RB.u;
            NEXT();
            CASE(MinI) RA.i = RB.i < RC.i ? RB.i : RC.i;
            NEXT();
            CASE(MaxI) RA.i = RB.i > RC.i ? RB.i : RC.i;
            NEXT();
            CASE(MinF) RA.f = javaMin(RB.f, RC.f);
            NEXT();
            CASE(MaxF) RA.f = javaMax(RB.f, RC.f);
            NEXT();
            CASE(Pow) RA.f = std::pow(RB.f, RC.f);
            NEXT();
            CASE(Sin) RA.f = std::sin(RB.f);
            NEXT();
            CASE(Cos) RA.f = std::cos(RB.f);
            NEXT();
            CASE(Panic) TRAP(ir::TrapCode::Panic, std::string(STR(RA)->view()));
#if QUARTZ_SWITCH_DISPATCH
        case Opc::Count: break;
        }
    }
#else
        }
    }
#endif

trap:
    fuel_ = fuel;
    trap_.function = fn->name;
    {
        auto at = static_cast<std::size_t>(pc - fn->code.data());
        trap_.line = at > 0 && at - 1 < fn->lines.size() ? fn->lines[at - 1] : 0;
    }
    calls_.resize(entryDepth);
    return false;
}

} // namespace quartz::vm
