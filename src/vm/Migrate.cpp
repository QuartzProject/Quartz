#include "vm/Sandbox.hpp"

#include "std/Std.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <map>

namespace quartz::vm {

using ir::TypeId;
using ir::TypeKind;

namespace {

// A small JSON tree: state documents are written by exportState, but they come back from disk, so read defensively.
struct JNode {
    enum class Kind : std::uint8_t { Null, Bool, Num, Str, Arr, Obj } kind = Kind::Null;
    bool b = false;
    std::string text; // numbers keep their text so 64-bit integers stay exact
    std::vector<JNode> items;
    std::vector<std::string> keys; // objects: keys[i] names values[i]
    std::vector<JNode> values;

    [[nodiscard]] const JNode* get(std::string_view key) const {
        for (std::size_t i = 0; i < keys.size(); ++i) {
            if (keys[i] == key) {
                return &values[i];
            }
        }
        return nullptr;
    }
};

class JReader {
public:
    explicit JReader(std::string_view s) : s_(s) {}

    bool read(JNode& out) {
        ws();
        if (!value(out, 0)) {
            return false;
        }
        ws();
        return pos_ == s_.size();
    }

private:
    void ws() {
        while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\n' || s_[pos_] == '\r' || s_[pos_] == '\t')) {
            ++pos_;
        }
    }

    bool str(std::string& out) {
        ++pos_;
        while (pos_ < s_.size()) {
            char c = s_[pos_++];
            if (c == '"') {
                return true;
            }
            if (c != '\\') {
                out += c;
                continue;
            }
            if (pos_ >= s_.size()) {
                return false;
            }
            char e = s_[pos_++];
            switch (e) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'u': {
                std::uint32_t cp = 0;
                if (pos_ + 4 > s_.size() || std::from_chars(s_.data() + pos_, s_.data() + pos_ + 4, cp, 16).ptr !=
                                                s_.data() + pos_ + 4) {
                    return false;
                }
                pos_ += 4;
                if (cp >= 0x80) {
                    return false; // exportState only escapes control characters
                }
                out += static_cast<char>(cp);
                break;
            }
            default: out += e; break;
            }
        }
        return false;
    }

    bool value(JNode& out, int depth) {
        if (depth > 512 || pos_ >= s_.size()) {
            return false;
        }
        char c = s_[pos_];
        if (c == '{' || c == '[') {
            bool obj = c == '{';
            out.kind = obj ? JNode::Kind::Obj : JNode::Kind::Arr;
            ++pos_;
            ws();
            if (pos_ < s_.size() && s_[pos_] == (obj ? '}' : ']')) {
                ++pos_;
                return true;
            }
            while (true) {
                ws();
                if (obj) {
                    std::string key;
                    if (pos_ >= s_.size() || s_[pos_] != '"' || !str(key)) {
                        return false;
                    }
                    ws();
                    if (pos_ >= s_.size() || s_[pos_++] != ':') {
                        return false;
                    }
                    ws();
                    out.keys.push_back(std::move(key));
                    out.values.emplace_back();
                    if (!value(out.values.back(), depth + 1)) {
                        return false;
                    }
                } else {
                    out.items.emplace_back();
                    if (!value(out.items.back(), depth + 1)) {
                        return false;
                    }
                }
                ws();
                if (pos_ < s_.size() && s_[pos_] == ',') {
                    ++pos_;
                    continue;
                }
                if (pos_ < s_.size() && s_[pos_] == (obj ? '}' : ']')) {
                    ++pos_;
                    return true;
                }
                return false;
            }
        }
        if (c == '"') {
            out.kind = JNode::Kind::Str;
            return str(out.text);
        }
        if (s_.substr(pos_, 4) == "null") {
            pos_ += 4;
            return true;
        }
        if (s_.substr(pos_, 4) == "true" || s_.substr(pos_, 5) == "false") {
            out.kind = JNode::Kind::Bool;
            out.b = s_[pos_] == 't';
            pos_ += out.b ? 4 : 5;
            return true;
        }
        std::size_t begin = pos_;
        while (pos_ < s_.size() && (std::isdigit(static_cast<unsigned char>(s_[pos_])) || s_[pos_] == '-' ||
                                    s_[pos_] == '+' || s_[pos_] == '.' || s_[pos_] == 'e' || s_[pos_] == 'E')) {
            ++pos_;
        }
        if (pos_ == begin) {
            return false;
        }
        out.kind = JNode::Kind::Num;
        out.text.assign(s_.substr(begin, pos_ - begin));
        return true;
    }

