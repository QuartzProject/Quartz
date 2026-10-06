#include "ir/LowerImpl.hpp"

#include <format>
#include <limits>

namespace quartz::ir {

namespace {

// Variant order of org.quartzlang.data.Value.
enum ValueTag : std::uint32_t { kTagNull, kTagBool, kTagInt, kTagFloat, kTagStr, kTagList, kTagTable };

struct IntRange {
    TypeId type;
    std::int64_t lo;
    std::int64_t hi;
};

constexpr IntRange kIntRanges[] = {
    {kI8, -128, 127},
    {kI16, -32768, 32767},
    {kI32, std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::max()},
    {kU8, 0, 255},
    {kU16, 0, 65535},
    {kU32, 0, 4294967295LL},
    {kU64, 0, std::numeric_limits<std::int64_t>::max()},
};

} // namespace

// Structs and enums use their derived function; every other type gets one generated function, made once.
std::uint32_t Lowerer::codecFunction(TypeId t, bool encode) {
    std::vector<TypeId> targs;
    if (sema::FnInfo* fn = userMethod(t, encode ? "$encode" : "$decode", targs)) {
        return fn->generic() ? instance(fn, targs) : fn->index;
    }
    auto key = std::make_pair(t, encode);
    if (auto it = codecFns_.find(key); it != codecFns_.end()) {
        return it->second;
    }
    std::uint32_t idx = allocFunction();
    codecFns_.emplace(key, idx);
    codecWork_.push_back({t, encode, idx});
    return idx;
}

Reg Lowerer::decodeCall(Reg value, TypeId t) {
    Reg r = newReg(types_->result(t, prog_.codec.error));
    emitArgs(Op::Call, r, codecFunction(t, false), {value});
    return r;
}

Reg Lowerer::encodeCall(Reg x, TypeId t) {
    Reg r = newReg(prog_.codec.value);
    emitArgs(Op::Call, r, codecFunction(t, true), {x});
    return r;
}

Reg Lowerer::lowerCodecCall(CallExpr* e, Reg dst) {
    bool decode = static_cast<SpecialOp>(e->builtin) == SpecialOp::DecodeValue;
    TypeId t = T(e->typeArgs[0]);
    Reg arg = lowerExpr(e->ordered[0], decode ? prog_.codec.value : t);
    emit(Op::Move, dst, decode ? decodeCall(arg, t) : encodeCall(arg, t));
    return dst;
}

std::string Lowerer::codecName(TypeId t) const {
    const TypeInfo& info = types_->info(t);
    std::string base = info.genericBase != 0 ? types_->info(info.genericBase).name : info.name;
    if (info.kind == TypeKind::Struct) {
        return "a table for " + base;
    }
    if (info.kind == TypeKind::Enum && t != prog_.codec.value) {
        std::string names;
        for (std::size_t k = 0; k < info.variants.size(); ++k) {
            names += (k == 0 ? "" : k + 1 == info.variants.size() ? " or " : ", ") + info.variants[k].name;
        }
        return std::format("{} ({})", base, names);
    }
    return types_->name(t);
}

void Lowerer::returnResult(Reg x, bool ok) {
    Reg out = newReg(fn_->result);
    emitArgs(Op::EnumMake, out, ok ? 0 : 1, {x});
    emit(Op::Return, kNoReg, out);
}

Reg Lowerer::callHelper(sema::FnInfo* fn, const std::vector<Reg>& args) {
    Reg e = newReg(prog_.codec.error);
    emitArgs(Op::Call, e, fn->index, args);
    return e;
}

void Lowerer::failDecode(const std::string& expected, Reg value) {
    returnResult(callHelper(prog_.codec.mismatch, {constStr(expected), value}), false);
}

Reg Lowerer::valueTagIs(Reg tag, std::uint32_t k) {
    Reg c = newReg(kBool);
    emit(Op::Eq, c, tag, constInt(k, kI64));
    return c;
}

Reg Lowerer::valuePayload(Reg value, std::uint32_t k, TypeId t, std::uint32_t index) {
    Reg r = newReg(t);
    enumField(r, value, fn_->regs[value], k, index);
    return r;
}

// Returns the `Ok` payload of a decode result; an `Err` returns at once, its error moved down `wrap`'s path.
Reg Lowerer::decoded(Reg result, TypeId t, const std::function<Reg(Reg)>& wrap) {
    Reg tag = newReg(kI64);
    emit(Op::EnumTag, tag, result);
    open(Op::If, valueTagIs(tag, 1));
    Reg e = valuePayload(result, 1, prog_.codec.error);
    returnResult(wrap ? wrap(e) : e, false);
    close();
    return valuePayload(result, 0, t);
}

Reg Lowerer::atKey(Reg e, Reg key) { return callHelper(prog_.codec.atKey, {e, key}); }

Reg Lowerer::atIndex(Reg e, Reg index) { return callHelper(prog_.codec.atIndex, {e, index}); }

void Lowerer::lowerCodec(const CodecWork& w) {
    Function& out = function(w.index);
    subst_.clear();
    fn_ = &out;
    info_ = nullptr;
    depth_ = 0;
    loops_.clear();
    scopes_.clear();
    retReg_ = kNoReg;
    out.name = std::format("{}<{}>", w.encode ? "encode" : "decode", types_->name(w.type));
    out.result = w.encode ? prog_.codec.value : types_->result(w.type, prog_.codec.error);
    Reg arg = newReg(w.encode ? w.type : prog_.codec.value);
    out.paramCount = 1;
    out.exported = false;
    if (w.encode) {
        emit(Op::Return, kNoReg, encodeBuiltin(arg, w.type));
    } else {
        decodeBuiltin(arg, w.type);
    }
}

void Lowerer::lowerCodecDerived(sema::FnInfo* fn, Function& out) {
    TypeId t = T(fn->owner);
    bool encode = fn->derived == sema::LangTrait::Encode;
    out.result = T(fn->result);
    Reg arg = newReg(encode ? t : prog_.codec.value);
    out.paramCount = 1;
    out.exported = false;
    scopes_.clear();
    retReg_ = kNoReg;
    bool isStruct = types_->kind(t) == TypeKind::Struct;
    if (encode) {
        emit(Op::Return, kNoReg, isStruct ? encodeStruct(arg, t) : encodeEnum(arg, t));
    } else if (isStruct) {
        decodeStruct(arg, t);
    } else {
        decodeEnum(arg, t);
    }
}

void Lowerer::decodeBuiltin(Reg v, TypeId t) {
    const TypeInfo info = types_->info(t);
    const sema::CodecInfo& c = prog_.codec;
    if (t == c.value) {
        returnResult(v, true);
        return;
    }
    Reg tag = newReg(kI64);
    emit(Op::EnumTag, tag, v);
    std::string name = codecName(t);
    if (types_->isInteger(t)) {
        open(Op::If, valueTagIs(tag, kTagInt));
        Reg x = valuePayload(v, kTagInt, kI64);
        for (const IntRange& r : kIntRanges) {
            if (r.type != t) {
                continue;
            }
            Reg low = newReg(kBool);
            Reg high = newReg(kBool);
            Reg out = newReg(kBool);
            emit(Op::Lt, low, x, constInt(r.lo, kI64));
            emit(Op::Gt, high, x, constInt(r.hi, kI64));
            emit(Op::BitOr, out, low, high);
            open(Op::If, out);
            failDecode(name, v);
            close();
        }
        Reg n = x;
        if (t != kI64) {
            n = newReg(t);
            emit(Op::Convert, n, x);
        }
        returnResult(n, true);
        close();
    } else if (types_->isFloat(t)) {
        for (std::uint32_t k : {kTagFloat, kTagInt}) {
            open(Op::If, valueTagIs(tag, k));
            Reg x = valuePayload(v, k, k == kTagFloat ? kF64 : kI64);
            Reg f = x;
            if (t != kF64 || k == kTagInt) {
                f = newReg(t);
                emit(Op::Convert, f, x);
            }
            returnResult(f, true);
            close();
        }
    } else if (t == kBool || t == kStr) {
        std::uint32_t k = t == kBool ? kTagBool : kTagStr;
        open(Op::If, valueTagIs(tag, k));
        returnResult(valuePayload(v, k, t), true);
        close();
    } else if (t == kChar) {
        name = "char (a str of one character)";
        open(Op::If, valueTagIs(tag, kTagStr));
        Reg s = valuePayload(v, kTagStr, kStr);
        Reg len = newReg(kI64);
        emitArgs(Op::Intrinsic, len, static_cast<std::uint32_t>(Intrinsic::StrLen), {s});
        Reg some = newReg(kBool);
        emit(Op::Gt, some, len, constInt(0, kI64));
        open(Op::If, some);
        Reg dec = newReg(types_->tuple({kChar, kI64}));
        emitArgs(Op::Intrinsic, dec, static_cast<std::uint32_t>(Intrinsic::StrDecode), {s, constInt(0, kI64)});
        Reg next = newReg(kI64);
        emit(Op::FieldGet, next, dec, kNoReg, 1);
        Reg one = newReg(kBool);
        emit(Op::Eq, one, next, len);
        open(Op::If, one);
        Reg ch = newReg(kChar);
        emit(Op::FieldGet, ch, dec, kNoReg, 0);
        returnResult(ch, true);
        close();
        close();
        close();
    } else if (info.kind == TypeKind::Nullable) {
        open(Op::If, valueTagIs(tag, kTagNull));
        Reg none = newReg(t);
        emit(Op::NullOf, none);
        returnResult(none, true);
        close();
        Reg x = decoded(decodeCall(v, info.elem), info.elem, nullptr);
        Reg w = newReg(t);
        emit(Op::Wrap, w, x);
        returnResult(w, true);
        return;
    } else if (info.kind == TypeKind::List) {
        open(Op::If, valueTagIs(tag, kTagList));
        Reg items = valuePayload(v, kTagList, types_->list(c.value));
        Reg out = newReg(t);
        emitArgs(Op::Intrinsic, out, static_cast<std::uint32_t>(Intrinsic::ListNew), {});
        Reg n = newReg(kI64);
        emitArgs(Op::Intrinsic, n, static_cast<std::uint32_t>(Intrinsic::ListLen), {items});
        Reg i = newReg(kI64);
        emit(Op::Move, i, constInt(0, kI64));
        std::size_t exit = depth_;
        open(Op::Block);
        std::size_t loop = depth_;
        open(Op::Loop);
        Reg end = newReg(kBool);
        emit(Op::Ge, end, i, n);
        brIf(end, exit);
        Reg item = newReg(c.value);
        emitArgs(Op::Intrinsic, item, static_cast<std::uint32_t>(Intrinsic::ListGet), {items, i});
        Reg x = decoded(decodeCall(item, info.elem), info.elem, [&](Reg e) { return atIndex(e, i); });
        emitArgs(Op::Intrinsic, kNoReg, static_cast<std::uint32_t>(Intrinsic::ListPush), {out, x});
        emit(Op::Add, i, i, constInt(1, kI64));
        br(loop);
        close();
        close();
        returnResult(out, true);
        close();
    } else if (info.kind == TypeKind::Map) {
        open(Op::If, valueTagIs(tag, kTagTable));
        Reg table = valuePayload(v, kTagTable, types_->map(kStr, c.value));
        Reg out = newReg(t);
        emitArgs(Op::Intrinsic, out, static_cast<std::uint32_t>(Intrinsic::MapNew), {});
        eachEntry(table, [&](Reg key, Reg item) {
            Reg x = decoded(decodeCall(item, info.elem), info.elem, [&](Reg e) { return atKey(e, key); });
            emitArgs(Op::Intrinsic, kNoReg, static_cast<std::uint32_t>(Intrinsic::MapSet), {out, key, x});
        });
        returnResult(out, true);
        close();
    }
    failDecode(name, v);
}

// Calls `body(key, value)` for each entry of a `Map<str, Value>` in insertion order.
void Lowerer::eachEntry(Reg table, const std::function<void(Reg, Reg)>& body) {
    Reg cursor = newReg(kI64);
    emitArgs(Op::Intrinsic, cursor, static_cast<std::uint32_t>(Intrinsic::MapNext), {table, constInt(-1, kI64)});
    std::size_t exit = depth_;
    open(Op::Block);
    std::size_t loop = depth_;
    open(Op::Loop);
    Reg done = newReg(kBool);
    emit(Op::Lt, done, cursor, constInt(0, kI64));
    brIf(done, exit);
    const TypeInfo info = types_->info(fn_->regs[table]);
    Reg key = newReg(info.key);
    Reg value = newReg(info.elem);
    emitArgs(Op::Intrinsic, key, static_cast<std::uint32_t>(Intrinsic::MapKeyAt), {table, cursor});
    emitArgs(Op::Intrinsic, value, static_cast<std::uint32_t>(Intrinsic::MapValueAt), {table, cursor});
    body(key, value);
    emitArgs(Op::Intrinsic, cursor, static_cast<std::uint32_t>(Intrinsic::MapNext), {table, cursor});
    br(loop);
    close();
    close();
}

Reg Lowerer::makeValue(std::uint32_t tag, Reg payload) {
    Reg v = newReg(prog_.codec.value);
    emitArgs(Op::EnumMake, v, tag, payload == kNoReg ? std::vector<Reg>{} : std::vector<Reg>{payload});
    return v;
}

Reg Lowerer::encodeBuiltin(Reg x, TypeId t) {
    const TypeInfo info = types_->info(t);
    const sema::CodecInfo& c = prog_.codec;
    if (t == c.value) {
        return x;
    }
    if (types_->isInteger(t) || types_->isFloat(t)) {
        TypeId wide = types_->isInteger(t) ? kI64 : kF64;
        Reg n = x;
        if (t != wide) {
            n = newReg(wide);
            emit(Op::Convert, n, x);
        }
        return makeValue(wide == kI64 ? kTagInt : kTagFloat, n);
    }
    if (t == kBool || t == kStr) {
        return makeValue(t == kBool ? kTagBool : kTagStr, x);
    }
    if (t == kChar) {
        Reg s = newReg(kStr);
        emitArgs(Op::Intrinsic, s, static_cast<std::uint32_t>(Intrinsic::StrFromChar), {x});
        return makeValue(kTagStr, s);
    }
    Reg out = newReg(c.value);
    if (info.kind == TypeKind::Nullable) {
        Reg isNull = newReg(kBool);
        emit(Op::IsNull, isNull, x);
        open(Op::If, isNull);
        emit(Op::Move, out, makeValue(kTagNull, kNoReg));
        emit(Op::Else);
        Reg u = newReg(info.elem);
        emit(Op::Unwrap, u, x);
        emit(Op::Move, out, encodeCall(u, info.elem));
        close();
        return out;
    }
    if (info.kind == TypeKind::List) {
        Reg items = newReg(types_->list(c.value));
        emitArgs(Op::Intrinsic, items, static_cast<std::uint32_t>(Intrinsic::ListNew), {});
        Reg n = newReg(kI64);
        emitArgs(Op::Intrinsic, n, static_cast<std::uint32_t>(Intrinsic::ListLen), {x});
        Reg i = newReg(kI64);
        emit(Op::Move, i, constInt(0, kI64));
        std::size_t exit = depth_;
        open(Op::Block);
        std::size_t loop = depth_;
        open(Op::Loop);
        Reg end = newReg(kBool);
        emit(Op::Ge, end, i, n);
        brIf(end, exit);
        Reg item = newReg(info.elem);
        emitArgs(Op::Intrinsic, item, static_cast<std::uint32_t>(Intrinsic::ListGet), {x, i});
        emitArgs(Op::Intrinsic, kNoReg, static_cast<std::uint32_t>(Intrinsic::ListPush),
                 {items, encodeCall(item, info.elem)});
        emit(Op::Add, i, i, constInt(1, kI64));
        br(loop);
        close();
        close();
        return makeValue(kTagList, items);
    }
    Reg table = newReg(types_->map(kStr, c.value));
    emitArgs(Op::Intrinsic, table, static_cast<std::uint32_t>(Intrinsic::MapNew), {});
    eachEntry(x, [&](Reg key, Reg item) {
        emitArgs(Op::Intrinsic, kNoReg, static_cast<std::uint32_t>(Intrinsic::MapSet),
                 {table, key, encodeCall(item, info.elem)});
    });
    return makeValue(kTagTable, table);
}

} // namespace quartz::ir
