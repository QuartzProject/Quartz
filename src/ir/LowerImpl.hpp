#pragma once

#include "ir/Lower.hpp"

#include <map>
#include <unordered_map>
#include <tuple>

namespace quartz::ir {

using namespace syntax;

class Lowerer {
public:
    Lowerer(sema::Program& prog, const SourceManager& sources, Diagnostics& diags);

    std::unique_ptr<Module> run();

    // Lower.cpp: functions, statements, control flow
    void lowerFunction(sema::FnInfo* fn, Function& out);
    void lowerStateInit(TypeId state, Function& out);
    void lowerFilter(sema::AnnotationInfo& a, sema::FnInfo* fn, Function& out);
    void lowerBlockStmts(BlockExpr* block);
    Reg lowerBlockValue(BlockExpr* block, TypeId want);
    void lowerStmt(Stmt* s);
    void lowerLet(LetStmt* s);
    void lowerScoped(BlockExpr* body);
    sema::Local* rootLocal(Expr* target);
    void lowerAssign(AssignStmt* s);
    void assignPlace(Expr* target, Reg value);
    void lowerWhile(WhileStmt* s);
    void lowerFor(ForStmt* s);
    Reg lowerIf(IfExpr* e, TypeId want, bool wantValue);
    Reg lowerMatch(MatchExpr* m, TypeId want, bool wantValue);
    Reg lowerArmBody(Expr* body, TypeId want, bool wantValue);
    void branchIfFalse(Expr* e, std::size_t frame);
    void branchIfTrue(Expr* e, std::size_t frame);
    Reg patternTest(Pattern* p, Reg subject, TypeId subjectType);
    void bindPattern(Pattern* p, Reg subject, TypeId subjectType);
    Reg zeroValue(TypeId t, Span span);

    // LowerLambda.cpp
    struct LambdaWork {
        LambdaExpr* expr;
        ir::TypeSubst subst;
        std::uint32_t index;
        std::string name;
    };

    std::vector<LambdaWork> lambdaWork_;
    Reg lowerClosure(LambdaExpr* e);
    Reg lowerFunctionValue(IdentExpr* e);
    Reg lowerIndirectCall(CallExpr* e, Reg dst);
    void lowerLambda(const LambdaWork& w);
    [[nodiscard]] bool signatureHasFn(const Function& f) const;

    // LowerExpr.cpp
    Reg lowerExpr(Expr* e, TypeId want = kError);
    Reg lowerInner(Expr* e);
    Reg lowerBinary(BinaryExpr* e);
    Reg lowerCall(CallExpr* e);
    Reg lowerCallCore(CallExpr* e);
    std::unordered_map<const Expr*, Reg> preLowered_;
    Reg lowerMember(MemberExpr* e);
    Reg lowerInterp(InterpExpr* e);
    Reg toStr(Reg r, TypeId t);
    Reg coerce(Reg r, TypeId from, TypeId to);
    Op binaryOp(BinaryOp op) const;
    // LowerBuiltins.cpp
    void lowerAssert(CallExpr* e);
    TypeId builtinElementParam(CallExpr* e, Intrinsic op, const Expr* arg);

    // emission
    Reg newReg(TypeId t);
    TypeId T(TypeId t) { return types_->subst(t, subst_); }
    std::uint32_t allocFunction();
    std::uint32_t instance(sema::FnInfo* fn, const std::vector<TypeId>& args);
    Function& function(std::uint32_t index);
    ir::TypeSubst subst_;
    std::uint32_t nextFn_ = 0;
    std::map<std::uint32_t, Function> extra_; // functions created while lowering (stable addresses)
    std::map<std::pair<sema::FnInfo*, std::vector<TypeId>>, std::uint32_t> instances_;
    std::vector<std::tuple<sema::FnInfo*, std::vector<TypeId>, std::uint32_t>> work_;
    Inst& emit(Op op, Reg dst = kNoReg, Reg a = kNoReg, Reg b = kNoReg, std::uint32_t imm = 0);
    Inst& emitArgs(Op op, Reg dst, std::uint32_t imm, const std::vector<Reg>& args);
    Reg constInt(std::int64_t v, TypeId t);
    Reg constFloat(double v, TypeId t);
    Reg constStr(std::string_view s);
    void open(Op op, Reg cond = kNoReg);
    void close();
    void br(std::size_t frame);
    void brIf(Reg cond, std::size_t frame);
    void setLine(Span span);

    sema::Program& prog_;
    const SourceManager& sources_;
    Diagnostics& diags_;
    std::unique_ptr<Module> mod_;
    TypeTable* types_ = nullptr;
    Function* fn_ = nullptr;
    sema::FnInfo* info_ = nullptr;
    std::uint32_t line_ = 0;
    std::size_t depth_ = 0; // open structured frames
    struct LoopTarget {
        std::size_t breakFrame;
        std::size_t continueFrame;
        std::size_t scopes; // open `scope`s when the loop started
    };
    struct OpenScope {
        Reg firstReg;
        std::vector<std::size_t> exits; // ScopeExit instructions whose register range ends at the scope's end
    };
    std::vector<OpenScope> scopes_;
    Reg retReg_ = kNoReg; // created before any scope so a return value survives the scope exits
    void enterScope();
    void exitScope();
    void exitScopesTo(std::size_t depth);
    Reg persistTo(Reg value, TypeId type, std::size_t depth);
    std::vector<LoopTarget> loops_;
    std::map<std::tuple<int, std::uint64_t, TypeId, std::string>, std::uint32_t> constIndex_;
};

} // namespace quartz::ir
