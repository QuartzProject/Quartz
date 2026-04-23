#include "driver/TomlLite.hpp"

#include <charconv>
#include <format>

namespace quartz::toml {

namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.remove_suffix(1);
    }
    return s;
}

bool bareKeyChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
}

class LineReader {
public:
    explicit LineReader(std::string_view s) : s_(s) {}

    [[nodiscard]] bool atEnd() {
        skipSpace();
        return pos_ == s_.size() || s_[pos_] == '#';
    }

    void skipSpace() {
        while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\r')) {
            ++pos_;
        }
    }

    bool eat(char c) {
        skipSpace();
        if (pos_ < s_.size() && s_[pos_] == c) {
            ++pos_;
            return true;
        }
        return false;
    }

    std::expected<std::string, std::string> string() {
        if (!eat('"')) {
            return std::unexpected("expected a quoted string");
        }
        std::size_t end = s_.find('"', pos_);
        if (end == std::string_view::npos) {
            return std::unexpected("unterminated string");
        }
        std::string_view body = s_.substr(pos_, end - pos_);
        if (!plainString(body)) {
            return std::unexpected("strings may not hold escapes or control characters");
        }
        pos_ = end + 1;
        return std::string(body);
    }

    std::expected<std::string, std::string> key() {
        skipSpace();
        if (pos_ < s_.size() && s_[pos_] == '"') {
            return string();
        }
        std::size_t begin = pos_;
        while (pos_ < s_.size() && bareKeyChar(s_[pos_])) {
            ++pos_;
        }
        if (begin == pos_) {
            return std::unexpected("expected a key");
        }
        return std::string(s_.substr(begin, pos_ - begin));
    }

    std::expected<void, std::string> value(Entry& e) {
        skipSpace();
        if (pos_ == s_.size()) {
            return std::unexpected("expected a value");
        }
        char c = s_[pos_];
        if (c == '"') {
            auto s = string();
            if (!s) {
                return std::unexpected(s.error());
            }
            e.kind = Entry::Kind::String;
            e.text = std::move(*s);
            return {};
        }
        if (c == '[') {
            ++pos_;
            e.kind = Entry::Kind::Strings;
            while (!eat(']')) {
                auto s = string();
                if (!s) {
                    return std::unexpected(s.error());
                }
                e.list.push_back(std::move(*s));
                if (!eat(',')) {
                    if (!eat(']')) {
                        return std::unexpected("expected ',' or ']' in a list");
                    }
                    break;
                }
            }
            return {};
        }
        std::size_t begin = pos_;
        while (pos_ < s_.size() && (bareKeyChar(s_[pos_]) || s_[pos_] == '+')) {
            ++pos_;
        }
        std::string_view word = s_.substr(begin, pos_ - begin);
        if (word == "true" || word == "false") {
            e.kind = Entry::Kind::Bool;
            e.flag = word == "true";
            return {};
        }
        auto r = std::from_chars(word.data(), word.data() + word.size(), e.integer);
        if (word.empty() || r.ec != std::errc{} || r.ptr != word.data() + word.size()) {
            return std::unexpected(std::format("unsupported value '{}'", trim(s_.substr(begin))));
        }
        e.kind = Entry::Kind::Integer;
        return {};
    }

private:
    std::string_view s_;
    std::size_t pos_ = 0;
};

} // namespace

const Entry* Section::find(std::string_view key) const {
    for (const Entry& e : entries) {
        if (e.key == key) {
            return &e;
        }
    }
    return nullptr;
}

bool plainString(std::string_view s) {
    for (char c : s) {
        auto u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7F || c == '"' || c == '\\') {
            return false;
        }
    }
    return true;
}

std::string quoteList(const std::vector<std::string>& items) {
    std::string out;
    for (const std::string& s : items) {
        out += std::format("{}\"{}\"", out.empty() ? "" : ", ", s);
    }
    return out;
}

std::expected<std::vector<Section>, std::string> parse(std::string_view text, std::string_view file) {
    std::vector<Section> out(1);
    std::size_t lineNo = 0;
    while (!text.empty()) {
        std::size_t nl = text.find('\n');
        std::string_view line = text.substr(0, nl);
        text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);
        ++lineNo;
        auto fail = [&](std::string_view what) {
            return std::unexpected(std::format("{}:{}: {}", file, lineNo, what));
        };
        LineReader r(line);
        if (r.atEnd()) {
            continue;
        }
        if (r.eat('[')) {
            Section s;
            s.array = r.eat('[');
            s.line = lineNo;
            auto name = r.key();
            if (!name) {
                return fail(name.error());
            }
            s.name = std::move(*name);
            if (!r.eat(']') || (s.array && !r.eat(']')) || !r.atEnd()) {
                return fail("bad table header");
            }
            out.push_back(std::move(s));
            continue;
        }
        Entry e;
        e.line = lineNo;
        auto key = r.key();
        if (!key) {
            return fail(key.error());
        }
        e.key = std::move(*key);
        if (!r.eat('=')) {
            return fail("expected key = value");
        }
        if (auto v = r.value(e); !v) {
            return fail(v.error());
        }
        if (!r.atEnd()) {
            return fail("unexpected text after the value");
        }
        if (out.back().find(e.key) != nullptr) {
            return fail(std::format("duplicate key '{}'", e.key));
        }
        out.back().entries.push_back(std::move(e));
    }
    return out;
}

} // namespace quartz::toml
