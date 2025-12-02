#pragma once

#include "ir/Ir.hpp"
#include "vm/Layout.hpp"
#include "vm/Value.hpp"

#include <memory>
#include <string>
#include <vector>

namespace quartz::vm {

// a, b, c are frame slot indices unless noted; jump offsets are relative to the next instruction.
#define QUARTZ_OPCODES(X)                                                                                              \
    X(Nop)                                                                                                             \
    X(Mov)       /* a = b */                                                                                           \
    X(MovN)      /* a..a+c = b..b+c */                                                                                 \
    X(LoadK)     /* a = constants[bc] */                                                                               \
    X(LoadI)     /* a = sign-extended imm32 bc */                                                                      \
    X(Jmp)       /* pc += bc */                                                                                        \
    X(Loop)      /* pc += bc (backward), charges fuel */                                                               \
    X(JmpT)      /* if a: pc += bc */                                                                                  \
    X(JmpF)      /* if !a: pc += bc */                                                                                 \
    X(LoopT)     /* if a: pc += bc (backward), charges fuel */                                                         \
    X(JLtI) X(JLeI) X(JEqI) X(JNeI) X(JLtU) X(JLeU) /* if a op b: pc += int16 c */                                     \
    X(JLtF) X(JLeF) X(JEqF) X(JNeF) X(JNLtF) X(JNLeF) /* negative offsets charge fuel */                              \
    X(ForI)      /* a += 1; if a < b: fuel, pc += int16 c */                                                           \
    X(ForIncl)   /* if a < b: a += 1, fuel, pc += int16 c */                                                           \
    X(ForList)   /* a += 1; if a < list(b).len: fuel, pc += int16 c */                                                 \
    X(AddI) X(SubI) X(MulI) X(DivI) X(RemI) X(DivU) X(RemU) X(AndI) X(OrI) X(XorI) X(ShlI) X(ShrI) X(ShrU)          \
    X(NegI) X(NotB) X(BNotI)                                                                                           \
    X(AddF) X(SubF) X(MulF) X(DivF) X(RemF) X(NegF)                                                                    \
    X(AddF32) X(SubF32) X(MulF32) X(DivF32) X(RemF32) X(NegF32)                                                        \
    X(EqI) X(NeI) X(LtI) X(LeI) X(LtU) X(LeU)                                                                          \
    X(EqF) X(NeF) X(LtF) X(LeF) X(EqF32) X(NeF32) X(LtF32) X(LeF32)                                                    \
    X(EqS) X(NeS) X(LtS) X(LeS)                                                                                        \
    X(EqN)       /* a = (b..b+n == c..c+n) bitwise over flagged nullables; n in next word */                          \
    X(Sext8) X(Sext16) X(Sext32) X(Zext8) X(Zext16) X(Zext32)                                                          \
    X(I2F) X(U2F) X(F2I) X(F2U) X(I2F32) X(U2F32) X(F322I) X(F322U) X(F2F32) X(F322F) X(ToBool)                       \
    X(IsNullP)   /* a = b == null */                                                                                   \
    X(IsNullF)   /* a = flag slot b == 0 */                                                                            \
    X(UnwrapP)   /* a = b, trap if null */                                                                             \
    X(UnwrapF)   /* a..a+c = b+1..; trap if flag b == 0 */                                                             \
    X(WrapF)     /* a = 1; a+1..a+1+c = b.. */                                                                         \
    X(NullP)     /* a = null */                                                                                        \
    X(NullF)     /* flag a = 0 */                                                                                      \
    X(Call)      /* a = dst, b = function, c = first argument slot; callee frame starts at c */                       \
    X(CallHost)  /* a = dst, b = import, c = first argument slot */                                                    \
    X(Ret)       /* return slots a..a+n */                                                                             \
    X(Ret0)                                                                                                            \
    X(Trap)      /* a = code */                                                                                        \
    X(SGet)      /* a = state(b).slot[c] */                                                                            \
    X(SGetN)     /* a..+n = state(b).slot[c..]; n in next word */                                                      \
    X(SSet)      /* state(a).slot[c] = b, scalars */                                                                   \
    X(SSetR)     /* state(a).slot[c..] = deep copy of b..; type in next word */                                        \
    X(ListNew)   /* a = new list; type in next word */                                                                 \
    X(ListFilled) /* a = list of b copies of slots c..; type in next word */                                          \
    X(ListLen)                                                                                                         \
    X(ListGet)   /* a = list(b)[c] for one-slot elements */                                                            \
    X(ListGetN)  /* a.. = list(b)[c] */                                                                                \
    X(ListSet)   /* list(a)[b] = c, one slot, no refs or arena owned */                                                \
    X(ListGetB) X(ListSetB) /* packed `[bool]` lists */                                                                \
    X(ListSetN)  /* list(a)[b] = c.., generic */                                                                       \
    X(ListPush)  /* list(a).push(b..) */                                                                               \
    X(ListPop)   /* a = list(b).pop() as nullable */                                                                   \
    X(ListClear)                                                                                                       \
    X(ListJoin)  /* a = join(list b, sep c) */                                                                         \
    X(MapNew)    /* a = new map; type in next word */                                                                  \
    X(MapLen)                                                                                                          \
    X(MapGet)    /* a = map(b).get(c) as nullable */                                                                   \
    X(MapSet)    /* map(a).set(b, c..) */                                                                              \
    X(MapHas)                                                                                                          \
    X(MapRemove)                                                                                                       \
    X(StrLen) X(StrConcat) /* a = concat of c strings at b.. */                                                        \
    X(StrSlice)  /* a = b[c .. c+1] */                                                                                 \
    X(StrLower) X(StrUpper) X(StrContains) X(StrFind) X(StrStarts) X(StrEnds) X(StrByteAt)                            \
    X(StrFromI) X(StrFromU) X(StrFromF) X(StrFromB) X(StrFromC)                                                        \
    X(Sqrt) X(Floor) X(Ceil) X(AbsF) X(AbsI) X(MinI) X(MaxI) X(MinF) X(MaxF) X(Pow) X(Sin) X(Cos)                       \
    X(Panic)                                                                                                           \
    X(Wait)      /* suspend the task on handle a; resume value goes to b unless 0xFFFF; c = kind; type in next word */ \
    X(ScopeIn) X(ScopeOut)                                                                                             \
    X(Zero)      /* slots a..a+c = 0 */                                                                                \
    X(Persist)   /* copy what slot a references out of inner regions, c levels up; type in next word */

enum class Opc : std::uint8_t {
#define QUARTZ_OPC_ENUM(name) name,
    QUARTZ_OPCODES(QUARTZ_OPC_ENUM)
#undef QUARTZ_OPC_ENUM
        Count
};

struct Ins {
    Opc op;
    std::uint8_t pad = 0;
    std::uint16_t a = 0;
    std::uint16_t b = 0;
    std::uint16_t c = 0;

