#include "std/Builder.hpp"

#include <charconv>
#include <cmath>
#include <format>
#include <limits>
#include <set>
#include <string>

namespace quartz::stdlib {

namespace {

constexpr int kMaxDepth = 128;

class TomlParser {
public:
    TomlParser(Builder& b, std::string_view text) : b_(b), s_(text) {}

    bool parse(vm::Slot* root) {
        vm::Map* top = b_.newTable(root);
        if (top == nullptr) {
            return false;
        }
        current_ = top;
        while (true) {
            skipBlankLines();
            if (pos_ >= s_.size()) {
                return true;
            }
            bool ok = s_[pos_] == '[' ? header(top) : keyValue(current_, true);
            if (!ok) {
                return false;
            }
            skipSpaces();
            skipComment();
            if (pos_ < s_.size() && s_[pos_] != '\n' && s_[pos_] != '\r') {
                return fail("expected a new line after the value");
            }
        }
    }

    std::string error;

private:
    bool fail(std::string_view what) {
        if (error.empty()) {
            std::size_t line = 1;
            for (std::size_t i = 0; i < pos_ && i < s_.size(); ++i) {
                line += s_[i] == '\n' ? 1 : 0;
            }
            error = std::format("toml line {}: {}", line, what);
        }
        return false;
    }

    [[nodiscard]] bool at(char c) const { return pos_ < s_.size() && s_[pos_] == c; }

    void skipSpaces() {
        while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t')) {
            ++pos_;
        }
    }

    void skipComment() {
        if (at('#')) {
            while (pos_ < s_.size() && s_[pos_] != '\n') {
                ++pos_;
            }
        }
    }

    void skipBlankLines() {
        while (pos_ < s_.size()) {
            skipSpaces();
            skipComment();
            if (at('\n')) {
                ++pos_;
            } else if (at('\r') && pos_ + 1 < s_.size() && s_[pos_ + 1] == '\n') {
                pos_ += 2;
            } else {
                return;
            }
        }
    }

    // Whitespace, newlines and comments inside arrays.
    void skipArrayTrivia() {
        while (pos_ < s_.size()) {
            skipSpaces();
            skipComment();
            if (at('\n') || at('\r')) {
                ++pos_;
            } else {
                return;
            }
        }
    }

    static bool bareChar(char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    }

    bool keyPart(std::string& out) {
        skipSpaces();
        if (at('"')) {
            return basicString(out);
        }
        if (at('\'')) {
            return literalString(out);
        }
        std::size_t begin = pos_;
        while (pos_ < s_.size() && bareChar(s_[pos_])) {
            ++pos_;
        }
        if (pos_ == begin) {
            return fail("expected a key");
        }
        out.assign(s_.substr(begin, pos_ - begin));
        return true;
    }

    bool dottedKey(std::vector<std::string>& parts) {
        while (true) {
            std::string part;
            if (!keyPart(part)) {
                return false;
            }
            parts.push_back(std::move(part));
            skipSpaces();
            if (!at('.')) {
                return true;
            }
            ++pos_;
        }
    }

    // Walks `parts[0 .. n)` from `table`, creating implicit tables; the last of an array of tables is used.
    vm::Map* descend(vm::Map* table, const std::vector<std::string>& parts, std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) {
            vm::Slot* found = b_.find(table, parts[i]);
            if (found == nullptr) {
                vm::Slot v[2];
                vm::Map* m = b_.newTable(v);
                if (m == nullptr || !b_.put(table, parts[i], v)) {
                    return nullptr;
                }
                table = m;
                continue;
            }
            vm::Slot* value = found + 1;
            if (static_cast<Tag>(value[0].u) == Tag::Table) {
                table = static_cast<vm::Map*>(value[1].p);
                if (sealed_.contains(table)) {
                    fail("cannot extend an inline table");
                    return nullptr;
                }
                continue;
            }
            if (static_cast<Tag>(value[0].u) == Tag::List && arraysOfTables_.contains(value[1].p)) {
                auto* l = static_cast<vm::List*>(value[1].p);
                table = static_cast<vm::Map*>(l->data[(l->len - 1) * l->elemSlots + 1].p);
                continue;
            }
            fail(std::format("key '{}' is not a table", parts[i]));
            return nullptr;
        }
        return table;
    }

