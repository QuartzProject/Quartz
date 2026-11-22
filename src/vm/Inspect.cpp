#include "vm/Inspect.hpp"

#include "vm/Sandbox.hpp"

#include <algorithm>
#include <charconv>
#include <format>

namespace quartz::vm {

using ir::TypeKind;

namespace {

constexpr int kMaxDepth = 4;

ValueRef at(ir::TypeId type, const Slot* slots) { return ValueRef{type, slots, {}, false}; }

void quote(std::string& out, std::string_view s, std::size_t budget) {
    out += '"';
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (out.size() >= budget) {
            out += "...";
            break;
        }
        char c = s[i];
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\t':
            out += "\\t";
            break;
        case '\r':
            out += "\\r";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                out += std::format("\\u{{{:x}}}", static_cast<unsigned>(c));
            } else {
                out += c;
            }
        }
    }
    out += '"';
}

void appendChar(std::string& out, std::uint64_t cp) {
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        out += std::format("\\u{{{:x}}}", cp);
    } else if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

class Printer {
public:
    Printer(const Program& p, std::size_t budget) : p_(p), types_(p.module->types), budget_(budget) {}

    void value(const ValueRef& v, int depth) {
        if (out.size() >= budget_) {
            return;
        }
        const Slot* s = v.slots();
        if (s == nullptr || v.type >= types_.size()) {
            out += "?";
            return;
        }
        const ir::TypeInfo& info = types_.info(v.type);
        switch (info.kind) {
        case TypeKind::Bool:
            out += s->u != 0 ? "true" : "false";
            return;
        case TypeKind::I8:
        case TypeKind::I16:
        case TypeKind::I32:
        case TypeKind::I64:
            out += std::to_string(s->i);
            return;
        case TypeKind::U8:
        case TypeKind::U16:
        case TypeKind::U32:
        case TypeKind::U64:
            out += std::to_string(s->u);
            return;
        case TypeKind::F64: {
            char buf[40];
            out += shortestF64(s->f, buf);
            return;
        }
        case TypeKind::F32: {
            char buf[32];
            auto r = std::to_chars(buf, buf + sizeof buf, s->f32);
            out.append(buf, r.ptr);
            return;
        }
        case TypeKind::Char:
            out += '\'';
            appendChar(out, s->u);
            out += '\'';
            return;
        case TypeKind::Str:
        case TypeKind::Bytes: {
            const auto* str = static_cast<const Str*>(s->p);
            if (str == nullptr) {
                out += "null";
            } else if (info.kind == TypeKind::Str) {
                quote(out, str->view(), budget_);
            } else {
                out += std::format("Bytes({})", str->len);
            }
            return;
        }
        case TypeKind::Handle:
            out += std::format("{}#{}", info.name, s->u);
            return;
        case TypeKind::Fn: {
            const auto* c = static_cast<const Closure*>(s->p);
            out += c != nullptr && c->fn < p_.functions.size() ? "fn " + p_.functions[c->fn].name : "fn";
            return;
        }
        case TypeKind::Nullable: {
            ValueRef inner = v;
            if (!unwrapNullable(p_, inner)) {
                out += "null";
                return;
            }
            value(inner, depth);
            return;
        }
        case TypeKind::Enum: {
            auto tag = static_cast<std::size_t>(s->u);
            if (tag >= info.variants.size()) {
                out += "?";
                return;
            }
            out += info.variants[tag].name;
            compound(v, depth, "(", ")", false);
            return;
        }
        case TypeKind::Struct:
        case TypeKind::State:
            out += info.name;
            out += ' ';
            compound(v, depth, "{ ", " }", true);
            return;
        case TypeKind::Tuple:
            compound(v, depth, info.array ? "[" : "(", info.array ? "]" : ")", false);
            return;
        case TypeKind::List:
        case TypeKind::Map: {
            const bool list = info.kind == TypeKind::List;
            if (s->p == nullptr) {
                out += "null";
                return;
            }
            compound(v, depth, list ? "[" : "{", list ? "]" : "}", !list);
            return;
        }
        default:
            out += "()";
            return;
        }
    }

    std::string out;

private:
    void compound(const ValueRef& v, int depth, std::string_view open, std::string_view close, bool named) {
        std::size_t n = childCount(p_, v);
        if (n == 0) {
            if (types_.kind(v.type) != TypeKind::Enum) {
                out += open.substr(0, 1);
                out += close.substr(close.size() - 1);
            }
            return;
        }
        if (depth >= kMaxDepth) {
            out += std::format("{}{} items{}", open, n, close);
            return;
        }
        out += open;
        std::vector<ValueChild> kids = childrenOf(p_, v, 0, 16);
        for (std::size_t i = 0; i < kids.size(); ++i) {
            if (out.size() >= budget_) {
                out += "...";
                return;
            }
            out += i > 0 ? ", " : "";
            if (named) {
                out += kids[i].name + ": ";
            }
            value(kids[i].value, depth + 1);
        }
        out += n > kids.size() ? ", ..." : "";
        out += close;
    }

