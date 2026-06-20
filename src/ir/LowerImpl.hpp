#pragma once

#include "ir/Lower.hpp"

#include <functional>
#include <map>
#include <tuple>
#include <unordered_map>

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
    void lowerStmtInner(Stmt* s);
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
    // LowerFor.cpp
    void lowerForEach(ForStmt* s, TypeId t);
    Reg lowerMapList(CallExpr* e, Reg dst);

    // LowerPattern.cpp
    void matchPattern(Pattern* p, Reg subject, TypeId type, std::size_t fail);
    Reg lowerIs(IsExpr* e);
    void lowerLetPattern(LetStmt* s);
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

    // LowerTraits.cpp: Eq, Ord and Show for any type, and the bodies of derived methods
    [[nodiscard]] bool vmEquatable(TypeId t) const;
    [[nodiscard]] bool vmOrdered(TypeId t) const;
    sema::FnInfo* userMethod(TypeId t, std::string_view name, std::vector<TypeId>& targs) const;
    Reg callMethod(sema::FnInfo* fn, const std::vector<TypeId>& targs, const std::vector<Reg>& args, TypeId result);
    Reg equalsOf(Reg a, Reg b, TypeId t);
    Reg compareOf(Reg a, Reg b, TypeId t);
    Reg signOf(Reg lt, Reg gt);
    Reg showOf(Reg r, TypeId t);
    Reg concatParts(const std::vector<Reg>& parts);
    Reg listLength(Reg list);
    Reg listAt(Reg list, Reg index, TypeId elem);
    /// Emits `body(i)` for i in 0..n.
    template <class Body>
    void forEachIndex(Reg n, Body&& body);
    Reg formatted(Reg v, TypeId t, std::uint64_t spec);
    Reg langTraitCall(CallExpr* e, TypeId recvType, Reg dst);
    Reg lowerTraitCompare(BinaryOp op, Reg l, Reg r, TypeId t);
    void lowerDerived(sema::FnInfo* fn, Function& out);

    // LowerCodec.cpp, LowerCodecTypes.cpp: Decode and Encode, one function per type to and from data.Value
    struct CodecWork {
        TypeId type;
        bool encode;
        std::uint32_t index;
    };

    std::vector<CodecWork> codecWork_;
    std::map<std::pair<TypeId, bool>, std::uint32_t> codecFns_;
    std::uint32_t codecFunction(TypeId t, bool encode);
    Reg decodeCall(Reg value, TypeId t);
    Reg encodeCall(Reg x, TypeId t);
    Reg lowerCodecCall(CallExpr* e, Reg dst);
    [[nodiscard]] std::string codecName(TypeId t) const;
    [[nodiscard]] std::vector<std::string> codecKeys(TypeId t) const;
    void returnResult(Reg x, bool ok);
    Reg callHelper(sema::FnInfo* fn, const std::vector<Reg>& args);
    void failDecode(const std::string& expected, Reg value);
    Reg valueTagIs(Reg tag, std::uint32_t k);
    Reg valuePayload(Reg value, std::uint32_t k, TypeId t, std::uint32_t index = 0);
    Reg decoded(Reg result, TypeId t, const std::function<Reg(Reg)>& wrap);
    Reg atKey(Reg e, Reg key);
    Reg atIndex(Reg e, Reg index);
    Reg makeValue(std::uint32_t tag, Reg payload);
    void eachEntry(Reg table, const std::function<void(Reg, Reg)>& body);
    void lowerCodec(const CodecWork& w);
    void lowerCodecDerived(sema::FnInfo* fn, Function& out);
    void decodeBuiltin(Reg v, TypeId t);
    Reg encodeBuiltin(Reg x, TypeId t);
    void decodeStruct(Reg v, TypeId t);
    void decodeEnum(Reg v, TypeId t);
    Reg encodeStruct(Reg x, TypeId t);
    Reg encodeEnum(Reg x, TypeId t);
    // LowerValue.cpp: value types built from parts
    Reg lowerWith(WithExpr* e);
    Reg lowerConst(const sema::ConstValue& v, TypeId t);
    Reg arrayIndex(Expr* index);
    void storeArray(IndexExpr* target, Reg array, Reg index, Reg value);
    Reg lowerArrayGet(IndexExpr* e);
    Reg lowerArrayLiteral(Expr* e);
    Reg lowerArrayMethod(CallExpr* e, Reg dst);
    void lowerForArray(ForStmt* s, TypeId t);

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
    // Debug names (ir::LocalName): a local's range opens where it gets its value and closes with its block.
    void nameLocal(const sema::Local* l);
    void closeLocals(std::size_t mark);
    std::vector<std::size_t> openLocals_;

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
