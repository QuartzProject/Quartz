#include "sema/SemaImpl.hpp"

#include <bit>
#include <cmath>
#include <cstdint>
#include <format>

namespace quartz::sema {

namespace {

using Kind = ConstValue::Kind;

ConstValue intValue(std::uint64_t bits, TypeId t) {
    ConstValue v;
    v.kind = Kind::Int;
    v.type = t;
    v.bits = bits;
    return v;
}

ConstValue floatValue(double d, TypeId t) {
    ConstValue v;
    v.kind = Kind::Float;
    v.type = t;
    v.bits = std::bit_cast<std::uint64_t>(t == ir::kF32 ? static_cast<double>(static_cast<float>(d)) : d);
    return v;
}

double asDouble(const ConstValue& v) { return std::bit_cast<double>(v.bits); }

// parity: vm/Interp.cpp `saturate`, the `as` conversion from a float to an integer.
std::uint64_t saturate(double d, TypeId t) {
    if (std::isnan(d)) {
        return 0;
    }
    auto clampSigned = [&](double lo, double hi, std::int64_t loV, std::int64_t hiV) {
        return static_cast<std::uint64_t>(d <= lo ? loV : d >= hi ? hiV : static_cast<std::int64_t>(d));
    };
    auto clampUnsigned = [&](double hi, std::uint64_t hiV) {
        return d <= 0.0 ? 0 : d >= hi ? hiV : static_cast<std::uint64_t>(d);
    };
    switch (t) {
    case ir::kI8:
        return clampSigned(-128.0, 127.0, -128, 127);
    case ir::kI16:
        return clampSigned(-32768.0, 32767.0, -32768, 32767);
    case ir::kI32:
        return clampSigned(-2147483648.0, 2147483647.0, INT32_MIN, INT32_MAX);
    case ir::kU8:
        return clampUnsigned(255.0, 255);
    case ir::kU16:
        return clampUnsigned(65535.0, 65535);
    case ir::kU32:
        return clampUnsigned(4294967295.0, 4294967295ULL);
    case ir::kU64:
        return clampUnsigned(18446744073709551616.0, UINT64_MAX);
    default:
        return clampSigned(-9223372036854775808.0, 9223372036854775808.0, INT64_MIN, INT64_MAX);
    }
}

} // namespace

std::uint64_t Checker::normalizeInt(std::uint64_t bits, TypeId t) const {
    switch (t) {
    case ir::kI8:
        return static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int8_t>(bits)));
    case ir::kI16:
        return static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int16_t>(bits)));
    case ir::kI32:
        return static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(bits)));
    case ir::kU8:
        return bits & 0xFF;
    case ir::kU16:
        return bits & 0xFFFF;
    case ir::kU32:
        return bits & 0xFFFFFFFF;
    default:
        return bits;
    }
}

std::optional<ConstValue> Checker::notConstant(Span span, std::string_view why) {
    diags_.error(span, std::format("a constant needs a value known at compile time: {}", why));
    return std::nullopt;
}

TypeId Checker::checkGlobal(GlobalInfo* g, Span use) {
    if (g->type != ir::kError) {
        return g->type;
    }
    if (g->checking) {
        diags_.error(use, std::format("constant '{}' depends on itself", str(g->decl->name)));
        return ir::kError;
    }
    g->checking = true;
    auto saved = std::move(scopes_);
    scopes_.clear();
    std::uint32_t savedFile = currentFile_;
    bool savedLint = lintFn_;
    lintFn_ = false;
    currentFile_ = g->decl->span.file;
    TypeId declared = g->decl->type != nullptr ? resolveType(g->decl->type) : ir::kError;
    g->type = checkValue(g->decl->value, declared);
    if (declared != ir::kError) {
        expectAssignable(g->type, declared, g->decl->value->span);
        g->type = declared;
    }
    scopes_ = std::move(saved);
    currentFile_ = savedFile;
    lintFn_ = savedLint;
    return g->type;
}