    const Program& p_;
    const ir::TypeTable& types_;
    std::size_t budget_;
};

} // namespace

bool unwrapNullable(const Program& p, ValueRef& v) {
    if (v.type >= p.module->types.size() || p.module->types.kind(v.type) != TypeKind::Nullable) {
        return true;
    }
    ir::TypeId inner = p.module->types.info(v.type).elem;
    const Slot* s = v.slots();
    if (p.layouts->nullableIsPointer(v.type)) {
        if (s->p == nullptr) {
            return false;
        }
        v.type = inner;
        return true;
    }
    if (s->u == 0 || v.copied) {
        return false;
    }
    v = at(inner, s + 1);
    return true;
}

std::size_t childCount(const Program& p, const ValueRef& v) {
    const ir::TypeTable& types = p.module->types;
    if (v.type >= types.size() || v.slots() == nullptr) {
        return 0;
    }
    const ir::TypeInfo& info = types.info(v.type);
    const Slot* s = v.slots();
    switch (info.kind) {
    case TypeKind::Struct:
    case TypeKind::State:
        return s->p == nullptr && info.kind == TypeKind::State ? 0 : info.fields.size();
    case TypeKind::Tuple:
        return info.elems.size();
    case TypeKind::Enum:
        return s->u < info.variants.size() ? info.variants[static_cast<std::size_t>(s->u)].payload.size() : 0;
    case TypeKind::List:
        return s->p != nullptr ? static_cast<const List*>(s->p)->len : 0;
    case TypeKind::Map:
        return s->p != nullptr ? static_cast<const Map*>(s->p)->count : 0;
    case TypeKind::Nullable: {
        ValueRef inner = v;
        return unwrapNullable(p, inner) ? childCount(p, inner) : 0;
    }
    default:
        return 0;
    }
}

std::vector<ValueChild> childrenOf(const Program& p, const ValueRef& v, std::size_t start, std::size_t limit) {
    std::vector<ValueChild> out;
    const ir::TypeTable& types = p.module->types;
    const Layouts& lay = *p.layouts;
    std::size_t n = childCount(p, v);
    if (n == 0 || start >= n) {
        return out;
    }
    std::size_t end = start + std::min(limit, n - start);
    const ir::TypeInfo& info = types.info(v.type);
    const Slot* s = v.slots();
    switch (info.kind) {
    case TypeKind::Struct:
    case TypeKind::State: {
        const Slot* base = info.kind == TypeKind::State ? static_cast<const Slot*>(s->p) : s;
        for (std::size_t i = start; i < end; ++i) {
            auto f = static_cast<std::uint32_t>(i);
            out.push_back({info.fields[i].name, at(info.fields[i].type, base + lay.fieldOffset(v.type, f))});
        }
        break;
    }
    case TypeKind::Tuple:
        for (std::size_t i = start; i < end; ++i) {
            out.push_back({info.array ? std::format("[{}]", i) : std::to_string(i),
                           at(info.elems[i], s + lay.fieldOffset(v.type, static_cast<std::uint32_t>(i)))});
        }
        break;
    case TypeKind::Enum: {
        const ir::VariantInfo& var = info.variants[static_cast<std::size_t>(s->u)];
        std::uint32_t off = 1;
        for (std::size_t i = 0; i < end; ++i) {
            if (i >= start) {
                out.push_back({std::to_string(i), at(var.payload[i], s + off)});
            }
            off += lay.slots(var.payload[i]);
        }
        break;
    }
    case TypeKind::List: {
        const auto* l = static_cast<const List*>(s->p);
        for (std::size_t i = start; i < end; ++i) {
            ValueRef e = at(info.elem, l->data + i * l->elemSlots);
            if (l->packed) {
                e.copy.u = l->bytes()[i];
                e.copied = true;
                e.at = nullptr;
            }
            out.push_back({std::format("[{}]", i), e});
        }
        break;
    }
    case TypeKind::Map: {
        const auto* m = static_cast<const Map*>(s->p);
        std::size_t i = 0;
        forEachEntry(m, [&](const Slot* entry) {
            if (i >= start && i < end) {
                std::string key = showValue(p, at(info.key, entry), 60);
                out.push_back({std::move(key), at(info.elem, entry + m->keySlots)});
            }
            ++i;
        });
        break;
    }
    case TypeKind::Nullable: {
        ValueRef inner = v;
        if (unwrapNullable(p, inner)) {
            return childrenOf(p, inner, start, limit);
        }
        break;
    }
    default:
        break;
    }
    return out;
}

std::string showValue(const Program& p, const ValueRef& v, std::size_t budget) {
    Printer pr(p, budget);
    pr.value(v, 0);
    return std::move(pr.out);
}

} // namespace quartz::vm
