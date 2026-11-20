#include "support/Chars.hpp"
#include "support/Utf8.hpp"
#include "vm/Sandbox.hpp"
#include "vm/StateJson.hpp"

#include <charconv>
#include <cmath>
#include <format>

namespace quartz::vm {

using ir::TypeId;
using ir::TypeKind;
using json::Node;

namespace {

enum class Out : std::uint8_t { List, MapObj, MapPairs, Pair, Fields, Tuple, Enum };

struct OutFrame {
    const void* p;
    const Slot* base;
    TypeId type;
    std::uint32_t i;
    std::uint32_t n;
    std::uint32_t off;
    Out kind;
};

constexpr std::string_view kClose[] = {"]", "}", "]", "]", "}", "]", "]}"};

} // namespace

// Iterative: containers wait on an explicit stack, so a value of any depth exports with a bounded native stack.
void Sandbox::exportValue(std::string& out, TypeId type, const Slot* v) {
    const ir::TypeTable& types = program_->module->types;
    const Layouts& lay = *program_->layouts;
    std::vector<OutFrame> st;
    auto open = [&](TypeId t, const Slot* at) {
        while (true) {
            const ir::TypeInfo& info = types.info(t);
            switch (info.kind) {
            case TypeKind::Bool:
                out += at->u != 0 ? "true" : "false";
                return;
            case TypeKind::I8:
            case TypeKind::I16:
            case TypeKind::I32:
            case TypeKind::I64:
                out += std::to_string(at->i);
                return;
            case TypeKind::U8:
            case TypeKind::U16:
            case TypeKind::U32:
            case TypeKind::U64:
            case TypeKind::Char:
                out += std::to_string(at->u);
                return;
            case TypeKind::F64:
                json::writeDouble(out, at->f);
                return;
            case TypeKind::F32:
                json::writeDouble(out, static_cast<double>(at->f32));
                return;
            case TypeKind::Str:
                json::writeString(out, static_cast<const Str*>(at->p)->view());
                return;
            case TypeKind::Bytes: {
                static constexpr char kDigits[] = "0123456789abcdef";
                out += '"';
                for (char c : static_cast<const Str*>(at->p)->view()) {
                    out += kDigits[static_cast<unsigned char>(c) >> 4];
                    out += kDigits[static_cast<unsigned char>(c) & 15];
                }
                out += '"';
                return;
            }
            case TypeKind::Nullable:
                if (lay.nullableIsPointer(t) ? at->p == nullptr : at[0].u == 0) {
                    out += "null";
                    return;
                }
                at = lay.nullableIsPointer(t) ? at : at + 1;
                t = info.elem;
                continue;
            case TypeKind::List: {
                const auto* l = static_cast<const List*>(at->p);
                out += '[';
                if (l->packed) {
                    for (std::uint32_t i = 0; i < l->len; ++i) {
                        out += i > 0 ? "," : "";
                        out += l->bytes()[i] != 0 ? "true" : "false";
                    }
                    out += ']';
                    return;
                }
                st.push_back({l, l->data, l->elemType, 0, l->len, 0, Out::List});
                return;
            }
            case TypeKind::Map: {
                // Insertion order, which is the iteration order and the same in every process; struct keys go as pairs.
                const auto* m = static_cast<const Map*>(at->p);
                bool pairs = m->keyKind == kMapKeyFields;
                out += pairs ? '[' : '{';
                st.push_back({m, nullptr, t, 0, m->ordered, 0, pairs ? Out::MapPairs : Out::MapObj});
                return;
            }
            case TypeKind::Struct:
            case TypeKind::State:
            case TypeKind::Tuple: {
                bool tuple = info.kind == TypeKind::Tuple;
                out += tuple ? '[' : '{';
                const Slot* base = info.kind == TypeKind::State ? static_cast<const Slot*>(at->p) : at;
                auto n = static_cast<std::uint32_t>(tuple ? info.elems.size() : info.fields.size());
                st.push_back({nullptr, base, t, 0, n, 0, tuple ? Out::Tuple : Out::Fields});
                return;
            }
            case TypeKind::Enum: {
                auto tag = static_cast<std::size_t>(at[0].u);
                const ir::VariantInfo& var = info.variants[tag < info.variants.size() ? tag : 0];
                out += "{\"variant\":";
                json::writeString(out, var.name);
                out += ",\"values\":[";
                st.push_back({&var, at, t, 0, static_cast<std::uint32_t>(var.payload.size()), 1, Out::Enum});
                return;
            }
            default:
                out += "null";
                return;
            }
        }
    };
    open(type, v);
    while (!st.empty()) {
        OutFrame& f = st.back();
        if (f.kind == Out::MapObj || f.kind == Out::MapPairs) {
            const auto* m = static_cast<const Map*>(f.p);
            while (f.i < f.n && m->order[f.i] == 0) {
                ++f.i;
            }
        }
        if (f.i >= f.n) {
            out += kClose[static_cast<int>(f.kind)];
            st.pop_back();
            continue;
        }
        std::uint32_t i = f.i++;
        switch (f.kind) {
        case Out::List: {
            const auto* l = static_cast<const List*>(f.p);
            out += i > 0 ? "," : "";
            open(l->elemType, l->data + static_cast<std::size_t>(i) * l->elemSlots);
            break;
        }
        case Out::MapObj:
        case Out::MapPairs: {
            const auto* m = static_cast<const Map*>(f.p);
            const Slot* e = m->entries + static_cast<std::size_t>(m->order[i] - 1) * (m->keySlots + m->valueSlots);
            out += f.off++ > 0 ? "," : "";
            if (f.kind == Out::MapPairs) {
                out += '[';
                st.push_back({m, e, 0, 0, 2, 0, Out::Pair});
                break;
            }
            std::string key;
            exportValue(key, m->keyType, e);
            if (key.empty() || key.front() != '"') {
                key = "\"" + key + "\"";
            }
            out += key;
            out += ':';
            open(m->valueType, e + m->keySlots);
            break;
        }
        case Out::Pair: {
            const auto* m = static_cast<const Map*>(f.p);
            out += i > 0 ? "," : "";
            open(i == 0 ? m->keyType : m->valueType, i == 0 ? f.base : f.base + m->keySlots);
            break;
        }
        case Out::Fields:
        case Out::Tuple: {
            const ir::TypeInfo& info = types.info(f.type);
            out += i > 0 ? "," : "";
            if (f.kind == Out::Fields) {
                json::writeString(out, info.fields[i].name);
                out += ':';
            }
            TypeId ft = f.kind == Out::Tuple ? info.elems[i] : info.fields[i].type;
            open(ft, f.base + lay.fieldOffset(f.type, i));
            break;
        }
        case Out::Enum: {
            const auto& var = *static_cast<const ir::VariantInfo*>(f.p);
            TypeId t = var.payload[i];
            const Slot* at = f.base + f.off;
            f.off += lay.slots(t);
            out += i > 0 ? "," : "";
            // A boxed recursive payload (D177) is written as the value it holds, not as a list of one.
            const auto* box = var.isBoxed(i) ? static_cast<const List*>(at->p) : nullptr;
            if (box != nullptr && box->len == 1 && !box->packed) {
                open(box->elemType, box->data);
            } else {
                open(t, at);
            }
            break;
        }
        }
    }
}

namespace {

std::string crumbText(const std::vector<Sandbox::ImportCrumb>& crumbs) {
    std::string out;
    for (const auto& c : crumbs) {
        out += c.name != nullptr ? "." + *c.name : std::format(".{}", c.index);
    }
    return out;
}

} // namespace

std::string Sandbox::importPath(const ImportCtx& ctx) const {
    std::string out = ctx.base;
    for (const ImportFrame& f : ctx.frames) {
        out += f.where;
        if (f.kind == ImportFrame::Kind::Map && f.node->kind == Node::Kind::Obj && f.i > 0) {
            out += "." + ctx.doc->key(*f.node, f.i - 1);
        } else if (f.kind != ImportFrame::Kind::Box && f.i > 0) {
            out += std::format("[{}]", f.i - 1);
        }
    }
    return out + crumbText(ctx.crumbs);
}

bool Sandbox::importBad(ImportCtx& ctx, std::string_view why) {
    if (ctx.error.empty()) {
        ctx.error = std::format("{}: {}", importPath(ctx), why);
    }
    return false;
}

bool Sandbox::importValue(ImportCtx& ctx, const void* nodePtr, TypeId type, Slot* out, const std::string& path) {
    ctx.base = path;
    ctx.frames.clear();
    ctx.crumbs.clear();
    return importNode(ctx, *static_cast<const Node*>(nodePtr), type, out) && importDrain(ctx, 0, 0);
}

// Fills the frames above `base`; a map key is filled completely before it is hashed, so it drains on its own.
bool Sandbox::importDrain(ImportCtx& ctx, std::size_t base, std::uint32_t keys) {
    if (keys > ir::kMaxTypeNesting) {
        return importBad(ctx, "map keys nested too deeply");
    }
    const json::Doc& doc = *ctx.doc;
    const Layouts& lay = *program_->layouts;
    while (ctx.frames.size() > base) {
        ImportFrame& f = ctx.frames.back();
        if (f.i >= f.n) {
            ctx.frames.pop_back();
            continue;
        }
        std::uint32_t i = f.i++;
        const Node* node = f.node;
        TypeId t = f.type;
        Slot* dst = f.dst;
        ctx.crumbs.clear();
        switch (f.kind) {
        case ImportFrame::Kind::List:
            if (!importNode(ctx, doc.child(*node, i), t, dst + static_cast<std::size_t>(i) * lay.slots(t))) {
                return false;
            }
            break;
        case ImportFrame::Kind::Box:
            if (!importNode(ctx, *node, t, dst)) {
                return false;
            }
            break;
        case ImportFrame::Kind::Map: {
            auto* m = static_cast<Map*>(f.map);
            const ir::TypeInfo& info = program_->module->types.info(t);
            std::uint32_t ks = lay.slots(info.key);
            std::vector<Slot> tmp(ks + std::max<std::uint32_t>(lay.slots(info.elem), 1));
            Node key;
            const Node* kn = &key;
            const Node* vn = nullptr;
            if (node->kind == Node::Kind::Arr) {
                const Node& pair = doc.child(*node, i);
                if (pair.kind != Node::Kind::Arr || pair.count != 2) {
                    return importBad(ctx, "expected [key, value] pairs");
                }
                kn = &doc.child(pair, 0);
                vn = &doc.child(pair, 1);
            } else {
                key.kind = info.key == ir::kStr    ? Node::Kind::Str
                           : info.key == ir::kBool ? Node::Kind::Bool
                                                   : Node::Kind::Num;
                key.text = doc.key(*node, i);
                key.b = key.text == "true";
                vn = &doc.child(*node, i);
            }
            std::size_t mark = ctx.frames.size();
            if (!importNode(ctx, *kn, info.key, tmp.data()) || !importDrain(ctx, mark, keys + 1) ||
                !importNode(ctx, *vn, info.elem, tmp.data() + ks) || !mapSet(m, tmp.data(), tmp.data() + ks)) {
                return false;
            }
            break;
        }
        }
    }
    return true;
}

// The inline part of a value (bounded by kMaxTypeNesting); lists, maps and boxes go to the frame stack.
bool Sandbox::importNode(ImportCtx& ctx, const Node& node, TypeId type, Slot* out) {
    const ir::TypeTable& types = program_->module->types;
    const Layouts& lay = *program_->layouts;
    const ir::TypeInfo& info = types.info(type);
    auto bad = [&](std::string_view why) { return importBad(ctx, why); };
    auto number = [&](bool wantFloat, Slot& s) {
        if (node.kind == Node::Kind::Str && wantFloat &&
            (node.text == "nan" || node.text == "inf" || node.text == "-inf")) {
            s.f = node.text == "nan" ? std::nan("") : node.text == "inf" ? INFINITY : -INFINITY;
            return true;
        }
        if (node.kind != Node::Kind::Num) {
            return false;
        }
        const char* b = node.text.data();
        const char* e = b + node.text.size();
        if (wantFloat) {
            return support::parseDouble(b, e, s.f).ptr == e;
        }
        if (types.isSigned(type)) {
            return std::from_chars(b, e, s.i).ptr == e;
        }
        return std::from_chars(b, e, s.u).ptr == e;
    };
    auto frame = [&](ImportFrame f) {
        f.where = crumbText(ctx.crumbs);
        ctx.frames.push_back(std::move(f));
        return true;
    };
    switch (info.kind) {
    case TypeKind::Bool:
        if (node.kind != Node::Kind::Bool) {
            return bad("expected a bool");
        }
        out->u = node.b ? 1 : 0;
        return true;
    case TypeKind::I8:
    case TypeKind::I16:
    case TypeKind::I32:
    case TypeKind::I64:
    case TypeKind::U8:
    case TypeKind::U16:
    case TypeKind::U32:
    case TypeKind::U64:
    case TypeKind::Char: {
        Slot s{};
        if (!number(false, s)) {
            return bad("expected an integer");
        }
        int bits = types.bitWidth(type);
        bool fits =
            bits == 64 ||
            (types.isSigned(type) ? (s.i >= -(std::int64_t{1} << (bits - 1)) && s.i < (std::int64_t{1} << (bits - 1)))
                                  : s.u < (std::uint64_t{1} << bits));
        if (!fits) {
            return bad("integer does not fit the new type");
        }
        *out = s;
        return true;
    }
    case TypeKind::F64:
    case TypeKind::F32: {
        Slot s{};
        if (!number(true, s)) {
            return bad("expected a number");
        }
        if (info.kind == TypeKind::F32) {
            out->u = 0;
            out->f32 = static_cast<float>(s.f);
        } else {
            *out = s;
        }
        return true;
    }
    case TypeKind::Str:
        if (node.kind != Node::Kind::Str) {
            return bad("expected a string");
        }
        if (!validUtf8(node.text)) {
            return bad("a string is not valid UTF-8");
        }
        out->p = copyStr(node.text);
        return out->p != nullptr;
    case TypeKind::Bytes: {
        if (node.kind != Node::Kind::Str || node.text.size() % 2 != 0) {
            return bad("expected hex bytes");
        }
        std::string raw;
        for (std::size_t i = 0; i < node.text.size(); i += 2) {
            unsigned v = 0;
            if (std::from_chars(node.text.data() + i, node.text.data() + i + 2, v, 16).ptr !=
                node.text.data() + i + 2) {
                return bad("expected hex bytes");
            }
            raw += static_cast<char>(v);
        }
        out->p = copyStr(raw);
        return out->p != nullptr;
    }
    case TypeKind::List: {
        if (node.kind != Node::Kind::Arr) {
            return bad("expected a list");
        }
        List* l = newList(info.elem);
        if (l == nullptr || (node.count > 0 && !growList(l, node.count, true))) {
            return false;
        }
        l->len = node.count;
        out->p = l;
        if (!l->packed) {
            return node.count == 0 ||
                   frame({&node, l->data, nullptr, info.elem, 0, node.count, ImportFrame::Kind::List});
        }
        for (std::uint32_t i = 0; i < node.count; ++i) {
            const Node& item = ctx.doc->child(node, i);
            if (item.kind != Node::Kind::Bool) {
                return bad("expected a bool");
            }
            l->bytes()[i] = item.b ? 1 : 0;
        }
        return true;
    }
    case TypeKind::Map: {
        TypeKind kk = types.kind(info.key);
        bool pairs = kk == TypeKind::Struct || kk == TypeKind::Tuple || kk == TypeKind::Enum || kk == TypeKind::List;
        Map* m = node.kind == (pairs ? Node::Kind::Arr : Node::Kind::Obj) ? newMap(type) : nullptr;
        if (m == nullptr) {
            return trapped_ ? false : bad(pairs ? "expected [key, value] pairs" : "expected a map");
        }
        out->p = m;
        return node.count == 0 || frame({&node, nullptr, m, type, 0, node.count, ImportFrame::Kind::Map});
    }
    case TypeKind::Nullable:
        if (node.kind == Node::Kind::Null) {
            if (lay.nullableIsPointer(type)) {
                out->p = nullptr;
            } else {
                out[0].u = 0;
            }
            return true;
        }
        if (lay.nullableIsPointer(type)) {
            return importNode(ctx, node, info.elem, out);
        }
        out[0].u = 1;
        return importNode(ctx, node, info.elem, out + 1);
    case TypeKind::Struct:
    case TypeKind::Tuple: {
        bool tuple = info.kind == TypeKind::Tuple;
        if (node.kind != (tuple ? Node::Kind::Arr : Node::Kind::Obj)) {
            return bad(tuple ? "expected a tuple" : "expected a struct");
        }
        std::size_t n = tuple ? info.elems.size() : info.fields.size();
        for (std::uint32_t i = 0; i < n; ++i) {
            const Node* f =
                tuple ? (i < node.count ? &ctx.doc->child(node, i) : nullptr) : ctx.doc->get(node, info.fields[i].name);
            if (f == nullptr) {
                return bad(std::format("missing field {}", tuple ? std::to_string(i) : info.fields[i].name));
            }
            ctx.crumbs.push_back({tuple ? nullptr : &info.fields[i].name, i});
            if (!importNode(ctx, *f, tuple ? info.elems[i] : info.fields[i].type, out + lay.fieldOffset(type, i))) {
                return false;
            }
            ctx.crumbs.pop_back();
        }
        return true;
    }
    case TypeKind::Enum: {
        const Node* name = ctx.doc->get(node, "variant");
        const Node* values = ctx.doc->get(node, "values");
        if (name == nullptr || name->kind != Node::Kind::Str || values == nullptr || values->kind != Node::Kind::Arr) {
            return bad("expected an enum value");
        }
        for (std::uint64_t tag = 0; tag < info.variants.size(); ++tag) {
            const ir::VariantInfo& var = info.variants[static_cast<std::size_t>(tag)];
            if (var.name != name->text) {
                continue;
            }
            if (values->count != var.payload.size()) {
                return bad("variant payload changed");
            }
            out[0].u = tag;
            std::uint32_t off = 1;
            ctx.crumbs.push_back({&var.name, 0});
            for (std::uint32_t i = 0; i < var.payload.size(); ++i) {
                const Node& item = ctx.doc->child(*values, i);
                TypeId p = var.payload[i];
                if (var.isBoxed(i) && ctx.boxesAsValues) {
                    TypeId elem = types.info(p).elem;
                    List* box = newList(elem);
                    if (box == nullptr || !growList(box, 1, true)) {
                        return false;
                    }
                    box->len = 1;
                    out[off].p = box;
                    frame({&item, box->data, nullptr, elem, 0, 1, ImportFrame::Kind::Box});
                } else if (!importNode(ctx, item, p, out + off)) {
                    return false;
                }
                off += lay.slots(p);
            }
            ctx.crumbs.pop_back();
            return true;
        }
        return bad(std::format("variant {} no longer exists", name->text));
    }
    default:
        return bad("this type cannot be restored");
    }
}

} // namespace quartz::vm
