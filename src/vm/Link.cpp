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
        auto found = provider.program_->lookupFunction(imp.name);
        if (!found && found.error() == LookupError::NotFound) {
            found = provider.program_->lookupFunction(name);
        }
        std::uint32_t f = found.value_or(~0U);
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

} // namespace quartz::vm
