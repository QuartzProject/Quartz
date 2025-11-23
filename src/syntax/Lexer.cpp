#include "syntax/Lexer.hpp"

#include <charconv>
#include <format>
#include <string>
#include <unordered_map>

namespace quartz::syntax {

const char* tokName(Tok t) {
    switch (t) {
    case Tok::Eof: return "end of file";
    case Tok::Error: return "invalid token";
    case Tok::Ident: return "identifier";
    case Tok::Int: return "integer literal";
    case Tok::Float: return "float literal";
    case Tok::Char: return "char literal";
    case Tok::String: return "string literal";
    case Tok::InterpBegin:
    case Tok::InterpMid:
    case Tok::InterpEnd: return "string literal";
#define QUARTZ_KW_NAME(name, text)                                                                                     \
    case Tok::Kw##name: return "'" text "'";
        QUARTZ_KEYWORDS(QUARTZ_KW_NAME)
#undef QUARTZ_KW_NAME
    case Tok::LParen: return "'('";
    case Tok::RParen: return "')'";
    case Tok::LBracket: return "'['";
    case Tok::RBracket: return "']'";
    case Tok::LBrace: return "'{'";
    case Tok::RBrace: return "'}'";
    case Tok::Comma: return "','";
    case Tok::Dot: return "'.'";
    case Tok::DotDot: return "'..'";
    case Tok::DotDotEq: return "'..='";
    case Tok::Colon: return "':'";
    case Tok::ColonColon: return "'::'";
    case Tok::Semi: return "';'";
    case Tok::Arrow: return "'->'";
    case Tok::FatArrow: return "'=>'";
    case Tok::At_: return "'@'";
    case Tok::Question: return "'?'";
    case Tok::QuestionDot: return "'?.'";
    case Tok::QuestionQuestion: return "'?" "?'";
    case Tok::Plus: return "'+'";
    case Tok::Minus: return "'-'";
    case Tok::Star: return "'*'";
    case Tok::Slash: return "'/'";
    case Tok::Percent: return "'%'";
    case Tok::Amp: return "'&'";
    case Tok::Pipe: return "'|'";
    case Tok::Caret: return "'^'";
    case Tok::Tilde: return "'~'";
    case Tok::Bang: return "'!'";
    case Tok::Eq: return "'='";
    case Tok::EqEq: return "'=='";
    case Tok::BangEq: return "'!='";
    case Tok::Lt: return "'<'";
    case Tok::LtEq: return "'<='";
    case Tok::Gt: return "'>'";
    case Tok::GtEq: return "'>='";
    case Tok::Shl: return "'<<'";
    case Tok::AmpAmp: return "'&&'";
    case Tok::PipePipe: return "'||'";
    case Tok::PlusEq: return "'+='";
    case Tok::MinusEq: return "'-='";
    case Tok::StarEq: return "'*='";
    case Tok::SlashEq: return "'/='";
    case Tok::PercentEq: return "'%='";
    case Tok::AmpEq: return "'&='";
    case Tok::PipeEq: return "'|='";
    case Tok::CaretEq: return "'^='";
    case Tok::ShlEq: return "'<<='";
    }
    return "token";
}

namespace {

const std::unordered_map<std::string_view, Tok>& keywords() {
    static const std::unordered_map<std::string_view, Tok> table = {
#define QUARTZ_KW_ENTRY(name, text) {text, Tok::Kw##name},
        QUARTZ_KEYWORDS(QUARTZ_KW_ENTRY)
#undef QUARTZ_KW_ENTRY
    };
    return table;
}

bool isIdentStart(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }

bool isDigit(char c) { return c >= '0' && c <= '9'; }

bool isIdentChar(char c) { return isIdentStart(c) || isDigit(c); }

int digitValue(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return 99;
}

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

enum class Mode : std::uint8_t { Brace, Interp, InterpMultiline };

class Lexer {
public:
    Lexer(const SourceFile& file, Arena& arena, Diagnostics& diags)
        : file_(file), src_(file.text()), arena_(arena), diags_(diags) {}

