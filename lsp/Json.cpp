#include "Json.hpp"

#include <charconv>
#include <cmath>

namespace quartz::lsp {

namespace {

const Json kNull;

constexpr int kMaxDepth = 256;

void appendUtf8(std::string& out, std::uint32_t cp) {
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

} // namespace

class JsonParser {
public:
    explicit JsonParser(std::string_view text) : text_(text) {}

    std::optional<Json> run() {
        Json out;
        if (!value(out, 0)) {
            return std::nullopt;
        }
        skipSpace();
        if (pos_ != text_.size()) {
            return std::nullopt;
        }
        return out;
    }

private:
    void skipSpace() {
        while (pos_ < text_.size() &&
               (text_[pos_] == ' ' || text_[pos_] == '\t' || text_[pos_] == '\n' || text_[pos_] == '\r')) {
            ++pos_;
        }
    }

    bool literal(std::string_view word) {
        if (text_.substr(pos_, word.size()) != word) {
            return false;
        }
        pos_ += word.size();
        return true;
    }

    bool value(Json& out, int depth) {
        if (depth > kMaxDepth) {
            return false;
        }
        skipSpace();
        if (pos_ >= text_.size()) {
            return false;
        }
        char c = text_[pos_];
        switch (c) {
        case 'n':
            return literal("null");
        case 't':
            out = Json(true);
            return literal("true");
        case 'f':
            out = Json(false);
            return literal("false");
        case '"':
            out.kind_ = Json::Kind::String;
            return string(out.str_);
        case '[':
            return array(out, depth);
        case '{':
            return object(out, depth);
        default:
            return number(out);
        }
    }

    bool array(Json& out, int depth) {
        ++pos_;
        out = Json::array();
        skipSpace();
        if (pos_ < text_.size() && text_[pos_] == ']') {
            ++pos_;
            return true;
        }
        while (true) {
            Json item;
            if (!value(item, depth + 1)) {
                return false;
            }
            out.items_.push_back(std::move(item));
            skipSpace();
            if (pos_ >= text_.size()) {
                return false;
            }
            if (text_[pos_] == ']') {
                ++pos_;
                return true;
            }
            if (text_[pos_++] != ',') {
                return false;
            }
        }
    }

    bool object(Json& out, int depth) {
        ++pos_;
        out = Json::object();
        skipSpace();
        if (pos_ < text_.size() && text_[pos_] == '}') {
            ++pos_;
            return true;
        }
        while (true) {
            skipSpace();
            std::string key;
            if (pos_ >= text_.size() || text_[pos_] != '"' || !string(key)) {
                return false;
            }
            skipSpace();
            if (pos_ >= text_.size() || text_[pos_++] != ':') {
                return false;
            }
            Json item;
            if (!value(item, depth + 1)) {
                return false;
            }
            out.members_.emplace_back(std::move(key), std::move(item));
            skipSpace();
            if (pos_ >= text_.size()) {
                return false;
            }
            if (text_[pos_] == '}') {
                ++pos_;
                return true;
            }
            if (text_[pos_++] != ',') {
                return false;
            }
        }
    }

    bool hex4(std::uint32_t& out) {
        if (pos_ + 4 > text_.size()) {
            return false;
        }
        out = 0;
        for (int i = 0; i < 4; ++i) {
            char h = text_[pos_++];
            out <<= 4;
            if (h >= '0' && h <= '9') {
                out |= static_cast<std::uint32_t>(h - '0');
            } else if (h >= 'a' && h <= 'f') {
                out |= static_cast<std::uint32_t>(h - 'a' + 10);
            } else if (h >= 'A' && h <= 'F') {
                out |= static_cast<std::uint32_t>(h - 'A' + 10);
            } else {
                return false;
            }
        }
        return true;
    }

    bool string(std::string& out) {
        ++pos_;
        while (pos_ < text_.size()) {
            char c = text_[pos_++];
            if (c == '"') {
                return true;
            }
            if (static_cast<unsigned char>(c) < 0x20) {
                return false;
            }
            if (c != '\\') {
                out += c;
                continue;
            }
            if (pos_ >= text_.size()) {
                return false;
            }
            switch (text_[pos_++]) {
            case '"':
                out += '"';
                break;
            case '\\':
                out += '\\';
                break;
            case '/':
                out += '/';
                break;
            case 'b':
                out += '\b';
                break;
            case 'f':
                out += '\f';
                break;
            case 'n':
                out += '\n';
                break;
            case 'r':
                out += '\r';
                break;
            case 't':
                out += '\t';
                break;
            case 'u': {
                std::uint32_t cp = 0;
                if (!hex4(cp)) {
                    return false;
                }
                if (cp >= 0xD800 && cp < 0xDC00 && text_.substr(pos_, 2) == "\\u") {
                    std::size_t save = pos_;
                    pos_ += 2;
                    std::uint32_t lo = 0;
                    if (hex4(lo) && lo >= 0xDC00 && lo < 0xE000) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    } else {
                        pos_ = save;
                        cp = 0xFFFD;
                    }
                } else if (cp >= 0xD800 && cp < 0xE000) {
                    cp = 0xFFFD;
                }
                appendUtf8(out, cp);
                break;
            }
            default:
                return false;
            }
        }
        return false;
    }

