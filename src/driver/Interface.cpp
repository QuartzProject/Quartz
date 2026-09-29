#include "driver/Docs.hpp"
#include "quartz/Package.hpp"
#include "quartz/Quartz.hpp"
#include "syntax/Token.hpp"

#include <algorithm>
#include <format>
#include <set>

namespace quartz {

namespace {

// Names and texts come from a module that may be hostile, so only what cannot change the declaration is copied.
bool identifier(std::string_view s) {
    static constexpr std::string_view kKeywords[] = {
#define QUARTZ_KW_TEXT(name, text) text,
        QUARTZ_KEYWORDS(QUARTZ_KW_TEXT)
#undef QUARTZ_KW_TEXT
    };
    if (s.empty() || s.size() > 256 || (s[0] >= '0' && s[0] <= '9')) {
        return false;
    }
    for (char c : s) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) {
            return false;
        }
    }
    return std::find(std::begin(kKeywords), std::end(kKeywords), s) == std::end(kKeywords);
}

void docLines(std::string& out, std::string_view doc, std::string_view indent) {
    while (!doc.empty()) {
        std::size_t end = doc.find_first_of("\r\n");
        std::string_view line = doc.substr(0, end);
        out += std::format("{}///{}{}\n", indent, line.empty() ? "" : " ", line);
        doc = end == std::string_view::npos ? std::string_view{} : doc.substr(end + 1);
    }
}

std::string defaultOf(const ir::DeclDocs* docs, std::size_t i) {
    if (docs == nullptr || i >= docs->defaults.size() || !interfaceLiteral(docs->defaults[i])) {
        return {};
    }
    return " = " + docs->defaults[i];
}

const std::string& memberDoc(const ir::DeclDocs* docs, std::size_t i) {
    static const std::string kNone;
    return docs != nullptr && i < docs->members.size() ? docs->members[i] : kNone;
}

const ir::DeclDocs* find(const std::vector<ir::DeclDocs>& all, std::uint32_t index) {
    auto it = std::find_if(all.begin(), all.end(), [&](const ir::DeclDocs& d) { return d.index == index; });
    return it != all.end() ? &*it : nullptr;
}

// Collects the structs and enums an exported signature needs, dependencies first; false for a type that cannot cross.
class InterfaceTypes {
public:
    explicit InterfaceTypes(const ir::Module& m) : module_(m), types_(m.types) {}

    bool visit(ir::TypeId t) {
        if (t <= ir::kBytes) {
            return t != ir::kError && t != ir::kNull && t != ir::kNever;
        }
        const ir::TypeInfo& info = types_.info(t);
        switch (info.kind) {
        case ir::TypeKind::List:
        case ir::TypeKind::Nullable:
            return visit(info.elem);
        case ir::TypeKind::Map:
            if (!visit(info.key) || !visit(info.elem)) {
                return false;
            }
            markKey(info.key);
            return true;
        case ir::TypeKind::Tuple:
            for (ir::TypeId e : info.elems) {
                if (!visit(e)) {
                    return false;
                }
            }
            return true;
        case ir::TypeKind::Struct:
        case ir::TypeKind::Enum: {
            if (info.genericBase != 0 || !info.typeParams.empty()) {
                return false;
            }
            if (std::find(order_.begin(), order_.end(), t) != order_.end() || !visiting_.insert(t).second) {
                return true;
            }
            for (const ir::Field& f : info.fields) {
                if (!visit(f.type)) {
                    return false;
                }
            }
            for (const ir::VariantInfo& v : info.variants) {
                for (ir::TypeId p : v.payload) {
                    if (!visit(p)) {
                        return false;
                    }
                }
            }
            order_.push_back(t);
            return true;
        }
        default:
            return false;
        }
    }