    std::vector<Token> run() {
        std::vector<Token> out;
        out.reserve(src_.size() / 4 + 4);
        while (true) {
            bool newline = skipTrivia();
            if (pos_ >= src_.size()) {
                Token eof = make(Tok::Eof, pos_);
                eof.newlineBefore = true;
                out.push_back(eof);
                break;
            }
            Token t = next();
            t.newlineBefore = newline;
            if (!out.empty() && out.back().span.end == t.span.begin) {
                out.back().joinedNext = true;
            }
            out.push_back(t);
        }
        if (!modes_.empty() && modes_.back() != Mode::Brace) {
            error(static_cast<std::uint32_t>(src_.size()), static_cast<std::uint32_t>(src_.size()),
                  "unterminated string interpolation");
        }
        return out;
    }

private:
    Token make(Tok kind, std::size_t begin) {
        Token t;
        t.kind = kind;
        t.span = {file_.id(), static_cast<std::uint32_t>(begin), static_cast<std::uint32_t>(pos_)};
        t.text = src_.substr(begin, pos_ - begin);
        return t;
    }

    void error(std::uint32_t begin, std::uint32_t end, std::string message) {
        diags_.error({file_.id(), begin, end}, std::move(message));
    }

    [[nodiscard]] char peek(std::size_t ahead = 0) const {
        return pos_ + ahead < src_.size() ? src_[pos_ + ahead] : '\0';
    }

    bool skipTrivia() {
        bool newline = false;
        while (pos_ < src_.size()) {
            char c = src_[pos_];
            if (c == '\n') {
                newline = true;
                ++pos_;
            } else if (c == ' ' || c == '\t' || c == '\r') {
                ++pos_;
            } else if (c == '/' && peek(1) == '/') {
                while (pos_ < src_.size() && src_[pos_] != '\n') {
                    ++pos_;
                }
            } else if (c == '/' && peek(1) == '*') {
                skipBlockComment();
            } else {
                break;
            }
        }
        return newline;
    }

    void skipBlockComment() {
        std::size_t begin = pos_;
        int depth = 0;
        while (pos_ < src_.size()) {
            if (src_[pos_] == '/' && peek(1) == '*') {
                ++depth;
                pos_ += 2;
            } else if (src_[pos_] == '*' && peek(1) == '/') {
                pos_ += 2;
                if (--depth == 0) {
                    return;
                }
            } else {
                ++pos_;
            }
        }
        error(static_cast<std::uint32_t>(begin), static_cast<std::uint32_t>(begin + 2), "unterminated block comment");
    }

    Token next() {
        std::size_t begin = pos_;
        char c = src_[pos_];
        if (isIdentStart(c)) {
            while (pos_ < src_.size() && isIdentChar(src_[pos_])) {
                ++pos_;
            }
            Token t = make(Tok::Ident, begin);
            auto it = keywords().find(t.text);
            if (it != keywords().end()) {
                t.kind = it->second;
            }
            return t;
        }
        if (isDigit(c)) {
            return number(begin);
        }
        if (c == '"') {
            bool multiline = peek(1) == '"' && peek(2) == '"';
            pos_ += multiline ? 3 : 1;
            return stringPart(begin, multiline, true);
        }
        if (c == '\'') {
            return charLiteral(begin);
        }
        if (c == '}' && !modes_.empty() && modes_.back() != Mode::Brace) {
            bool multiline = modes_.back() == Mode::InterpMultiline;
            modes_.pop_back();
            ++pos_;
            return stringPart(begin, multiline, false);
        }
        return punct(begin);
    }

    Token number(std::size_t begin) {
        int base = 10;
        if (src_[pos_] == '0' && (peek(1) == 'x' || peek(1) == 'X')) {
            base = 16;
            pos_ += 2;
        } else if (src_[pos_] == '0' && (peek(1) == 'b' || peek(1) == 'B')) {
            base = 2;
            pos_ += 2;
        } else if (src_[pos_] == '0' && (peek(1) == 'o' || peek(1) == 'O')) {
            base = 8;
            pos_ += 2;
        }
        std::string digits;
        auto scanDigits = [&](int radix) {
            while (pos_ < src_.size()) {
                char d = src_[pos_];
                if (d == '_') {
                    ++pos_;
                    continue;
                }
                if (digitValue(d) >= radix) {
                    break;
                }
                digits += d;
                ++pos_;
            }
        };
        scanDigits(base);
        bool isFloat = false;
        if (base == 10 && peek() == '.' && isDigit(peek(1))) {
            isFloat = true;
            digits += '.';
            ++pos_;
            scanDigits(10);
        }
        if (base == 10 && (peek() == 'e' || peek() == 'E') &&
            (isDigit(peek(1)) || ((peek(1) == '+' || peek(1) == '-') && isDigit(peek(2))))) {
            isFloat = true;
            digits += 'e';
            ++pos_;
            if (peek() == '+' || peek() == '-') {
                digits += src_[pos_++];
            }
            scanDigits(10);
        }
        if (isIdentChar(peek())) {
            std::size_t badBegin = pos_;
            while (pos_ < src_.size() && isIdentChar(src_[pos_])) {
                ++pos_;
            }
            error(static_cast<std::uint32_t>(badBegin), static_cast<std::uint32_t>(pos_),
                  "invalid suffix on numeric literal");
        }
        Token t = make(isFloat ? Tok::Float : Tok::Int, begin);
        if (digits.empty()) {
            error(t.span.begin, t.span.end, "numeric literal has no digits");
            return t;
        }
        if (isFloat) {
            auto res = std::from_chars(digits.data(), digits.data() + digits.size(), t.floatValue);
            if (res.ec != std::errc{}) {
                error(t.span.begin, t.span.end, "invalid float literal");
            }
        } else {
            auto res = std::from_chars(digits.data(), digits.data() + digits.size(), t.intValue, base);
            if (res.ec == std::errc::result_out_of_range) {
                error(t.span.begin, t.span.end, "integer literal does not fit in 64 bits");
            }
        }
        return t;
    }