    bool header(vm::Map* top) {
        ++pos_;
        bool array = at('[');
        if (array) {
            ++pos_;
        }
        std::vector<std::string> parts;
        if (!dottedKey(parts)) {
            return false;
        }
        skipSpaces();
        if (!at(']') || (array && (pos_ + 1 >= s_.size() || s_[pos_ + 1] != ']'))) {
            return fail("unterminated table header");
        }
        pos_ += array ? 2 : 1;
        vm::Map* parent = descend(top, parts, parts.size() - 1);
        if (parent == nullptr) {
            return false;
        }
        const std::string& last = parts.back();
        vm::Slot* found = b_.find(parent, last);
        if (array) {
            vm::List* list = nullptr;
            if (found == nullptr) {
                vm::Slot lv[2];
                list = b_.newList(lv);
                if (list == nullptr || !b_.put(parent, last, lv)) {
                    return false;
                }
                arraysOfTables_.insert(list);
            } else if (static_cast<Tag>(found[1].u) == Tag::List && arraysOfTables_.contains(found[2].p)) {
                list = static_cast<vm::List*>(found[2].p);
            } else {
                return fail(std::format("'{}' is already defined", last));
            }
            vm::Slot tv[2];
            current_ = b_.newTable(tv);
            return current_ != nullptr && b_.push(list, tv);
        }
        if (found == nullptr) {
            vm::Slot tv[2];
            current_ = b_.newTable(tv);
            if (current_ == nullptr || !b_.put(parent, last, tv)) {
                return false;
            }
            defined_.insert(current_);
            return true;
        }
        if (static_cast<Tag>(found[1].u) != Tag::Table) {
            return fail(std::format("'{}' is already defined", last));
        }
        current_ = static_cast<vm::Map*>(found[2].p);
        if (defined_.contains(current_) || sealed_.contains(current_)) {
            return fail(std::format("table '{}' is defined twice", last));
        }
        defined_.insert(current_);
        return true;
    }