    std::string_view s_;
    std::size_t pos_ = 0;
};

void writeString(std::string& out, std::string_view s) {
    out += '"';
    for (char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (static_cast<unsigned char>(c) < 0x20) {
            out += std::format("\\u{:04x}", static_cast<unsigned>(c));
        } else {
            out += c;
        }
    }
    out += '"';
}

std::string toText(const JNode& n) {
    switch (n.kind) {
    case JNode::Kind::Null: return "null";
    case JNode::Kind::Bool: return n.b ? "true" : "false";
    case JNode::Kind::Num: return n.text;
    case JNode::Kind::Str: {
        std::string out;
        out += '"';
        for (char c : n.text) {
            if (c == '"' || c == '\\') {
                out += '\\';
                out += c;
            } else if (static_cast<unsigned char>(c) < 0x20) {
                out += std::format("\\u{:04x}", static_cast<unsigned>(c));
            } else {
                out += c;
            }
        }
        return out + '"';
    }
    case JNode::Kind::Arr: {
        std::string out = "[";
        for (std::size_t i = 0; i < n.items.size(); ++i) {
            out += (i ? "," : "") + toText(n.items[i]);
        }
        return out + "]";
    }
    case JNode::Kind::Obj: {
        std::string out = "{";
        for (std::size_t i = 0; i < n.keys.size(); ++i) {
            JNode key;
            key.kind = JNode::Kind::Str;
            key.text = n.keys[i];
            out += (i ? "," : "") + toText(key) + ":" + toText(n.values[i]);
        }
        return out + "}";
    }
    }
    return "null";
}

void writeDouble(std::string& out, double d) {
    if (std::isnan(d)) {
        out += "\"nan\"";
    } else if (std::isinf(d)) {
        out += d > 0 ? "\"inf\"" : "\"-inf\"";
    } else {
        char buf[40];
        auto r = std::to_chars(buf, buf + sizeof buf, d);
        out.append(buf, static_cast<std::size_t>(r.ptr - buf));
    }
}

} // namespace