    bool escape(std::string& out) {
        std::size_t begin = pos_;
        ++pos_;
        char e = peek();
        ++pos_;
        switch (e) {
        case 'n': out += '\n'; return true;
        case 't': out += '\t'; return true;
        case 'r': out += '\r'; return true;
        case '0': out += '\0'; return true;
        case '\\': out += '\\'; return true;
        case '"': out += '"'; return true;
        case '\'': out += '\''; return true;
        case '{': out += '{'; return true;
        case '}': out += '}'; return true;
        case 'u': {
            if (peek() != '{') {
                break;
            }
            ++pos_;
            std::uint32_t cp = 0;
            int count = 0;
            while (pos_ < src_.size() && src_[pos_] != '}' && count < 7) {
                int v = digitValue(src_[pos_]);
                if (v >= 16) {
                    break;
                }
                cp = cp * 16 + static_cast<std::uint32_t>(v);
                ++count;
                ++pos_;
            }
            if (peek() != '}' || count == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
                break;
            }
            ++pos_;
            appendUtf8(out, cp);
            return true;
        }
        default: break;
        }
        error(static_cast<std::uint32_t>(begin), static_cast<std::uint32_t>(pos_), "invalid escape sequence");
        return false;
    }

    Token stringPart(std::size_t begin, bool multiline, bool first) {
        std::string text;
        while (true) {
            if (pos_ >= src_.size() || (!multiline && src_[pos_] == '\n')) {
                error(static_cast<std::uint32_t>(begin), static_cast<std::uint32_t>(pos_), "unterminated string");
                Token t = make(first ? Tok::String : Tok::InterpEnd, begin);
                t.text = arena_.copy(text);
                return t;
            }
            char c = src_[pos_];
            if (c == '\\') {
                escape(text);
                continue;
            }
            if (c == '"' && (!multiline || (peek(1) == '"' && peek(2) == '"'))) {
                pos_ += multiline ? 3 : 1;
                Token t = make(first ? Tok::String : Tok::InterpEnd, begin);
                t.text = arena_.copy(text);
                return t;
            }
            if (c == '{') {
                ++pos_;
                modes_.push_back(multiline ? Mode::InterpMultiline : Mode::Interp);
                Token t = make(first ? Tok::InterpBegin : Tok::InterpMid, begin);
                t.text = arena_.copy(text);
                return t;
            }
            text += c;
            ++pos_;
        }
    }

    Token charLiteral(std::size_t begin) {
        ++pos_;
        std::string text;
        if (peek() == '\\') {
            escape(text);
        } else if (pos_ < src_.size() && src_[pos_] != '\'' && src_[pos_] != '\n') {
            auto lead = static_cast<unsigned char>(src_[pos_]);
            std::size_t len = lead < 0x80 ? 1 : lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4;
            text.append(src_.substr(pos_, len));
            pos_ += len;
        }
        if (peek() != '\'') {
            error(static_cast<std::uint32_t>(begin), static_cast<std::uint32_t>(pos_), "unterminated char literal");
        } else {
            ++pos_;
        }
        Token t = make(Tok::Char, begin);
        t.intValue = decodeFirst(text);
        if (text.empty()) {
            error(t.span.begin, t.span.end, "empty char literal");
        }
        return t;
    }

