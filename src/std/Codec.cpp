#include "std/Builder.hpp"
#include "std/Natives.hpp"

#include <charconv>
#include <cmath>
#include <format>
#include <string>
#include <vector>

namespace quartz::stdlib {

namespace {

constexpr int kMaxDepth = 128;

bool bareKey(std::string_view key) {
    if (key.empty()) {
        return false;
    }
    for (char c : key) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) {
            return false;
        }
    }
    return true;
}

void basicString(std::string& out, std::string_view s) {
    out += '"';
    for (char c : s) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\t':
            out += "\\t";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\r':
            out += "\\r";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) {
                out += std::format("\\u{:04X}", static_cast<unsigned>(static_cast<unsigned char>(c)));
            } else {
                out += c;
            }
        }
    }
    out += '"';
}

void keyTo(std::string& out, std::string_view key) {
    if (bareKey(key)) {
        out += key;
    } else {
        basicString(out, key);
    }
}

std::string_view keyOf(const vm::Slot* entry) { return static_cast<const vm::Str*>(entry[0].p)->view(); }

const vm::Slot* valueOf(const vm::Slot* entry) { return entry + 1; }

Tag tagOf(const vm::Slot* v) { return static_cast<Tag>(v[0].u); }

bool tableList(const vm::Slot* v) {
    if (tagOf(v) != Tag::List) {
        return false;
    }
    const auto* l = static_cast<const vm::List*>(v[1].p);
    for (std::uint32_t i = 0; i < l->len; ++i) {
        if (tagOf(l->data + static_cast<std::size_t>(i) * l->elemSlots) != Tag::Table) {
            return false;
        }
    }
    return l->len > 0;
}

// Plain keys first, then `[tables]` and `[[arrays of tables]]`, each group in insertion order.
class TomlWriter {
public:
    explicit TomlWriter(vm::HostCall& call) : call_(call) {}

    bool document(const vm::Slot* root) {
        if (tagOf(root) != Tag::Table) {
            error = "a TOML document is a table";
            return false;
        }
        return table(static_cast<const vm::Map*>(root[1].p), "", 0);
    }

    std::string out;
    std::string error;

private:
    bool charge() { return call_.charge(1); }

    bool deep(int depth, std::string_view path) {
        if (depth <= kMaxDepth) {
            return false;
        }
        error = std::format("'{}' nests deeper than {} (or contains itself)", path, kMaxDepth);
        return true;
    }

    std::string join(std::string_view path, std::string_view key) {
        std::string p(path);
        if (!p.empty()) {
            p += '.';
        }
        keyTo(p, key);
        return p;
    }

    bool table(const vm::Map* m, const std::string& path, int depth) {
        if (deep(depth, path)) {
            return false;
        }
        std::vector<const vm::Slot*> entries;
        vm::forEachEntry(m, [&](const vm::Slot* e) { entries.push_back(e); });
        for (const vm::Slot* e : entries) {
            const vm::Slot* v = valueOf(e);
            if (tagOf(v) == Tag::Table || tableList(v)) {
                continue;
            }
            keyTo(out, keyOf(e));
            out += " = ";
            if (!inlineValue(v, join(path, keyOf(e)), depth + 1)) {
                return false;
            }
            out += '\n';
        }
        for (const vm::Slot* e : entries) {
            const vm::Slot* v = valueOf(e);
            std::string sub = join(path, keyOf(e));
            if (tagOf(v) == Tag::Table) {
                const auto* inner = static_cast<const vm::Map*>(v[1].p);
                if (!onlySections(inner)) {
                    out += (out.empty() ? "[" : "\n[") + sub + "]\n";
                }
                if (!table(inner, sub, depth + 1)) {
                    return false;
                }
            } else if (tableList(v)) {
                const auto* l = static_cast<const vm::List*>(v[1].p);
                for (std::uint32_t i = 0; i < l->len; ++i) {
                    out += (out.empty() ? "[[" : "\n[[") + sub + "]]\n";
                    const vm::Slot* item = l->data + static_cast<std::size_t>(i) * l->elemSlots;
                    if (!table(static_cast<const vm::Map*>(item[1].p), sub, depth + 1)) {
                        return false;
                    }
                }
            }
        }
        return true;
    }

    // A table whose entries are all tables needs no header of its own: theirs create it.
    static bool onlySections(const vm::Map* m) {
        bool any = false;
        bool plain = false;
        vm::forEachEntry(m, [&](const vm::Slot* e) {
            any = true;
            plain = plain || (tagOf(valueOf(e)) != Tag::Table && !tableList(valueOf(e)));
        });
        return any && !plain;
    }

