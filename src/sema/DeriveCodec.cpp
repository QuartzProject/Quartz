#include "sema/SemaImpl.hpp"

#include <format>
#include <set>

namespace quartz::sema {

// Finds Value and DecodeError, and once functions are declared the error helpers the derived bodies call.
bool Checker::codecTypes(Span span) {
    CodecInfo& c = prog_->codec;
    if (c.error != 0 && (c.mismatch != nullptr || !functionsDeclared_)) {
        return true;
    }
    std::uint32_t saved = currentFile_;
    for (std::uint32_t f = 0; f < files_.size(); ++f) {
        currentFile_ = files_[f].package == kDataPackage ? f : currentFile_;
    }
    bool found = files_[currentFile_].package == kDataPackage;
    auto type = [&](std::string_view name) {
        auto* te = found
                       ? static_cast<TypeEntry*>(findIn(std::string(kDataPackage), names_.intern(name), NameKind::Type))
                       : nullptr;
        return te != nullptr ? te->type : ir::TypeId{0};
    };
    auto fn = [&](std::string_view name) {
        return found ? static_cast<FnInfo*>(findIn(std::string(kDataPackage), names_.intern(name), NameKind::Function))
                     : nullptr;
    };
    c.value = type("Value");
    c.error = type("DecodeError");
    if (functionsDeclared_) {
        c.mismatch = fn("decodeMismatch");
        c.missing = fn("decodeMissing");
        c.atKey = fn("decodeAtKey");
        c.atIndex = fn("decodeAtIndex");
    }
    currentFile_ = saved;
    bool helpers = !functionsDeclared_ ||
                   (c.mismatch != nullptr && c.missing != nullptr && c.atKey != nullptr && c.atIndex != nullptr);
    if (c.value == 0 || c.error == 0 || !helpers) {
        c.error = 0;
        diags_.error(span, "Decode and Encode work through org.quartzlang.data: add 'import org.quartzlang.data.*'");
        return false;
    }
    return true;
}

// `Type.$decode(value) -> Result<Type, DecodeError>` or `Type.$encode(self) -> Value`; lowering writes the body.
FnInfo* Checker::deriveCodec(TypeId type, TypeId owner, TraitInfo* trait) {
    bool decode = trait->lang == LangTrait::Decode;
    auto* fn = prog_->fnStorage.emplace_back(std::make_unique<FnInfo>()).get();
    fn->name = std::format("{}.{}", types_->info(type).name, decode ? "$decode" : "$encode");
    fn->hasSelf = !decode;
    fn->params.push_back(decode ? prog_->codec.value : owner);
    fn->paramNames.push_back(names_.intern(decode ? "value" : "self"));
    fn->defaults.push_back(nullptr);
    fn->result = decode ? types_->result(owner, prog_->codec.error) : prog_->codec.value;
    prog_->methodTable[{type, decode ? "$decode" : "$encode"}] = fn;
    return fn;
}

// Scalars, str, char and Value itself; lists, nullables and maps with str keys of such types.
bool Checker::codecBuiltin(TypeId t, LangTrait lang) {
    const ir::TypeInfo& info = types_->info(t);
    TraitInfo* trait = langTrait(lang);
    if (info.kind == ir::TypeKind::List || info.kind == ir::TypeKind::Nullable) {
        return implements(info.elem, trait);
    }
    if (info.kind == ir::TypeKind::Map) {
        return info.key == ir::kStr && implements(info.elem, trait);
    }
    return types_->isNumeric(t) || t == ir::kBool || t == ir::kChar || t == ir::kStr ||
           (t != 0 && t == prog_->codec.value);
}

// `decodeAs::<T>(value)` and `encodeAs(x)`, which only org.quartzlang.data can call.
bool Checker::checkCodecBuiltin(CallExpr* call, bool decode, TypeId& result) {
    result = ir::kError;
    if (!codecTypes(call->span)) {
        return true;
    }
    auto* ident = call->callee->kind == ExprKind::Ident ? static_cast<IdentExpr*>(call->callee) : nullptr;
    TypeId t = ir::kError;
    if (decode) {
        if (ident == nullptr || ident->typeArgs.size() != 1) {
            diags_.error(call->span, "decodeAs needs its type: decodeAs::<T>(value)");
            return true;
        }
        t = resolveType(ident->typeArgs[0]);
        bindArgs(call, {prog_->codec.value}, {}, {}, 0);
    } else {
        if (call->args.size() != 1) {
            diags_.error(call->span, "encodeAs takes one value");
            return true;
        }
        t = checkValue(call->args[0].value, ir::kError);
        bindArgs(call, {t}, {}, {}, 0);
    }
    TraitInfo* trait = langTrait(decode ? LangTrait::Decode : LangTrait::Encode);
    if (t == ir::kError || !implements(t, trait)) {
        if (t != ir::kError) {
            diags_.error(call->span, std::format("'{}' does not implement {}", typeName(t), trait->name));
        }
        return true;
    }
    call->callKind = CallKind::Special;
    call->builtin = static_cast<std::uint16_t>(decode ? SpecialOp::DecodeValue : SpecialOp::EncodeValue);
    call->typeArgs = arena_.copy(std::vector<std::uint32_t>{t});
    result = decode ? types_->result(t, prog_->codec.error) : prog_->codec.value;
    return true;
}

bool Checker::derivesCodec(const Decl* d) const {
    for (const Attribute& a : d->attrs) {
        for (const AttrArg& arg : str(a.name) == "derive" ? a.args : std::span<AttrArg>{}) {
            auto* id = arg.value->kind == ExprKind::Ident ? static_cast<IdentExpr*>(arg.value) : nullptr;
            if (id != nullptr && (str(id->name) == "Decode" || str(id->name) == "Encode")) {
                return true;
            }
        }
    }
    return false;
}

// `@key("max-players")` renames a field in the decoded and encoded table; two fields cannot share a key.
void Checker::checkCodecKeys(const DeriveInfo& d) {
    StructDecl* decl = structDeclOf(d.type);
    if (decl == nullptr) {
        return;
    }
    std::set<std::string> seen;
    std::vector<std::string>& keys = prog_->codec.keys[d.type];
    keys.clear();
    for (const FieldDecl& f : decl->fields) {
        std::string key(str(f.name));
        for (const Attribute& a : f.attrs) {
            if (str(a.name) != "key") {
                continue;
            }
            Expr* arg = a.args.size() == 1 && !a.args[0].name.valid() ? a.args[0].value : nullptr;
            if (arg == nullptr || arg->kind != ExprKind::StringLit) {
                diags_.error(a.span, "@key takes the key as one string: @key(\"max-players\")");
                continue;
            }
            key = std::string(static_cast<StringLitExpr*>(arg)->value);
            if (key.empty()) {
                diags_.error(a.span, "@key cannot be empty");
            }
        }
        if (!seen.insert(key).second) {
            diags_.error(f.span, std::format("two fields of '{}' use the key '{}'", typeName(d.type), key));
        }
        keys.push_back(key);
    }
}

} // namespace quartz::sema
