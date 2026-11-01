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
    If, // a = bool condition
    Else,
    End,
    Br,     // imm = depth; to a Loop it continues, to a Block/If it exits
    BrIf,   // a = bool, imm = depth
    Return, // a = value or kNoReg
    Trap,   // imm = TrapCode
    // data
    Const, // dst, imm = constant index
    Move,  // dst, a
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
    Wait,       // dst (optional, value given on resume), a = awaited handle, imm = WaitKind; suspend functions only
    ScopeEnter, // opens an arena region
    ScopeExit,  // closes the innermost region; registers [a, b) are cleared and count as unwritten after it
    Persist,    // a = value: copies what it references from inner regions into the region imm levels out
    Closure,    // dst = function imm as a value; args are captured and fill its last parameters
    CallValue,  // dst, a = function value, args
    ArrayGet,   // dst, a = fixed-size array, b = i64 index: traps when out of bounds
    ArraySet,   // a = fixed-size array register, b = value, args = [i64 index]: traps when out of bounds
};

enum class WaitKind : std::uint8_t { Wait, AtEnter, AtLeave, Yield };

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
    ListNew,    // dst [T]
    ListFilled, // dst [T], args: count i64, value T
    ListLen,    // dst i64, args: list
    ListGet,    // dst T, args: list, index
    ListSet,    // args: list, index, value
    ListPush,   // args: list, value
    ListPop,    // dst T?, args: list
    ListClear,  // args: list
    ListJoin,   // dst str, args: [str], sep
    MapNew,     // dst Map<K,V>
    MapLen,     // dst i64, args: map
    MapGet,     // dst V?, args: map, key
    MapSet,     // args: map, key, value
    MapHas,     // dst bool, args: map, key
    MapRemove,  // dst bool, args: map, key
    StrLen,     // dst i64 (bytes), args: str
    StrConcat,  // dst str, args: str...
    StrSlice,   // dst str, args: str, begin, end (byte offsets, checked)
    StrLower,
    StrUpper,
    StrContains, // dst bool
    StrFind,     // dst i64? byte offset
    StrStartsWith,
    StrEndsWith,
    StrByteAt, // dst i64
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
    // Runtime builtins, StrToInt to ListSort: one implementation in vm/*Builtins.cpp that both tiers call.
    StrToInt,            // dst int?, args: str
    StrToFloat,          // dst float?, args: str
    StrReplace,          // dst str, args: str, from, to
    StrSplit,            // dst [str], args: str, separator
    StrTrim,             // dst str, args: str
    StrTrimStart,        // dst str, args: str
    StrTrimEnd,          // dst str, args: str
    StrRepeat,           // dst str, args: str, count
    StrEqualsIgnoreCase, // dst bool, args: str, str
    StrChars,            // dst [char], args: str
    CharFromInt,         // dst char?, args: int
    ListInsert,          // args: list, index, value
    ListRemoveAt,        // dst T, args: list, index
    ListContains,        // dst bool, args: list, value
    ListIndexOf,         // dst int?, args: list, value
    ListReverse,         // args: list
    ListSlice,           // dst [T], args: list, begin, end
    ListInsertAll,       // args: list, index, [T]
    ListSort,            // args: list
    CharCode,            // dst int, args: char
    MathTan,             // dst float, args: float (also MathAsin to MathRound)
    MathAsin,
    MathAcos,
    MathAtan,
    MathExp,
    MathLog,
    MathLog10,
    MathRound,
    MathAtan2,           // dst float, args: y, x
    MathFmod,            // dst float, args: x, y
    MathClampI,          // dst int, args: value, lo, hi
    MathClampF,          // dst float, args: value, lo, hi
    ListSortByKeys,      // args: list, keys (same length, comparable): reorders the list stably by its keys
    IntCheckedAdd,       // dst T?, args: a, b, the type id of T (an integer type): null on overflow
    IntCheckedSub,
    IntCheckedMul,
    IntCheckedDiv,       // also null when dividing by zero
    IntSaturatingAdd,    // dst T, args: a, b, the type id of T: clamped to T's range
    IntSaturatingSub,
    IntSaturatingMul,
    MapNext,    // dst i64 cursor of the next entry in insertion order or -1, args: map, cursor (-1 to start)
    MapKeyAt,   // dst K, args: map, cursor; traps if the map gained or lost a key since the cursor was made
    MapValueAt, // dst V, args: map, cursor; same check
    StrDecode,  // dst (char, i64): the scalar at a byte index and the index after it, args: str, index
    StrFormat,  // dst str, args: value (i64, u64, f64 or str), spec i64 packed as Sandbox::formatValue reads it
    Count,
};

const char* intrinsicName(Intrinsic in);

struct Inst {
    Op op;
    std::uint16_t column = 0; // where the statement or match arm starts, for debug probes; not serialized
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

enum class Inlining : std::uint8_t { Default, Always, Never };

/// A source name of a register for debuggers, in scope from instruction `from` up to (not including) `to`.
struct LocalName {
    Reg reg = 0;
    std::string name;
    std::uint32_t from = 0;
    std::uint32_t to = 0;
};

/// What the source says about who may call a function: nothing, `private`, `internal` or `pub`.
enum class FnVisibility : std::uint8_t { Default, Private, Internal, Public };

struct Function {
    std::string name;
    std::string package;        // of the declaring file; empty without a `package` line
    std::string file;           // source file of the code, for trap traces; empty when lines are stripped
    std::vector<TypeId> regs;   // regs[0 .. paramCount) are the parameters
    std::uint32_t paramCount = 0;
    TypeId result = kVoid;
    std::vector<Inst> code;
    std::vector<Reg> args;      // argument lists referenced by Inst::argBegin
    bool exported = false;      // callable by the host
    bool isSuspend = false;
    Inlining inlining = Inlining::Default; // `@inline` / `@noinline`, hints for the native tier (D71)
    std::vector<LocalName> locals;         // parameters and locals by name; dropped with the lines
    FnVisibility visibility = FnVisibility::Default;
    std::vector<std::string> paramNames{}; // exported functions: every parameter's name, kept without the lines
};

struct HostImport {
    std::string name; // `package.fn` or `package.Type.member`, without `package.` when the `.qzi` has none
    std::vector<TypeId> params;
    TypeId result = kVoid;
    std::string package{}; // a prefix of `name`; binding by the rest works while it is unambiguous
};

/// One argument of a declaration attribute, kept for the host (`@command("heal", perm = "example.heal")`).
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

/// What generated interfaces repeat of a declaration: its `///` text and the defaults written as literals.
struct DeclDocs {
    std::uint32_t index = 0;           // the function, or the type id
    std::string doc;                   // `///` lines without the markers, joined by newlines
    std::vector<std::string> members;  // per field or variant: its `///` text; empty when none has one
    std::vector<std::string> defaults; // per parameter or field: a literal default's source text, or empty
};

struct Module {
    TypeTable types;
    std::vector<Annotation> annotations;
    std::vector<Constant> constants;
    std::vector<Function> functions;
    std::vector<HostImport> imports;
    std::vector<TypeId> states;         // state types; one instance each per sandbox
    std::vector<std::uint32_t> stateInit; // per state: function index that fills its defaults, or ~0
    std::string manifest;                 // `package.toml` text of the package it was built from, if any
    std::vector<DeclDocs> functionDocs;   // exported functions with a doc or a default, by index
    std::vector<DeclDocs> typeDocs;       // structs and enums with a doc or a default, by type id
};

std::string print(const Module& m);

} // namespace quartz::ir