    bool number(Json& out) {
        std::size_t begin = pos_;
        if (pos_ < text_.size() && text_[pos_] == '-') {
            ++pos_;
        }
        bool isFloat = false;
        std::size_t digits = 0;
        while (pos_ < text_.size()) {
            char c = text_[pos_];
            if (c >= '0' && c <= '9') {
                ++digits;
            } else if (c == '.' || c == 'e' || c == 'E' || c == '+' || (c == '-' && pos_ > begin)) {
                isFloat = true;
            } else {
                break;
            }
            ++pos_;
        }
        if (digits == 0) {
            return false;
        }
        const char* first = text_.data() + begin;
        const char* last = text_.data() + pos_;
        if (!isFloat) {
            std::int64_t v = 0;
            auto r = std::from_chars(first, last, v);
            if (r.ec == std::errc{} && r.ptr == last) {
                out = Json(v);
                return true;
            }
        }
        double d = 0;
        auto r = std::from_chars(first, last, d);
        if (r.ec != std::errc{} || r.ptr != last) {
            return false;
        }
        out = Json(d);
        return true;
    }

    std::string_view text_;
    std::size_t pos_ = 0;
};

const Json& Json::operator[](std::string_view key) const {
    if (kind_ == Kind::Object) {
        for (const auto& [k, v] : members_) {
            if (k == key) {
                return v;
            }
        }
    }
    return kNull;
}

const Json& Json::operator[](std::size_t index) const {
    return kind_ == Kind::Array && index < items_.size() ? items_[index] : kNull;
}

bool Json::has(std::string_view key) const {
    if (kind_ == Kind::Object) {
        for (const auto& member : members_) {
            if (member.first == key) {
                return true;
            }
        }
    }
    return false;
}

std::int64_t Json::integer(std::int64_t fallback) const {
    if (kind_ == Kind::Int) {
        return int_;
    }
    if (kind_ == Kind::Float && std::isfinite(float_) && std::abs(float_) < 9.0e18) {
        return static_cast<std::int64_t>(float_);
    }
    return fallback;
}

Json& Json::set(std::string key, Json value) {
    if (kind_ != Kind::Object) {
        *this = object();
    }
    for (auto& [k, v] : members_) {
        if (k == key) {
            v = std::move(value);
            return *this;
        }
    }
    members_.emplace_back(std::move(key), std::move(value));
    return *this;
}

Json& Json::push(Json value) {
    if (kind_ != Kind::Array) {
        *this = array();
    }
    items_.push_back(std::move(value));
    return *this;
}

void appendJsonString(std::string& out, std::string_view text) {
    static constexpr char kHex[] = "0123456789abcdef";
    out += '"';
    for (char c : text) {
        auto u = static_cast<unsigned char>(c);
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
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (u < 0x20) {
                out += "\\u00";
                out += kHex[u >> 4];
                out += kHex[u & 0xF];
            } else {
                out += c;
            }
        }
    }
    out += '"';
}

void Json::dumpTo(std::string& out) const {
    switch (kind_) {
    case Kind::Null:
        out += "null";
        break;
    case Kind::Bool:
        out += int_ != 0 ? "true" : "false";
        break;
    case Kind::Int:
        out += std::to_string(int_);
        break;
    case Kind::Float: {
        if (!std::isfinite(float_)) {
            out += "null";
            break;
        }
        char buf[32];
        auto r = std::to_chars(buf, buf + sizeof buf, float_);
        out.append(buf, r.ptr);
        break;
    }
    case Kind::String:
        appendJsonString(out, str_);
        break;
    case Kind::Array:
        out += '[';
        for (std::size_t i = 0; i < items_.size(); ++i) {
            if (i > 0) {
                out += ',';
            }
            items_[i].dumpTo(out);
        }
        out += ']';
        break;
    case Kind::Object:
        out += '{';
        for (std::size_t i = 0; i < members_.size(); ++i) {
            if (i > 0) {
                out += ',';
            }
            appendJsonString(out, members_[i].first);
            out += ':';
            members_[i].second.dumpTo(out);
        }
        out += '}';
        break;
    }
}

std::string Json::dump() const {
    std::string out;
    dumpTo(out);
    return out;
}

std::optional<Json> Json::parse(std::string_view text) { return JsonParser(text).run(); }

} // namespace quartz::lsp