const ConstValue* Checker::globalValue(GlobalInfo* g, Span use) {
    if (g->value != nullptr || g->evaluated) {
        return g->value.get();
    }
    if (g->evaluating || checkGlobal(g, use) == ir::kError) {
        return nullptr;
    }
    g->evaluating = true;
    std::uint32_t savedFile = currentFile_;
    currentFile_ = g->decl->span.file;
    if (std::optional<ConstValue> v = evalConst(g->decl->value)) {
        g->value = std::make_shared<const ConstValue>(coerceConst(std::move(*v), g->type));
    }
    currentFile_ = savedFile;
    g->evaluating = false;
    g->evaluated = true;
    return g->value.get();
}

void Checker::evaluateGlobals() {
    for (GlobalInfo* g : globalList_) {
        if (g->type != ir::kError) {
            globalValue(g, g->decl->span);
        }
    }
}

std::shared_ptr<const ConstValue> Checker::constantOf(Expr* e) {
    std::optional<ConstValue> v = evalConst(e);
    return v ? std::make_shared<const ConstValue>(std::move(*v)) : nullptr;
}

ConstValue Checker::coerceConst(ConstValue v, TypeId to) const {
    if (to == ir::kError || v.type == to || types_->kind(to) != ir::TypeKind::Nullable) {
        return v;
    }
    if (v.kind == Kind::Null || types_->kind(v.type) == ir::TypeKind::Nullable) {
        v.type = to;
        return v;
    }
    ConstValue some;
    some.kind = Kind::Some;
    some.type = to;
    some.parts.push_back(std::move(v));
    return some;
}