    static std::uint32_t decodeFirst(std::string_view s) {
        if (s.empty()) {
            return 0;
        }
        auto b0 = static_cast<unsigned char>(s[0]);
        if (b0 < 0x80 || s.size() == 1) {
            return b0;
        }
        std::uint32_t cp = 0;
        std::size_t n = b0 < 0xE0 ? 2 : b0 < 0xF0 ? 3 : 4;
        cp = b0 & (0xFF >> (n + 1));
        for (std::size_t i = 1; i < n && i < s.size(); ++i) {
            cp = (cp << 6) | (static_cast<unsigned char>(s[i]) & 0x3F);
        }
        return cp;
    }

    Token punct(std::size_t begin) {
        char c = src_[pos_++];
        auto two = [&](char next, Tok yes, Tok no) {
            if (peek() == next) {
                ++pos_;
                return yes;
            }
            return no;
        };
        Tok kind = Tok::Error;
        switch (c) {
        case '(': kind = Tok::LParen; break;
        case ')': kind = Tok::RParen; break;
        case '[': kind = Tok::LBracket; break;
        case ']': kind = Tok::RBracket; break;
        case '{':
            modes_.push_back(Mode::Brace);
            kind = Tok::LBrace;
            break;
        case '}':
            if (!modes_.empty()) {
                modes_.pop_back();
            }
            kind = Tok::RBrace;
            break;
        case ',': kind = Tok::Comma; break;
        case ';': kind = Tok::Semi; break;
        case '@': kind = Tok::At_; break;
        case '~': kind = Tok::Tilde; break;
        case '^': kind = two('=', Tok::CaretEq, Tok::Caret); break;
        case '+': kind = two('=', Tok::PlusEq, Tok::Plus); break;
        case '*': kind = two('=', Tok::StarEq, Tok::Star); break;
        case '/': kind = two('=', Tok::SlashEq, Tok::Slash); break;
        case '%': kind = two('=', Tok::PercentEq, Tok::Percent); break;
        case '!': kind = two('=', Tok::BangEq, Tok::Bang); break;
        case ':': kind = two(':', Tok::ColonColon, Tok::Colon); break;
        case '-':
            kind = peek() == '>' ? (++pos_, Tok::Arrow) : two('=', Tok::MinusEq, Tok::Minus);
            break;
        case '=':
            kind = peek() == '>' ? (++pos_, Tok::FatArrow) : two('=', Tok::EqEq, Tok::Eq);
            break;
        case '&':
            kind = peek() == '&' ? (++pos_, Tok::AmpAmp) : two('=', Tok::AmpEq, Tok::Amp);
            break;
        case '|':
            kind = peek() == '|' ? (++pos_, Tok::PipePipe) : two('=', Tok::PipeEq, Tok::Pipe);
            break;
        case '<':
            if (peek() == '<') {
                ++pos_;
                kind = two('=', Tok::ShlEq, Tok::Shl);
            } else {
                kind = two('=', Tok::LtEq, Tok::Lt);
            }
            break;
        case '>': kind = two('=', Tok::GtEq, Tok::Gt); break;
        case '?':
            if (peek() == '.' && !isDigit(peek(1))) {
                ++pos_;
                kind = Tok::QuestionDot;
            } else {
                kind = two('?', Tok::QuestionQuestion, Tok::Question);
            }
            break;
        case '.':
            if (peek() == '.') {
                ++pos_;
                kind = two('=', Tok::DotDotEq, Tok::DotDot);
            } else {
                kind = Tok::Dot;
            }
            break;
        default: break;
        }
        Token t = make(kind, begin);
        if (kind == Tok::Error) {
            auto lead = static_cast<unsigned char>(c);
            if (lead >= 0x80) {
                while (pos_ < src_.size() && (static_cast<unsigned char>(src_[pos_]) & 0xC0) == 0x80) {
                    ++pos_;
                }
                t = make(kind, begin);
            }
            error(t.span.begin, t.span.end, std::format("unexpected character '{}'", t.text));
        }
        return t;
    }

    const SourceFile& file_;
    std::string_view src_;
    Arena& arena_;
    Diagnostics& diags_;
    std::size_t pos_ = 0;
    std::vector<Mode> modes_;
};

} // namespace

std::vector<Token> tokenize(const SourceFile& file, Arena& arena, Diagnostics& diags) {
    return Lexer(file, arena, diags).run();
}

} // namespace quartz::syntax
