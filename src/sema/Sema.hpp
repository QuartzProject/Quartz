#pragma once

#include "ir/Ir.hpp"
#include "ir/Types.hpp"
#include "support/Diagnostics.hpp"
#include "support/Interner.hpp"
#include "syntax/Ast.hpp"

#include <map>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace quartz::sema {

using ir::TypeId;

/// A value computed by the compiler for a `const` (D143).
struct ConstValue {
    enum class Kind : std::uint8_t { Int, Float, Str, Null, Some, Aggregate, Variant };
    Kind kind = Kind::Int;
    TypeId type = ir::kError;
    std::uint64_t bits = 0; // Int: integers, bool and char, normalized to the type; Float: the double's bits
    std::string text;
    std::vector<ConstValue> parts; // Some: the value; Aggregate: fields or elements; Variant: payload
    std::uint32_t variant = 0;
};

struct Local {
    Symbol name;
    TypeId type = ir::kError;
    bool mutable_ = false;
    bool stateView = false; // holds a value read from a state
    Span span;
    std::uint32_t reg = ~0U; // assigned by lowering
    std::uint32_t scopeDepth = 0; // `scope` nesting where it was declared, set by lowering
    std::uint32_t lambdaDepth = 0; // lambdas around its declaration; a deeper lambda captures it
    bool captured = false;
    bool param = false;      // a function or lambda parameter, for the unused warnings
    std::uint32_t reads = 0; // uses as a value; assignments to it count in `writes`
    std::uint32_t writes = 0;
    std::shared_ptr<const ConstValue> constant; // a local `const`
};

struct AnnotationInfo {
    std::string name;
    std::vector<ir::AnnotationArg> args;
    syntax::Expr* where = nullptr;
    Local* whereParam = nullptr;
};

struct TraitInfo;

/// Traits the compiler knows: operators, interpolation, map keys and `for` use them.
enum class LangTrait : std::uint8_t { None, Eq, Hash, Ord, Show, Iterator, Decode, Encode };

struct FnInfo {
    syntax::FnDecl* decl = nullptr;
    std::vector<TypeId> typeParams;          // impl parameters first, then the function's own
    std::vector<Symbol> typeParamNames;
    std::vector<std::vector<TraitInfo*>> bounds; // per type parameter
    TraitInfo* trait = nullptr;              // set on the abstract methods of a trait declaration
    LangTrait derived = LangTrait::None;     // made by `@derive`: no declaration, lowering builds the body
    [[nodiscard]] bool generic() const { return !typeParams.empty(); }
    std::vector<AnnotationInfo> annotations;
    std::string name;            // `Type.method` for methods; host functions start with their package
    std::string package;         // of the declaring file
    TypeId owner = ir::kError;   // receiver type for methods
    bool hasSelf = false;
    bool isHost = false;         // declared without a body in a `.qzi`
    bool isSuspend = false;
    ir::Inlining inlining = ir::Inlining::Default;
    bool isProperty = false; // `@property` host method read without parentheses
    bool deprecated = false; // `@deprecated("why")`: callers get a warning with `deprecation`
    std::string deprecation;
    std::vector<TypeId> params;  // includes the receiver first when hasSelf
    std::vector<Symbol> paramNames;
    std::vector<syntax::Expr*> defaults;
    TypeId result = ir::kVoid;
    std::uint32_t index = ~0U;   // function index, or import index for host functions
    std::vector<Local*> paramLocals;
    Local* selfLocal = nullptr;
    std::uint32_t uses = 0; // calls and references from other functions, for the unused private warning
};

struct TraitInfo {
    std::string name;
    LangTrait lang = LangTrait::None;
    TypeId self = ir::kError; // the `Self` parameter in method signatures
    std::vector<FnInfo*> methods;
};

struct GlobalInfo {
    syntax::ConstDecl* decl = nullptr;
    TypeId type = ir::kError;
    bool checking = false;
    bool evaluating = false;
    bool evaluated = false;
    std::shared_ptr<const ConstValue> value; // null until evaluated, or when the initializer is not constant
};

/// A handle property maps a field of a `@handle struct` to a host getter.
struct Property {
    std::uint32_t getter = ~0U; // import index
    TypeId type = ir::kError;
};

/// What `@derive(Decode, Encode)` bodies use from org.quartzlang.data; zero until a program derives or calls them.
struct CodecInfo {
    TypeId value = 0;
    TypeId error = 0;
    FnInfo* mismatch = nullptr;                      // (expected: str, found: Value) -> DecodeError
    FnInfo* missing = nullptr;                       // (key: str, expected: str) -> DecodeError
    FnInfo* atKey = nullptr;                         // (e: DecodeError, key: str) -> DecodeError
    FnInfo* atIndex = nullptr;                       // (e: DecodeError, index: int) -> DecodeError
    std::map<TypeId, std::vector<std::string>> keys; // per struct (generic base) deriving them: each field's key
};

struct Program {
    ir::TypeTable types;
    std::vector<FnInfo*> functions; // non-generic functions with bodies, in index order
    std::vector<FnInfo*> generics;  // generic functions with bodies, instantiated by lowering
    /// Methods by receiver: a concrete type, or the generic base of an instance type.
    std::map<std::pair<TypeId, std::string>, FnInfo*> methodTable;
    std::vector<std::unique_ptr<TraitInfo>> traitStorage;
    std::vector<FnInfo*> imports;   // host functions and handle getters, in import order
    std::vector<TypeId> states;
    std::unordered_map<TypeId, syntax::StructDecl*> structDecls;
    std::unordered_map<TypeId, syntax::EnumDecl*> enumDecls;
    std::vector<std::unique_ptr<FnInfo>> fnStorage;
    std::vector<std::unique_ptr<Local>> localStorage;
    std::vector<std::unique_ptr<GlobalInfo>> globalStorage;
    const Interner* names = nullptr; // local names for the IR's debug table
    CodecInfo codec;
};

/// Resolves names and types over all modules; returns null only if nothing could be analyzed.
std::unique_ptr<Program> analyze(std::span<syntax::Module*> modules, Interner& names, Arena& arena,
                                 Diagnostics& diags);

} // namespace quartz::sema
