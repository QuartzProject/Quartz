#include "vm/Sandbox.hpp"

#include <format>
#include <set>
#include <utility>

namespace quartz::vm {

using ir::TypeId;
using ir::TypeKind;

namespace {

// Two programs number their types independently; a type crosses packages when both sides spell it the same way.
class Shapes {
public:
    Shapes(const ir::TypeTable& a, const ir::TypeTable& b) : a_(a), b_(b) {}

    bool same(TypeId x, TypeId y) {
        if (x <= ir::kBytes || y <= ir::kBytes) {
            return x == y;
        }
        if (!seen_.insert({x, y}).second) {
            return true;
        }
        const ir::TypeInfo& p = a_.info(x);
        const ir::TypeInfo& q = b_.info(y);
        if (p.kind != q.kind) {
            return false;
        }
        switch (p.kind) {
        case TypeKind::List:
        case TypeKind::Nullable:
            return same(p.elem, q.elem);
        case TypeKind::Map:
            return same(p.key, q.key) && same(p.elem, q.elem);
        case TypeKind::Struct:
            if (p.name != q.name || p.fields.size() != q.fields.size() || p.reprC != q.reprC) {
                return false;
            }
            for (std::size_t i = 0; i < p.fields.size(); ++i) {
                if (p.fields[i].name != q.fields[i].name || !same(p.fields[i].type, q.fields[i].type)) {
                    return false;
                }
            }
            return true;
        case TypeKind::Tuple:
            if (p.elems.size() != q.elems.size()) {
                return false;
            }
            for (std::size_t i = 0; i < p.elems.size(); ++i) {
                if (!same(p.elems[i], q.elems[i])) {
                    return false;
                }
            }
            return true;
        case TypeKind::Enum:
            if (p.name != q.name || p.variants.size() != q.variants.size()) {
                return false;
            }
            for (std::size_t v = 0; v < p.variants.size(); ++v) {
                const auto& pv = p.variants[v];
                const auto& qv = q.variants[v];
                if (pv.name != qv.name || pv.payload.size() != qv.payload.size()) {
                    return false;
                }
                for (std::size_t k = 0; k < pv.payload.size(); ++k) {
                    if (!same(pv.payload[k], qv.payload[k])) {
                        return false;
                    }
                }
            }
            return true;
        default:
            return false; // states, handles and functions stay inside their sandbox
        }
    }

private:
    const ir::TypeTable& a_;
    const ir::TypeTable& b_;
    std::set<std::pair<TypeId, TypeId>> seen_;
};

} // namespace

std::expected<void, std::string> Sandbox::link(std::string_view package, Sandbox& provider) {
    if (&provider == this) {
        return std::unexpected("a sandbox cannot link to itself");
    }
    const ir::TypeTable& ptypes = provider.program_->module->types;
    std::string prefix = std::string(package) + ".";
    bool any = false;
    const auto& imports = program_->module->imports;
    for (std::size_t i = 0; i < imports.size(); ++i) {
        const ir::HostImport& imp = imports[i];
        if (!imp.name.starts_with(prefix)) {
            continue;
        }
        std::string name = imp.name.substr(prefix.size());
        std::uint32_t f = provider.program_->findFunction(name);
        if (f == ~0U || !provider.program_->functions[f].exported || provider.program_->functions[f].isSuspend) {
            return std::unexpected(std::format("{} does not export '{}'", package, name));
        }
        const CompiledFunction& fn = provider.program_->functions[f];
        std::vector<TypeId> params;
        for (TypeId p : fn.params) {
            if (ptypes.kind(p) != TypeKind::State) {
                params.push_back(p);
            }
        }
        Shapes shapes(program_->module->types, ptypes);
        bool match = params.size() == imp.params.size() && shapes.same(imp.result, fn.result);
        for (std::size_t k = 0; match && k < params.size(); ++k) {
            match = shapes.same(imp.params[k], params[k]);
        }
        if (!match) {
            return std::unexpected(
                std::format("'{}' of {} does not match its interface; rebuild against it", name, package));
        }
        links_.push_back(std::make_unique<LinkTarget>(LinkTarget{&provider, f, &imp, std::move(params), fn.result}));
        bindings_[i] = {&Sandbox::linkedCall, links_.back().get()};
        any = true;
    }
    if (!any) {
        return std::unexpected(std::format("nothing imports from {}", package));
    }
    return {};
}

// Arguments go into the provider's host arena and the result into the caller's invocation, so neither side ever
// holds a pointer into the other's memory.
bool Sandbox::linkedCall(HostCall& call, void* user) {
    const auto* t = static_cast<const LinkTarget*>(user);
    Sandbox& client = call.sandbox;
    Sandbox& provider = *t->provider;
    const Layouts& cl = *client.program_->layouts;
    const Layouts& pl = *provider.program_->layouts;
    std::size_t width = 0;
    for (TypeId p : t->providerParams) {
        width += pl.slots(p);
    }
    std::vector<Slot> args(width);
    std::size_t from = 0;
    std::size_t to = 0;
    for (std::size_t k = 0; k < t->providerParams.size(); ++k) {
        if (!provider.copyAcross(client, t->import->params[k], call.args + from, t->providerParams[k],
                                 args.data() + to)) {
            call.error = "out of memory passing an argument";
            return false;
        }
        from += cl.slots(t->import->params[k]);
        to += pl.slots(t->providerParams[k]);
    }
    auto r = provider.call(t->function, args);
    if (!r) {
        call.error = std::format("in {}: {}", t->import->name, r.error().describe());
        return false;
    }
    if (t->import->result == ir::kVoid) {
        return true;
    }
    Slot* dst = cl.slots(t->import->result) > 1 && call.out != nullptr ? call.out : &call.result;
    if (!client.copyAcross(provider, t->providerResult, provider.stack_.data(), t->import->result, dst)) {
        call.error = "out of memory returning a result";
        return false;
    }
    return true;
}

bool Sandbox::copyAcross(const Sandbox& from, TypeId fromType, const Slot* src, TypeId toType, Slot* dst) {
    const ir::TypeTable& ft = from.program_->module->types;
    const ir::TypeTable& tt = program_->module->types;
    const Layouts& fl = *from.program_->layouts;
    const Layouts& tl = *program_->layouts;
    switch (tt.kind(toType)) {
    case TypeKind::Str:
    case TypeKind::Bytes: {
        const auto* s = static_cast<const Str*>(src->p);
        dst->p = s == nullptr ? nullptr : newStr(s->view());
        return s == nullptr || dst->p != nullptr;
    }
    case TypeKind::List: {
        const auto* l = static_cast<const List*>(src->p);
        if (l == nullptr) {
            dst->p = nullptr;
            return true;
        }
        List* out = hostList(toType);
        if (out == nullptr) {
            return false;
        }
        TypeId fe = ft.info(fromType).elem;
        TypeId te = tt.info(toType).elem;
        std::vector<Slot> element(tl.slots(te));
        for (std::uint32_t i = 0; i < l->len; ++i) {
            Slot packed{};
            const Slot* e = l->data + static_cast<std::size_t>(i) * l->elemSlots;
            if (l->packed) {
                packed.u = l->bytes()[i];
                e = &packed;
            }
            if (!copyAcross(from, fe, e, te, element.data()) || !hostListPush(out, element)) {
                return false;
            }
        }
        dst->p = out;
        return true;
    }
    case TypeKind::Map: {
        const auto* m = static_cast<const Map*>(src->p);
        if (m == nullptr) {
            dst->p = nullptr;
            return true;
        }
        Map* out = hostMap(toType);
        if (out == nullptr) {
            return false;
        }
        const ir::TypeInfo& fi = ft.info(fromType);
        const ir::TypeInfo& ti = tt.info(toType);
        std::vector<Slot> key(tl.slots(ti.key));
        std::vector<Slot> value(tl.slots(ti.elem));
        bool ok = true;
        forEachEntry(m, [&](const Slot* e) {
            ok = ok && copyAcross(from, fi.key, e, ti.key, key.data()) &&
                 copyAcross(from, fi.elem, e + m->keySlots, ti.elem, value.data()) && hostMapSet(out, key, value);
        });
        dst->p = out;
        return ok;
    }
    case TypeKind::Nullable: {
        TypeId fe = ft.info(fromType).elem;
        TypeId te = tt.info(toType).elem;
        if (tl.nullableIsPointer(toType)) {
            if (src->p == nullptr) {
                dst->p = nullptr;
                return true;
            }
            return copyAcross(from, fe, src, te, dst);
        }
        dst[0] = src[0];
        return src[0].u == 0 || copyAcross(from, fe, src + 1, te, dst + 1);
    }
    case TypeKind::Struct:
    case TypeKind::Tuple: {
        const ir::TypeInfo& fi = ft.info(fromType);
        const ir::TypeInfo& ti = tt.info(toType);
        std::size_t count = ti.kind == TypeKind::Struct ? ti.fields.size() : ti.elems.size();
        for (std::uint32_t i = 0; i < count; ++i) {
            TypeId a = ti.kind == TypeKind::Struct ? fi.fields[i].type : fi.elems[i];
            TypeId b = ti.kind == TypeKind::Struct ? ti.fields[i].type : ti.elems[i];
            if (!copyAcross(from, a, src + fl.fieldOffset(fromType, i), b, dst + tl.fieldOffset(toType, i))) {
                return false;
            }
        }
        return true;
    }
    case TypeKind::Enum: {
        const ir::TypeInfo& fi = ft.info(fromType);
        const ir::TypeInfo& ti = tt.info(toType);
        std::size_t width = tl.slots(toType);
        for (std::size_t k = 0; k < width; ++k) {
            dst[k].u = 0;
        }
        dst[0] = src[0];
        auto tag = static_cast<std::size_t>(src[0].u);
        if (tag >= ti.variants.size()) {
            return true;
        }
        std::uint32_t a = 1;
        std::uint32_t b = 1;
        for (std::size_t k = 0; k < ti.variants[tag].payload.size(); ++k) {
            TypeId pa = fi.variants[tag].payload[k];
            TypeId pb = ti.variants[tag].payload[k];
            if (!copyAcross(from, pa, src + a, pb, dst + b)) {
                return false;
            }
            a += fl.slots(pa);
            b += tl.slots(pb);
        }
        return true;
    }
    default:
        std::copy_n(src, tl.slots(toType), dst);
        return true;
    }
}

} // namespace quartz::vm
