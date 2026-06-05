#include "std/Builder.hpp"
#include "std/Natives.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <string>

namespace quartz::stdlib {

namespace {

constexpr int kMaxDepth = 256;

class JsonParser {
public:
    JsonParser(Builder& b, std::string_view text) : b_(b), s_(text), counter_(text) {}

    bool parse(vm::Slot* out) {
        skipWs();
        if (!value(out, 0)) {
            return false;
        }
        skipWs();
        if (pos_ != s_.size()) {
            return fail("unexpected trailing characters");
        }
        return true;
    }

    std::string error;

private:
    bool fail(std::string_view what) {
        if (error.empty()) {
            std::size_t line = 1;
            std::size_t col = 1;
            for (std::size_t i = 0; i < pos_ && i < s_.size(); ++i) {
                if (s_[i] == '\n') {
                    ++line;
                    col = 1;
                } else {
                    ++col;
                }
            }
            error = std::format("json {}:{}: {}", line, col, what);
        }
        return false;
    }

    std::uint32_t lineAt(std::size_t pos) { return b_.lines != nullptr ? counter_.at(pos) : 0; }

    void skipWs() {
        while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\n' || s_[pos_] == '\r')) {
            ++pos_;
        }
    }

    bool literal(std::string_view word) {
        if (s_.substr(pos_, word.size()) != word) {
            return fail("invalid literal");
        }
        pos_ += word.size();
        return true;
    }

    static void appendUtf8(std::string& out, std::uint32_t cp) {
        if (cp < 0x80) {
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

    bool hex4(std::uint32_t& out) {
        if (pos_ + 4 > s_.size()) {
            return fail("truncated \\u escape");
        }
        out = 0;
        for (int i = 0; i < 4; ++i) {
            char c = s_[pos_++];
            std::uint32_t d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
                              : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 99;
            if (d > 15) {
                return fail("invalid \\u escape");
            }
            out = out * 16 + d;
        }
        return true;
    }

    bool string(std::string& out) {
        ++pos_;
        while (true) {
            if (pos_ >= s_.size()) {
                return fail("unterminated string");
            }
            char c = s_[pos_++];
            if (c == '"') {
                return true;
            }
            if (static_cast<unsigned char>(c) < 0x20) {
                return fail("control character in string");
            }
            if (c != '\\') {
                out += c;
                continue;
            }
            if (pos_ >= s_.size()) {
                return fail("unterminated escape");
            }
            char e = s_[pos_++];
            switch (e) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                std::uint32_t cp = 0;
                if (!hex4(cp)) {
                    return false;
                }
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    std::uint32_t lo = 0;
                    if (s_.substr(pos_, 2) != "\\u") {
                        return fail("unpaired surrogate");
                    }
                    pos_ += 2;
                    if (!hex4(lo) || lo < 0xDC00 || lo > 0xDFFF) {
                        return fail("invalid surrogate pair");
                    }
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    return fail("unpaired surrogate");
                }
                appendUtf8(out, cp);
                break;
            }
            default: return fail("invalid escape");
            }
        }
    }

