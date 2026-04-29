#pragma once

#include "ir/Ir.hpp"
#include "sema/Sema.hpp"

#include <map>
#include <set>
#include <optional>

namespace quartz::sema {

inline constexpr std::string_view kMathPackage = "org.quartzlang.math";
inline constexpr std::string_view kListsPackage = "org.quartzlang.lists";

/// The math functions added after sqrt..max (CheckBuiltins.cpp).
bool isExtraMathFunction(std::string_view name);

using namespace syntax;

struct TypeEntry {
    TypeId type = ir::kError;
    StructDecl* decl = nullptr;
    EnumDecl* enumDecl = nullptr;
};

struct BuiltinSig {
    ir::Intrinsic op;
    std::vector<TypeId> params; // excluding the receiver
    TypeId result;
};

class Checker {
public:
    Checker(std::span<Module*> modules, Interner& names, Arena& arena, Diagnostics& diags);

    std::unique_ptr<Program> run();

    // Declare.cpp
    void declareTypes();
    void completeTypes();
    void declareFunctions();
    struct ImplParams {
        std::vector<TypeId> ids;
        std::vector<Symbol> names;
        std::vector<std::vector<TraitInfo*>> bounds;
    };
    FnInfo* declareFn(FnDecl* fn, TypeId owner, bool inInterface, const ImplParams& impl = {});
    std::unordered_map<TypeId, std::vector<std::pair<Symbol, TypeId>>> baseParams_;
    syntax::StructDecl* structDeclOf(TypeId t);
    TypeId resolveType(TypeExpr* t);
    TypeId resolveTypeExpr(TypeExpr* t);
    TypeId lookupTypeName(Symbol name, Span span, bool report);
    bool hasAttr(std::span<Attribute> attrs, std::string_view name) const;

    // CheckStmt.cpp
    void checkFunction(FnInfo* fn);
    void checkAttributes(FnInfo* fn);
    bool annotationArg(Expr* e, ir::AnnotationArg& out);
    TypeId checkBlock(BlockExpr* block, TypeId expected, bool wantValue);
    void checkStmt(Stmt* s);
    void checkLet(LetStmt* s);
    void checkAssign(AssignStmt* s);
    void checkFor(ForStmt* s);
    TypeId checkMatch(MatchExpr* m, TypeId expected, bool wantValue);
    void checkPattern(Pattern* p, TypeId subject);
    bool isPlace(Expr* e, bool& mutableOut);

    // CheckExpr.cpp
    TypeId check(Expr* e, TypeId expected = ir::kError);
    TypeId checkValue(Expr* e, TypeId expected);
    TypeId checkInner(Expr* e, TypeId expected);
    TypeId checkIdent(IdentExpr* e, TypeId expected);
    TypeId checkBinary(BinaryExpr* e, TypeId expected);
    TypeId checkUnary(UnaryExpr* e, TypeId expected);
    TypeId checkIf(IfExpr* e, TypeId expected, bool wantValue);
    TypeId checkCast(CastExpr* e);
    TypeId checkIndex(IndexExpr* e);
    TypeId checkInterp(InterpExpr* e);
    TypeId checkWait(Expr* operand, Span span);
    bool derivedFromState(const Expr* e) const;
    bool holdsMutable(TypeId t) const;
    void checkViewBinding(Local* l, const Expr* init, Span span);
    bool assignable(TypeId from, TypeId to) const;
    TypeId unify(TypeId a, TypeId b, Span span);
    void expectAssignable(TypeId from, TypeId to, Span span);
    static bool isUntypedLiteral(const Expr* e);

    // CheckCall.cpp
    TypeId checkCall(CallExpr* e, TypeId expected);
    TypeId checkMember(MemberExpr* e, TypeId expected);
    TypeId checkMethodCall(CallExpr* call, MemberExpr* callee, TypeId expected);
    TypeId checkBuiltinMethod(CallExpr* call, TypeId recvType, Symbol name, TypeId expected);
    bool checkSetter(AssignStmt* s, TypeId type);
    TypeId checkBuiltinFunction(CallExpr* call, Symbol name, TypeId expected, bool& found, bool qualified = false);
    TypeId checkConstruct(CallExpr* call, TypeId type);
    TypeId finishCall(CallExpr* call, FnInfo* fn, Expr* receiver);
    bool bindArgs(CallExpr* call, const std::vector<TypeId>& params, const std::vector<Symbol>& names,
                  const std::vector<Expr*>& defaults, std::size_t skip);
    std::optional<BuiltinSig> builtinMethod(TypeId recv, std::string_view name);
    // CheckBuiltins.cpp
    std::optional<BuiltinSig> extraBuiltinMethod(TypeId recv, std::string_view name);
    bool checkExtraBuiltinFunction(CallExpr* call, std::string_view name, TypeId expected, TypeId& result);
    /// `xs.map(f)` and the other higher order list methods: calls to org.quartzlang.lists with the list first.
    bool checkListFunctionMethod(CallExpr* call, TypeId recv, Symbol name, TypeId& result);

