#include "format/Printer.hpp"

#include <algorithm>

namespace quartz::format {

namespace {

bool isBlank(char c) { return c == ' ' || c == '\t' || c == '\r'; }

} // namespace

Indent::Indent(Printer& p) : p_(p), indent_(p.indent_), cont_(p.contActive_) { ++p_.indent_; }

Indent::~Indent() {
    p_.indent_ = indent_;
    p_.contActive_ = cont_;
}

Printer::Printer(std::string_view src, std::span<const Token> toks, std::span<const Span> comments,
                 const Interner& names)
    : src_(src), toks_(toks), names_(names) {
    comments_.reserve(comments.size());
    for (const Span& s : comments) {
        Comment c{s.begin, s.end, true, blankLineBefore(s.begin), true};
        std::size_t i = s.begin;
        while (i > 0 && isBlank(src_[i - 1])) {
            --i;
        }
        c.ownLine = i == 0 || src_[i - 1] == '\n';
        std::size_t j = s.end;
        while (j < src_.size() && isBlank(src_[j])) {
            ++j;
        }
        c.newlineAfter = j == src_.size() || src_[j] == '\n';
        while (c.end > c.begin && isBlank(src_[c.end - 1])) {
            --c.end;
        }
        comments_.push_back(c);
    }
}

std::string Printer::run(const Module& m) {
    out_.reserve(src_.size() + src_.size() / 8);
    module(m);
    auto eof = static_cast<std::uint32_t>(src_.size());
    newline(eof);
    flush(eof);
    while (!out_.empty() && (out_.back() == '\n' || isBlank(out_.back()))) {
        out_.pop_back();
    }
    if (!out_.empty()) {
        out_ += '\n';
    }
    return std::move(out_);
}

void Printer::text(std::string_view s) {
    if (s.empty()) {
        return;
    }
    if (lineStart_) {
        out_.append(static_cast<std::size_t>(indent_) * 4, ' ');
        lineStart_ = false;
    } else if (pendingSpace_ && s[0] != ')' && s[0] != ']' && s[0] != '}' && s[0] != ',') {
        out_ += ' ';
    }
    pendingSpace_ = false;
    out_ += s;
}

void Printer::breakLine() {
    pendingSpace_ = false;
    if (lineStart_) {
        return;
    }
    while (!out_.empty() && isBlank(out_.back())) {
        out_.pop_back();
    }
    out_ += '\n';
    lineStart_ = true;
}

void Printer::newline(std::uint32_t next) {
    flushTrailing(next);
    breakLine();
}

void Printer::blankLine() {
    breakLine();
    std::size_t i = out_.size();
    while (i > 0 && out_[i - 1] == '\n') {
        --i;
    }
    if (i == 0 || out_.size() - i >= 2) {
        return;
    }
    char last = out_[i - 1];
    if (last == '{' || last == '(' || last == '[') {
        return;
    }
    out_ += '\n';
}

void Printer::contBreak(std::uint32_t next) {
    newline(next);
    if (!contActive_) {
        ++indent_;
        contActive_ = true;
    }
}

void Printer::spaceBefore(const Comment& c) {
    char last = out_.empty() ? '\n' : out_.back();
    if ((last != '(' && last != '[') || src_[c.begin + 1] == '/') {
        space();
    }
}

bool Printer::commentBefore(std::uint32_t offset) const {
    return nextComment_ < comments_.size() && comments_[nextComment_].begin < offset;
}

void Printer::flush(std::uint32_t offset) {
    while (commentBefore(offset)) {
        const Comment& c = comments_[nextComment_++];
        bool mid = !lineStart_;
        if (c.ownLine) {
            if (mid) {
                contBreak(c.begin);
            }
            if (c.blankBefore) {
                blankLine();
            }
        } else if (mid) {
            spaceBefore(c);
        }
        text(raw(c.begin, c.end));
        if (c.newlineAfter) {
            if (mid) {
                contBreak(c.end);
            } else {
                breakLine();
            }
        } else {
            space();
        }
    }
}

void Printer::flushTrailing(std::uint32_t before) {
    while (commentBefore(before) && !comments_[nextComment_].ownLine) {
        const Comment& c = comments_[nextComment_++];
        spaceBefore(c);
        text(raw(c.begin, c.end));
        if (c.newlineAfter) {
            breakLine();
        } else {
            space();
        }
    }
}

bool Printer::newlineIn(std::uint32_t from, std::uint32_t to) const {
    auto c = std::lower_bound(comments_.begin(), comments_.end(), from,
                              [](const Comment& k, std::uint32_t o) { return k.begin < o; });
    for (std::uint32_t i = from; i < to && i < src_.size(); ++i) {
        if (c != comments_.end() && c->begin == i) {
            i = c->end - 1;
            ++c;
        } else if (src_[i] == '\n') {
            return true;
        }
    }
    return false;
}

bool Printer::blankLineBefore(std::uint32_t offset) const {
    int lines = 0;
    std::size_t i = offset;
    while (i > 0 && (isBlank(src_[i - 1]) || src_[i - 1] == '\n')) {
        lines += src_[i - 1] == '\n' ? 1 : 0;
        --i;
    }
    return lines >= 2 && i > 0;
}

std::size_t Printer::tokenAt(std::uint32_t offset) const {
    auto it = std::lower_bound(toks_.begin(), toks_.end(), offset,
                               [](const Token& t, std::uint32_t o) { return t.span.begin < o; });
    if (it == toks_.end() || it->span.begin != offset) {
        return toks_.size();
    }
    return static_cast<std::size_t>(it - toks_.begin());
}

std::size_t Printer::tokenEndingAt(std::uint32_t end) const {
    auto it = std::lower_bound(toks_.begin(), toks_.end(), end,
                               [](const Token& t, std::uint32_t o) { return t.span.end < o; });
    if (it == toks_.end() || it->span.end != end) {
        return toks_.size();
    }
    return static_cast<std::size_t>(it - toks_.begin());
}

std::size_t Printer::matching(std::size_t open) const {
    Tok o = toks_[open].kind;
    Tok c = o == Tok::LParen ? Tok::RParen : o == Tok::LBracket ? Tok::RBracket : Tok::RBrace;
    int depth = 0;
    for (std::size_t i = open; i < toks_.size(); ++i) {
        if (toks_[i].kind == o) {
            ++depth;
        } else if (toks_[i].kind == c && --depth == 0) {
            return i;
        }
    }
    return toks_.size();
}

std::size_t Printer::nextToken(std::uint32_t from, Tok kind) const {
    auto it = std::lower_bound(toks_.begin(), toks_.end(), from,
                               [](const Token& t, std::uint32_t o) { return t.span.begin < o; });
    for (; it != toks_.end(); ++it) {
        if (it->kind == kind) {
            return static_cast<std::size_t>(it - toks_.begin());
        }
    }
    return toks_.size();
}

} // namespace quartz::format