std::optional<ConstValue> Checker::evalConst(const Expr* e) {
    TypeId t = e->type;
    switch (e->kind) {
    case ExprKind::IntLit: {
        std::uint64_t raw = static_cast<const IntLitExpr*>(e)->value;
        if (types_->isFloat(t)) {
            return floatValue(static_cast<double>(static_cast<std::int64_t>(raw)), t);
        }
        return intValue(normalizeInt(raw, t), t);
    }
    case ExprKind::FloatLit:
        return floatValue(static_cast<const FloatLitExpr*>(e)->value, t);
    case ExprKind::BoolLit:
        return intValue(static_cast<const BoolLitExpr*>(e)->value ? 1 : 0, ir::kBool);
    case ExprKind::CharLit:
        return intValue(static_cast<const CharLitExpr*>(e)->value, ir::kChar);
    case ExprKind::StringLit: {
        ConstValue v;
        v.kind = Kind::Str;
        v.type = ir::kStr;
        v.text = std::string(static_cast<const StringLitExpr*>(e)->value);
        return v;
    }
    case ExprKind::NullLit: {
        ConstValue v;
        v.kind = Kind::Null;
        v.type = t;
        return v;
    }
    case ExprKind::Ident: {
        const auto* id = static_cast<const IdentExpr*>(e);
        if (id->ref == IdentRef::Const && id->global != nullptr) {
            const ConstValue* v = globalValue(id->global, e->span);
            return v != nullptr ? std::optional(*v) : std::nullopt;
        }
        if (id->ref == IdentRef::Local && id->local != nullptr && id->local->constant != nullptr) {
            return *id->local->constant;
        }
        return notConstant(e->span, std::format("'{}' is not a constant", str(id->name)));
    }
    case ExprKind::Member:
        return evalMember(static_cast<const MemberExpr*>(e));
    case ExprKind::Unary: {
        const auto* u = static_cast<const UnaryExpr*>(e);
        std::optional<ConstValue> v = evalConst(u->operand);
        if (!v || (v->kind != Kind::Int && v->kind != Kind::Float)) {
            return v ? notConstant(e->span, "this operator needs a number or a bool") : std::nullopt;
        }
        switch (u->op) {
        case UnaryOp::Neg:
            return v->kind == Kind::Float ? floatValue(-asDouble(*v), v->type)
                                          : intValue(normalizeInt(0 - v->bits, v->type), v->type);
        case UnaryOp::Not:
            return intValue(v->bits ^ 1, ir::kBool);
        case UnaryOp::BitNot:
            return intValue(normalizeInt(~v->bits, v->type), v->type);
        }
        return std::nullopt;
    }
    case ExprKind::Binary:
        return evalBinary(static_cast<const BinaryExpr*>(e));
    case ExprKind::Cast:
        return evalCast(static_cast<const CastExpr*>(e));
    case ExprKind::TupleLit: {
        ConstValue v;
        v.kind = Kind::Aggregate;
        v.type = t;
        const auto* tl = static_cast<const TupleLitExpr*>(e);
        for (std::size_t i = 0; i < tl->elems.size(); ++i) {
            std::optional<ConstValue> part = evalConst(tl->elems[i]);
            if (!part) {
                return std::nullopt;
            }
            v.parts.push_back(coerceConst(std::move(*part), types_->info(t).elems[i]));
        }
        return v;
    }
    case ExprKind::Call: {
        const auto* c = static_cast<const CallExpr*>(e);
        if (c->callKind != CallKind::Construct && c->callKind != CallKind::Variant) {
            return notConstant(e->span, "function calls run when the program runs");
        }
        ConstValue v;
        v.kind = c->callKind == CallKind::Construct ? Kind::Aggregate : Kind::Variant;
        v.type = t;
        v.variant = c->variant;
        const ir::TypeInfo& info = types_->info(t);
        for (std::size_t i = 0; i < c->ordered.size(); ++i) {
            std::optional<ConstValue> part = evalConst(c->ordered[i]);
            if (!part) {
                return std::nullopt;
            }
            TypeId want = v.kind == Kind::Aggregate ? info.fields[i].type : info.variants[c->variant].payload[i];
            v.parts.push_back(coerceConst(std::move(*part), want));
        }
        return v;
    }
    case ExprKind::With: {
        const auto* w = static_cast<const WithExpr*>(e);
        std::optional<ConstValue> v = evalConst(w->value);
        for (std::size_t i = 0; v && i < w->fields.size(); ++i) {
            std::optional<ConstValue> part = evalConst(w->fields[i].value);
            if (!part) {
                return std::nullopt;
            }
            v->parts[w->fieldIndex[i]] = coerceConst(std::move(*part), types_->info(t).fields[w->fieldIndex[i]].type);
        }
        return v;
    }
    case ExprKind::ListLit:
    case ExprKind::ListRepeat: {
        if (!types_->isArray(t)) {
            return notConstant(e->span, "lists can change, so they cannot be constants; use a function");
        }
        ConstValue v;
        v.kind = Kind::Aggregate;
        v.type = t;
        TypeId elem = types_->info(t).elems[0];
        if (e->kind == ExprKind::ListLit) {
            for (const Expr* x : static_cast<const ListLitExpr*>(e)->elems) {
                std::optional<ConstValue> part = evalConst(x);
                if (!part) {
                    return std::nullopt;
                }
                v.parts.push_back(coerceConst(std::move(*part), elem));
            }
        } else {
            std::optional<ConstValue> part = evalConst(static_cast<const ListRepeatExpr*>(e)->value);
            if (!part) {
                return std::nullopt;
            }
            v.parts.assign(types_->info(t).elems.size(), coerceConst(std::move(*part), elem));
        }
        return v;
    }
    case ExprKind::Index: {
        const auto* ix = static_cast<const IndexExpr*>(e);
        if (!types_->isArray(ix->object->type)) {
            return notConstant(e->span, "only fixed-size arrays are indexed at compile time");
        }
        std::optional<ConstValue> arr = evalConst(ix->object);
        std::optional<ConstValue> at = arr ? evalConst(ix->index) : std::nullopt;
        if (!arr || !at) {
            return std::nullopt;
        }
        if (at->bits >= arr->parts.size()) {
            return notConstant(ix->index->span, "the index is out of bounds");
        }
        return arr->parts[static_cast<std::size_t>(at->bits)];
    }
    case ExprKind::Interp:
        return notConstant(e->span, "interpolation is not evaluated at compile time; use '+'");
    case ExprKind::Lambda:
        return notConstant(e->span, "a function value is not a constant");
    default:
        return notConstant(e->span, "only literals, operators, casts, constants and constructors are allowed");
    }
}