    // Generics.cpp
    void declareTraits();
    void declareImplTrait(ImplDecl* impl, TypeId owner, const std::vector<FnInfo*>& methods);
    void pushTypeParams(std::span<TypeParam> params, std::vector<TypeId>& ids, std::vector<Symbol>& names,
                        std::vector<std::vector<TraitInfo*>>& bounds);
    bool unify(TypeId pattern, TypeId actual, ir::TypeSubst& bindings);
    TypeId finishGenericCall(CallExpr* call, FnInfo* fn, Expr* receiver, ir::TypeSubst bindings);
    bool implements(TypeId type, TraitInfo* trait);
    TypeId checkTraitCall(CallExpr* call, MemberExpr* callee, TypeId recv);
    TypeId inferInstance(TypeId base, const std::vector<TypeId>& patterns, std::span<Arg> args, TypeId expected,
                         Span span);
    std::vector<std::unordered_map<Symbol, TypeId>> typeScopes_;
    std::unordered_map<TypeId, std::vector<TraitInfo*>> paramBounds_;
    std::vector<std::pair<TraitDecl*, TraitInfo*>> traitList_;
    std::map<std::pair<TypeId, std::uint32_t>, FnInfo*> genericMethods_; // (generic base, method)
    std::set<std::pair<TraitInfo*, TypeId>> traitImpls_;                // (trait, concrete type or base)

    // Lambdas.cpp
    struct LambdaScope {
        FnInfo* info;
        std::vector<Local*> captures;
        bool inferResult;
        bool sawReturn;
    };

    /// What a generic call already knows about a lambda argument; `unknown` lists the callee's unbound parameters.
    struct LambdaHint {
        std::vector<TypeId> params;
        TypeId result = ir::kError;
        std::vector<TypeId> unknown;
    };

    std::vector<LambdaScope> lambdas_;
    std::optional<LambdaHint> lambdaHint_;
    TypeId checkLambda(LambdaExpr* e, TypeId expected);
    bool lambdaReturn(ReturnStmt* r);
    TypeId checkFunctionValue(IdentExpr* e, FnInfo* fn, TypeId expected);
    TypeId checkIndirectCall(CallExpr* call, TypeId fnType);
    TypeId fnField(CallExpr* call, MemberExpr* callee, TypeId recv);
    void noteCapture(Local* l);
    Local* assignedLocal(Expr* target) const;
    void checkCapturedAssign(Expr* target, Span span);
    void checkFunctionTypes();
    bool mentions(TypeId t, const std::vector<TypeId>& ids) const;

    // scopes
    void pushScope() { scopes_.emplace_back(); }
    void popScope() { scopes_.pop_back(); }
    Local* declareLocal(Symbol name, TypeId type, bool mutable_, Span span);
    Local* findLocal(Symbol name);

    std::string typeName(TypeId t) const { return prog_->types.name(t); }
    std::string_view str(Symbol s) const { return names_.name(s); }

    std::span<Module*> modules_;
    Interner& names_;
    Arena& arena_;
    Diagnostics& diags_;
    std::unique_ptr<Program> prog_;
    ir::TypeTable* types_ = nullptr;

    // Names.cpp: one table for every top-level name, filtered by what the current file can see.
    enum class NameKind : std::uint8_t { Type, Function, Global, Trait };
    struct NameEntry {
        NameKind kind;
        void* target;
        std::uint32_t file;
        Visibility vis;
    };
    struct FileScope {
        std::string package;
        bool interface = false;
        Module* module = nullptr;
        std::vector<std::string> starImports;
        std::unordered_map<Symbol, std::pair<std::string, Symbol>> symbolImports; // local name -> (package, name)
        std::unordered_map<Symbol, std::string> packageImports;                   // `import a.b.c` -> c names a.b.c
    };
    void indexFiles();
    void addName(Symbol name, NameKind kind, void* target, const Decl* decl, std::uint32_t file);
    bool visibleFrom(const NameEntry& e, Symbol name, std::uint32_t file) const;
    bool imported(const NameEntry& e, Symbol alias, std::uint32_t file) const;
    void* findName(Symbol name, NameKind kind, Span span = {});
    void* findIn(const std::string& package, Symbol name, NameKind kind);
    bool qualifier(std::span<const Symbol> path, std::string& out) const;
    TypeId qualifiedType(const Expr* e);
    bool mathImported(Symbol name, std::string_view& original) const;
    bool packagePath(const Expr* e, std::string& out);
    TypeEntry* findType(Symbol n) { return static_cast<TypeEntry*>(findName(n, NameKind::Type)); }
    FnInfo* findFunction(Symbol n) { return static_cast<FnInfo*>(findName(n, NameKind::Function)); }
    GlobalInfo* findGlobal(Symbol n) { return static_cast<GlobalInfo*>(findName(n, NameKind::Global)); }
    TraitInfo* findTrait(Symbol n) { return static_cast<TraitInfo*>(findName(n, NameKind::Trait)); }
    std::unordered_map<Symbol, std::vector<NameEntry>> table_;
    std::vector<FileScope> files_;
    std::set<std::string> packages_;
    std::uint32_t currentFile_ = 0;
    std::vector<std::unique_ptr<TypeEntry>> typeEntries_;
    std::vector<GlobalInfo*> globalList_;
    std::map<std::pair<TypeId, std::uint32_t>, FnInfo*> methods_;
    std::map<std::pair<TypeId, std::uint32_t>, Property> properties_;
    std::unordered_map<TypeId, TypeId> awaitable_; // handle type -> value produced by `wait`
    int atDepth_ = 0;
    std::size_t atScopeBase_ = 0;
    std::vector<std::unordered_map<Symbol, Local*>> scopes_;
    FnInfo* currentFn_ = nullptr;
    TypeId selfType_ = ir::kError;
    int loopDepth_ = 0;
    std::set<const Expr*> defaultsInProgress_;

    // Loops of a suspend function that `wait` and allocate outside a `scope` grow the task's arena (spec 5, D36).
    struct LoopWatch {
        int scopeLevel;
        bool waits = false;
        Span firstAlloc{};
        bool allocates = false;
    };

    std::vector<LoopWatch> loopWatch_;
    int scopeLevel_ = 0;
    void noteAllocation(const Expr* e, TypeId t);
    void watchLoop(BlockExpr* body);
};

} // namespace quartz::sema
