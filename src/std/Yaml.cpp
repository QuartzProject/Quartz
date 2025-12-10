#include "std/Builder.hpp"

#include <charconv>
#include <cmath>
#include <format>
#include <limits>
#include <string>

namespace quartz::stdlib {

namespace {

constexpr int kMaxDepth = 128;

struct Line {
    std::size_t indent;
    std::string_view text; // without indentation and trailing comment
    std::size_t number;
};

std::string_view stripComment(std::string_view s) {
    char quote = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (quote != 0) {
            if (c == quote) {
                quote = 0;
            }
            continue;
        }
        if (c == '"' || c == '\'') {
            quote = c;
        } else if (c == '#' && (i == 0 || s[i - 1] == ' ' || s[i - 1] == '\t')) {
            s = s.substr(0, i);
            break;
        }
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.remove_suffix(1);
    }
    return s;
}

class YamlParser {
public:
    YamlParser(Builder& b, std::string_view text) : b_(b), raw_(text) {}

    bool parse(vm::Slot* out) {
        std::size_t start = 0;
        std::size_t number = 0;
        while (start <= raw_.size()) {
            std::size_t end = raw_.find('\n', start);
            if (end == std::string_view::npos) {
                end = raw_.size();
            }
            ++number;
            std::string_view whole = raw_.substr(start, end - start);
            rawLines_.push_back(whole);
            std::size_t indent = 0;
            while (indent < whole.size() && whole[indent] == ' ') {
                ++indent;
            }
            if (indent < whole.size() && whole[indent] == '\t') {
                return fail(number, "tabs cannot indent YAML");
            }
            std::string_view text = stripComment(whole.substr(indent));
            if (text == "---" && lines_.empty()) {
                start = end + 1;
                continue;
            }
            if (text == "..." || (text == "---" && !lines_.empty())) {
                break;
            }
            lines_.push_back({indent, text, number});
            start = end + 1;
        }
        skipEmpty();
        if (cur_ >= lines_.size()) {
            Builder::setNull(out);
            return true;
        }
        if (!block(out, lines_[cur_].indent, 0)) {
            return false;
        }
        skipEmpty();
        if (cur_ < lines_.size()) {
            return fail(lines_[cur_].number, "unexpected indentation");
        }
        return true;
    }

    std::string error;

private:
    bool fail(std::size_t line, std::string_view what) {
        if (error.empty()) {
            error = std::format("yaml line {}: {}", line, what);
        }
        return false;
    }

    void skipEmpty() {
        while (cur_ < lines_.size() && lines_[cur_].text.empty()) {
            ++cur_;
        }
    }

    static bool isSeqItem(std::string_view t) { return t == "-" || t.starts_with("- "); }

    // `key: rest` outside quotes and flow brackets; returns false when the line is not a mapping entry.
    static bool splitKey(std::string_view t, std::string_view& key, std::string_view& rest) {
        char quote = 0;
        int depth = 0;
        for (std::size_t i = 0; i < t.size(); ++i) {
            char c = t[i];
            if (quote != 0) {
                if (c == quote) {
                    quote = 0;
                }
                continue;
            }
            if (c == '"' || c == '\'') {
                quote = c;
            } else if (c == '[' || c == '{') {
                ++depth;
            } else if (c == ']' || c == '}') {
                --depth;
            } else if (c == ':' && depth == 0 && (i + 1 == t.size() || t[i + 1] == ' ')) {
                key = t.substr(0, i);
                rest = i + 1 < t.size() ? t.substr(i + 2) : std::string_view{};
                while (!rest.empty() && rest.front() == ' ') {
                    rest.remove_prefix(1);
                }
                while (!key.empty() && key.back() == ' ') {
                    key.remove_suffix(1);
                }
                return !key.empty();
            }
        }
        return false;
    }

    bool block(vm::Slot* out, std::size_t indent, int depth) {
        if (depth > kMaxDepth) {
            return fail(lines_[cur_].number, "nesting too deep");
        }
        const Line& first = lines_[cur_];
        std::string_view key;
        std::string_view rest;
        if (isSeqItem(first.text)) {
            return sequence(out, indent, depth);
        }
        if (splitKey(first.text, key, rest)) {
            return mapping(out, indent, depth);
        }
        ++cur_;
        return scalarOrFlow(out, first.text, first.number, depth);
    }

