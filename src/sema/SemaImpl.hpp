#pragma once

#include "ir/Ir.hpp"
#include "sema/Sema.hpp"

#include <map>
#include <optional>

namespace quartz::sema {

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
    void declareFn(FnDecl* fn, TypeId owner, bool inInterface);
    TypeId resolveType(TypeExpr* t);
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
    bool assignable(TypeId from, TypeId to) const;
    TypeId unify(TypeId a, TypeId b, Span span);
    void expectAssignable(TypeId from, TypeId to, Span span);
    static bool isUntypedLiteral(const Expr* e);

    // CheckCall.cpp
    TypeId checkCall(CallExpr* e, TypeId expected);
    TypeId checkMember(MemberExpr* e, TypeId expected);
    TypeId checkMethodCall(CallExpr* call, MemberExpr* callee, TypeId expected);
    TypeId checkBuiltinMethod(CallExpr* call, TypeId recvType, Symbol name, TypeId expected);
    TypeId checkBuiltinFunction(CallExpr* call, Symbol name, TypeId expected, bool& found);
    TypeId checkConstruct(CallExpr* call, TypeId type);
    TypeId finishCall(CallExpr* call, FnInfo* fn, Expr* receiver);
    bool bindArgs(CallExpr* call, const std::vector<TypeId>& params, const std::vector<Symbol>& names,
                  const std::vector<Expr*>& defaults, std::size_t skip);
    std::optional<BuiltinSig> builtinMethod(TypeId recv, std::string_view name);

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

    std::unordered_map<Symbol, TypeEntry> typeNames_;
    std::unordered_map<Symbol, FnInfo*> functions_;
    std::unordered_map<Symbol, GlobalInfo*> globals_;
    std::map<std::pair<TypeId, std::uint32_t>, FnInfo*> methods_;
    std::map<std::pair<TypeId, std::uint32_t>, Property> properties_;
    std::vector<std::unordered_map<Symbol, Local*>> scopes_;
    FnInfo* currentFn_ = nullptr;
    TypeId selfType_ = ir::kError;
    int loopDepth_ = 0;
};

} // namespace quartz::sema