    [[nodiscard]] std::int32_t bc() const {
        return static_cast<std::int32_t>(static_cast<std::uint32_t>(b) | (static_cast<std::uint32_t>(c) << 16));
    }
    void setBc(std::int32_t v) {
        auto u = static_cast<std::uint32_t>(v);
        b = static_cast<std::uint16_t>(u & 0xFFFF);
        c = static_cast<std::uint16_t>(u >> 16);
    }
};
static_assert(sizeof(Ins) == 8);

struct CompiledFunction {
    std::string name;
    std::vector<Ins> code;
    std::uint32_t paramSlots = 0;
    std::uint32_t frameSlots = 0; // registers plus the outgoing argument area
    std::uint32_t resultSlots = 0;
    std::vector<std::uint32_t> lines; // source line per instruction
    std::vector<ir::TypeId> params;
    ir::TypeId result = ir::kVoid;
    bool exported = false;
    bool isSuspend = false;
    std::vector<std::pair<std::uint32_t, ir::TypeId>> refSlots; // slots that may hold references, for task suspension
};

/// A validated module lowered to bytecode; immutable and shareable between sandboxes.
struct Program {
    std::unique_ptr<ir::Module> module;
    std::unique_ptr<Layouts> layouts;
    std::vector<CompiledFunction> functions;
    std::vector<Slot> constants;
    std::vector<std::unique_ptr<std::byte[]>> staticStrings;
    std::uint32_t maxFrameSlots = 0;
    std::vector<std::uint32_t> importResultSlots;

    [[nodiscard]] std::uint32_t findFunction(std::string_view name) const;
};

const char* opcodeName(Opc op);

inline bool isShortJump(Opc op) { return op >= Opc::JLtI && op <= Opc::ForList; }

/// Lowers a validated module. The module must have passed ir::validate.
std::unique_ptr<Program> compile(std::unique_ptr<ir::Module> module);

std::string disassemble(const Program& p, std::uint32_t function);

} // namespace quartz::vm
