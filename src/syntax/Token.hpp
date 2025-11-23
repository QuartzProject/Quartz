#pragma once

#include "support/Source.hpp"

#include <cstdint>
#include <string_view>

namespace quartz::syntax {

#define QUARTZ_KEYWORDS(X)                                                                                             \
    X(Package, "package")                                                                                              \
    X(Import, "import")                                                                                                \
    X(Pub, "pub")                                                                                                      \
    X(Internal, "internal")                                                                                            \
    X(Private, "private")                                                                                              \
    X(Let, "let")                                                                                                      \
    X(Var, "var")                                                                                                      \
    X(Const, "const")                                                                                                  \
    X(Fn, "fn")                                                                                                        \
    X(Suspend, "suspend")                                                                                              \
    X(Struct, "struct")                                                                                                \
    X(State, "state")                                                                                                  \
    X(Enum, "enum")                                                                                                    \
    X(Trait, "trait")                                                                                                  \
    X(Impl, "impl")                                                                                                    \
    X(If, "if")                                                                                                        \
    X(Else, "else")                                                                                                    \
    X(Match, "match")                                                                                                  \
    X(For, "for")                                                                                                      \
    X(In, "in")                                                                                                        \
    X(While, "while")                                                                                                  \
    X(Break, "break")                                                                                                  \
    X(Continue, "continue")                                                                                            \
    X(Return, "return")                                                                                                \
    X(Wait, "wait")                                                                                                    \
    X(At, "at")                                                                                                        \
    X(Scope, "scope")                                                                                                  \
    X(Every, "every")                                                                                                  \
    X(Ref, "ref")                                                                                                      \
    X(As, "as")                                                                                                        \
    X(Is, "is")                                                                                                        \
    X(True, "true")                                                                                                    \
    X(False, "false")                                                                                                  \
    X(Null, "null")                                                                                                    \
    X(Self, "self")

enum class Tok : std::uint8_t {
    Eof,
    Error,
    Ident,
    Int,
    Float,
    Char,
    String,      // plain string literal, no interpolation
    InterpBegin, // text before the first `{`
    InterpMid,   // text between `}` and the next `{`
    InterpEnd,   // text after the last `}`
#define QUARTZ_KW_ENUM(name, text) Kw##name,
    QUARTZ_KEYWORDS(QUARTZ_KW_ENUM)
#undef QUARTZ_KW_ENUM
        LParen,
    RParen,
    LBracket,
    RBracket,
    LBrace,
    RBrace,
    Comma,
    Dot,
    DotDot,
    DotDotEq,
    Colon,
    ColonColon,
    Semi,
    Arrow,    // ->
    FatArrow, // =>
    At_,      // @
    Question,
    QuestionDot,
    QuestionQuestion,
    Plus,
    Minus,
    Star,
    Slash,
    Percent,
    Amp,
    Pipe,
    Caret,
    Tilde,
    Bang,
    Eq,
    EqEq,
    BangEq,
    Lt,
    LtEq,
    Gt, // never `>>`: the parser joins adjacent `>` (spec 3.6)
    GtEq,
    Shl,
    AmpAmp,
    PipePipe,
    PlusEq,
    MinusEq,
    StarEq,
    SlashEq,
    PercentEq,
    AmpEq,
    PipeEq,
    CaretEq,
    ShlEq,
};

const char* tokName(Tok t);

struct Token {
    Tok kind = Tok::Eof;
    bool joinedNext = false; // no whitespace before the next token (used to join `>` `>`)
    bool newlineBefore = false;
    Span span;
    std::string_view text; // raw source text; for string parts, the decoded content
    std::uint64_t intValue = 0;
    double floatValue = 0;
};

} // namespace quartz::syntax