void Sandbox::exportValue(std::string& out, TypeId type, const Slot* v) {
    const ir::TypeTable& types = program_->module->types;
    const Layouts& lay = *program_->layouts;
    const ir::TypeInfo& info = types.info(type);
    switch (info.kind) {
    case TypeKind::Bool: out += v->u != 0 ? "true" : "false"; return;
    case TypeKind::I8:
    case TypeKind::I16:
    case TypeKind::I32:
    case TypeKind::I64: out += std::to_string(v->i); return;
    case TypeKind::U8:
    case TypeKind::U16:
    case TypeKind::U32:
    case TypeKind::U64:
    case TypeKind::Char: out += std::to_string(v->u); return;
    case TypeKind::F64: writeDouble(out, v->f); return;
    case TypeKind::F32: writeDouble(out, static_cast<double>(v->f32)); return;
    case TypeKind::Str: writeString(out, static_cast<const Str*>(v->p)->view()); return;
    case TypeKind::Bytes: {
        static constexpr char kDigits[] = "0123456789abcdef";
        std::string_view b = static_cast<const Str*>(v->p)->view();
        out += '"';
        for (char c : b) {
            out += kDigits[static_cast<unsigned char>(c) >> 4];
            out += kDigits[static_cast<unsigned char>(c) & 15];
        }
        out += '"';
        return;
    }
    case TypeKind::List: {
        const auto* l = static_cast<const List*>(v->p);
        out += '[';
        for (std::uint32_t i = 0; i < l->len; ++i) {
            if (i > 0) {
                out += ',';
            }
            if (l->packed) {
                out += l->bytes()[i] != 0 ? "true" : "false";
            } else {
                exportValue(out, l->elemType, l->data + static_cast<std::size_t>(i) * l->elemSlots);
            }
        }
        out += ']';
        return;
    }
    case TypeKind::Map: {
        const auto* m = static_cast<const Map*>(v->p);
        // Sorted by key: the hash order depends on the per-process hash key (D56).
        std::vector<std::pair<std::string, const Slot*>> entries;
        std::uint32_t es = 1u + m->valueSlots;
        for (std::uint32_t i = 0; i < m->cap; ++i) {
            if (m->hashes[i] < 2) {
                continue;
            }
            const Slot* e = m->entries + static_cast<std::size_t>(i) * es;
            std::string key;
            exportValue(key, m->keyType, e);
            if (key.empty() || key.front() != '"') {
                key = "\"" + key + "\"";
            }
            entries.emplace_back(std::move(key), e);
        }
        std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        out += '{';
        for (std::size_t i = 0; i < entries.size(); ++i) {
            out += i > 0 ? "," : "";
            out += entries[i].first;
            out += ':';
            exportValue(out, m->valueType, entries[i].second + 1);
        }
        out += '}';
        return;
    }
    case TypeKind::Nullable:
        if (lay.nullableIsPointer(type) ? v->p == nullptr : v[0].u == 0) {
            out += "null";
        } else {
            exportValue(out, info.elem, lay.nullableIsPointer(type) ? v : v + 1);
        }
        return;
    case TypeKind::Struct:
    case TypeKind::State:
    case TypeKind::Tuple: {
        bool tuple = info.kind == TypeKind::Tuple;
        out += tuple ? '[' : '{';
        std::size_t n = tuple ? info.elems.size() : info.fields.size();
        const Slot* base = info.kind == TypeKind::State ? static_cast<const Slot*>(v->p) : v;
        for (std::uint32_t i = 0; i < n; ++i) {
            out += i > 0 ? "," : "";
            if (!tuple) {
                writeString(out, info.fields[i].name);
                out += ':';
            }
            exportValue(out, tuple ? info.elems[i] : info.fields[i].type, base + lay.fieldOffset(type, i));
        }
        out += tuple ? ']' : '}';
        return;
    }
    case TypeKind::Enum: {
        auto tag = static_cast<std::size_t>(v[0].u);
        const ir::VariantInfo& var = info.variants[tag < info.variants.size() ? tag : 0];
        out += "{\"variant\":";
        writeString(out, var.name);
        out += ",\"values\":[";
        std::uint32_t off = 1;
        for (std::size_t i = 0; i < var.payload.size(); ++i) {
            out += i > 0 ? "," : "";
            exportValue(out, var.payload[i], v + off);
            off += lay.slots(var.payload[i]);
        }
        out += "]}";
        return;
    }
    default: out += "null"; return;
    }
}

std::expected<std::string, Trap> Sandbox::exportState() {
    if (!started_) {
        return std::unexpected(Trap{ir::TrapCode::HostError, "sandbox not started", {}, 0});
    }
    const ir::Module& m = *program_->module;
    std::string out = "{\"states\":{";
    for (std::size_t i = 0; i < m.states.size(); ++i) {
        const ir::TypeInfo& info = m.types.info(m.states[i]);
        out += i > 0 ? "," : "";
        writeString(out, info.name);
        out += std::format(":{{\"version\":{},\"fields\":", info.version);
        Slot ref{};
        ref.p = states_[i];
        exportValue(out, m.states[i], &ref);
        out += '}';
    }
    out += "}}";
    return out;
}

struct Sandbox::ImportCtx {
    std::string error;
    bool lenient = false; // a migrate function takes care of what does not convert
};

