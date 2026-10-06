#include "ir/LowerImpl.hpp"

#include <format>

namespace quartz::ir {

namespace {

enum ValueTag : std::uint32_t { kTagNull, kTagBool, kTagInt, kTagFloat, kTagStr, kTagList, kTagTable };

} // namespace

std::vector<std::string> Lowerer::codecKeys(TypeId t) const {
    const TypeInfo& info = types_->info(t);
    auto it = prog_.codec.keys.find(info.genericBase != 0 ? info.genericBase : t);
    if (it != prog_.codec.keys.end() && it->second.size() == info.fields.size()) {
        return it->second;
    }
    std::vector<std::string> keys;
    for (const Field& f : info.fields) {
        keys.push_back(f.name);
    }
    return keys;
}

// A missing key takes the field's default, or null for a nullable field; any other field must be there.
void Lowerer::decodeStruct(Reg v, TypeId t) {
    const TypeInfo info = types_->info(t);
    const sema::CodecInfo& c = prog_.codec;
    std::vector<std::string> keys = codecKeys(t);
    auto decl = prog_.structDecls.find(info.genericBase != 0 ? info.genericBase : t);
    Reg tag = newReg(kI64);
    emit(Op::EnumTag, tag, v);
    Reg notTable = newReg(kBool);
    emit(Op::Ne, notTable, tag, constInt(kTagTable, kI64));
    open(Op::If, notTable);
    failDecode(codecName(t), v);
    close();
    Reg table = valuePayload(v, kTagTable, types_->map(kStr, c.value));
    std::vector<Reg> fields;
    for (std::size_t i = 0; i < info.fields.size(); ++i) {
        TypeId ft = info.fields[i].type;
        Reg key = constStr(keys[i]);
        Reg field = newReg(ft);
        Reg got = newReg(types_->nullable(c.value));
        emitArgs(Op::Intrinsic, got, static_cast<std::uint32_t>(Intrinsic::MapGet), {table, key});
        Reg missing = newReg(kBool);
        emit(Op::IsNull, missing, got);
        open(Op::If, missing);
        Expr* def = decl != prog_.structDecls.end() && i < decl->second->fields.size()
                        ? decl->second->fields[i].defaultValue
                        : nullptr;
        if (def != nullptr) {
            emit(Op::Move, field, lowerExpr(def, ft));
        } else if (types_->kind(ft) == TypeKind::Nullable) {
            emit(Op::NullOf, field);
        } else {
            returnResult(callHelper(c.missing, {key, constStr(codecName(ft))}), false);
        }
        emit(Op::Else);
        Reg present = newReg(c.value);
        emit(Op::Unwrap, present, got);
        emit(Op::Move, field, decoded(decodeCall(present, ft), ft, [&](Reg e) { return atKey(e, key); }));
        close();
        fields.push_back(field);
    }
    Reg out = newReg(t);
    emitArgs(Op::Make, out, 0, fields);
    returnResult(out, true);
}

// A variant without payload is its name as a str (exact, else ignoring ASCII case); one with a payload is a table
// with its name as the only key, holding the payload, or a list of them when there are several.
void Lowerer::decodeEnum(Reg v, TypeId t) {
    const TypeInfo info = types_->info(t);
    const sema::CodecInfo& c = prog_.codec;
    Reg tag = newReg(kI64);
    emit(Op::EnumTag, tag, v);
    auto byName = [&](Reg text, bool withPayload, const std::function<void(std::uint32_t)>& found) {
        for (bool exact : {true, false}) {
            for (std::uint32_t k = 0; k < info.variants.size(); ++k) {
                if (info.variants[k].payload.empty() == withPayload) {
                    continue;
                }
                Reg same = newReg(kBool);
                if (exact) {
                    emit(Op::Eq, same, text, constStr(info.variants[k].name));
                } else {
                    emitArgs(Op::Intrinsic, same, static_cast<std::uint32_t>(Intrinsic::StrEqualsIgnoreCase),
                             {text, constStr(info.variants[k].name)});
                }
                open(Op::If, same);
                found(k);
                close();
            }
        }
    };
    open(Op::If, valueTagIs(tag, kTagStr));
    byName(valuePayload(v, kTagStr, kStr), false, [&](std::uint32_t k) {
        Reg out = newReg(t);
        emitArgs(Op::EnumMake, out, k, {});
        returnResult(out, true);
    });
    close();
    open(Op::If, valueTagIs(tag, kTagTable));
    Reg table = valuePayload(v, kTagTable, types_->map(kStr, c.value));
    Reg n = newReg(kI64);
    emitArgs(Op::Intrinsic, n, static_cast<std::uint32_t>(Intrinsic::MapLen), {table});
    Reg one = newReg(kBool);
    emit(Op::Eq, one, n, constInt(1, kI64));
    open(Op::If, one);
    Reg cursor = newReg(kI64);
    emitArgs(Op::Intrinsic, cursor, static_cast<std::uint32_t>(Intrinsic::MapNext), {table, constInt(-1, kI64)});
    Reg name = newReg(kStr);
    Reg payload = newReg(c.value);
    emitArgs(Op::Intrinsic, name, static_cast<std::uint32_t>(Intrinsic::MapKeyAt), {table, cursor});
    emitArgs(Op::Intrinsic, payload, static_cast<std::uint32_t>(Intrinsic::MapValueAt), {table, cursor});
    byName(name, true, [&](std::uint32_t k) {
        const std::vector<TypeId> types = info.variants[k].payload;
        std::vector<Reg> parts;
        if (types.size() == 1) {
            parts.push_back(decoded(decodeCall(payload, types[0]), types[0], [&](Reg e) { return atKey(e, name); }));
            Reg out = newReg(t);
            enumMake(out, t, k, parts);
            returnResult(out, true);
            return;
        }
        Reg ptag = newReg(kI64);
        emit(Op::EnumTag, ptag, payload);
        open(Op::If, valueTagIs(ptag, kTagList));
        Reg items = valuePayload(payload, kTagList, types_->list(c.value));
        Reg len = newReg(kI64);
        emitArgs(Op::Intrinsic, len, static_cast<std::uint32_t>(Intrinsic::ListLen), {items});
        Reg fits = newReg(kBool);
        emit(Op::Eq, fits, len, constInt(static_cast<std::int64_t>(types.size()), kI64));
        open(Op::If, fits);
        for (std::uint32_t j = 0; j < types.size(); ++j) {
            Reg item = newReg(c.value);
            emitArgs(Op::Intrinsic, item, static_cast<std::uint32_t>(Intrinsic::ListGet), {items, constInt(j, kI64)});
            parts.push_back(decoded(decodeCall(item, types[j]), types[j],
                                    [&](Reg e) { return atKey(atIndex(e, constInt(j, kI64)), name); }));
        }
        Reg out = newReg(t);
        enumMake(out, t, k, parts);
        returnResult(out, true);
        close();
        close();
        Reg e = callHelper(c.mismatch, {constStr(std::format("a list of {} values", types.size())), payload});
        returnResult(atKey(e, name), false);
    });
    close();
    close();
    failDecode(codecName(t), v);
}

// Fields in declaration order (so the encoded table keeps it); a null nullable field is left out.
Reg Lowerer::encodeStruct(Reg x, TypeId t) {
    const TypeInfo info = types_->info(t);
    std::vector<std::string> keys = codecKeys(t);
    Reg table = newReg(types_->map(kStr, prog_.codec.value));
    emitArgs(Op::Intrinsic, table, static_cast<std::uint32_t>(Intrinsic::MapNew), {});
    for (std::uint32_t i = 0; i < info.fields.size(); ++i) {
        TypeId ft = info.fields[i].type;
        Reg f = newReg(ft);
        emit(Op::FieldGet, f, x, kNoReg, i);
        if (types_->kind(ft) == TypeKind::Nullable) {
            Reg isNull = newReg(kBool);
            emit(Op::IsNull, isNull, f);
            Reg present = newReg(kBool);
            emit(Op::Not, present, isNull);
            open(Op::If, present);
            Reg u = newReg(types_->info(ft).elem);
            emit(Op::Unwrap, u, f);
            emitArgs(Op::Intrinsic, kNoReg, static_cast<std::uint32_t>(Intrinsic::MapSet),
                     {table, constStr(keys[i]), encodeCall(u, types_->info(ft).elem)});
            close();
            continue;
        }
        emitArgs(Op::Intrinsic, kNoReg, static_cast<std::uint32_t>(Intrinsic::MapSet),
                 {table, constStr(keys[i]), encodeCall(f, ft)});
    }
    return makeValue(kTagTable, table);
}

Reg Lowerer::encodeEnum(Reg x, TypeId t) {
    const TypeInfo info = types_->info(t);
    const sema::CodecInfo& c = prog_.codec;
    Reg tag = newReg(kI64);
    emit(Op::EnumTag, tag, x);
    Reg out = newReg(c.value);
    std::size_t done = depth_;
    open(Op::Block);
    for (std::uint32_t k = 0; k < info.variants.size(); ++k) {
        const std::vector<TypeId> types = info.variants[k].payload;
        open(Op::If, valueTagIs(tag, k));
        Reg name = constStr(info.variants[k].name);
        if (types.empty()) {
            emit(Op::Move, out, makeValue(kTagStr, name));
        } else {
            Reg inner = kNoReg;
            if (types.size() == 1) {
                inner = encodeCall(valuePayload(x, k, types[0]), types[0]);
            } else {
                Reg items = newReg(types_->list(c.value));
                emitArgs(Op::Intrinsic, items, static_cast<std::uint32_t>(Intrinsic::ListNew), {});
                for (std::uint32_t j = 0; j < types.size(); ++j) {
                    emitArgs(Op::Intrinsic, kNoReg, static_cast<std::uint32_t>(Intrinsic::ListPush),
                             {items, encodeCall(valuePayload(x, k, types[j], j), types[j])});
                }
                inner = makeValue(kTagList, items);
            }
            Reg table = newReg(types_->map(kStr, c.value));
            emitArgs(Op::Intrinsic, table, static_cast<std::uint32_t>(Intrinsic::MapNew), {});
            emitArgs(Op::Intrinsic, kNoReg, static_cast<std::uint32_t>(Intrinsic::MapSet), {table, name, inner});
            emit(Op::Move, out, makeValue(kTagTable, table));
        }
        br(done);
        close();
    }
    emit(Op::Trap, kNoReg, kNoReg, kNoReg, static_cast<std::uint32_t>(TrapCode::Unreachable));
    close();
    return out;
}

} // namespace quartz::ir
