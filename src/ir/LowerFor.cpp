#include "ir/LowerImpl.hpp"

namespace quartz::ir {

void Lowerer::lowerFor(ForStmt* s) {
    // Loops are rotated so each iteration ends in one conditional backward branch.
    std::size_t exit = depth_;
    if (s->iterable->kind == ExprKind::Range) {
        auto* r = static_cast<RangeExpr*>(s->iterable);
        TypeId t = T(r->type);
        Reg lo = lowerExpr(r->lo, t);
        Reg hi = lowerExpr(r->hi, t);
        Reg i = newReg(t);
        s->local->reg = i;
        s->local->scopeDepth = static_cast<std::uint32_t>(scopes_.size());
        emit(Op::Move, i, lo);
        nameLocal(s->local);
        Reg one = constInt(1, t);
        open(Op::Block);
        Reg skip = newReg(kBool);
        emit(r->inclusive ? Op::Gt : Op::Ge, skip, i, hi);
        brIf(skip, exit);
        std::size_t loop = depth_;
        open(Op::Loop);
        std::size_t cont = depth_;
        open(Op::Block);
        loops_.push_back({exit, cont, scopes_.size()});
        lowerBlockStmts(s->body);
        loops_.pop_back();
        close();
        if (r->inclusive) {
            Reg more = newReg(kBool);
            emit(Op::Lt, more, i, hi);
            Reg done = newReg(kBool);
            emit(Op::Not, done, more);
            brIf(done, exit);
            emit(Op::Add, i, i, one);
            br(loop);
        } else {
            emit(Op::Add, i, i, one);
            Reg more = newReg(kBool);
            emit(Op::Lt, more, i, hi);
            brIf(more, loop);
        }
        close();
        close();
        return;
    }
    TypeId lt = T(s->iterable->type);
    if (types_->kind(lt) != TypeKind::List) {
        lowerForEach(s, lt);
        return;
    }
    TypeId elem = types_->info(lt).elem;
    Reg list = lowerExpr(s->iterable);
    Reg idx = newReg(kI64);
    emit(Op::Move, idx, constInt(0, kI64));
    Reg one = constInt(1, kI64);
    open(Op::Block);
    Reg len = newReg(kI64);
    emitArgs(Op::Intrinsic, len, static_cast<std::uint32_t>(Intrinsic::ListLen), {list});
    Reg empty = newReg(kBool);
    emit(Op::Ge, empty, idx, len);
    brIf(empty, exit);
    std::size_t loop = depth_;
    open(Op::Loop);
    Reg x = newReg(elem);
    s->local->reg = x;
    s->local->scopeDepth = static_cast<std::uint32_t>(scopes_.size());
    emitArgs(Op::Intrinsic, x, static_cast<std::uint32_t>(Intrinsic::ListGet), {list, idx});
    nameLocal(s->local);
    if (s->indexLocal != nullptr) {
        s->indexLocal->reg = newReg(kI64);
        emit(Op::Move, s->indexLocal->reg, idx);
        nameLocal(s->indexLocal);
    }
    std::size_t cont = depth_;
    open(Op::Block);
    loops_.push_back({exit, cont, scopes_.size()});
    lowerBlockStmts(s->body);
    loops_.pop_back();
    close();
    emit(Op::Add, idx, idx, one);
    Reg len2 = newReg(kI64);
    emitArgs(Op::Intrinsic, len2, static_cast<std::uint32_t>(Intrinsic::ListLen), {list});
    Reg more = newReg(kBool);
    emit(Op::Lt, more, idx, len2);
    brIf(more, loop);
    close();
    close();
}

// Maps, strings and Iterators: `step` advances the cursor before the body, so `continue` only has to loop back.
void Lowerer::lowerForEach(ForStmt* s, TypeId t) {
    const TypeInfo info = types_->info(t);
    Reg src = lowerExpr(s->iterable);
    auto bind = [&](sema::Local* l, TypeId type) {
        Reg r = newReg(type);
        if (l != nullptr) {
            l->reg = r;
            l->scopeDepth = static_cast<std::uint32_t>(scopes_.size());
            nameLocal(l);
        }
        return r;
    };
    Reg cursor = newReg(info.kind == TypeKind::Map || t == kStr ? kI64 : t);
    Reg count = newReg(kI64);
    emit(Op::Move, count, constInt(0, kI64));
    Reg end = kNoReg;
    if (info.kind == TypeKind::Map) {
        emitArgs(Op::Intrinsic, cursor, static_cast<std::uint32_t>(Intrinsic::MapNext), {src, constInt(-1, kI64)});
    } else if (t == kStr) {
        emit(Op::Move, cursor, constInt(0, kI64));
        end = newReg(kI64);
        emitArgs(Op::Intrinsic, end, static_cast<std::uint32_t>(Intrinsic::StrLen), {src});
    } else {
        emit(Op::Move, cursor, src);
    }
    std::size_t exit = depth_;
    open(Op::Block);
    std::size_t loop = depth_;
    open(Op::Loop);
    Reg done = newReg(kBool);
    Reg next = kNoReg;
    if (info.kind == TypeKind::Map) {
        emit(Op::Lt, done, cursor, constInt(0, kI64));
        brIf(done, exit);
        bool pair = s->indexLocal != nullptr;
        Reg key = bind(pair ? s->indexLocal : s->local, info.key);
        emitArgs(Op::Intrinsic, key, static_cast<std::uint32_t>(Intrinsic::MapKeyAt), {src, cursor});
        if (pair) {
            emitArgs(Op::Intrinsic, bind(s->local, info.elem), static_cast<std::uint32_t>(Intrinsic::MapValueAt),
                     {src, cursor});
        }
        next = newReg(kI64);
        emitArgs(Op::Intrinsic, next, static_cast<std::uint32_t>(Intrinsic::MapNext), {src, cursor});
    } else if (t == kStr) {
        emit(Op::Ge, done, cursor, end);
        brIf(done, exit);
        Reg decoded = newReg(types_->tuple({kChar, kI64}));
        emitArgs(Op::Intrinsic, decoded, static_cast<std::uint32_t>(Intrinsic::StrDecode), {src, cursor});
        if (s->indexLocal != nullptr) {
            emit(Op::Move, bind(s->indexLocal, kI64), cursor);
        }
        emit(Op::FieldGet, bind(s->local, kChar), decoded, kNoReg, 0);
        next = newReg(kI64);
        emit(Op::FieldGet, next, decoded, kNoReg, 1);
    } else {
        std::vector<TypeId> targs;
        sema::FnInfo* fn = userMethod(t, "next", targs);
        TypeId result = fn != nullptr ? types_->subst(fn->result,
                                                      [&] {
                                                          TypeSubst sub;
                                                          for (std::size_t i = 0;
                                                               i < fn->typeParams.size() && i < targs.size(); ++i) {
                                                              sub.emplace_back(fn->typeParams[i], targs[i]);
                                                          }
                                                          return sub;
                                                      }())
                                      : kError;
        if (fn == nullptr || types_->kind(result) != TypeKind::Nullable) {
            diags_.error(s->span, "internal: Iterator without 'next'");
            close();
            close();
            return;
        }
        Reg step = callMethod(fn, targs, {cursor}, result);
        emit(Op::IsNull, done, step);
        brIf(done, exit);
        TypeId pairType = types_->info(result).elem;
        Reg pair = newReg(pairType);
        emit(Op::Unwrap, pair, step);
        if (s->indexLocal != nullptr) {
            emit(Op::Move, bind(s->indexLocal, kI64), count);
        }
        emit(Op::FieldGet, bind(s->local, types_->info(pairType).elems[0]), pair, kNoReg, 0);
        next = newReg(t);
        emit(Op::FieldGet, next, pair, kNoReg, 1);
    }
    std::size_t cont = depth_;
    open(Op::Block);
    loops_.push_back({exit, cont, scopes_.size()});
    lowerBlockStmts(s->body);
    loops_.pop_back();
    close();
    emit(Op::Move, cursor, next);
    emit(Op::Add, count, count, constInt(1, kI64));
    br(loop);
    close();
    close();
}

// `m.keys()` and `m.values()`: copies in insertion order.
Reg Lowerer::lowerMapList(CallExpr* e, Reg dst) {
    bool keys = static_cast<SpecialOp>(e->builtin) == SpecialOp::MapKeys;
    Reg m = lowerExpr(e->receiver);
    const TypeInfo& info = types_->info(T(e->receiver->type));
    TypeId item = keys ? info.key : info.elem;
    emitArgs(Op::Intrinsic, dst, static_cast<std::uint32_t>(Intrinsic::ListNew), {});
    Reg cursor = newReg(kI64);
    emitArgs(Op::Intrinsic, cursor, static_cast<std::uint32_t>(Intrinsic::MapNext), {m, constInt(-1, kI64)});
    std::size_t exit = depth_;
    open(Op::Block);
    std::size_t loop = depth_;
    open(Op::Loop);
    Reg done = newReg(kBool);
    emit(Op::Lt, done, cursor, constInt(0, kI64));
    brIf(done, exit);
    Reg x = newReg(item);
    emitArgs(Op::Intrinsic, x, static_cast<std::uint32_t>(keys ? Intrinsic::MapKeyAt : Intrinsic::MapValueAt),
             {m, cursor});
    emitArgs(Op::Intrinsic, kNoReg, static_cast<std::uint32_t>(Intrinsic::ListPush), {dst, x});
    emitArgs(Op::Intrinsic, cursor, static_cast<std::uint32_t>(Intrinsic::MapNext), {m, cursor});
    br(loop);
    close();
    close();
    return dst;
}

} // namespace quartz::ir