    [[nodiscard]] std::string declarations() const {
        std::string out;
        for (ir::TypeId t : order_) {
            const ir::TypeInfo& info = types_.info(t);
            const ir::DeclDocs* docs = find(module_.typeDocs, t);
            out += "\n";
            docLines(out, docs != nullptr ? docs->doc : "", "");
            out += info.reprC ? "@repr(C)\n" : "";
            out += keys_.contains(t) ? "@derive(Eq, Hash)\n" : "";
            if (info.kind == ir::TypeKind::Struct) {
                out += std::format("struct {} {{\n", info.name);
                for (std::size_t k = 0; k < info.fields.size(); ++k) {
                    const ir::Field& f = info.fields[k];
                    docLines(out, memberDoc(docs, k), "    ");
                    out += std::format("    {}: {}{}\n", f.name, types_.name(f.type), defaultOf(docs, k));
                }
            } else {
                out += std::format("enum {} {{\n", info.name);
                for (std::size_t k = 0; k < info.variants.size(); ++k) {
                    const ir::VariantInfo& v = info.variants[k];
                    docLines(out, memberDoc(docs, k), "    ");
                    out += "    " + v.name;
                    for (std::size_t i = 0; i < v.payload.size(); ++i) {
                        out += (i ? ", " : "(") + types_.name(v.payload[i]);
                    }
                    out += v.payload.empty() ? "\n" : ")\n";
                }
            }
            out += "}\n";
        }
        return out;
    }

private:
    // A struct or enum used as a map key has Hash in the provider, so the client declares it with the same derive.
    void markKey(ir::TypeId t) {
        const ir::TypeInfo& info = types_.info(t);
        if (info.kind == ir::TypeKind::Tuple) {
            for (ir::TypeId e : info.elems) {
                markKey(e);
            }
            return;
        }
        if ((info.kind != ir::TypeKind::Struct && info.kind != ir::TypeKind::Enum) || !keys_.insert(t).second) {
            return;
        }
        for (const ir::Field& f : info.fields) {
            markKey(f.type);
        }
        for (const ir::VariantInfo& v : info.variants) {
            for (ir::TypeId p : v.payload) {
                markKey(p);
            }
        }
    }

    const ir::Module& module_;
    const ir::TypeTable& types_;
    std::vector<ir::TypeId> order_;
    std::set<ir::TypeId> visiting_;
    std::set<ir::TypeId> keys_;
};

} // namespace

std::expected<std::string, std::string> interfaceOf(const vm::Program& program, const Manifest& manifest) {
    const ir::Module& m = *program.module;
    const ir::TypeTable& types = m.types;
    InterfaceTypes needed(m);
    std::string fns;
    for (const std::string& name : manifest.exports) {
        std::uint32_t f = program.findFunction(name);
        if (f == ~0U || !program.functions[f].exported || program.functions[f].isSuspend) {
            return std::unexpected(std::format("export '{}' is not an exported non-suspend function", name));
        }
        const vm::CompiledFunction& fn = program.functions[f];
        const std::vector<std::string>& names = m.functions[f].paramNames;
        const ir::DeclDocs* docs = find(m.functionDocs, f);
        constexpr std::string_view kCross = "only scalars, str, Bytes, lists, maps, tuples and non-generic structs and "
                                            "enums cross packages";
        fns += "\n";
        docLines(fns, docs != nullptr ? docs->doc : "", "");
        fns += std::format("fn {}(", name);
        std::size_t n = 0;
        for (std::size_t i = 0; i < fn.params.size(); ++i) {
            ir::TypeId p = fn.params[i];
            if (types.kind(p) == ir::TypeKind::State) {
                continue; // the provider supplies its own state
            }
            if (!needed.visit(p)) {
                return std::unexpected(std::format("export '{}' takes a '{}': {}", name, types.name(p), kCross));
            }
            bool named = names.size() == fn.params.size() && identifier(names[i]);
            fns += std::format("{}{}: {}{}", n ? ", " : "", named ? names[i] : std::format("a{}", n), types.name(p),
                               defaultOf(docs, i));
            ++n;
        }
        if (fn.result != ir::kVoid && !needed.visit(fn.result)) {
            return std::unexpected(std::format("export '{}' returns a '{}': {}", name, types.name(fn.result), kCross));
        }
        fns += fn.result == ir::kVoid ? ")\n" : std::format(") -> {}\n", types.name(fn.result));
    }
    return std::format("package {}\n", manifest.name) + needed.declarations() + fns;
}

} // namespace quartz