std::optional<ConstValue> Checker::evalMember(const MemberExpr* e) {
    if (e->global != nullptr) {
        const ConstValue* v = globalValue(e->global, e->span);
        return v != nullptr ? std::optional(*v) : std::nullopt;
    }
    if (e->isVariant) {
        ConstValue v;
        v.kind = Kind::Variant;
        v.type = e->type;
        v.variant = e->fieldIndex;
        return v;
    }
    ir::TypeKind k = types_->kind(e->object->type);
    if (e->safe || e->isProperty || (k != ir::TypeKind::Struct && k != ir::TypeKind::Tuple) ||
        types_->isArray(e->object->type)) {
        return notConstant(e->span, "only fields of constant structs and tuples are constant");
    }
    std::optional<ConstValue> v = evalConst(e->object);
    if (!v || v->kind != Kind::Aggregate || e->fieldIndex >= v->parts.size()) {
        return std::nullopt;
    }
    return v->parts[e->fieldIndex];
}

std::optional<ConstValue> Checker::evalBinary(const BinaryExpr* e) {
    std::optional<ConstValue> l = evalConst(e->lhs);
    std::optional<ConstValue> r = l ? evalConst(e->rhs) : std::nullopt;
    if (!l || !r) {
        return std::nullopt;
    }
    TypeId t = e->lhs->type;
    if (e->op == BinaryOp::Coalesce) {
        if (l->kind == Kind::Null) {
            return coerceConst(std::move(*r), e->type);
        }
        ConstValue inner = l->kind == Kind::Some ? l->parts[0] : *l;
        return coerceConst(std::move(inner), e->type);
    }
    bool isFloat = l->kind == Kind::Float && r->kind == Kind::Float;
    bool isInt = l->kind == Kind::Int && r->kind == Kind::Int;
    bool isStr = l->kind == Kind::Str && r->kind == Kind::Str;
    if (!isFloat && !isInt && !isStr) {
        return notConstant(e->span,
                           std::format("'{}' on these values is not evaluated at compile time", binaryOpText(e->op)));
    }
    bool sgn = types_->isSigned(t);
    auto compare = [&]() -> int {
        if (isStr) {
            int c = l->text.compare(r->text);
            return c < 0 ? -1 : c > 0 ? 1 : 0;
        }
        if (isInt) {
            if (sgn) {
                auto a = static_cast<std::int64_t>(l->bits);
                auto b = static_cast<std::int64_t>(r->bits);
                return a < b ? -1 : a > b ? 1 : 0;
            }
            return l->bits < r->bits ? -1 : l->bits > r->bits ? 1 : 0;
        }
        return 2; // floats compare with IEEE rules below
    };
    switch (e->op) {
    case BinaryOp::Eq:
    case BinaryOp::Ne:
    case BinaryOp::Lt:
    case BinaryOp::Le:
    case BinaryOp::Gt:
    case BinaryOp::Ge: {
        bool out = false;
        if (isFloat) {
            double a = asDouble(*l);
            double b = asDouble(*r);
            out = e->op == BinaryOp::Eq   ? a == b
                  : e->op == BinaryOp::Ne ? a != b
                  : e->op == BinaryOp::Lt ? a < b
                  : e->op == BinaryOp::Le ? a <= b
                  : e->op == BinaryOp::Gt ? a > b
                                          : a >= b;
        } else {
            int c = compare();
            out = e->op == BinaryOp::Eq   ? c == 0
                  : e->op == BinaryOp::Ne ? c != 0
                  : e->op == BinaryOp::Lt ? c < 0
                  : e->op == BinaryOp::Le ? c <= 0
                  : e->op == BinaryOp::Gt ? c > 0
                                          : c >= 0;
        }
        return intValue(out ? 1 : 0, ir::kBool);
    }
    case BinaryOp::And:
        return intValue(l->bits & r->bits & 1, ir::kBool);
    case BinaryOp::Or:
        return intValue((l->bits | r->bits) & 1, ir::kBool);
    default:
        break;
    }
    if (isStr) {
        if (e->op != BinaryOp::Add) {
            return notConstant(e->span, "strings only join with '+'");
        }
        ConstValue v = *l;
        v.text += r->text;
        return v;
    }
    if (isFloat) {
        double a = asDouble(*l);
        double b = asDouble(*r);
        bool f32 = t == ir::kF32;
        auto fa = static_cast<float>(a);
        auto fb = static_cast<float>(b);
        switch (e->op) {
        case BinaryOp::Add:
            return floatValue(f32 ? static_cast<double>(fa + fb) : a + b, t);
        case BinaryOp::Sub:
            return floatValue(f32 ? static_cast<double>(fa - fb) : a - b, t);
        case BinaryOp::Mul:
            return floatValue(f32 ? static_cast<double>(fa * fb) : a * b, t);
        case BinaryOp::Div:
            return floatValue(f32 ? static_cast<double>(fa / fb) : a / b, t);
        case BinaryOp::Rem:
            return floatValue(f32 ? static_cast<double>(std::fmod(fa, fb)) : std::fmod(a, b), t);
        default:
            return notConstant(e->span, "this operator needs integers");
        }
    }
    std::uint64_t a = l->bits;
    std::uint64_t b = r->bits;
    std::uint64_t out = 0;
    unsigned mask = static_cast<unsigned>(types_->bitWidth(t) - 1);
    switch (e->op) {
    case BinaryOp::Add:
        out = a + b;
        break;
    case BinaryOp::Sub:
        out = a - b;
        break;
    case BinaryOp::Mul:
        out = a * b;
        break;
    case BinaryOp::Div:
    case BinaryOp::Rem: {
        if (b == 0) {
            return notConstant(e->rhs->span, "this divides by zero");
        }
        bool div = e->op == BinaryOp::Div;
        if (sgn) {
            auto sa = static_cast<std::int64_t>(a);
            auto sb = static_cast<std::int64_t>(b);
            out = sb == -1 ? (div ? 0 - a : 0) : static_cast<std::uint64_t>(div ? sa / sb : sa % sb);
        } else {
            out = div ? a / b : a % b;
        }
        break;
    }
    case BinaryOp::BitAnd:
        out = a & b;
        break;
    case BinaryOp::BitOr:
        out = a | b;
        break;
    case BinaryOp::BitXor:
        out = a ^ b;
        break;
    case BinaryOp::Shl:
        out = a << (b & mask);
        break;
    case BinaryOp::Shr:
        out = sgn ? static_cast<std::uint64_t>(static_cast<std::int64_t>(a) >> (b & mask)) : a >> (b & mask);
        break;
    default:
        return notConstant(e->span, "this operator is not evaluated at compile time");
    }
    return intValue(t == ir::kBool ? out & 1 : normalizeInt(out, t), t);
}

std::optional<ConstValue> Checker::evalCast(const CastExpr* e) {
    std::optional<ConstValue> v = evalConst(e->value);
    if (!v) {
        return std::nullopt;
    }
    TypeId from = e->value->type;
    TypeId to = e->type;
    if (from == to) {
        return v;
    }
    bool fromF = v->kind == Kind::Float;
    bool toF = types_->isFloat(to);
    if ((v->kind != Kind::Int && !fromF) || (!toF && !types_->isScalar(to))) {
        return notConstant(e->span, "only numbers convert with 'as' at compile time");
    }
    if (fromF) {
        return toF ? floatValue(asDouble(*v), to) : intValue(saturate(asDouble(*v), to), to);
    }
    if (toF) {
        bool sgn = types_->isSigned(from);
        auto i = static_cast<std::int64_t>(v->bits);
        if (to == ir::kF32) {
            return floatValue(static_cast<double>(sgn ? static_cast<float>(i) : static_cast<float>(v->bits)), to);
        }
        return floatValue(sgn ? static_cast<double>(i) : static_cast<double>(v->bits), to);
    }
    return intValue(normalizeInt(v->bits, to), to);
}

} // namespace quartz::sema