    bool inlineValue(const vm::Slot* v, const std::string& path, int depth) {
        if (!charge() || deep(depth, path)) {
            return false;
        }
        switch (tagOf(v)) {
        case Tag::Null:
            error = std::format("'{}' is null, and TOML has no null", path);
            return false;
        case Tag::Bool:
            out += v[1].u != 0 ? "true" : "false";
            return true;
        case Tag::Int:
            out += std::to_string(v[1].i);
            return true;
        case Tag::Float:
            floatTo(v[1].f);
            return true;
        case Tag::Str:
            basicString(out, static_cast<const vm::Str*>(v[1].p)->view());
            return true;
        case Tag::List: {
            const auto* l = static_cast<const vm::List*>(v[1].p);
            out += '[';
            for (std::uint32_t i = 0; i < l->len; ++i) {
                out += i > 0 ? ", " : "";
                if (!inlineValue(l->data + static_cast<std::size_t>(i) * l->elemSlots, std::format("{}[{}]", path, i),
                                 depth + 1)) {
                    return false;
                }
            }
            out += ']';
            return true;
        }
        case Tag::Table: {
            std::vector<const vm::Slot*> entries;
            vm::forEachEntry(static_cast<const vm::Map*>(v[1].p), [&](const vm::Slot* e) { entries.push_back(e); });
            out += entries.empty() ? "{" : "{ ";
            for (std::size_t i = 0; i < entries.size(); ++i) {
                out += i > 0 ? ", " : "";
                keyTo(out, keyOf(entries[i]));
                out += " = ";
                if (!inlineValue(valueOf(entries[i]), join(path, keyOf(entries[i])), depth + 1)) {
                    return false;
                }
            }
            out += entries.empty() ? "}" : " }";
            return true;
        }
        }
        return true;
    }

    void floatTo(double f) {
        if (std::isnan(f)) {
            out += "nan";
            return;
        }
        if (std::isinf(f)) {
            out += f < 0 ? "-inf" : "inf";
            return;
        }
        char buf[40];
        auto r = std::to_chars(buf, buf + sizeof buf, f);
        std::string_view text(buf, static_cast<std::size_t>(r.ptr - buf));
        out += text;
        if (text.find_first_of(".e") == std::string_view::npos) {
            out += ".0";
        }
    }

    vm::HostCall& call_;
};

bool toToml(vm::HostCall& call, void*) {
    TomlWriter w(call);
    if (!w.document(call.args)) {
        return w.error.empty() ? false : errStr(call, w.error);
    }
    return okStr(call, w.out);
}

struct Segment {
    std::string key;
    std::int64_t index = -1; // a list index when not negative
};

// Reads back the paths DecodeError builds: `a.b[2]["odd key"]`; false on anything else.
bool parsePath(std::string_view p, std::vector<Segment>& out) {
    std::size_t i = 0;
    while (i < p.size()) {
        if (p[i] == '.' && !out.empty()) {
            ++i;
        }
        if (i < p.size() && p[i] == '[') {
            ++i;
            Segment s;
            if (i < p.size() && p[i] == '"') {
                for (++i; i < p.size() && p[i] != '"'; ++i) {
                    if (p[i] == '\\' && i + 1 < p.size()) {
                        ++i;
                    }
                    s.key += p[i];
                }
                i += 1;
            } else {
                std::size_t begin = i;
                while (i < p.size() && p[i] >= '0' && p[i] <= '9') {
                    ++i;
                }
                if (std::from_chars(p.data() + begin, p.data() + i, s.index).ec != std::errc{}) {
                    return false;
                }
            }
            if (i >= p.size() || p[i] != ']') {
                return false;
            }
            ++i;
            out.push_back(std::move(s));
            continue;
        }
        std::size_t begin = i;
        while (i < p.size() && p[i] != '.' && p[i] != '[') {
            ++i;
        }
        if (i == begin) {
            return false;
        }
        out.push_back({std::string(p.substr(begin, i - begin))});
    }
    return true;
}

// Parses the text again, noting where each value starts, and follows the path as far as it exists.
bool lineOf(vm::HostCall& call, void* user) {
    std::string_view text = strArg(call.args[0]);
    std::int64_t format = call.args[1].i;
    std::string_view path = strArg(call.args[2]);
    call.result.i = 0;
    if (!call.charge(static_cast<std::int64_t>(text.size() / 2 + path.size() / 4 + 1))) {
        return false;
    }
    std::vector<Segment> segments;
    if (!parsePath(path, segments)) {
        return true;
    }
    LineMap lines;
    Builder b(call.sandbox, *static_cast<const NativeCtx*>(user)->data);
    b.lines = &lines;
    vm::Slot root[2];
    std::string error;
    bool parsed = format == 0   ? parseTomlText(b, text, root, error)
                  : format == 1 ? parseJsonText(b, text, root, error)
                                : parseYamlText(b, text, root, error);
    if (!parsed) {
        return error.empty() ? false : true;
    }
    const vm::Slot* cur = root;
    for (const Segment& s : segments) {
        const void* container = cur[1].p;
        const vm::Slot* next = nullptr;
        std::string key = s.key;
        if (tagOf(cur) == Tag::Table && s.index < 0) {
            vm::Slot* found = b.find(static_cast<vm::Map*>(cur[1].p), s.key);
            next = found != nullptr ? found + 1 : nullptr;
        } else if (tagOf(cur) == Tag::List && s.index >= 0) {
            const auto* l = static_cast<const vm::List*>(cur[1].p);
            key = std::format("[{}]", s.index);
            next = s.index < l->len ? l->data + static_cast<std::size_t>(s.index) * l->elemSlots : nullptr;
        }
        if (next == nullptr) {
            break;
        }
        if (auto it = lines.find({container, key}); it != lines.end()) {
            call.result.i = it->second;
        }
        cur = next;
    }
    return true;
}

} // namespace

void addCodecNatives(std::vector<Native>& out) {
    out.push_back({"org.quartzlang.data.toToml", toToml});
    out.push_back({"org.quartzlang.data.lineOf", lineOf});
}

} // namespace quartz::stdlib
