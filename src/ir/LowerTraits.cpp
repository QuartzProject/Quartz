#include "ir/LowerImpl.hpp"

#include <format>

namespace quartz::ir {

bool Lowerer::vmEquatable(TypeId t) const {
    const TypeInfo& info = types_->info(t);
    if (types_->isScalar(t) || t == kStr || info.kind == TypeKind::Handle) {
        return true;
    }
    if (info.kind == TypeKind::Nullable) {
        TypeId e = info.elem;
        return types_->isScalar(e) || e == kStr || types_->kind(e) == TypeKind::Handle;
    }
    if (info.kind == TypeKind::Enum && !types_->isResult(t) && info.genericBase == 0 &&
        !prog_.methodTable.contains({t, "eq"})) {
        for (const VariantInfo& v : info.variants) {
            if (!v.payload.empty()) {
                return false;
            }
        }
        return true;
    }
    return false;
}

bool Lowerer::vmOrdered(TypeId t) const { return types_->isNumeric(t) || t == kChar || t == kStr; }

Reg Lowerer::listLength(Reg list) {
    Reg n = newReg(kI64);
    emitArgs(Op::Intrinsic, n, static_cast<std::uint32_t>(Intrinsic::ListLen), {list});
    return n;
}

Reg Lowerer::listAt(Reg list, Reg index, TypeId elem) {
    Reg x = newReg(elem);
    emitArgs(Op::Intrinsic, x, static_cast<std::uint32_t>(Intrinsic::ListGet), {list, index});
    return x;
}

template <class Body>
void Lowerer::forEachIndex(Reg n, Body&& body) {
    Reg i = newReg(kI64);
    emit(Op::Move, i, constInt(0, kI64));
    std::size_t end = depth_;
    open(Op::Block);
    std::size_t loop = depth_;
    open(Op::Loop);
    Reg past = newReg(kBool);
    emit(Op::Ge, past, i, n);
    brIf(past, end);
    body(i);
    emit(Op::Add, i, i, constInt(1, kI64));
    br(loop);
    close();
    close();
}

sema::FnInfo* Lowerer::userMethod(TypeId t, std::string_view name, std::vector<TypeId>& targs) const {
    const TypeInfo& info = types_->info(t);
    if (info.kind != TypeKind::Struct && info.kind != TypeKind::Enum) {
        return nullptr;
    }
    TypeId key = info.genericBase != 0 ? info.genericBase : t;
    auto it = prog_.methodTable.find({key, std::string(name)});
    if (it == prog_.methodTable.end()) {
        return nullptr;
    }
    targs = info.genericBase != 0 ? info.typeArgs : std::vector<TypeId>{};
    if (it->second->trait != nullptr) {
        targs = {t};
    }
    return it->second;
}

Reg Lowerer::callMethod(sema::FnInfo* fn, const std::vector<TypeId>& targs, const std::vector<Reg>& args,
                        TypeId result) {
    Reg dst = newReg(result);
    emitArgs(Op::Call, dst, fn->generic() ? instance(fn, targs) : fn->index, args);
    return dst;
}

Reg Lowerer::equalsOf(Reg a, Reg b, TypeId t) {
    if (vmEquatable(t)) {
        Reg r = newReg(kBool);
        emit(Op::Eq, r, a, b);
        return r;
    }
    std::vector<TypeId> targs;
    if (sema::FnInfo* fn = userMethod(t, "eq", targs)) {
        return callMethod(fn, targs, {a, b}, kBool);
    }
    const TypeInfo& info = types_->info(t);
    Reg res = newReg(kBool);
    std::size_t done = depth_;
    open(Op::Block);
    if (info.kind == TypeKind::Nullable) {
        TypeId inner = info.elem;
        Reg na = newReg(kBool);
        Reg nb = newReg(kBool);
        emit(Op::IsNull, na, a);
        emit(Op::IsNull, nb, b);
        emit(Op::Eq, res, na, nb);
        Reg differ = newReg(kBool);
        emit(Op::Not, differ, res);
        brIf(differ, done);
        brIf(na, done);
        Reg ua = newReg(inner);
        Reg ub = newReg(inner);
        emit(Op::Unwrap, ua, a);
        emit(Op::Unwrap, ub, b);
        emit(Op::Move, res, equalsOf(ua, ub, inner));
    } else if (info.kind == TypeKind::Tuple) {
        emit(Op::Move, res, constInt(1, kBool));
        std::vector<TypeId> elems = info.elems;
        for (std::uint32_t i = 0; i < elems.size(); ++i) {
            Reg fa = newReg(elems[i]);
            Reg fb = newReg(elems[i]);
            emit(Op::FieldGet, fa, a, kNoReg, i);
            emit(Op::FieldGet, fb, b, kNoReg, i);
            emit(Op::Move, res, equalsOf(fa, fb, elems[i]));
            Reg differ = newReg(kBool);
            emit(Op::Not, differ, res);
            brIf(differ, done);
        }
    } else if (info.kind == TypeKind::List) {
        TypeId elem = info.elem;
        Reg na = listLength(a);
        emit(Op::Eq, res, na, listLength(b));
        Reg differ = newReg(kBool);
        emit(Op::Not, differ, res);
        brIf(differ, done);
        forEachIndex(na, [&](Reg i) {
            emit(Op::Move, res, equalsOf(listAt(a, i, elem), listAt(b, i, elem), elem));
            Reg unequal = newReg(kBool);
            emit(Op::Not, unequal, res);
            brIf(unequal, done);
        });
    } else {
        diags_.error({}, std::format("internal: no equality for '{}'", types_->name(t)));
        emit(Op::Move, res, constInt(0, kBool));
    }
    close();
    return res;
}

Reg Lowerer::signOf(Reg lt, Reg gt) {
    Reg l = newReg(kI64);
    Reg g = newReg(kI64);
    emit(Op::Convert, l, lt);
    emit(Op::Convert, g, gt);
    Reg r = newReg(kI64);
    emit(Op::Sub, r, g, l);
    return r;
}

Reg Lowerer::compareOf(Reg a, Reg b, TypeId t) {
    std::vector<TypeId> targs;
    if (sema::FnInfo* fn = userMethod(t, "cmp", targs)) {
        return callMethod(fn, targs, {a, b}, kI64);
    }
    if (vmOrdered(t) && !types_->isFloat(t)) {
        Reg lt = newReg(kBool);
        Reg gt = newReg(kBool);
        emit(Op::Lt, lt, a, b);
        emit(Op::Lt, gt, b, a);
        return signOf(lt, gt);
    }
    if (t == kBool) {
        Reg ia = newReg(kI64);
        Reg ib = newReg(kI64);
        emit(Op::Convert, ia, a);
        emit(Op::Convert, ib, b);
        return compareOf(ia, ib, kI64);
    }
    Reg res = newReg(kI64);
    emit(Op::Move, res, constInt(0, kI64));
    std::size_t done = depth_;
    open(Op::Block);
    if (types_->isFloat(t)) {
        // parity: java.lang.Double.compare, a total order: -0.0 < 0.0 and NaN above everything, equal to itself.
        Reg c = newReg(kBool);
        emit(Op::Lt, c, a, b);
        open(Op::If, c);
        emit(Op::Move, res, constInt(-1, kI64));
        br(done);
        close();
        Reg d = newReg(kBool);
        emit(Op::Lt, d, b, a);
        open(Op::If, d);
        emit(Op::Move, res, constInt(1, kI64));
        br(done);
        close();
        Reg an = newReg(kBool);
        Reg bn = newReg(kBool);
        emit(Op::Ne, an, a, a);
        emit(Op::Ne, bn, b, b);
        Reg nn = newReg(kBool);
        emit(Op::BitOr, nn, an, bn);
        open(Op::If, nn);
        emit(Op::Move, res, signOf(bn, an));
        br(done);
        close();
        Reg one = constFloat(1.0, t);
        Reg ia = newReg(t);
        Reg ib = newReg(t);
        emit(Op::Div, ia, one, a);
        emit(Op::Div, ib, one, b);
        Reg zl = newReg(kBool);
        Reg zg = newReg(kBool);
        emit(Op::Lt, zl, ia, ib);
        emit(Op::Lt, zg, ib, ia);
        emit(Op::Move, res, signOf(zl, zg));
    } else if (types_->kind(t) == TypeKind::Tuple) {
        std::vector<TypeId> elems = types_->info(t).elems;
        for (std::uint32_t i = 0; i < elems.size(); ++i) {
            Reg fa = newReg(elems[i]);
            Reg fb = newReg(elems[i]);
            emit(Op::FieldGet, fa, a, kNoReg, i);
            emit(Op::FieldGet, fb, b, kNoReg, i);
            emit(Op::Move, res, compareOf(fa, fb, elems[i]));
            Reg differ = newReg(kBool);
            emit(Op::Ne, differ, res, constInt(0, kI64));
            brIf(differ, done);
        }
    } else if (types_->kind(t) == TypeKind::List) {
        // Lexicographic, like tuples; a list that is a prefix of the other comes first.
        TypeId elem = types_->info(t).elem;
        Reg na = listLength(a);
        Reg nb = listLength(b);
        Reg shorter = newReg(kI64);
        Reg aFirst = newReg(kBool);
        emit(Op::Lt, aFirst, na, nb);
        open(Op::If, aFirst);
        emit(Op::Move, shorter, na);
        emit(Op::Else);
        emit(Op::Move, shorter, nb);
        close();
        forEachIndex(shorter, [&](Reg i) {
            emit(Op::Move, res, compareOf(listAt(a, i, elem), listAt(b, i, elem), elem));
            Reg differ = newReg(kBool);
            emit(Op::Ne, differ, res, constInt(0, kI64));
            brIf(differ, done);
        });
        emit(Op::Move, res, compareOf(na, nb, kI64));
    } else {
        diags_.error({}, std::format("internal: no ordering for '{}'", types_->name(t)));
    }
    close();
    return res;
}

Reg Lowerer::concatParts(const std::vector<Reg>& parts) {
    if (parts.size() == 1) {
        return parts[0];
    }
    Reg out = newReg(kStr);
    emitArgs(Op::Intrinsic, out, static_cast<std::uint32_t>(Intrinsic::StrConcat), parts);
    return out;
}

Reg Lowerer::showOf(Reg r, TypeId t) {
    const TypeInfo& info = types_->info(t);
    if (types_->isScalar(t) || t == kStr) {
        return toStr(r, t);
    }
    std::vector<TypeId> targs;
    if (sema::FnInfo* fn = userMethod(t, "show", targs)) {
        return callMethod(fn, targs, {r}, kStr);
    }
    if (info.kind == TypeKind::Tuple) {
        std::vector<TypeId> elems = info.elems;
        std::vector<Reg> parts{constStr("(")};
        for (std::uint32_t i = 0; i < elems.size(); ++i) {
            if (i > 0) {
                parts.push_back(constStr(", "));
            }
            Reg f = newReg(elems[i]);
            emit(Op::FieldGet, f, r, kNoReg, i);
            parts.push_back(showOf(f, elems[i]));
        }
        if (info.array) {
            parts.front() = constStr("[");
        }
        parts.push_back(constStr(info.array ? "]" : elems.size() == 1 ? ",)" : ")"));
        return concatParts(parts);
    }
    Reg res = newReg(kStr);
    if (info.kind == TypeKind::Nullable) {
        TypeId inner = info.elem;
        Reg isNull = newReg(kBool);
        emit(Op::IsNull, isNull, r);
        open(Op::If, isNull);
        emit(Op::Move, res, constStr("null"));
        emit(Op::Else);
        Reg u = newReg(inner);
        emit(Op::Unwrap, u, r);
        emit(Op::Move, res, showOf(u, inner));
        close();
        return res;
    }
    if (info.kind == TypeKind::List) {
        // "[a, b]": each element shown, then joined.
        TypeId elem = info.elem;
        Reg texts = newReg(types_->list(kStr));
        emitArgs(Op::Intrinsic, texts, static_cast<std::uint32_t>(Intrinsic::ListNew), {});
        Reg n = newReg(kI64);
        emitArgs(Op::Intrinsic, n, static_cast<std::uint32_t>(Intrinsic::ListLen), {r});
        Reg i = newReg(kI64);
        emit(Op::Move, i, constInt(0, kI64));
        std::size_t exit = depth_;
        open(Op::Block);
        std::size_t loop = depth_;
        open(Op::Loop);
        Reg end = newReg(kBool);
        emit(Op::Ge, end, i, n);
        brIf(end, exit);
        Reg x = newReg(elem);
        emitArgs(Op::Intrinsic, x, static_cast<std::uint32_t>(Intrinsic::ListGet), {r, i});
        emitArgs(Op::Intrinsic, kNoReg, static_cast<std::uint32_t>(Intrinsic::ListPush), {texts, showOf(x, elem)});
        emit(Op::Add, i, i, constInt(1, kI64));
        br(loop);
        close();
        close();
        Reg joined = newReg(kStr);
        emitArgs(Op::Intrinsic, joined, static_cast<std::uint32_t>(Intrinsic::ListJoin), {texts, constStr(", ")});
        return concatParts({constStr("["), joined, constStr("]")});
    }
    if (info.kind == TypeKind::Map) {
        // "{k=v, k2=v2}" in insertion order, as Kotlin prints a map.
        TypeId key = info.key;
        TypeId value = info.elem;
        Reg texts = newReg(types_->list(kStr));
        emitArgs(Op::Intrinsic, texts, static_cast<std::uint32_t>(Intrinsic::ListNew), {});
        Reg cursor = newReg(kI64);
        emitArgs(Op::Intrinsic, cursor, static_cast<std::uint32_t>(Intrinsic::MapNext), {r, constInt(-1, kI64)});
        std::size_t exit = depth_;
        open(Op::Block);
        std::size_t loop = depth_;
        open(Op::Loop);
        Reg end = newReg(kBool);
        emit(Op::Lt, end, cursor, constInt(0, kI64));
        brIf(end, exit);
        Reg k = newReg(key);
        emitArgs(Op::Intrinsic, k, static_cast<std::uint32_t>(Intrinsic::MapKeyAt), {r, cursor});
        Reg v = newReg(value);
        emitArgs(Op::Intrinsic, v, static_cast<std::uint32_t>(Intrinsic::MapValueAt), {r, cursor});
        Reg entry = concatParts({showOf(k, key), constStr("="), showOf(v, value)});
        emitArgs(Op::Intrinsic, kNoReg, static_cast<std::uint32_t>(Intrinsic::ListPush), {texts, entry});
        emitArgs(Op::Intrinsic, cursor, static_cast<std::uint32_t>(Intrinsic::MapNext), {r, cursor});
        br(loop);
        close();
        close();
        Reg joined = newReg(kStr);
        emitArgs(Op::Intrinsic, joined, static_cast<std::uint32_t>(Intrinsic::ListJoin), {texts, constStr(", ")});
        return concatParts({constStr("{"), joined, constStr("}")});
    }
    diags_.error({}, std::format("internal: no text form for '{}'", types_->name(t)));
    emit(Op::Move, res, constStr(""));
    return res;
}

// `"{x:spec}"`: numbers keep their value for the runtime to lay out; anything else is shown first, then padded.
Reg Lowerer::formatted(Reg v, TypeId t, std::uint64_t spec) {
    Reg arg = v;
    TypeId at = types_->isInteger(t) ? (types_->isSigned(t) ? kI64 : kU64) : types_->isFloat(t) ? kF64 : kStr;
    if (at == kStr) {
        arg = toStr(v, t);
    } else if (at != t) {
        arg = newReg(at);
        emit(Op::Convert, arg, v);
    }
    Reg out = newReg(kStr);
    emitArgs(Op::Intrinsic, out, static_cast<std::uint32_t>(Intrinsic::StrFormat),
             {arg, constInt(static_cast<std::int64_t>(spec), kI64)});
    return out;
}

Reg Lowerer::lowerTraitCompare(BinaryOp op, Reg l, Reg r, TypeId t) {
    Reg res = newReg(kBool);
    if (op == BinaryOp::Eq || op == BinaryOp::Ne) {
        Reg e = equalsOf(l, r, t);
        if (op == BinaryOp::Eq) {
            return e;
        }
        emit(Op::Not, res, e);
        return res;
    }
    emit(binaryOp(op), res, compareOf(l, r, t), constInt(0, kI64));
    return res;
}

Reg Lowerer::langTraitCall(CallExpr* e, TypeId recvType, Reg dst) {
    std::string method = e->fn->name.substr(e->fn->name.rfind('.') + 1);
    Reg self = lowerExpr(e->receiver, recvType);
    Reg out = kNoReg;
    if (method == "show") {
        out = showOf(self, recvType);
    } else if (method == "eq" || method == "cmp") {
        Reg other = lowerExpr(e->ordered[0], recvType);
        out = method == "eq" ? equalsOf(self, other, recvType) : compareOf(self, other, recvType);
    }
    if (out != kNoReg && dst != kNoReg) {
        emit(Op::Move, dst, out);
    }
    return dst;
}

void Lowerer::lowerDerived(sema::FnInfo* fn, Function& out) {
    if (fn->derived == sema::LangTrait::Decode || fn->derived == sema::LangTrait::Encode) {
        lowerCodecDerived(fn, out);
        return;
    }
    TypeId t = T(fn->owner);
    const TypeInfo info = types_->info(t);
    std::string shown = info.genericBase != 0 ? types_->info(info.genericBase).name : info.name;
    out.result = T(fn->result);
    Reg a = newReg(t);
    Reg b = fn->derived == sema::LangTrait::Show ? kNoReg : newReg(t);
    out.paramCount = static_cast<std::uint32_t>(out.regs.size());
    out.exported = false;
    auto ret = [&](Reg v) { emit(Op::Return, kNoReg, v); };
    // Compares the fields (or one variant's payload) in order; returns early on the first difference.
    auto fields = [&](const std::vector<TypeId>& types, auto&& get) {
        for (std::uint32_t i = 0; i < types.size(); ++i) {
            Reg fa = newReg(types[i]);
            Reg fb = newReg(types[i]);
            get(fa, a, i);
            get(fb, b, i);
            if (fn->derived == sema::LangTrait::Eq) {
                Reg differ = newReg(kBool);
                emit(Op::Not, differ, equalsOf(fa, fb, types[i]));
                open(Op::If, differ);
                ret(constInt(0, kBool));
                close();
            } else {
                Reg c = compareOf(fa, fb, types[i]);
                Reg differ = newReg(kBool);
                emit(Op::Ne, differ, c, constInt(0, kI64));
                open(Op::If, differ);
                ret(c);
                close();
            }
        }
    };
    auto showFields = [&](std::vector<Reg>& parts, const std::vector<TypeId>& types, const std::vector<Field>* names,
                          auto&& get) {
        for (std::uint32_t i = 0; i < types.size(); ++i) {
            parts.push_back(constStr(std::format("{}{}{}", i > 0 ? ", " : "", names != nullptr ? (*names)[i].name : "",
                                                 names != nullptr ? "=" : "")));
            Reg f = newReg(types[i]);
            get(f, a, i);
            parts.push_back(showOf(f, types[i]));
        }
    };
    if (info.kind == TypeKind::Struct) {
        std::vector<TypeId> types;
        for (const Field& f : info.fields) {
            types.push_back(f.type);
        }
        auto get = [&](Reg dst, Reg from, std::uint32_t i) { emit(Op::FieldGet, dst, from, kNoReg, i); };
        if (fn->derived == sema::LangTrait::Show) {
            std::vector<Reg> parts{constStr(shown + "(")};
            showFields(parts, types, &info.fields, get);
            parts.push_back(constStr(")"));
            ret(concatParts(parts));
            return;
        }
        fields(types, get);
        ret(fn->derived == sema::LangTrait::Eq ? constInt(1, kBool) : constInt(0, kI64));
        return;
    }
    Reg ta = newReg(kI64);
    emit(Op::EnumTag, ta, a);
    Reg tb = kNoReg;
    if (b != kNoReg) {
        tb = newReg(kI64);
        emit(Op::EnumTag, tb, b);
        Reg differ = newReg(kBool);
        emit(Op::Ne, differ, ta, tb);
        open(Op::If, differ);
        ret(fn->derived == sema::LangTrait::Eq ? constInt(0, kBool) : compareOf(ta, tb, kI64));
        close();
    }
    for (std::uint32_t v = 0; v < info.variants.size(); ++v) {
        const VariantInfo& var = info.variants[v];
        if (var.payload.empty() && b != kNoReg) {
            continue;
        }
        Reg is = newReg(kBool);
        emit(Op::Eq, is, ta, constInt(v, kI64));
        open(Op::If, is);
        auto get = [&](Reg dst, Reg from, std::uint32_t i) { enumField(dst, from, t, v, i); };
        if (b == kNoReg) {
            std::vector<Reg> parts{constStr(var.payload.empty() ? var.name : var.name + "(")};
            showFields(parts, var.payload, nullptr, get);
            if (!var.payload.empty()) {
                parts.push_back(constStr(")"));
            }
            ret(concatParts(parts));
        } else {
            fields(var.payload, get);
        }
        close();
    }
    if (b == kNoReg) {
        emit(Op::Trap, kNoReg, kNoReg, kNoReg, static_cast<std::uint32_t>(TrapCode::Unreachable));
        return;
    }
    ret(fn->derived == sema::LangTrait::Eq ? constInt(1, kBool) : constInt(0, kI64));
}

} // namespace quartz::ir