    bool keyValue(vm::Map* table, bool topLevel) {
        std::vector<std::string> parts;
        if (!dottedKey(parts)) {
            return false;
        }
        skipSpaces();
        if (!at('=')) {
            return fail("expected '=' after the key");
        }
        ++pos_;
        skipSpaces();
        vm::Map* target = descend(table, parts, parts.size() - 1);
        if (target == nullptr) {
            return false;
        }
        if (b_.find(target, parts.back()) != nullptr) {
            return fail(std::format("key '{}' is defined twice", parts.back()));
        }
        vm::Slot v[2];
        if (!value(v, 0)) {
            return false;
        }
        (void)topLevel;
        return b_.put(target, parts.back(), v);
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

    bool escape(std::string& out) {
        if (pos_ >= s_.size()) {
            return fail("unterminated escape");
        }
        char e = s_[pos_++];
        switch (e) {
        case 'b': out += '\b'; return true;
        case 't': out += '\t'; return true;
        case 'n': out += '\n'; return true;
        case 'f': out += '\f'; return true;
        case 'r': out += '\r'; return true;
        case 'e': out += '\x1b'; return true;
        case '"': out += '"'; return true;
        case '\\': out += '\\'; return true;
        case 'u':
        case 'U': {
            int n = e == 'u' ? 4 : 8;
            if (pos_ + static_cast<std::size_t>(n) > s_.size()) {
                return fail("truncated unicode escape");
            }
            std::uint32_t cp = 0;
            auto r = std::from_chars(s_.data() + pos_, s_.data() + pos_ + n, cp, 16);
            if (r.ptr != s_.data() + pos_ + n || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
                return fail("invalid unicode escape");
            }
            pos_ += static_cast<std::size_t>(n);
            appendUtf8(out, cp);
            return true;
        }
        default: return fail("invalid escape");
        }
    }

    bool basicString(std::string& out) {
        bool multi = s_.substr(pos_, 3) == "\"\"\"";
        pos_ += multi ? 3 : 1;
        if (multi && at('\n')) {
            ++pos_;
        } else if (multi && s_.substr(pos_, 2) == "\r\n") {
            pos_ += 2;
        }
        while (true) {
            if (pos_ >= s_.size()) {
                return fail("unterminated string");
            }
            char c = s_[pos_];
            if (multi && s_.substr(pos_, 3) == "\"\"\"") {
                pos_ += 3;
                for (int extra = 0; extra < 2 && at('"'); ++extra) { // a closing run of 4 or 5 quotes keeps the extra ones
                    out += '"';
                    ++pos_;
                }
                return true;
            }
            if (!multi && c == '"') {
                ++pos_;
                return true;
            }
            if (!multi && (c == '\n' || c == '\r')) {
                return fail("newline in a single-line string");
            }
            ++pos_;
            if (c == '\\') {
                if (multi && (at(' ') || at('\t') || at('\n') || at('\r'))) {
                    std::size_t save = pos_;
                    skipSpaces();
                    if (at('\n') || at('\r')) {
                        while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\n' ||
                                                    s_[pos_] == '\r')) {
                            ++pos_;
                        }
                        continue;
                    }
                    pos_ = save;
                }
                if (!escape(out)) {
                    return false;
                }
                continue;
            }
            out += c;
        }
    }

    bool literalString(std::string& out) {
        bool multi = s_.substr(pos_, 3) == "'''";
        pos_ += multi ? 3 : 1;
        if (multi && at('\n')) {
            ++pos_;
        } else if (multi && s_.substr(pos_, 2) == "\r\n") {
            pos_ += 2;
        }
        std::size_t end = s_.find(multi ? "'''" : "'", pos_);
        if (end == std::string_view::npos) {
            return fail("unterminated literal string");
        }
        std::string_view body = s_.substr(pos_, end - pos_);
        if (!multi && body.find('\n') != std::string_view::npos) {
            return fail("newline in a single-line string");
        }
        out.assign(body);
        pos_ = end + (multi ? 3 : 1);
        for (int extra = 0; multi && extra < 2 && at('\''); ++extra) {
            out += '\'';
            ++pos_;
        }
        return true;
    }

    bool number(vm::Slot* out) {
        std::size_t begin = pos_;
        while (pos_ < s_.size() && (bareChar(s_[pos_]) || s_[pos_] == '.' || s_[pos_] == '+' || s_[pos_] == ':' ||
                                    ((s_[pos_] == ' ') && pos_ + 1 < s_.size() && s_[pos_ + 1] >= '0' &&
                                     s_[pos_ + 1] <= '9' && pos_ - begin == 10))) {
            ++pos_;
        }
        std::string_view raw = s_.substr(begin, pos_ - begin);
        if (raw.empty()) {
            return fail("expected a value");
        }
        // Dates and times have no type of their own: they stay as text.
        bool date = raw.size() >= 10 && raw[4] == '-' && raw[7] == '-';
        bool time = raw.size() >= 8 && raw[2] == ':' && raw[5] == ':';
        if (date || time) {
            return b_.setStr(out, raw);
        }
        std::string text;
        for (std::size_t i = 0; i < raw.size(); ++i) {
            if (raw[i] == '_') {
                bool okUnderscore = i > 0 && i + 1 < raw.size() && std::isalnum(static_cast<unsigned char>(raw[i - 1])) &&
                                    std::isalnum(static_cast<unsigned char>(raw[i + 1]));
                if (!okUnderscore) {
                    return fail("misplaced '_' in a number");
                }
                continue;
            }
            text += raw[i];
        }
        std::string_view t = text;
        bool neg = false;
        if (!t.empty() && (t[0] == '+' || t[0] == '-')) {
            neg = t[0] == '-';
            t.remove_prefix(1);
        }
        if (t == "inf" || t == "nan") {
            double d = t == "inf" ? std::numeric_limits<double>::infinity() : std::numeric_limits<double>::quiet_NaN();
            Builder::setFloat(out, neg ? -d : d);
            return true;
        }
        int base = 10;
        if (t.size() > 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'o' || t[1] == 'b')) {
            if (text[0] == '+' || text[0] == '-') {
                return fail("prefixed integers cannot have a sign");
            }
            base = t[1] == 'x' ? 16 : t[1] == 'o' ? 8 : 2;
            t.remove_prefix(2);
        }
        bool isFloat = base == 10 && t.find_first_of(".eE") != std::string_view::npos;
        if (!isFloat) {
            if (base == 10 && t.size() > 1 && t[0] == '0') {
                return fail("leading zeros are not allowed");
            }
            std::uint64_t u = 0;
            auto r = std::from_chars(t.data(), t.data() + t.size(), u, base);
            if (r.ec != std::errc{} || r.ptr != t.data() + t.size() ||
                u > static_cast<std::uint64_t>(INT64_MAX) + (neg ? 1 : 0)) {
                return fail("invalid integer");
            }
            Builder::setInt(out, neg ? static_cast<std::int64_t>(0 - u) : static_cast<std::int64_t>(u));
            return true;
        }
        double d = 0;
        auto r = std::from_chars(t.data(), t.data() + t.size(), d);
        if (r.ec != std::errc{} || r.ptr != t.data() + t.size() || t.front() == '.' || t.back() == '.') {
            return fail("invalid float");
        }
        Builder::setFloat(out, neg ? -d : d);
        return true;
    }

    bool value(vm::Slot* out, int depth) {
        if (depth > kMaxDepth) {
            return fail("nesting too deep");
        }
        if (pos_ >= s_.size()) {
            return fail("expected a value");
        }
        char c = s_[pos_];
        if (c == '"' || c == '\'') {
            std::string text;
            bool ok = c == '"' ? basicString(text) : literalString(text);
            return ok && b_.setStr(out, text);
        }
        if (s_.substr(pos_, 4) == "true") {
            pos_ += 4;
            Builder::setBool(out, true);
            return true;
        }
        if (s_.substr(pos_, 5) == "false") {
            pos_ += 5;
            Builder::setBool(out, false);
            return true;
        }
        if (c == '[') {
            ++pos_;
            vm::List* l = b_.newList(out);
            if (l == nullptr) {
                return false;
            }
            while (true) {
                skipArrayTrivia();
                if (at(']')) {
                    ++pos_;
                    return true;
                }
                vm::Slot item[2];
                if (!value(item, depth + 1) || !b_.push(l, item)) {
                    return false;
                }
                skipArrayTrivia();
                if (at(',')) {
                    ++pos_;
                    continue;
                }
                if (at(']')) {
                    ++pos_;
                    return true;
                }
                return fail("expected ',' or ']' in an array");
            }
        }
        if (c == '{') {
            ++pos_;
            vm::Map* m = b_.newTable(out);
            if (m == nullptr) {
                return false;
            }
            skipSpaces();
            if (at('}')) {
                ++pos_;
                sealed_.insert(m);
                return true;
            }
            while (true) {
                skipSpaces();
                if (!keyValue(m, false)) {
                    return false;
                }
                skipSpaces();
                if (at(',')) {
                    ++pos_;
                    continue;
                }
                if (at('}')) {
                    ++pos_;
                    sealed_.insert(m);
                    return true;
                }
                return fail("expected ',' or '}' in an inline table");
            }
        }
        return number(out);
    }

    Builder& b_;
    std::string_view s_;
    std::size_t pos_ = 0;
    vm::Map* current_ = nullptr;
    std::set<const void*> defined_;
    std::set<const void*> sealed_;
    std::set<const void*> arraysOfTables_;
};

} // namespace

bool parseToml(vm::HostCall& call, void* user) {
    Builder b(call.sandbox, *static_cast<const DataTypes*>(user));
    std::string_view text = static_cast<const vm::Str*>(call.args[0].p)->view();
    if (!call.charge(static_cast<std::int64_t>(text.size() / 4 + 1))) {
        return false;
    }
    TomlParser p(b, text);
    vm::Slot root[2];
    if (!p.parse(root)) {
        return p.error.empty() ? false : b.err(call.out, p.error);
    }
    return b.ok(call.out, root);
}

} // namespace quartz::stdlib