    // Value after `key:` or `- `: inline, a block scalar, or a nested block on the following lines.
    bool nested(vm::Slot* out, std::string_view rest, std::size_t parentIndent, std::size_t lineNo, int depth,
                bool allowSameIndentSeq) {
        if (rest == "|" || rest == ">" || rest == "|-" || rest == ">-" || rest == "|+" || rest == ">+") {
            return blockScalar(out, rest, parentIndent);
        }
        if (!rest.empty()) {
            return scalarOrFlow(out, rest, lineNo, depth);
        }
        skipEmpty();
        if (cur_ < lines_.size() && (lines_[cur_].indent > parentIndent ||
                                     (allowSameIndentSeq && lines_[cur_].indent == parentIndent &&
                                      isSeqItem(lines_[cur_].text)))) {
            return block(out, lines_[cur_].indent, depth + 1);
        }
        Builder::setNull(out);
        return true;
    }

    bool mapping(vm::Slot* out, std::size_t indent, int depth) {
        vm::Map* m = b_.newTable(out);
        if (m == nullptr) {
            return false;
        }
        while (true) {
            skipEmpty();
            if (cur_ >= lines_.size() || lines_[cur_].indent < indent) {
                return true;
            }
            const Line& line = lines_[cur_];
            if (line.indent > indent) {
                return fail(line.number, "unexpected indentation");
            }
            std::string_view key;
            std::string_view rest;
            if (isSeqItem(line.text) || !splitKey(line.text, key, rest)) {
                return line.indent == indent && isSeqItem(line.text) ? true : fail(line.number, "expected 'key: value'");
            }
            std::string k;
            if (!keyText(key, k, line.number)) {
                return false;
            }
            if (b_.find(m, k) != nullptr) {
                return fail(line.number, std::format("duplicate key '{}'", k));
            }
            ++cur_;
            vm::Slot v[2];
            if (!nested(v, rest, indent, line.number, depth, true) || !b_.put(m, k, v)) {
                return false;
            }
        }
    }

    bool sequence(vm::Slot* out, std::size_t indent, int depth) {
        vm::List* l = b_.newList(out);
        if (l == nullptr) {
            return false;
        }
        while (true) {
            skipEmpty();
            if (cur_ >= lines_.size() || lines_[cur_].indent != indent || !isSeqItem(lines_[cur_].text)) {
                if (cur_ < lines_.size() && lines_[cur_].indent > indent) {
                    return fail(lines_[cur_].number, "unexpected indentation");
                }
                return true;
            }
            Line line = lines_[cur_];
            std::string_view rest = line.text.size() > 1 ? line.text.substr(2) : std::string_view{};
            while (!rest.empty() && rest.front() == ' ') {
                rest.remove_prefix(1);
            }
            vm::Slot v[2];
            std::string_view key;
            std::string_view after;
            if (!rest.empty() && (isSeqItem(rest) || splitKey(rest, key, after)) && rest.front() != '[' &&
                rest.front() != '{') {
                // `- key: value` starts a mapping (or `- - x` a sequence) whose column is after the dash.
                std::size_t inner = indent + static_cast<std::size_t>(rest.data() - line.text.data());
                lines_[cur_].indent = inner;
                lines_[cur_].text = rest;
                if (!block(v, inner, depth + 1)) {
                    return false;
                }
            } else {
                ++cur_;
                if (!nested(v, rest, indent, line.number, depth, false)) {
                    return false;
                }
            }
            if (!b_.push(l, v)) {
                return false;
            }
        }
    }

