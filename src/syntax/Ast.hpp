#pragma once

#include "support/Interner.hpp"
#include "support/Source.hpp"

#include <cstdint>
#include <span>
#include <string_view>

namespace quartz::sema {
struct Local;
struct FnInfo;
struct GlobalInfo;
} // namespace quartz::sema

namespace quartz::syntax {

// ---------------------------------------------------------------- types

enum class TypeExprKind : std::uint8_t { Named, List, Array, Tuple, Nullable, Fn };

struct TypeExpr {
    TypeExprKind kind;
    Span span;
};

struct NamedTypeExpr : TypeExpr {
    std::span<Symbol> path;
    std::span<TypeExpr*> args;
};

struct ListTypeExpr : TypeExpr {
    TypeExpr* elem;
};

struct ArrayTypeExpr : TypeExpr {
    TypeExpr* elem;
    struct Expr* size;
};

struct TupleTypeExpr : TypeExpr {
    std::span<TypeExpr*> elems;
};

struct NullableTypeExpr : TypeExpr {
    TypeExpr* inner;
};

struct FnTypeExpr : TypeExpr {
    std::span<TypeExpr*> params;
    TypeExpr* result; // null means void
};

// ---------------------------------------------------------------- expressions

enum class ExprKind : std::uint8_t {
    IntLit,
    FloatLit,
    BoolLit,
    NullLit,
    CharLit,
    StringLit,
    Interp,
    Ident,
    SelfRef,
    Unary,
    Binary,
    Call,
    Member,
    Index,
    Propagate,
    Range,
    ListLit,
    ListRepeat,
    TupleLit,
    Lambda,
    Cast,
    Is,
    Wait,
    If,
    Match,
    Block,
};

enum class UnaryOp : std::uint8_t { Neg, Not, BitNot };

enum class BinaryOp : std::uint8_t {
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
    And,
    Or,
    Coalesce,
};

const char* binaryOpText(BinaryOp op);

struct Expr {
    ExprKind kind;
    Span span;
    std::uint32_t type = 0; // ir::TypeId filled by sema; 0 is the error type
};

struct IntLitExpr : Expr {
    std::uint64_t value;
};

struct FloatLitExpr : Expr {
    double value;
};

struct BoolLitExpr : Expr {
    bool value;
};

struct NullLitExpr : Expr {};

struct CharLitExpr : Expr {
    std::uint32_t value;
};

struct StringLitExpr : Expr {
    std::string_view value;
};

struct InterpExpr : Expr {
    std::span<std::string_view> texts; // texts.size() == exprs.size() + 1
    std::span<Expr*> exprs;
};

enum class IdentRef : std::uint8_t { Unresolved, Local, Global, Function, Type, Const, Module };

struct IdentExpr : Expr {
    Symbol name;
    std::span<struct TypeExpr*> typeArgs; // turbofish `::<T>`
    IdentRef ref = IdentRef::Unresolved;
    sema::Local* local = nullptr;
    sema::FnInfo* fn = nullptr;
    sema::GlobalInfo* global = nullptr;
};

struct SelfExpr : Expr {
    sema::Local* local = nullptr;
};

struct UnaryExpr : Expr {
    UnaryOp op;
    Expr* operand;
};

struct BinaryExpr : Expr {
    BinaryOp op;
    Expr* lhs;
    Expr* rhs;
};

struct Arg {
    Symbol name; // invalid when positional
    Expr* value;
};

enum class CallKind : std::uint8_t { Unresolved, Function, Host, Construct, Variant, Builtin, Special, Indirect };

/// Calls lowered to small IR sequences instead of an intrinsic.
enum class SpecialOp : std::uint8_t { ResultIsOk, ResultIsErr, ResultUnwrap, ResultUnwrapOr, NullUnwrap, NullUnwrapOr };

struct CallExpr : Expr {
    Expr* callee;
    std::span<Arg> args;
    CallKind callKind = CallKind::Unresolved;
    sema::FnInfo* fn = nullptr;
    std::span<std::uint32_t> typeArgs; // generic calls: the callee's type arguments (may contain parameters)
    bool traitCall = false;            // resolved per instantiation from the receiver's concrete type
    bool safeCall = false;             // `a?.m()`: the receiver is nullable and `type` is the nullable result
    std::uint32_t innerType = 0;       // safe calls: the method's own result type
    std::uint16_t builtin = 0;  // ir::Intrinsic for CallKind::Builtin
    std::uint32_t variant = 0;  // CallKind::Variant
    Expr* receiver = nullptr; // for methods: the object
    std::span<Expr*> ordered; // args in parameter order after sema (defaults filled)
};

struct MemberExpr : Expr {
    Expr* object;
    Symbol name;
    bool safe; // `?.`
    std::uint32_t fieldIndex = 0;
    bool isProperty = false; // host handle property, read through import `getter`
    bool isVariant = false;  // `Enum.Variant` without payload; fieldIndex holds the variant
    std::uint32_t getter = 0;
};

struct IndexExpr : Expr {
    Expr* object;
    Expr* index;
};

struct PropagateExpr : Expr {
    Expr* inner;
};

struct RangeExpr : Expr {
    Expr* lo;
    Expr* hi;
    bool inclusive;
};

struct ListLitExpr : Expr {
    std::span<Expr*> elems;
};

struct ListRepeatExpr : Expr {
    Expr* value;
    Expr* count;
};

struct TupleLitExpr : Expr {
    std::span<Expr*> elems;
};

struct Param {
    Symbol name;
    Span span;
    TypeExpr* type; // may be null for lambdas
    Expr* defaultValue;
    sema::Local* local = nullptr;
};

struct LambdaExpr : Expr {
    std::span<Param> params;
    Expr* body; // block or expression
};

struct CastExpr : Expr {
    Expr* value;
    TypeExpr* target;
};

struct IsExpr : Expr {
    Expr* value;
    TypeExpr* target;
};

struct WaitExpr : Expr {
    Expr* value;
};

struct Stmt;

struct BlockExpr : Expr {
    std::span<Stmt*> stmts;
};

struct IfExpr : Expr {
    Expr* cond;           // null for `if let`
    Symbol bindName;      // `if let name = value`
    Expr* bindValue;
    sema::Local* bindLocal = nullptr;
    BlockExpr* then;
    Expr* otherwise; // BlockExpr, IfExpr or null
};

enum class PatternKind : std::uint8_t { Wildcard, Binding, Literal, Variant, Range };

struct Pattern {
    PatternKind kind;
    Span span;
    Symbol name;                    // binding name, or variant name
    std::span<Symbol> path;         // `Enum.Variant`
    std::span<struct Pattern*> sub; // variant payload patterns
    Expr* literal;                  // literal or range lo
    Expr* rangeHi;
    bool inclusive = false; // `a..=b`; `a..b` excludes b, as in `for`
    sema::Local* local = nullptr;
    std::uint32_t variantIndex = 0;
};

struct MatchArm {
    Pattern* pattern;
    Expr* guard;
    Expr* body;
};

struct MatchExpr : Expr {
    Expr* subject;
    std::span<MatchArm> arms;
};

// ---------------------------------------------------------------- statements

enum class StmtKind : std::uint8_t {
    Expr,
    Let,
    Assign,
    While,
    For,
    Return,
    Break,
    Continue,
    Scope,
    Every,
    At,
};

enum class AssignOp : std::uint8_t { Set, Add, Sub, Mul, Div, Rem, BitAnd, BitOr, BitXor, Shl, Shr };

struct Stmt {
    StmtKind kind;
    Span span;
};

struct ExprStmt : Stmt {
    Expr* expr;
};

struct LetStmt : Stmt {
    bool mutable_;
    bool isConst;
    bool isRef;
    Symbol name;
    Span nameSpan;
    TypeExpr* type;
    Expr* init;
    sema::Local* local = nullptr;
};

struct AssignStmt : Stmt {
    AssignOp op;
    Expr* target;
    Expr* value;
};

struct WhileStmt : Stmt {
    Expr* cond;
    BlockExpr* body;
};

struct ForStmt : Stmt {
    Symbol name;
    Span nameSpan;
    Symbol indexName; // `for i, x in list`
    Expr* iterable;
    BlockExpr* body;
    sema::Local* local = nullptr;
    sema::Local* indexLocal = nullptr;
};

struct ReturnStmt : Stmt {
    Expr* value;
};

struct ScopeStmt : Stmt {
    BlockExpr* body;
};

struct EveryStmt : Stmt {
    Expr* interval;
    BlockExpr* body;
};

struct AtStmt : Stmt {
    Expr* context;
    BlockExpr* body;
};

// ---------------------------------------------------------------- declarations

enum class DeclKind : std::uint8_t { Fn, Struct, State, Enum, Impl, Trait, Const };

enum class Visibility : std::uint8_t { Private, Internal, Pub };

struct AttrArg {
    Symbol name;
    Expr* value;
};

struct Attribute {
    Symbol name;
    Span span;
    std::span<AttrArg> args;
};

struct Decl {
    DeclKind kind;
    Span span;
    Symbol name;
    Visibility visibility;
    std::span<Attribute> attrs;
};

struct TypeParam {
    Symbol name;
    Span span;
    std::span<Symbol> bounds; // `T: Show + Eq`
};

struct FnDecl : Decl {
    bool isSuspend;
    bool hasSelf;
    std::span<TypeParam> typeParams;
    std::span<Param> params;
    TypeExpr* result;
    BlockExpr* body; // null for host declarations in `.qzi`
    sema::FnInfo* info = nullptr;
};

struct FieldDecl {
    Symbol name;
    Span span;
    TypeExpr* type;
    Expr* defaultValue;
    std::span<Attribute> attrs;
};

struct StructDecl : Decl {
    std::span<TypeParam> typeParams;
    std::span<FieldDecl> fields;
};

struct Variant {
    Symbol name;
    Span span;
    std::span<TypeExpr*> payload;
};

struct EnumDecl : Decl {
    std::span<TypeParam> typeParams;
    std::span<Variant> variants;
};

struct ImplDecl : Decl {
    std::span<TypeParam> typeParams;
    TypeExpr* target;
    TypeExpr* trait;
    std::span<FnDecl*> methods;
};

struct TraitDecl : Decl {
    std::span<FnDecl*> methods;
};

struct ConstDecl : Decl {
    TypeExpr* type;
    Expr* value;
    sema::GlobalInfo* info = nullptr;
};

struct Import {
    Span span;
    std::span<Symbol> path;
    std::span<Symbol> names; // `{a, b}`; empty with `star == false` imports the last path element
    bool star;
    Symbol alias;
};

struct Module {
    const SourceFile* file;
    std::span<Symbol> package;
    std::span<Import> imports;
    std::span<Decl*> decls;
};

} // namespace quartz::syntax