    bool number(vm::Slot* out) {
        std::size_t begin = pos_;
        if (s_[pos_] == '-') {
            ++pos_;
        }
        auto digits = [&]() {
            std::size_t d = pos_;
            while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') {
                ++pos_;
            }
            return pos_ - d;
        };
        if (digits() == 0) {
            return fail("invalid number");
        }
        bool isFloat = false;
        if (pos_ < s_.size() && s_[pos_] == '.') {
            ++pos_;
            isFloat = true;
            if (digits() == 0) {
                return fail("invalid number");
            }
        }
        if (pos_ < s_.size() && (s_[pos_] == 'e' || s_[pos_] == 'E')) {
            ++pos_;
            isFloat = true;
            if (pos_ < s_.size() && (s_[pos_] == '+' || s_[pos_] == '-')) {
                ++pos_;
            }
            if (digits() == 0) {
                return fail("invalid number");
            }
        }
        std::string_view text = s_.substr(begin, pos_ - begin);
        if (!isFloat) {
            std::int64_t i = 0;
            auto r = std::from_chars(text.data(), text.data() + text.size(), i);
            if (r.ec == std::errc{}) {
                Builder::setInt(out, i);
                return true;
            }
        }
        double d = 0;
        std::from_chars(text.data(), text.data() + text.size(), d);
        Builder::setFloat(out, d);
        return true;
    }

    bool value(vm::Slot* out, int depth) {
        if (depth > kMaxDepth) {
            return fail("nesting too deep");
        }
        if (pos_ >= s_.size()) {
            return fail("unexpected end of input");
        }
        switch (s_[pos_]) {
        case 'n': Builder::setNull(out); return literal("null");
        case 't': Builder::setBool(out, true); return literal("true");
        case 'f': Builder::setBool(out, false); return literal("false");
        case '"': {
            std::string text;
            return string(text) && b_.setStr(out, text);
        }
        case '[': {
            ++pos_;
            vm::List* l = b_.newList(out);
            if (l == nullptr) {
                return false;
            }
            skipWs();
            if (pos_ < s_.size() && s_[pos_] == ']') {
                ++pos_;
                return true;
            }
            while (true) {
                skipWs();
                vm::Slot item[2];
                std::uint32_t line = lineAt(pos_);
                if (!value(item, depth + 1)) {
                    return false;
                }
                b_.line = line;
                if (!b_.push(l, item)) {
                    return false;
                }
                skipWs();
                if (pos_ < s_.size() && s_[pos_] == ',') {
                    ++pos_;
                    continue;
                }
                if (pos_ < s_.size() && s_[pos_] == ']') {
                    ++pos_;
                    return true;
                }
                return fail("expected ',' or ']'");
            }
        }
        case '{': {
            ++pos_;
            vm::Map* m = b_.newTable(out);
            if (m == nullptr) {
                return false;
            }
            skipWs();
            if (pos_ < s_.size() && s_[pos_] == '}') {
                ++pos_;
                return true;
            }
            while (true) {
                skipWs();
                if (pos_ >= s_.size() || s_[pos_] != '"') {
                    return fail("expected a string key");
                }
                std::uint32_t line = lineAt(pos_);
                std::string key;
                if (!string(key)) {
                    return false;
                }
                skipWs();
                if (pos_ >= s_.size() || s_[pos_] != ':') {
                    return fail("expected ':'");
                }
                ++pos_;
                skipWs();
                vm::Slot item[2];
                if (!value(item, depth + 1)) {
                    return false;
                }
                b_.line = line;
                if (!b_.put(m, key, item)) {
                    return false;
                }
                skipWs();
                if (pos_ < s_.size() && s_[pos_] == ',') {
                    ++pos_;
                    continue;
                }
                if (pos_ < s_.size() && s_[pos_] == '}') {
                    ++pos_;
                    return true;
                }
                return fail("expected ',' or '}'");
            }
        }
        default:
            if (s_[pos_] == '-' || (s_[pos_] >= '0' && s_[pos_] <= '9')) {
                return number(out);
            }
            return fail("unexpected character");
        }
    }

    Builder& b_;
    std::string_view s_;
    std::size_t pos_ = 0;
    LineCounter counter_;
};

void escapeTo(std::string& out, std::string_view s) {
    out += '"';
    for (char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                out += std::format("\\u{:04x}", static_cast<unsigned>(c));
            } else {
                out += c;
            }
        }
    }
    out += '"';
}

