#include "sema/SemaImpl.hpp"

#include <algorithm>
#include <format>

namespace quartz::sema {

namespace {

bool isNullPattern(const Pattern* p) {
    return p->kind == PatternKind::Literal && p->literal->kind == ExprKind::NullLit;
}

bool matchesNull(const Pattern* p) {
    return isNullPattern(p) ||
           (p->kind == PatternKind::Or &&
            std::any_of(p->sub.begin(), p->sub.end(), [](const Pattern* a) { return matchesNull(a); }));
}

bool isLiteral(const Expr* lit) {
    switch (lit->kind) {
    case ExprKind::IntLit:
    case ExprKind::CharLit:
    case ExprKind::StringLit:
    case ExprKind::BoolLit:
    case ExprKind::NullLit:
        return true;
    case ExprKind::Unary:
        return static_cast<const UnaryExpr*>(lit)->operand->kind == ExprKind::IntLit;
    default:
        return false;
    }
}

} // namespace

bool Checker::matchable(TypeId t) const {
    const ir::TypeInfo& info = types_->info(t);
    switch (info.kind) {
    case ir::TypeKind::Enum:
    case ir::TypeKind::Struct:
    case ir::TypeKind::Tuple:
        return true;
    case ir::TypeKind::Nullable:
        return matchable(info.elem);
    default:
        return types_->isInteger(t) || t == ir::kChar || t == ir::kStr || t == ir::kBool || t == ir::kError;
    }
}

// A pattern that cannot match its subject still binds its names, so their uses do not report again.
void Checker::bindAsErrors(Pattern* p) {
    for (Pattern* s : p->sub) {
        if (s->kind == PatternKind::Binding && orFrames_.empty()) {
            s->local = declareLocal(s->name, ir::kError, patternMutable_, s->span);
        } else if (s->kind == PatternKind::Binding) {
            bindOrName(s, ir::kError);
        } else {
            bindAsErrors(s);
        }
    }
}

void Checker::checkPattern(Pattern* p, TypeId subject) {
    TypeId inner = subject;
    if (types_->kind(subject) == ir::TypeKind::Nullable) {
        inner = types_->info(subject).elem;
    }
    const ir::TypeInfo info = types_->info(inner); // a copy: checking literals may create types
    switch (p->kind) {
    case PatternKind::Wildcard:
        return;
    case PatternKind::Binding:
        if (info.kind == ir::TypeKind::Enum) {
            for (std::uint32_t v = 0; v < info.variants.size(); ++v) {
                if (info.variants[v].name == str(p->name)) {
                    p->kind = PatternKind::Variant;
                    p->variantIndex = v;
                    if (!info.variants[v].payload.empty()) {
                        diags_.error(p->span, std::format("variant '{}' has a payload; match it with '{}(...)'",
                                                          str(p->name), str(p->name)));
                    }
                    return;
                }
            }
        }
        if (!orFrames_.empty()) {
            bindOrName(p, subject);
            return;
        }
        p->local = declareLocal(p->name, subject, patternMutable_, p->span);
        return;
    case PatternKind::Or:
        checkOrPattern(p, subject);
        return;
    case PatternKind::Literal:
    case PatternKind::Range: {
        if (isNullPattern(p)) {
            if (types_->kind(subject) != ir::TypeKind::Nullable && subject != ir::kError) {
                diags_.error(p->span, std::format("'null' cannot match a value of type '{}'", typeName(subject)));
            }
            return;
        }
        Expr* lits[] = {p->literal, p->rangeHi};
        for (Expr* lit : lits) {
            if (lit == nullptr) {
                continue;
            }
            if (!isLiteral(lit) || lit->kind == ExprKind::NullLit) {
                diags_.error(lit->span, "patterns must be literals");
            }
            expectAssignable(checkValue(lit, inner), inner, lit->span);
        }
        if (p->kind == PatternKind::Range && !types_->isInteger(inner) && inner != ir::kChar && inner != ir::kError) {
            diags_.error(p->span, "range patterns need an integer or char subject");
        }
        return;
    }
    case PatternKind::Tuple:
        if (info.kind != ir::TypeKind::Tuple || info.array || info.elems.size() != p->sub.size()) {
            if (subject != ir::kError) {
                diags_.error(p->span,
                             std::format("a tuple pattern of {} cannot match '{}'", p->sub.size(), typeName(subject)));
            }
            bindAsErrors(p);
            return;
        }
        for (std::size_t i = 0; i < p->sub.size(); ++i) {
            checkPattern(p->sub[i], info.elems[i]);
        }
        return;
    case PatternKind::Struct:
    case PatternKind::Variant: {
        if (info.kind == ir::TypeKind::Struct) {
            StructDecl* decl = structDeclOf(inner);
            if (decl == nullptr || p->path.size() != 1 || decl->name != p->name) {
                diags_.error(p->span, std::format("pattern does not belong to '{}'", typeName(inner)));
                bindAsErrors(p);
                return;
            }
            p->kind = PatternKind::Struct;
            if (p->sub.size() != info.fields.size()) {
                diags_.error(p->span, std::format("'{}' has {} field(s), the pattern has {}", typeName(inner),
                                                  info.fields.size(), p->sub.size()));
                bindAsErrors(p);
                return;
            }
            for (std::size_t i = 0; i < p->sub.size(); ++i) {
                checkPattern(p->sub[i], info.fields[i].type);
            }
            return;
        }
        if (info.kind != ir::TypeKind::Enum) {
            if (subject != ir::kError) {
                diags_.error(p->span, std::format("variant pattern on a value of type '{}'", typeName(subject)));
            }
            bindAsErrors(p);
            return;
        }
        std::string enumName = info.genericBase != 0 ? types_->info(info.genericBase).name : info.name;
        if (p->path.size() > 2 || (p->path.size() == 2 && str(p->path[0]) != enumName)) {
            diags_.error(p->span, std::format("pattern does not belong to '{}'", info.name));
            bindAsErrors(p);
            return;
        }
        for (std::uint32_t v = 0; v < info.variants.size(); ++v) {
            if (info.variants[v].name != str(p->name)) {
                continue;
            }
            p->variantIndex = v;
            const std::vector<TypeId>& payload = info.variants[v].payload;
            if (p->sub.size() != payload.size()) {
                diags_.error(p->span, std::format("variant '{}' has {} field(s), the pattern has {}", str(p->name),
                                                  payload.size(), p->sub.size()));
                bindAsErrors(p);
                return;
            }
            for (std::size_t i = 0; i < payload.size(); ++i) {
                checkPattern(p->sub[i], payload[i]);
            }
            return;
        }
        diags_.error(p->span, std::format("'{}' has no variant '{}'", info.name, str(p->name)));
        bindAsErrors(p);
        return;
    }
    }
}

namespace {

void collectLocals(const Pattern* p, std::map<std::uint32_t, Local*>& out) {
    if (p->kind == PatternKind::Binding && p->local != nullptr) {
        out.emplace(p->name.id, p->local);
    }
    std::span<Pattern* const> subs = p->kind == PatternKind::Or ? p->sub.first(std::min<std::size_t>(1, p->sub.size()))
                                                                : std::span<Pattern* const>(p->sub);
    for (const Pattern* s : subs) {
        collectLocals(s, out);
    }
}

} // namespace

void Checker::checkOrPattern(Pattern* p, TypeId subject) {
    if (p->sub.empty()) {
        return;
    }
    checkPattern(p->sub[0], subject);
    std::map<std::uint32_t, Local*> names;
    collectLocals(p->sub[0], names);
    for (std::size_t i = 1; i < p->sub.size(); ++i) {
        orFrames_.push_back({names, {}});
        checkPattern(p->sub[i], subject);
        std::set<std::uint32_t> seen = std::move(orFrames_.back().seen);
        orFrames_.pop_back();
        for (const auto& [id, local] : names) {
            if (!seen.contains(id)) {
                diags_.error(p->sub[i]->span, std::format("this alternative does not bind '{}'; every alternative of "
                                                          "a '|' pattern binds the same names",
                                                          str(local->name)));
            }
        }
    }
}

void Checker::bindOrName(Pattern* p, TypeId subject) {
    OrFrame& frame = orFrames_.back();
    auto it = frame.names.find(p->name.id);
    if (it == frame.names.end()) {
        diags_.error(p->span, std::format("'{}' is not bound by the first alternative; every alternative of a '|' "
                                          "pattern binds the same names",
                                          str(p->name)));
        p->local = declareLocal(p->name, subject, patternMutable_, p->span);
        return;
    }
    if (it->second->type != subject && it->second->type != ir::kError && subject != ir::kError) {
        diags_.error(p->span, std::format("'{}' is a '{}' here but a '{}' in the first alternative", str(p->name),
                                          typeName(subject), typeName(it->second->type)));
    }
    p->local = it->second;
    frame.seen.insert(p->name.id);
}

std::vector<Checker::PatternRow> Checker::expandOr(const std::vector<PatternRow>& rows) {
    bool any = std::any_of(rows.begin(), rows.end(), [](const PatternRow& r) {
        return !r.empty() && r[0] != nullptr && r[0]->kind == PatternKind::Or;
    });
    if (!any) {
        return rows;
    }
    std::vector<PatternRow> out;
    for (const PatternRow& r : rows) {
        if (r.empty() || r[0] == nullptr || r[0]->kind != PatternKind::Or) {
            out.push_back(r);
            continue;
        }
        std::vector<PatternRow> alts;
        for (const Pattern* alt : r[0]->sub) {
            PatternRow row = r;
            row[0] = alt;
            alts.push_back(std::move(row));
        }
        for (PatternRow& row : expandOr(alts)) {
            out.push_back(std::move(row));
        }
    }
    return out;
}

bool Checker::patternBinds(const Pattern* p) const {
    if (p->kind == PatternKind::Binding) {
        return true;
    }
    return std::any_of(p->sub.begin(), p->sub.end(), [&](const Pattern* s) { return patternBinds(s); });
}

// Exhaustiveness: the usefulness algorithm over a pattern matrix (Maranget), returning a value no row matches.
std::optional<std::vector<Checker::Ctor>> Checker::ctorsOf(TypeId t) const {
    const ir::TypeInfo& info = types_->info(t);
    std::vector<Ctor> out;
    switch (info.kind) {
    case ir::TypeKind::Enum:
        for (const ir::VariantInfo& v : info.variants) {
            out.push_back({v.name, v.payload});
        }
        return out;
    case ir::TypeKind::Bool:
        return std::vector<Ctor>{{"false", {}}, {"true", {}}};
    case ir::TypeKind::Tuple:
        return std::vector<Ctor>{{"", info.elems}};
    case ir::TypeKind::Struct: {
        std::vector<TypeId> fields;
        for (const ir::Field& f : info.fields) {
            fields.push_back(f.type);
        }
        return std::vector<Ctor>{{info.genericBase != 0 ? types_->info(info.genericBase).name : info.name, fields}};
    }
    case ir::TypeKind::Nullable:
        return std::vector<Ctor>{{"null", {}}, {"", {info.elem}}};
    default:
        return std::nullopt;
    }
}

// The constructor of a pattern's head in a column of type `t`: -1 for a wildcard, -2 for a literal or a range.
int Checker::headCtor(const Pattern* p, TypeId t, std::vector<const Pattern*>& args) const {
    args.clear();
    if (p->kind == PatternKind::Wildcard || p->kind == PatternKind::Binding) {
        return -1;
    }
    const ir::TypeInfo& info = types_->info(t);
    switch (info.kind) {
    case ir::TypeKind::Nullable:
        if (isNullPattern(p)) {
            return 0;
        }
        args.push_back(p);
        return 1;
    case ir::TypeKind::Enum:
        if (p->kind != PatternKind::Variant || p->variantIndex >= info.variants.size() ||
            p->sub.size() != info.variants[p->variantIndex].payload.size()) {
            return -2; // already reported by checkPattern
        }
        args.assign(p->sub.begin(), p->sub.end());
        return static_cast<int>(p->variantIndex);
    case ir::TypeKind::Bool:
        if (p->kind == PatternKind::Literal && p->literal->kind == ExprKind::BoolLit) {
            return static_cast<BoolLitExpr*>(p->literal)->value ? 1 : 0;
        }
        return -2;
    case ir::TypeKind::Tuple:
    case ir::TypeKind::Struct:
        if ((p->kind != PatternKind::Tuple && p->kind != PatternKind::Struct) ||
            p->sub.size() != (info.kind == ir::TypeKind::Tuple ? info.elems.size() : info.fields.size())) {
            return -2;
        }
        args.assign(p->sub.begin(), p->sub.end());
        return 0;
    default:
        return -2; // a literal or a range: never covers its whole type
    }
}

std::string Checker::ctorText(const Ctor& c, TypeId t, std::span<const std::string> args) const {
    std::string inside;
    for (std::size_t i = 0; i < args.size(); ++i) {
        inside += (i ? ", " : "") + args[i];
    }
    ir::TypeKind k = types_->kind(t);
    if (k == ir::TypeKind::Nullable) {
        return args.empty() ? c.name : args[0];
    }
    if (k == ir::TypeKind::Tuple) {
        return "(" + inside + (args.size() == 1 ? ",)" : ")");
    }
    return args.empty() && k != ir::TypeKind::Struct ? c.name : c.name + "(" + inside + ")";
}

std::optional<std::vector<std::string>> Checker::uncovered(const std::vector<PatternRow>& unexpanded,
                                                           const std::vector<TypeId>& cols) const {
    std::vector<PatternRow> rows = expandOr(unexpanded);
    if (cols.empty()) {
        return rows.empty() ? std::optional(std::vector<std::string>{}) : std::nullopt;
    }
    for (const PatternRow& r : rows) {
        if (std::all_of(r.begin(), r.end(), [](const Pattern* p) {
                return p == nullptr || p->kind == PatternKind::Wildcard || p->kind == PatternKind::Binding;
            })) {
            return std::nullopt;
        }
    }
    TypeId t = cols[0];
    std::vector<TypeId> rest(cols.begin() + 1, cols.end());
    std::optional<std::vector<Ctor>> ctors = ctorsOf(t);
    std::vector<const Pattern*> args;
    std::vector<bool> seen(ctors ? ctors->size() : 0, false);
    for (const PatternRow& r : rows) {
        int k = r[0] == nullptr ? -1 : headCtor(r[0], t, args);
        if (k >= 0 && static_cast<std::size_t>(k) < seen.size()) {
            seen[k] = true;
        }
    }
    bool complete = ctors && !seen.empty() && std::all_of(seen.begin(), seen.end(), [](bool b) { return b; });
    if (!complete) {
        std::vector<PatternRow> def;
        for (const PatternRow& r : rows) {
            if (r[0] == nullptr || headCtor(r[0], t, args) == -1) {
                def.emplace_back(r.begin() + 1, r.end());
            }
        }
        std::optional<std::vector<std::string>> w = uncovered(def, rest);
        if (!w) {
            return std::nullopt;
        }
        std::string head = "_";
        for (std::size_t k = 0; ctors && k < ctors->size(); ++k) {
            if (!seen[k]) {
                std::vector<std::string> wild((*ctors)[k].args.size(), "_");
                head = ctorText((*ctors)[k], t, wild);
                break;
            }
        }
        w->insert(w->begin(), head);
        return w;
    }
    for (std::size_t k = 0; k < ctors->size(); ++k) {
        if (std::optional<std::vector<std::string>> w = specialized(rows, cols, (*ctors)[k], static_cast<int>(k))) {
            return w;
        }
    }
    return std::nullopt;
}

std::optional<std::vector<std::string>> Checker::specialized(const std::vector<PatternRow>& unexpanded,
                                                             const std::vector<TypeId>& cols, const Ctor& c,
                                                             int k) const {
    std::vector<PatternRow> rows = expandOr(unexpanded);
    TypeId t = cols[0];
    std::vector<const Pattern*> args;
    std::vector<PatternRow> spec;
    for (const PatternRow& r : rows) {
        int h = r[0] == nullptr ? -1 : headCtor(r[0], t, args);
        if (h == -1) {
            PatternRow row(c.args.size(), nullptr);
            row.insert(row.end(), r.begin() + 1, r.end());
            spec.push_back(std::move(row));
        } else if (h == k) {
            PatternRow row(args.begin(), args.end());
            row.insert(row.end(), r.begin() + 1, r.end());
            spec.push_back(std::move(row));
        }
    }
    std::vector<TypeId> sub = c.args;
    sub.insert(sub.end(), cols.begin() + 1, cols.end());
    std::optional<std::vector<std::string>> w = uncovered(spec, sub);
    if (!w) {
        return std::nullopt;
    }
    std::vector<std::string> out{ctorText(c, t, std::span(*w).first(c.args.size()))};
    out.insert(out.end(), w->begin() + static_cast<std::ptrdiff_t>(c.args.size()), w->end());
    return out;
}

std::vector<std::string> Checker::missingCases(const std::vector<const Pattern*>& arms, TypeId subject) const {
    std::vector<PatternRow> rows;
    for (const Pattern* p : arms) {
        rows.push_back({p});
    }
    std::optional<std::vector<Ctor>> ctors = ctorsOf(subject);
    std::vector<std::string> out;
    if (!ctors || ctors->size() < 2) {
        if (std::optional<std::vector<std::string>> w = uncovered(rows, {subject})) {
            out.push_back((*w)[0]);
        }
        return out;
    }
    for (std::size_t k = 0; k < ctors->size(); ++k) {
        if (std::optional<std::vector<std::string>> w =
                specialized(rows, {subject}, (*ctors)[k], static_cast<int>(k))) {
            out.push_back((*w)[0]);
        }
    }
    return out;
}

TypeId Checker::checkMatch(MatchExpr* m, TypeId expected, bool wantValue) {
    TypeId subject = checkValue(m->subject, ir::kError);
    if (!matchable(subject)) {
        diags_.error(m->subject->span, std::format("cannot match on '{}'", typeName(subject)));
        subject = ir::kError;
    }
    TypeId result = ir::kNever;
    std::vector<const Pattern*> unguarded;
    bool badPatterns = false;
    TypeId armSubject = subject;
    m->narrowFrom = ~0U;
    bool holes = wantValue && expected == ir::kError;
    if (holes) {
        holes_.emplace_back();
    }
    for (std::size_t a = 0; a < m->arms.size(); ++a) {
        MatchArm& arm = m->arms[a];
        if (holes) {
            markHoleSites(arm.body);
        }
        pushScope();
        std::size_t errors = diags_.errorCount();
        checkPattern(arm.pattern, armSubject);
        if (arm.guard == nullptr && armSubject == subject && types_->kind(subject) == ir::TypeKind::Nullable &&
            matchesNull(arm.pattern)) {
            armSubject = types_->info(subject).elem;
            m->narrowFrom = static_cast<std::uint32_t>(a + 1);
        }
        badPatterns = badPatterns || diags_.errorCount() != errors;
        if (arm.guard != nullptr) {
            expectAssignable(checkValue(arm.guard, ir::kBool), ir::kBool, arm.guard->span);
        } else {
            unguarded.push_back(arm.pattern);
        }
        TypeId t = ir::kVoid;
        if (wantValue) {
            t = checkValue(arm.body, expected);
            result = unify(result, t, arm.body->span);
        } else if (arm.body->kind == ExprKind::Block) {
            t = checkBlock(static_cast<BlockExpr*>(arm.body), ir::kError, false);
            arm.body->type = t;
        } else if (arm.body->kind == ExprKind::If) {
            t = checkIf(static_cast<IfExpr*>(arm.body), ir::kError, false);
            arm.body->type = t;
        } else {
            t = check(arm.body, ir::kError);
        }
        if (!wantValue && t != ir::kNever) {
            result = ir::kVoid;
        }
        popScope();
    }
    if (holes) {
        std::vector<Expr*> bodies;
        for (MatchArm& arm : m->arms) {
            bodies.push_back(arm.body);
        }
        result = fillResultHoles(result, bodies);
    }
    if (subject != ir::kError && !badPatterns) {
        std::vector<std::string> missing = missingCases(unguarded, subject);
        if (missing.size() == 1 && missing[0] == "_") {
            diags_.error(m->span, "match is not exhaustive; add a '_' arm");
        } else if (!missing.empty()) {
            std::string list;
            for (const std::string& s : missing) {
                list += (list.empty() ? "" : ", ") + s;
            }
            diags_.error(m->span, std::format("match is not exhaustive; missing: {}", list));
        }
        warnUnreachableArms(m, subject);
    }
    if (m->arms.empty()) {
        return wantValue ? ir::kError : ir::kVoid;
    }
    return result;
}

TypeId Checker::checkIs(IsExpr* e) {
    TypeId t = checkValue(e->value, ir::kError);
    if (!matchable(t)) {
        diags_.error(e->value->span, std::format("cannot match on '{}'", typeName(t)));
        t = ir::kError;
    }
    if (e->pattern->kind == PatternKind::Binding && types_->kind(t) == ir::TypeKind::Enum) {
        bool variant = false;
        for (const ir::VariantInfo& v : types_->info(t).variants) {
            variant = variant || v.name == str(e->pattern->name);
        }
        if (!variant) {
            diags_.error(e->pattern->span, std::format("'{}' has no variant '{}'", typeName(t), str(e->pattern->name)));
        }
    }
    checkPattern(e->pattern, t);
    if (patternBinds(e->pattern) && !isBindOk_.contains(e)) {
        diags_.error(e->pattern->span, "names bound by 'is' can only be used in an 'if' or 'while' condition, alone "
                                       "or joined with '&&'");
    }
    return ir::kBool;
}

void Checker::allowIsBindings(Expr* cond) {
    if (cond->kind == ExprKind::Is) {
        isBindOk_.insert(cond);
    } else if (cond->kind == ExprKind::Binary && static_cast<BinaryExpr*>(cond)->op == BinaryOp::And) {
        allowIsBindings(static_cast<BinaryExpr*>(cond)->lhs);
        allowIsBindings(static_cast<BinaryExpr*>(cond)->rhs);
    }
}

void Checker::checkLetPattern(LetStmt* s) {
    TypeId declared = s->type != nullptr ? resolveType(s->type) : ir::kError;
    TypeId t = checkValue(s->init, declared);
    if (declared != ir::kError) {
        expectAssignable(t, declared, s->init->span);
        t = declared;
    }
    patternMutable_ = s->mutable_;
    std::size_t errors = diags_.errorCount();
    checkPattern(s->pattern, t);
    patternMutable_ = false;
    if (t == ir::kError || diags_.errorCount() != errors) {
        return;
    }
    std::vector<std::string> missing = missingCases({s->pattern}, t);
    if (!missing.empty()) {
        diags_.error(s->pattern->span, std::format("this pattern can fail (it does not match {}); use 'match' or "
                                                   "'if ... is'",
                                                   missing[0]));
    }
}

} // namespace quartz::sema