    bool blockScalar(vm::Slot* out, std::string_view header, std::size_t parentIndent) {
        bool folded = header[0] == '>';
        char chomp = header.size() > 1 ? header[1] : ' ';
        std::size_t first = cur_;
        std::size_t indent = 0;
        while (cur_ < lines_.size()) {
            const Line& l = lines_[cur_];
            if (!l.text.empty()) {
                if (l.indent <= parentIndent) {
                    break;
                }
                if (indent == 0) {
                    indent = l.indent;
                }
                if (l.indent < indent) {
                    break;
                }
            }
            ++cur_;
        }
        std::string text;
        bool prevBlank = true;
        for (std::size_t i = first; i < cur_; ++i) {
            std::string_view rawLine = rawLines_[lines_[i].number - 1];
            std::string_view content = rawLine.size() > indent ? rawLine.substr(indent) : std::string_view{};
            while (!content.empty() && content.back() == '\r') {
                content.remove_suffix(1);
            }
            bool blank = lines_[i].text.empty() && content.find_first_not_of(' ') == std::string_view::npos;
            if (folded && !text.empty() && !blank && !prevBlank && text.back() == '\n') {
                text.back() = ' ';
            }
            text += blank ? std::string_view{} : content;
            text += '\n';
            prevBlank = blank;
        }
        if (chomp != '+') {
            while (!text.empty() && text.back() == '\n') {
                text.pop_back();
            }
            if (chomp != '-' && cur_ > first) {
                text += '\n';
            }
        }
        return b_.setStr(out, text);
    }

    bool keyText(std::string_view key, std::string& out, std::size_t line) {
        if (!key.empty() && (key.front() == '"' || key.front() == '\'')) {
            std::size_t pos = 0;
            return quoted(key, pos, out, line);
        }
        out.assign(key);
        return true;
    }

    bool quoted(std::string_view s, std::size_t& pos, std::string& out, std::size_t line) {
        char q = s[pos++];
        while (pos < s.size()) {
            char c = s[pos++];
            if (q == '\'' && c == '\'') {
                if (pos < s.size() && s[pos] == '\'') {
                    out += '\'';
                    ++pos;
                    continue;
                }
                return true;
            }
            if (q == '"' && c == '"') {
                return true;
            }
            if (q == '"' && c == '\\' && pos < s.size()) {
                char e = s[pos++];
                switch (e) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case '0': out += '\0'; break;
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                default: return fail(line, "unsupported escape in a double-quoted string");
                }
                continue;
            }
            out += c;
        }
        return fail(line, "unterminated quoted string");
    }

    bool plain(vm::Slot* out, std::string_view t) {
        if (t.empty() || t == "~" || t == "null" || t == "Null" || t == "NULL") {
            Builder::setNull(out);
            return true;
        }
        if (t == "true" || t == "True" || t == "TRUE") {
            Builder::setBool(out, true);
            return true;
        }
        if (t == "false" || t == "False" || t == "FALSE") {
            Builder::setBool(out, false);
            return true;
        }
        std::string_view n = t;
        bool neg = false;
        if (!n.empty() && (n[0] == '-' || n[0] == '+')) {
            neg = n[0] == '-';
            n.remove_prefix(1);
        }
        if (n == ".inf" || n == ".Inf" || n == ".INF") {
            Builder::setFloat(out, neg ? -std::numeric_limits<double>::infinity() : std::numeric_limits<double>::infinity());
            return true;
        }
        if (t == ".nan" || t == ".NaN" || t == ".NAN") {
            Builder::setFloat(out, std::numeric_limits<double>::quiet_NaN());
            return true;
        }
        int base = 10;
        if (n.size() > 2 && n[0] == '0' && (n[1] == 'x' || n[1] == 'o')) {
            base = n[1] == 'x' ? 16 : 8;
            n.remove_prefix(2);
        }
        std::uint64_t u = 0;
        auto r = std::from_chars(n.data(), n.data() + n.size(), u, base);
        if (!n.empty() && r.ec == std::errc{} && r.ptr == n.data() + n.size() &&
            u <= static_cast<std::uint64_t>(INT64_MAX) + (neg ? 1 : 0)) {
            Builder::setInt(out, neg ? static_cast<std::int64_t>(0 - u) : static_cast<std::int64_t>(u));
            return true;
        }
        if (base == 10 && !n.empty() && (std::isdigit(static_cast<unsigned char>(n[0])) || n[0] == '.')) {
            double d = 0;
            auto rf = std::from_chars(n.data(), n.data() + n.size(), d);
            if (rf.ec == std::errc{} && rf.ptr == n.data() + n.size()) {
                Builder::setFloat(out, neg ? -d : d);
                return true;
            }
        }
        if (t.front() == '&' || t.front() == '*' || t.front() == '!') {
            error = "yaml: anchors, aliases and tags are not supported";
            return false;
        }
        return b_.setStr(out, t);
    }

