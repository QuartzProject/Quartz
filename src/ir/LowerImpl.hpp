#pragma once

#include "ir/Lower.hpp"

#include <map>
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
    void lowerBlockStmts(BlockExpr* block);
    Reg lowerBlockValue(BlockExpr* block, TypeId want);
    void lowerStmt(Stmt* s);
    void lowerLet(LetStmt* s);
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

    // LowerExpr.cpp
    Reg lowerExpr(Expr* e, TypeId want = kError);
    Reg lowerInner(Expr* e);
    Reg lowerBinary(BinaryExpr* e);
    Reg lowerCall(CallExpr* e);
    Reg lowerMember(MemberExpr* e);
    Reg lowerInterp(InterpExpr* e);
    Reg toStr(Reg r, TypeId t);
    Reg coerce(Reg r, TypeId from, TypeId to);
    Op binaryOp(BinaryOp op) const;

    // emission
    Reg newReg(TypeId t);
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
    };
    std::vector<LoopTarget> loops_;
    std::map<std::tuple<int, std::uint64_t, TypeId, std::string>, std::uint32_t> constIndex_;
};

} // namespace quartz::ir
