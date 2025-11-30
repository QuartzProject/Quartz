#pragma once

#include "ir/Types.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace quartz::ir {

using Reg = std::uint32_t;
inline constexpr Reg kNoReg = 0xFFFFFFFF;

/// Structured, typed, register based. Blocks nest like Wasm: Block/Loop/If ... End; Br targets by depth.
enum class Op : std::uint8_t {
    // structure
    Block,
    Loop,
    If,     // a = bool condition
    Else,
    End,
    Br,     // imm = depth; to a Loop it continues, to a Block/If it exits
    BrIf,   // a = bool, imm = depth
    Return, // a = value or kNoReg
    Trap,   // imm = TrapCode
    // data
    Const,  // dst, imm = constant index
    Move,   // dst, a
    Neg,
    Not,
    BitNot,
    Add,
    Sub,
    Mul,
    Div,
    Rem,
    BitAnd,
    BitOr,
    BitXor,
    Shl,
    Shr,
    Eq,
    Ne,
    Lt,
    Le,
    Gt,
    Ge,
    Convert,    // dst, a: numeric conversion with Rust `as` semantics
    Call,       // dst, imm = function index, args
    CallHost,   // dst, imm = import index, args
    Intrinsic,  // dst, imm = Intrinsic, args
    FieldGet,   // dst, a = struct/tuple/state value, imm = field
    FieldSet,   // a = struct/tuple register or state, imm = field, b = value
    Make,       // dst = struct or tuple built from args in field order
    IsNull,     // dst bool, a nullable
    Unwrap,     // dst, a nullable: traps when null
    Wrap,       // dst nullable, a inner value
    NullOf,     // dst nullable set to null
    EnumMake,   // dst, imm = variant, args = payload
    EnumTag,    // dst i64, a enum
    EnumField,  // dst, a enum, imm = variant, b = payload index
};

enum class TrapCode : std::uint8_t {
    Unreachable,
    DivideByZero,
    IndexOutOfBounds,
    NullUnwrap,
    OutOfFuel,
    OutOfMemory,
    StackOverflow,
    InvalidHandle,
    Panic,
    HostError,
};

const char* trapName(TrapCode code);

enum class Intrinsic : std::uint16_t {
    ListNew,      // dst [T]
    ListFilled,   // dst [T], args: count i64, value T
    ListLen,      // dst i64, args: list
    ListGet,      // dst T, args: list, index
    ListSet,      // args: list, index, value
    ListPush,     // args: list, value
    ListPop,      // dst T?, args: list
    ListClear,    // args: list
    ListJoin,     // dst str, args: [str], sep
    MapNew,       // dst Map<K,V>
    MapLen,       // dst i64, args: map
    MapGet,       // dst V?, args: map, key
    MapSet,       // args: map, key, value
    MapHas,       // dst bool, args: map, key
    MapRemove,    // dst bool, args: map, key
    StrLen,       // dst i64 (bytes), args: str
    StrConcat,    // dst str, args: str...
    StrSlice,     // dst str, args: str, begin, end (byte offsets, checked)
    StrLower,
    StrUpper,
    StrContains,  // dst bool
    StrFind,      // dst i64? byte offset
    StrStartsWith,
    StrEndsWith,
    StrByteAt,    // dst i64
    StrFromI64,
    StrFromU64,
    StrFromF64,
    StrFromBool,
    StrFromChar,
    MathSqrt,
    MathFloor,
    MathCeil,
    MathAbsF,
    MathAbsI,
    MathMinI,
    MathMaxI,
    MathMinF,
    MathMaxF,
    MathPow,
    MathSin,
    MathCos,
    Panic,        // args: str message
    Count,
};

const char* intrinsicName(Intrinsic in);

struct Inst {
    Op op;
    Reg dst = kNoReg;
    Reg a = kNoReg;
    Reg b = kNoReg;
    std::uint32_t imm = 0;
    std::uint32_t argBegin = 0;
    std::uint32_t argCount = 0;
    std::uint32_t line = 0;
};

enum class ConstKind : std::uint8_t { Int, Float, Str };

struct Constant {
    ConstKind kind;
    TypeId type;
    std::uint64_t bits = 0; // Int: value; Float: IEEE bits
    std::string text;       // Str
};

struct Function {
    std::string name;
    std::vector<TypeId> regs;   // regs[0 .. paramCount) are the parameters
    std::uint32_t paramCount = 0;
    TypeId result = kVoid;
    std::vector<Inst> code;
    std::vector<Reg> args;      // argument lists referenced by Inst::argBegin
    bool exported = false;      // callable by the host
    bool isSuspend = false;
};

struct HostImport {
    std::string name; // `Type.member` for handle members, plain name otherwise
    std::vector<TypeId> params;
    TypeId result = kVoid;
};

/// One argument of a declaration attribute, kept for the host (`@command("heal", perm = "basalt.heal")`).
struct AnnotationArg {
    enum class Kind : std::uint8_t { Int, Float, Bool, Str, Name };
    std::string key; // empty for positional arguments
    Kind kind = Kind::Name;
    std::int64_t intValue = 0;
    double floatValue = 0;
    std::string text; // Str value or dotted Name
};

/// A function attribute the host reads to register handlers (`@on`, `@command`, ...).
struct Annotation {
    std::string name;
    std::uint32_t function = 0;
    std::vector<AnnotationArg> args;
    std::uint32_t filter = ~0U; // `@on(..., where = expr)`: exported function taking the first parameter
};

struct Module {
    TypeTable types;
    std::vector<Annotation> annotations;
    std::vector<Constant> constants;
    std::vector<Function> functions;
    std::vector<HostImport> imports;
    std::vector<TypeId> states;         // state types; one instance each per sandbox
    std::vector<std::uint32_t> stateInit; // per state: function index that fills its defaults, or ~0
};

std::string print(const Module& m);

} // namespace quartz::ir
