#pragma once

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

struct Local {
    Symbol name;
    TypeId type = ir::kError;
    bool mutable_ = false;
    bool stateView = false; // holds a value read from a state
    Span span;
    std::uint32_t reg = ~0U; // assigned by lowering
    std::uint32_t scopeDepth = 0; // `scope` nesting where it was declared, set by lowering
};

struct AnnotationInfo {
    std::string name;
    std::vector<ir::AnnotationArg> args;
    syntax::Expr* where = nullptr;
    Local* whereParam = nullptr;
};

struct TraitInfo;

struct FnInfo {
    syntax::FnDecl* decl = nullptr;
    std::vector<TypeId> typeParams;          // impl parameters first, then the function's own
    std::vector<Symbol> typeParamNames;
    std::vector<std::vector<TraitInfo*>> bounds; // per type parameter
    TraitInfo* trait = nullptr;              // set on the abstract methods of a trait declaration
    [[nodiscard]] bool generic() const { return !typeParams.empty(); }
    std::vector<AnnotationInfo> annotations;
    std::string name;            // `Type.method` for methods
    TypeId owner = ir::kError;   // receiver type for methods
    bool hasSelf = false;
    bool isHost = false;         // declared without a body in a `.qzi`
    bool isSuspend = false;
    bool isProperty = false; // `@property` host method read without parentheses
    std::vector<TypeId> params;  // includes the receiver first when hasSelf
    std::vector<Symbol> paramNames;
    std::vector<syntax::Expr*> defaults;
    TypeId result = ir::kVoid;
    std::uint32_t index = ~0U;   // function index, or import index for host functions
    std::vector<Local*> paramLocals;
    Local* selfLocal = nullptr;
};

struct TraitInfo {
    std::string name;
    TypeId self = ir::kError; // the `Self` parameter in method signatures
    std::vector<FnInfo*> methods;
};

struct GlobalInfo {
    syntax::ConstDecl* decl = nullptr;
    TypeId type = ir::kError;
    bool checking = false;
};

/// A handle property maps a field of a `@handle struct` to a host getter.
struct Property {
    std::uint32_t getter = ~0U; // import index
    TypeId type = ir::kError;
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
    std::vector<std::unique_ptr<FnInfo>> fnStorage;
    std::vector<std::unique_ptr<Local>> localStorage;
    std::vector<std::unique_ptr<GlobalInfo>> globalStorage;
};

/// Resolves names and types over all modules; returns null only if nothing could be analyzed.
std::unique_ptr<Program> analyze(std::span<syntax::Module*> modules, Interner& names, Arena& arena,
                                 Diagnostics& diags);

} // namespace quartz::sema
