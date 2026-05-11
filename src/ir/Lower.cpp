#include "ir/LowerImpl.hpp"

#include <bit>
#include <format>

namespace quartz::ir {

Lowerer::Lowerer(sema::Program& prog, const SourceManager& sources, Diagnostics& diags)
    : prog_(prog), sources_(sources), diags_(diags), mod_(std::make_unique<Module>()) {
    // The module owns the program's type table from here on.
    mod_->types = std::move(prog.types);
    types_ = &mod_->types;
}

void Lowerer::setLine(Span span) {
    if (span.file < sources_.size()) {
        line_ = sources_.file(span.file).lineCol(span.begin).line;
        if (fn_ != nullptr && fn_->file.empty()) {
            fn_->file = sources_.file(span.file).name();
        }
    }
}

Reg Lowerer::newReg(TypeId t) {
    fn_->regs.push_back(T(t));
    return static_cast<Reg>(fn_->regs.size() - 1);
}

Inst& Lowerer::emit(Op op, Reg dst, Reg a, Reg b, std::uint32_t imm) {
    Inst in{};
    in.op = op;
    in.dst = dst;
    in.a = a;
    in.b = b;
    in.imm = imm;
    in.line = line_;
    fn_->code.push_back(in);
    return fn_->code.back();
}

Inst& Lowerer::emitArgs(Op op, Reg dst, std::uint32_t imm, const std::vector<Reg>& args) {
    Inst& in = emit(op, dst, kNoReg, kNoReg, imm);
    in.argBegin = static_cast<std::uint32_t>(fn_->args.size());
    in.argCount = static_cast<std::uint32_t>(args.size());
    fn_->args.insert(fn_->args.end(), args.begin(), args.end());
    return in;
}

std::uint32_t Lowerer::allocFunction() {
    std::uint32_t idx = nextFn_++;
    extra_[idx];
    return idx;
}

Function& Lowerer::function(std::uint32_t index) {
    return index < mod_->functions.size() ? mod_->functions[index] : extra_[index];
}

std::uint32_t Lowerer::instance(sema::FnInfo* fn, const std::vector<TypeId>& args) {
    auto key = std::make_pair(fn, args);
    auto it = instances_.find(key);
    if (it != instances_.end()) {
        return it->second;
    }
    std::uint32_t idx = allocFunction();
    instances_.emplace(key, idx);
    work_.emplace_back(fn, args, idx);
    return idx;
}

Reg Lowerer::constInt(std::int64_t v, TypeId t) {
    t = T(t);
    auto key = std::make_tuple(0, static_cast<std::uint64_t>(v), t, std::string());
    auto it = constIndex_.find(key);
    std::uint32_t idx = 0;
    if (it == constIndex_.end()) {
        idx = static_cast<std::uint32_t>(mod_->constants.size());
        mod_->constants.push_back({ConstKind::Int, t, static_cast<std::uint64_t>(v), {}});
        constIndex_[key] = idx;
    } else {
        idx = it->second;
    }
    Reg r = newReg(t);
    emit(Op::Const, r, kNoReg, kNoReg, idx);
    return r;
}

Reg Lowerer::constFloat(double v, TypeId t) {
    t = T(t);
    if (t == kF32) {
        v = static_cast<double>(static_cast<float>(v));
    }
    auto bits = std::bit_cast<std::uint64_t>(v);
    auto key = std::make_tuple(1, bits, t, std::string());
    auto it = constIndex_.find(key);
    std::uint32_t idx = 0;
    if (it == constIndex_.end()) {
        idx = static_cast<std::uint32_t>(mod_->constants.size());
        mod_->constants.push_back({ConstKind::Float, t, bits, {}});
        constIndex_[key] = idx;
    } else {
        idx = it->second;
    }
    Reg r = newReg(t);
    emit(Op::Const, r, kNoReg, kNoReg, idx);
    return r;
}

Reg Lowerer::constStr(std::string_view s) {
    auto key = std::make_tuple(2, std::uint64_t{0}, kStr, std::string(s));
    auto it = constIndex_.find(key);
    std::uint32_t idx = 0;
    if (it == constIndex_.end()) {
        idx = static_cast<std::uint32_t>(mod_->constants.size());
        mod_->constants.push_back({ConstKind::Str, kStr, 0, std::string(s)});
        constIndex_[key] = idx;
    } else {
        idx = it->second;
    }
    Reg r = newReg(kStr);
    emit(Op::Const, r, kNoReg, kNoReg, idx);
    return r;
}

void Lowerer::open(Op op, Reg cond) {
    emit(op, kNoReg, cond);
    ++depth_;
}

void Lowerer::close() {
    emit(Op::End);
    --depth_;
}

void Lowerer::br(std::size_t frame) { emit(Op::Br, kNoReg, kNoReg, kNoReg, static_cast<std::uint32_t>(depth_ - 1 - frame)); }

void Lowerer::brIf(Reg cond, std::size_t frame) {
    emit(Op::BrIf, kNoReg, cond, kNoReg, static_cast<std::uint32_t>(depth_ - 1 - frame));
}

Reg Lowerer::zeroValue(TypeId t, Span span) {
    const TypeInfo& info = types_->info(t);
    switch (info.kind) {
    case TypeKind::Bool:
    case TypeKind::I8:
    case TypeKind::I16:
    case TypeKind::I32:
    case TypeKind::I64:
    case TypeKind::U8:
    case TypeKind::U16:
    case TypeKind::U32:
    case TypeKind::U64:
    case TypeKind::Char: return constInt(0, t);
    case TypeKind::F32:
    case TypeKind::F64: return constFloat(0.0, t);
    case TypeKind::Str: return constStr("");
    case TypeKind::List: {
        Reg r = newReg(t);
        emitArgs(Op::Intrinsic, r, static_cast<std::uint32_t>(Intrinsic::ListNew), {});
        return r;
    }
    case TypeKind::Map: {
        Reg r = newReg(t);
        emitArgs(Op::Intrinsic, r, static_cast<std::uint32_t>(Intrinsic::MapNew), {});
        return r;
    }
    case TypeKind::Nullable: {
        Reg r = newReg(t);
        emit(Op::NullOf, r);
        return r;
    }
    case TypeKind::Struct:
    case TypeKind::Tuple: {
        std::vector<Reg> fields;
        if (info.kind == TypeKind::Struct) {
            for (const Field& f : info.fields) {
                fields.push_back(zeroValue(f.type, span));
            }
        } else {
            for (TypeId e : info.elems) {
                fields.push_back(zeroValue(e, span));
            }
        }
        Reg r = newReg(t);
        emitArgs(Op::Make, r, 0, fields);
        return r;
    }
    case TypeKind::Enum:
        if (!info.variants.empty() && info.variants[0].payload.empty()) {
            Reg r = newReg(t);
            emitArgs(Op::EnumMake, r, 0, {});
            return r;
        }
        break;
    default: break;
    }
    diags_.error(span, std::format("a value of type '{}' has no default; give it an initial value", types_->name(t)));
    Reg r = newReg(t);
    emit(Op::Trap, kNoReg, kNoReg, kNoReg, static_cast<std::uint32_t>(TrapCode::Unreachable));
    return r;
}

void Lowerer::lowerStateInit(TypeId state, Function& out) {
    fn_ = &out;
    info_ = nullptr;
    depth_ = 0;
    out.name = std::format("{}.<init>", types_->name(state));
    out.regs.push_back(state);
    out.paramCount = 1;
    out.result = kVoid;
    StructDecl* decl = prog_.structDecls[state];
    const TypeInfo& info = types_->info(state);
    for (std::uint32_t i = 0; i < info.fields.size(); ++i) {
        const FieldDecl& f = decl->fields[i];
        setLine(f.span);
        Reg v = f.defaultValue != nullptr ? lowerExpr(f.defaultValue, info.fields[i].type)
                                          : zeroValue(info.fields[i].type, f.span);
        emit(Op::FieldSet, kNoReg, 0, v, i);
    }
    emit(Op::Return);
}

void Lowerer::lowerFilter(sema::AnnotationInfo& a, sema::FnInfo* fn, Function& out) {
    fn_ = &out;
    info_ = nullptr;
    depth_ = 0;
    loops_.clear();
    out.name = fn->name + ".where";
    out.result = kBool;
    out.exported = true;
    a.whereParam->reg = newReg(T(a.whereParam->type));
    out.paramCount = 1;
    setLine(a.where->span);
    Reg v = lowerExpr(a.where, kBool);
    emit(Op::Return, kNoReg, v);
}

void Lowerer::lowerFunction(sema::FnInfo* fn, Function& out) {
    fn_ = &out;
    info_ = fn;
    depth_ = 0;
    loops_.clear();
    out.name = fn->name;
    if (!subst_.empty()) {
        out.name += "<";
        for (std::size_t i = 0; i < subst_.size(); ++i) {
            out.name += (i ? ", " : "") + types_->name(subst_[i].second);
        }
        out.name += ">";
    }
    out.result = T(fn->result);
    if (fn->derived != sema::LangTrait::None) {
        lowerDerived(fn, out);
        return;
    }
    out.isSuspend = fn->isSuspend;
    out.inlining = fn->inlining;
    scopes_.clear();
    for (sema::Local* l : fn->paramLocals) {
        l->reg = newReg(T(l->type));
        l->scopeDepth = 0;
    }
    out.paramCount = static_cast<std::uint32_t>(out.regs.size());
    out.exported = fn->owner == kError && !signatureHasFn(out);
    retReg_ = fn->result != kVoid ? newReg(fn->result) : kNoReg;
    setLine(fn->decl->span);
    lowerBlockStmts(fn->decl->body);
    if (fn->result == kVoid) {
        emit(Op::Return);
    } else {
        emit(Op::Trap, kNoReg, kNoReg, kNoReg, static_cast<std::uint32_t>(TrapCode::Unreachable));
    }
}

void Lowerer::lowerBlockStmts(BlockExpr* block) {
    for (Stmt* s : block->stmts) {
        lowerStmt(s);
    }
}

Reg Lowerer::lowerBlockValue(BlockExpr* block, TypeId want) {
    if (block->stmts.empty()) {
        return kNoReg;
    }
    for (std::size_t i = 0; i + 1 < block->stmts.size(); ++i) {
        lowerStmt(block->stmts[i]);
    }
    Stmt* last = block->stmts.back();
    if (last->kind == StmtKind::Expr) {
        return lowerExpr(static_cast<ExprStmt*>(last)->expr, want);
    }
    lowerStmt(last);
    return kNoReg;
}

void Lowerer::lowerStmt(Stmt* s) {
    setLine(s->span);
    switch (s->kind) {
    case StmtKind::Expr: {
        Expr* e = static_cast<ExprStmt*>(s)->expr;
        if (e->kind == ExprKind::If) {
            lowerIf(static_cast<IfExpr*>(e), kError, false);
        } else if (e->kind == ExprKind::Match) {
            lowerMatch(static_cast<MatchExpr*>(e), kError, false);
        } else if (e->kind == ExprKind::Block) {
            lowerBlockStmts(static_cast<BlockExpr*>(e));
        } else {
            lowerExpr(e);
        }
        return;
    }
    case StmtKind::Let: lowerLet(static_cast<LetStmt*>(s)); return;
    case StmtKind::Assign: lowerAssign(static_cast<AssignStmt*>(s)); return;
    case StmtKind::While: lowerWhile(static_cast<WhileStmt*>(s)); return;
    case StmtKind::For: lowerFor(static_cast<ForStmt*>(s)); return;
    case StmtKind::Return: {
        auto* r = static_cast<ReturnStmt*>(s);
        Reg v = r->value != nullptr ? lowerExpr(r->value, fn_->result) : kNoReg;
        if (!scopes_.empty() && v != kNoReg) {
            persistTo(v, fn_->result, 0);
            emit(Op::Move, retReg_, v);
            v = retReg_;
        }
        exitScopesTo(0);
        emit(Op::Return, kNoReg, v);
        return;
    }
    case StmtKind::Break:
        exitScopesTo(loops_.back().scopes);
        br(loops_.back().breakFrame);
        return;
    case StmtKind::Continue:
        exitScopesTo(loops_.back().scopes);
        br(loops_.back().continueFrame);
        return;
    case StmtKind::Scope: lowerScoped(static_cast<ScopeStmt*>(s)->body); return;
    case StmtKind::Every: {
        auto* ev = static_cast<EveryStmt*>(s);
        std::size_t exit = depth_;
        open(Op::Block);
        std::size_t loop = depth_;
        open(Op::Loop);
        std::size_t cont = depth_;
        open(Op::Block);
        Reg v = lowerExpr(ev->interval);
        TypeId produced = T(ev->interval->type);
        (void)produced;
        emit(Op::Wait, kNoReg, v, kNoReg, static_cast<std::uint32_t>(WaitKind::Wait));
        loops_.push_back({exit, cont, scopes_.size()});
        lowerScoped(ev->body);
        loops_.pop_back();
        close();
        br(loop);
        close();
        close();
        return;
    }
    case StmtKind::At: {
        auto* at = static_cast<AtStmt*>(s);
        Reg ctx = lowerExpr(at->context);
        emit(Op::Wait, kNoReg, ctx, kNoReg, static_cast<std::uint32_t>(WaitKind::AtEnter));
        lowerBlockStmts(at->body);
        emit(Op::Wait, kNoReg, ctx, kNoReg, static_cast<std::uint32_t>(WaitKind::AtLeave));
        return;
    }

    }
}

void Lowerer::enterScope() {
    emit(Op::ScopeEnter);
    scopes_.push_back({static_cast<Reg>(fn_->regs.size()), {}});
}

void Lowerer::exitScope() {
    OpenScope sc = std::move(scopes_.back());
    scopes_.pop_back();
    auto end = static_cast<Reg>(fn_->regs.size());
    Inst& in = emit(Op::ScopeExit, kNoReg, sc.firstReg, end);
    (void)in;
    for (std::size_t at : sc.exits) {
        fn_->code[at].b = end;
    }
}

void Lowerer::exitScopesTo(std::size_t depth) {
    for (std::size_t i = scopes_.size(); i > depth; --i) {
        scopes_[i - 1].exits.push_back(fn_->code.size());
        emit(Op::ScopeExit, kNoReg, scopes_[i - 1].firstReg, scopes_[i - 1].firstReg);
    }
}

Reg Lowerer::persistTo(Reg value, TypeId type, std::size_t depth) {
    if (value == kNoReg || scopes_.size() <= depth || !types_->hasRefs(type)) {
        return value;
    }
    emit(Op::Persist, kNoReg, value, kNoReg, static_cast<std::uint32_t>(scopes_.size() - depth));
    return value;
}

void Lowerer::lowerScoped(BlockExpr* body) {
    enterScope();
    lowerBlockStmts(body);
    exitScope();
}

sema::Local* Lowerer::rootLocal(Expr* target) {
    while (true) {
        if (target->kind == ExprKind::Ident) {
            auto* id = static_cast<IdentExpr*>(target);
            return id->ref == IdentRef::Local ? id->local : nullptr;
        }
        if (target->kind == ExprKind::SelfRef) {
            return static_cast<SelfExpr*>(target)->local;
        }
        if (target->kind == ExprKind::Member && types_->kind(T(static_cast<MemberExpr*>(target)->object->type)) !=
                                                    TypeKind::State) {
            target = static_cast<MemberExpr*>(target)->object;
            continue;
        }
        return nullptr;
    }
}

void Lowerer::lowerLet(LetStmt* s) {
    if (s->pattern != nullptr) {
        lowerLetPattern(s);
        return;
    }
    sema::Local* l = s->local;
    Reg v = s->init != nullptr ? lowerExpr(s->init, T(l->type)) : zeroValue(T(l->type), s->span);
    l->scopeDepth = static_cast<std::uint32_t>(scopes_.size());
    l->reg = newReg(T(l->type));
    emit(Op::Move, l->reg, v);
}

void Lowerer::assignPlace(Expr* target, Reg value) {
    switch (target->kind) {
    case ExprKind::Ident: {
        auto* id = static_cast<IdentExpr*>(target);
        emit(Op::Move, id->local->reg, value);
        return;
    }
    case ExprKind::Member: {
        auto* m = static_cast<MemberExpr*>(target);
        TypeId ot = T(m->object->type);
        Reg obj = lowerExpr(m->object);
        emit(Op::FieldSet, kNoReg, obj, value, m->fieldIndex);
        bool inPlace = types_->kind(ot) == TypeKind::State ||
                       (m->object->kind == ExprKind::Ident &&
                        static_cast<IdentExpr*>(m->object)->ref == IdentRef::Local);
        if (!inPlace) {
            assignPlace(m->object, obj);
        }
        return;
    }
    case ExprKind::Index: {
        auto* ix = static_cast<IndexExpr*>(target);
        Reg obj = lowerExpr(ix->object);
        bool isMap = types_->kind(T(ix->object->type)) == TypeKind::Map;
        Reg idx = lowerExpr(ix->index, isMap ? types_->info(T(ix->object->type)).key : kI64);
        emitArgs(Op::Intrinsic, kNoReg, static_cast<std::uint32_t>(isMap ? Intrinsic::MapSet : Intrinsic::ListSet),
                 {obj, idx, value});
        return;
    }
    default: return;
    }
}

namespace {

BinaryOp compoundOp(AssignOp op) {
    static const BinaryOp kOps[] = {BinaryOp::Add,    BinaryOp::Add, BinaryOp::Sub,    BinaryOp::Mul,
                                    BinaryOp::Div,    BinaryOp::Rem, BinaryOp::BitAnd, BinaryOp::BitOr,
                                    BinaryOp::BitXor, BinaryOp::Shl, BinaryOp::Shr};
    return kOps[static_cast<std::size_t>(op)];
}

} // namespace

void Lowerer::lowerAssign(AssignStmt* s) {
    TypeId tt = T(s->target->type);
    if (s->setter != ~0U) {
        auto* m = static_cast<MemberExpr*>(s->target);
        Reg obj = lowerExpr(m->object);
        Reg v = kNoReg;
        if (s->op == AssignOp::Set) {
            v = lowerExpr(s->value, tt);
        } else {
            Reg cur = newReg(tt);
            emitArgs(Op::CallHost, cur, m->getter, {obj});
            BinaryOp bop = compoundOp(s->op);
            Reg rhs = lowerExpr(s->value, bop == BinaryOp::Shl || bop == BinaryOp::Shr ? kError : tt);
            v = newReg(tt);
            if (tt == kStr && bop == BinaryOp::Add) {
                emitArgs(Op::Intrinsic, v, static_cast<std::uint32_t>(Intrinsic::StrConcat), {cur, rhs});
            } else {
                emit(binaryOp(bop), v, cur, rhs);
            }
        }
        emitArgs(Op::CallHost, kNoReg, s->setter, {obj, v});
        return;
    }
    if (s->op == AssignOp::Set) {
        Reg v = lowerExpr(s->value, tt);
        if (sema::Local* root = rootLocal(s->target)) {
            persistTo(v, tt, root->scopeDepth);
        }
        assignPlace(s->target, v);
        return;
    }
    BinaryOp bop = compoundOp(s->op);
    // Evaluate the container and index once for `xs[i] op= v`.
    if (s->target->kind == ExprKind::Index) {
        auto* ix = static_cast<IndexExpr*>(s->target);
        bool isMap = types_->kind(T(ix->object->type)) == TypeKind::Map;
        Reg obj = lowerExpr(ix->object);
        Reg idx = lowerExpr(ix->index, isMap ? types_->info(T(ix->object->type)).key : kI64);
        Reg cur = newReg(isMap ? types_->nullable(tt) : tt);
        emitArgs(Op::Intrinsic, cur, static_cast<std::uint32_t>(isMap ? Intrinsic::MapGet : Intrinsic::ListGet),
                 {obj, idx});
        if (isMap) {
            Reg unwrapped = newReg(tt);
            emit(Op::Unwrap, unwrapped, cur);
            cur = unwrapped;
        }
        Reg rhs = lowerExpr(s->value, bop == BinaryOp::Shl || bop == BinaryOp::Shr ? kError : tt);
        Reg res = newReg(tt);
        Op op = binaryOp(bop);
        if (tt == kStr && bop == BinaryOp::Add) {
            emitArgs(Op::Intrinsic, res, static_cast<std::uint32_t>(Intrinsic::StrConcat), {cur, rhs});
        } else {
            emit(op, res, cur, rhs);
        }
        emitArgs(Op::Intrinsic, kNoReg, static_cast<std::uint32_t>(isMap ? Intrinsic::MapSet : Intrinsic::ListSet),
                 {obj, idx, res});
        return;
    }
    Reg cur = lowerExpr(s->target);
    Reg rhs = lowerExpr(s->value, bop == BinaryOp::Shl || bop == BinaryOp::Shr ? kError : tt);
    Reg res = newReg(tt);
    if (tt == kStr && bop == BinaryOp::Add) {
        emitArgs(Op::Intrinsic, res, static_cast<std::uint32_t>(Intrinsic::StrConcat), {cur, rhs});
    } else {
        emit(binaryOp(bop), res, cur, rhs);
    }
    assignPlace(s->target, res);
}

void Lowerer::lowerWhile(WhileStmt* s) {
    // Rotated: block $exit { if !cond br $exit; loop $top { block $cont { body } if cond br $top } }
    std::size_t exit = depth_;
    open(Op::Block);
    branchIfFalse(s->cond, exit);
    std::size_t loop = depth_;
    open(Op::Loop);
    std::size_t cont = depth_;
    open(Op::Block);
    loops_.push_back({exit, cont, scopes_.size()});
    lowerBlockStmts(s->body);
    loops_.pop_back();
    close();
    branchIfTrue(s->cond, loop);
    close();
    close();
}

void Lowerer::branchIfFalse(Expr* e, std::size_t frame) {
    if (e->kind == ExprKind::Binary) {
        auto* b = static_cast<BinaryExpr*>(e);
        if (b->op == BinaryOp::And) {
            branchIfFalse(b->lhs, frame);
            branchIfFalse(b->rhs, frame);
            return;
        }
        if (b->op == BinaryOp::Or) {
            std::size_t done = depth_;
            open(Op::Block);
            branchIfTrue(b->lhs, done);
            branchIfFalse(b->rhs, frame);
            close();
            return;
        }
    }
    if (e->kind == ExprKind::Unary && static_cast<UnaryExpr*>(e)->op == UnaryOp::Not) {
        branchIfTrue(static_cast<UnaryExpr*>(e)->operand, frame);
        return;
    }
    if (e->kind == ExprKind::Is) {
        auto* is = static_cast<IsExpr*>(e);
        matchPattern(is->pattern, lowerExpr(is->value), T(is->value->type), frame);
        return;
    }
    Reg c = lowerExpr(e, kBool);
    Reg nc = newReg(kBool);
    emit(Op::Not, nc, c);
    brIf(nc, frame);
}

void Lowerer::branchIfTrue(Expr* e, std::size_t frame) {
    if (e->kind == ExprKind::Binary) {
        auto* b = static_cast<BinaryExpr*>(e);
        if (b->op == BinaryOp::Or) {
            branchIfTrue(b->lhs, frame);
            branchIfTrue(b->rhs, frame);
            return;
        }
        if (b->op == BinaryOp::And) {
            std::size_t skip = depth_;
            open(Op::Block);
            branchIfFalse(b->lhs, skip);
            branchIfTrue(b->rhs, frame);
            close();
            return;
        }
    }
    if (e->kind == ExprKind::Unary && static_cast<UnaryExpr*>(e)->op == UnaryOp::Not) {
        branchIfFalse(static_cast<UnaryExpr*>(e)->operand, frame);
        return;
    }
    brIf(lowerExpr(e, kBool), frame);
}

Reg Lowerer::lowerIf(IfExpr* e, TypeId want, bool wantValue) {
    TypeId rt = wantValue ? (want != kError ? want : T(e->type)) : kError;
    Reg result = wantValue && rt != kVoid && rt != kNever ? newReg(rt) : kNoReg;
    // block $end { block $else { test; then; br $end } else }
    std::size_t endFrame = depth_;
    open(Op::Block);
    std::size_t elseFrame = depth_;
    open(Op::Block);
    if (e->bindValue != nullptr) {
        Reg bound = lowerExpr(e->bindValue);
        Reg isNull = newReg(kBool);
        emit(Op::IsNull, isNull, bound);
        brIf(isNull, elseFrame);
        e->bindLocal->reg = newReg(T(e->bindLocal->type));
        emit(Op::Unwrap, e->bindLocal->reg, bound);
    } else {
        branchIfFalse(e->cond, elseFrame);
    }
    if (result != kNoReg) {
        Reg v = lowerBlockValue(e->then, rt);
        if (v != kNoReg && T(e->then->type) != kNever) {
            emit(Op::Move, result, v);
        }
    } else {
        lowerBlockStmts(e->then);
    }
    if (e->otherwise != nullptr) {
        br(endFrame);
    }
    close();
    if (e->otherwise != nullptr) {
        if (e->otherwise->kind == ExprKind::If) {
            Reg v = lowerIf(static_cast<IfExpr*>(e->otherwise), rt, wantValue);
            if (result != kNoReg && v != kNoReg && T(e->otherwise->type) != kNever) {
                emit(Op::Move, result, v);
            }
        } else if (result != kNoReg) {
            Reg v = lowerBlockValue(static_cast<BlockExpr*>(e->otherwise), rt);
            if (v != kNoReg && T(e->otherwise->type) != kNever) {
                emit(Op::Move, result, v);
            }
        } else {
            lowerBlockStmts(static_cast<BlockExpr*>(e->otherwise));
        }
    }
    close();
    return result;
}

} // namespace quartz::ir