// Fuel per value and a depth limit: a list may hold itself, or the same list many times over.
bool writeJson(vm::HostCall& call, std::string& out, const vm::Slot* v, std::int64_t indent, int depth, bool sorted) {
    if (depth > 256) {
        call.error = "toJson: the value nests deeper than 256 (or contains itself)";
        return false;
    }
    if (!call.charge(1)) {
        return false;
    }
    auto newline = [&](int d) {
        if (indent > 0) {
            out += '\n';
            out.append(static_cast<std::size_t>(indent * d), ' ');
        }
    };
    switch (static_cast<Tag>(v[0].u)) {
    case Tag::Null: out += "null"; return true;
    case Tag::Bool: out += v[1].u != 0 ? "true" : "false"; return true;
    case Tag::Int: out += std::to_string(v[1].i); return true;
    case Tag::Float: {
        if (!std::isfinite(v[1].f)) {
            out += "null";
            return true;
        }
        char buf[40];
        auto r = std::to_chars(buf, buf + sizeof buf, v[1].f);
        std::string_view text(buf, static_cast<std::size_t>(r.ptr - buf));
        out += text;
        if (text.find_first_of(".e") == std::string_view::npos) {
            out += ".0";
        }
        return true;
    }
    case Tag::Str: escapeTo(out, static_cast<const vm::Str*>(v[1].p)->view()); return true;
    case Tag::List: {
        const auto* l = static_cast<const vm::List*>(v[1].p);
        out += '[';
        for (std::uint32_t i = 0; i < l->len; ++i) {
            if (i > 0) {
                out += indent > 0 ? "," : ", ";
            }
            newline(depth + 1);
            if (!writeJson(call, out, l->data + static_cast<std::size_t>(i) * l->elemSlots, indent, depth + 1,
                           sorted)) {
                return false;
            }
        }
        if (l->len > 0) {
            newline(depth);
        }
        out += ']';
        return true;
    }
    case Tag::Table: {
        const auto* m = static_cast<const vm::Map*>(v[1].p);
        std::vector<const vm::Slot*> entries;
        vm::forEachEntry(m, [&](const vm::Slot* e) { entries.push_back(e); });
        if (sorted) {
            std::sort(entries.begin(), entries.end(), [](const vm::Slot* a, const vm::Slot* b) {
                return static_cast<const vm::Str*>(a[0].p)->view() < static_cast<const vm::Str*>(b[0].p)->view();
            });
        }
        out += '{';
        for (std::size_t i = 0; i < entries.size(); ++i) {
            if (i > 0) {
                out += indent > 0 ? "," : ", ";
            }
            newline(depth + 1);
            escapeTo(out, static_cast<const vm::Str*>(entries[i][0].p)->view());
            out += ": ";
            if (!writeJson(call, out, entries[i] + 1, indent, depth + 1, sorted)) {
                return false;
            }
        }
        if (!entries.empty()) {
            newline(depth);
        }
        out += '}';
        return true;
    }
    }
    return true;
}

bool parseJson(vm::HostCall& call, void* user) {
    Builder b(call.sandbox, *static_cast<const NativeCtx*>(user)->data);
    std::string_view text = static_cast<const vm::Str*>(call.args[0].p)->view();
    if (!call.charge(static_cast<std::int64_t>(text.size() / 4 + 1))) {
        return false;
    }
    JsonParser p(b, text);
    vm::Slot value[2];
    if (!p.parse(value)) {
        return p.error.empty() ? false : b.err(call.out, p.error);
    }
    return b.ok(call.out, value);
}

bool writeJsonText(vm::HostCall& call, bool sorted) {
    std::string out;
    std::int64_t indent = std::clamp<std::int64_t>(call.args[2].i, 0, 16);
    if (!writeJson(call, out, call.args, indent, 0, sorted)) {
        return false;
    }
    if (!call.charge(static_cast<std::int64_t>(out.size() / 4 + 1))) {
        return false;
    }
    call.result.p = call.string(out);
    return call.result.p != nullptr;
}

bool toJson(vm::HostCall& call, void*) { return writeJsonText(call, true); }

bool toJsonOrdered(vm::HostCall& call, void*) { return writeJsonText(call, false); }

} // namespace

bool jsonToValue(vm::Sandbox& sb, const DataTypes& types, std::string_view text, vm::Slot* out, std::string& error) {
    Builder b(sb, types);
    JsonParser p(b, text);
    if (!p.parse(out)) {
        error = p.error;
        return false;
    }
    return true;
}

bool parseJsonText(Builder& b, std::string_view text, vm::Slot* root, std::string& error) {
    JsonParser p(b, text);
    bool ok = p.parse(root);
    error = p.error;
    return ok;
}

bool parseToml(vm::HostCall& call, void* user);
bool parseYaml(vm::HostCall& call, void* user);

void addBinaryNatives(std::vector<Native>& out);

const std::vector<Native>& natives() {
    static const std::vector<Native> kNatives = [] {
        std::vector<Native> v = {
            {"org.quartzlang.data.parseJson", parseJson},         {"org.quartzlang.data.toJson", toJson},
            {"org.quartzlang.data.toJsonOrdered", toJsonOrdered}, {"org.quartzlang.data.parseToml", parseToml},
            {"org.quartzlang.data.parseYaml", parseYaml},
        };
        addBinaryNatives(v);
        addCryptoNatives(v);
        addEncodingNatives(v);
        addDecimalNatives(v);
        addTextNatives(v);
        addFormatNatives(v);
        addBitsNatives(v);
        addSemverNatives(v);
        addHttpNatives(v);
        addRegexNatives(v);
        addCodecNatives(v);
        return v;
    }();
    return kNatives;
}

bool isNative(std::string_view name) {
    const auto& all = natives();
    return std::any_of(all.begin(), all.end(), [&](const Native& n) { return n.name == name; });
}

} // namespace quartz::stdlib