    // Flow collections: `[a, b]`, `{k: v}`, nested; scalars inside follow the same typing as plain values.
    bool flow(std::string_view s, std::size_t& pos, vm::Slot* out, std::size_t line, int depth) {
        if (depth > kMaxDepth) {
            return fail(line, "nesting too deep");
        }
        while (pos < s.size() && s[pos] == ' ') {
            ++pos;
        }
        if (pos >= s.size()) {
            return fail(line, "unterminated flow collection");
        }
        char c = s[pos];
        if (c == '[' || c == '{') {
            bool isMap = c == '{';
            ++pos;
            vm::List* l = isMap ? nullptr : b_.newList(out);
            vm::Map* m = isMap ? b_.newTable(out) : nullptr;
            if (l == nullptr && m == nullptr) {
                return false;
            }
            while (true) {
                while (pos < s.size() && s[pos] == ' ') {
                    ++pos;
                }
                if (pos < s.size() && s[pos] == (isMap ? '}' : ']')) {
                    ++pos;
                    return true;
                }
                if (isMap) {
                    std::size_t colon = pos;
                    std::string key;
                    if (pos < s.size() && (s[pos] == '"' || s[pos] == '\'')) {
                        if (!quoted(s, pos, key, line)) {
                            return false;
                        }
                        colon = pos;
                    } else {
                        while (colon < s.size() && s[colon] != ':' && s[colon] != ',' && s[colon] != '}') {
                            ++colon;
                        }
                        key.assign(s.substr(pos, colon - pos));
                        while (!key.empty() && key.back() == ' ') {
                            key.pop_back();
                        }
                    }
                    pos = colon;
                    if (pos >= s.size() || s[pos] != ':') {
                        return fail(line, "expected ':' in a flow mapping");
                    }
                    ++pos;
                    vm::Slot v[2];
                    if (!flow(s, pos, v, line, depth + 1) || !b_.put(m, key, v)) {
                        return false;
                    }
                } else {
                    vm::Slot v[2];
                    if (!flow(s, pos, v, line, depth + 1) || !b_.push(l, v)) {
                        return false;
                    }
                }
                while (pos < s.size() && s[pos] == ' ') {
                    ++pos;
                }
                if (pos < s.size() && s[pos] == ',') {
                    ++pos;
                    continue;
                }
                if (pos < s.size() && s[pos] == (isMap ? '}' : ']')) {
                    ++pos;
                    return true;
                }
                return fail(line, "expected ',' in a flow collection");
            }
        }
        if (c == '"' || c == '\'') {
            std::string text;
            return quoted(s, pos, text, line) && b_.setStr(out, text);
        }
        std::size_t end = pos;
        while (end < s.size() && s[end] != ',' && s[end] != ']' && s[end] != '}') {
            ++end;
        }
        std::string_view t = s.substr(pos, end - pos);
        while (!t.empty() && t.back() == ' ') {
            t.remove_suffix(1);
        }
        pos = end;
        return plain(out, t);
    }

    bool scalarOrFlow(vm::Slot* out, std::string_view t, std::size_t line, int depth) {
        if (!t.empty() && (t.front() == '[' || t.front() == '{' || t.front() == '"' || t.front() == '\'')) {
            std::size_t pos = 0;
            if (!flow(t, pos, out, line, depth)) {
                return false;
            }
            while (pos < t.size() && t[pos] == ' ') {
                ++pos;
            }
            return pos == t.size() || fail(line, "unexpected text after a value");
        }
        return plain(out, t);
    }

    Builder& b_;
    std::string_view raw_;
    std::vector<std::string_view> rawLines_;
    std::vector<Line> lines_;
    std::size_t cur_ = 0;
};

} // namespace

bool parseYaml(vm::HostCall& call, void* user) {
    Builder b(call.sandbox, *static_cast<const DataTypes*>(user));
    std::string_view text = static_cast<const vm::Str*>(call.args[0].p)->view();
    if (!call.charge(static_cast<std::int64_t>(text.size() / 4 + 1))) {
        return false;
    }
    YamlParser p(b, text);
    vm::Slot root[2];
    if (!p.parse(root)) {
        return p.error.empty() ? false : b.err(call.out, p.error);
    }
    return b.ok(call.out, root);
}

} // namespace quartz::stdlib