bool Sandbox::importValue(ImportCtx& ctx, const void* nodePtr, TypeId type, Slot* out, const std::string& path) {
    const auto& node = *static_cast<const JNode*>(nodePtr);
    const ir::TypeTable& types = program_->module->types;
    const Layouts& lay = *program_->layouts;
    const ir::TypeInfo& info = types.info(type);
    auto bad = [&](std::string_view why) {
        if (ctx.error.empty()) {
            ctx.error = std::format("{}: {}", path, why);
        }
        return false;
    };
    auto number = [&](bool wantFloat, Slot& s) {
        if (node.kind == JNode::Kind::Str && wantFloat && (node.text == "nan" || node.text == "inf" || node.text == "-inf")) {
            s.f = node.text == "nan" ? std::nan("") : node.text == "inf" ? INFINITY : -INFINITY;
            return true;
        }
        if (node.kind != JNode::Kind::Num) {
            return false;
        }
        const char* b = node.text.data();
        const char* e = b + node.text.size();
        if (wantFloat) {
            return std::from_chars(b, e, s.f).ptr == e;
        }
        if (types.isSigned(type)) {
            return std::from_chars(b, e, s.i).ptr == e;
        }
        return std::from_chars(b, e, s.u).ptr == e;
    };
    switch (info.kind) {
    case TypeKind::Bool:
        if (node.kind != JNode::Kind::Bool) {
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
        bool fits = bits == 64 || (types.isSigned(type) ? (s.i >= -(std::int64_t{1} << (bits - 1)) &&
                                                           s.i < (std::int64_t{1} << (bits - 1)))
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
        if (node.kind != JNode::Kind::Str) {
            return bad("expected a string");
        }
        out->p = newStr(node.text);
        return out->p != nullptr;
    case TypeKind::Bytes: {
        if (node.kind != JNode::Kind::Str || node.text.size() % 2 != 0) {
            return bad("expected hex bytes");
        }
        std::string raw;
        for (std::size_t i = 0; i < node.text.size(); i += 2) {
            unsigned v = 0;
            if (std::from_chars(node.text.data() + i, node.text.data() + i + 2, v, 16).ptr != node.text.data() + i + 2) {
                return bad("expected hex bytes");
            }
            raw += static_cast<char>(v);
        }
        out->p = newStr(raw);
        return out->p != nullptr;
    }
    case TypeKind::List: {
        if (node.kind != JNode::Kind::Arr) {
            return bad("expected a list");
        }
        List* l = newList(info.elem);
        if (l == nullptr) {
            return false;
        }
        std::vector<Slot> tmp(std::max<std::uint32_t>(lay.slots(info.elem), 1));
        for (std::size_t i = 0; i < node.items.size(); ++i) {
            if (!importValue(ctx, &node.items[i], info.elem, tmp.data(), std::format("{}[{}]", path, i)) ||
                !listPush(l, tmp.data())) {
                return false;
            }
        }
        out->p = l;
        return true;
    }
    case TypeKind::Map: {
        if (node.kind != JNode::Kind::Obj) {
            return bad("expected a map");
        }
        Map* m = newMap(type);
        if (m == nullptr) {
            return false;
        }
        std::vector<Slot> tmp(1 + std::max<std::uint32_t>(lay.slots(info.elem), 1));
        for (std::size_t idx = 0; idx < node.keys.size(); ++idx) {
            const std::string& k = node.keys[idx];
            const JNode& v = node.values[idx];
            JNode key;
            if (info.key == ir::kStr) {
                key.kind = JNode::Kind::Str;
                key.text = k;
            } else if (info.key == ir::kBool) {
                key.kind = JNode::Kind::Bool;
                key.b = k == "true";
            } else {
                key.kind = JNode::Kind::Num;
                key.text = k;
            }
            if (!importValue(ctx, &key, info.key, tmp.data(), path + ".<key>") ||
                !importValue(ctx, &v, info.elem, tmp.data() + 1, path + "." + k) || !mapSet(m, tmp.data(), tmp.data() + 1)) {
                return false;
            }
        }
        out->p = m;
        return true;
    }
    case TypeKind::Nullable:
        if (node.kind == JNode::Kind::Null) {
            if (lay.nullableIsPointer(type)) {
                out->p = nullptr;
            } else {
                out[0].u = 0;
            }
            return true;
        }
        if (lay.nullableIsPointer(type)) {
            return importValue(ctx, &node, info.elem, out, path);
        }
        out[0].u = 1;
        return importValue(ctx, &node, info.elem, out + 1, path);
    case TypeKind::Struct:
    case TypeKind::Tuple: {
        bool tuple = info.kind == TypeKind::Tuple;
        if (node.kind != (tuple ? JNode::Kind::Arr : JNode::Kind::Obj)) {
            return bad(tuple ? "expected a tuple" : "expected a struct");
        }
        std::size_t n = tuple ? info.elems.size() : info.fields.size();
        for (std::uint32_t i = 0; i < n; ++i) {
            const JNode* f = tuple ? (i < node.items.size() ? &node.items[i] : nullptr) : node.get(info.fields[i].name);
            if (f == nullptr) {
                return bad(std::format("missing field {}", tuple ? std::to_string(i) : info.fields[i].name));
            }
            TypeId ft = tuple ? info.elems[i] : info.fields[i].type;
            if (!importValue(ctx, f, ft, out + lay.fieldOffset(type, i), path + "." + (tuple ? std::to_string(i) : info.fields[i].name))) {
                return false;
            }
        }
        return true;
    }
    case TypeKind::Enum: {
        const JNode* name = node.get("variant");
        const JNode* values = node.get("values");
        if (name == nullptr || name->kind != JNode::Kind::Str || values == nullptr) {
            return bad("expected an enum value");
        }
        for (std::uint64_t tag = 0; tag < info.variants.size(); ++tag) {
            const ir::VariantInfo& var = info.variants[tag];
            if (var.name != name->text) {
                continue;
            }
            if (values->items.size() != var.payload.size()) {
                return bad("variant payload changed");
            }
            out[0].u = tag;
            std::uint32_t off = 1;
            for (std::size_t i = 0; i < var.payload.size(); ++i) {
                if (!importValue(ctx, &values->items[i], var.payload[i], out + off, path + "." + var.name)) {
                    return false;
                }
                off += lay.slots(var.payload[i]);
            }
            return true;
        }
        return bad(std::format("variant {} no longer exists", name->text));
    }
    default: return bad("this type cannot be restored");
    }
}

std::expected<void, Trap> Sandbox::importState(std::string_view json) {
    auto failWith = [](std::string msg) {
        return std::unexpected(Trap{ir::TrapCode::HostError, std::move(msg), {}, 0});
    };
    if (!started_) {
        return failWith("sandbox not started");
    }
    JNode doc;
    if (!JReader(json).read(doc) || doc.kind != JNode::Kind::Obj || doc.get("states") == nullptr) {
        return failWith("state document is not valid");
    }
    const ir::Module& m = *program_->module;
    resetRegions(&arena_);
    arena_.reset();
    inCall_ = true;
    trapped_ = false;
    for (std::size_t i = 0; i < m.states.size(); ++i) {
        const ir::TypeInfo& info = m.types.info(m.states[i]);
        const JNode* saved = doc.get("states")->get(info.name);
        if (saved == nullptr) {
            continue; // a new state keeps its defaults
        }
        const JNode* versionNode = saved->get("version");
        const JNode* fields = saved->get("fields");
        std::uint64_t oldVersion = 0;
        if (versionNode != nullptr && versionNode->kind == JNode::Kind::Num) {
            std::from_chars(versionNode->text.data(), versionNode->text.data() + versionNode->text.size(), oldVersion);
        }
        if (fields == nullptr || fields->kind != JNode::Kind::Obj) {
            inCall_ = false;
            return failWith(std::format("state {} is malformed", info.name));
        }
        if (oldVersion > info.version) {
            inCall_ = false;
            return failWith(std::format("state {} was saved by version {}, newer than {}", info.name, oldVersion,
                                        info.version));
        }
        std::uint32_t migrate = program_->findFunction("migrate" + info.name);
        ImportCtx ctx;
        ctx.lenient = migrate != ~0U;
        for (std::uint32_t f = 0; f < info.fields.size(); ++f) {
            const ir::Field& field = info.fields[f];
            const JNode* value = fields->get(field.name);
            if (value == nullptr && !field.renamedFrom.empty()) {
                value = fields->get(field.renamedFrom);
            }
            if (value == nullptr) {
                continue; // added field: keeps its default
            }
            std::vector<Slot> tmp(std::max<std::uint32_t>(program_->layouts->slots(field.type), 1));
            ImportCtx fieldCtx;
            bool ok = importValue(fieldCtx, value, field.type, tmp.data(), info.name + "." + field.name);
            if (!ok && trapped_) {
                inCall_ = false;
                return std::unexpected(trap_);
            }
            if (!ok) {
                if (!ctx.lenient) {
                    inCall_ = false;
                    return failWith(std::format("cannot migrate {}; bump @version and add migrate{}(old: Value, "
                                                "data: {})",
                                                fieldCtx.error, info.name, info.name));
                }
                continue;
            }
            if (!storeState(field.type, tmp.data(), states_[i] + program_->layouts->fieldOffset(m.states[i], f))) {
                inCall_ = false;
                return std::unexpected(trap_);
            }
        }
        if (migrate != ~0U) {
            // migrate<State>(old: Value, data: State) sees the saved fields as a std.data value.
            std::string err;
            Slot value[2];
            if (dataTypes_->value == 0 || !stdlib::jsonToValue(*this, *dataTypes_, toText(*fields), value, err)) {
                inCall_ = false;
                return failWith("cannot hand the old state to migrate" + info.name + ": " + err);
            }
            inCall_ = false;
            Slot args[2] = {value[0], value[1]};
            heap_.flush();
            const CompiledFunction& fn = program_->functions[migrate];
            if (fn.params.size() != 2 || fn.params[1] != m.states[i] || program_->layouts->slots(fn.params[0]) != 2) {
                return failWith(std::format("migrate{} must take (old: Value, data: {})", info.name, info.name));
            }
            stack_[0] = args[0];
            stack_[1] = args[1];
            stack_[2].p = states_[i];
            fuel_ = limits_.fuelPerCall;
            inCall_ = true;
            bool ok = run(migrate, stack_.data());
            inCall_ = false;
            heap_.flush();
            if (!ok) {
                return std::unexpected(trap_);
            }
            inCall_ = true;
        }
    }
    inCall_ = false;
    heap_.flush();
    return {};
}

std::expected<std::unique_ptr<Sandbox>, Trap> Sandbox::reload(std::shared_ptr<const Program> next) {
    auto fresh = std::make_unique<Sandbox>(std::move(next), limits_);
    const auto& oldImports = program_->module->imports;
    for (std::size_t i = 0; i < oldImports.size(); ++i) {
        if (bindings_[i].fn != nullptr && !oldImports[i].name.starts_with("std.")) {
            fresh->bind(oldImports[i].name, bindings_[i].fn, bindings_[i].user);
        }
    }
    if (auto s = fresh->start(); !s) {
        return std::unexpected(s.error());
    }
    std::uint32_t unload = program_->findFunction("onUnload");
    if (unload != ~0U && program_->functions[unload].params.empty()) {
        if (auto r = call(unload, {}); !r) {
            return std::unexpected(r.error());
        }
    }
    auto saved = exportState();
    if (!saved) {
        return std::unexpected(saved.error());
    }
    if (auto r = fresh->importState(*saved); !r) {
        return std::unexpected(r.error());
    }
    return fresh;
}

} // namespace quartz::vm
