#include "tree_sitter/parser.h"

#include <stdbool.h>
#include <stdint.h>

enum TokenType {
    AUTOMATIC_SEMICOLON,
    STRING_CONTENT,
    MULTILINE_STRING_CONTENT,
    BLOCK_COMMENT,
    ERROR_SENTINEL,
};

void* tree_sitter_quartz_external_scanner_create(void) { return NULL; }

void tree_sitter_quartz_external_scanner_destroy(void* payload) { (void)payload; }

unsigned tree_sitter_quartz_external_scanner_serialize(void* payload, char* buffer) {
    (void)payload;
    (void)buffer;
    return 0;
}

void tree_sitter_quartz_external_scanner_deserialize(void* payload, const char* buffer, unsigned length) {
    (void)payload;
    (void)buffer;
    (void)length;
}

static void advance(TSLexer* lexer) { lexer->advance(lexer, false); }

static void skip(TSLexer* lexer) { lexer->advance(lexer, true); }

static bool is_ident_char(int32_t c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

static bool is_digit(int32_t c) { return c >= '0' && c <= '9'; }

// Called with the lexer just past the opening "/*"; block comments nest.
static void finish_block_comment(TSLexer* lexer) {
    unsigned depth = 1;
    while (!lexer->eof(lexer)) {
        if (lexer->lookahead == '/') {
            advance(lexer);
            if (lexer->lookahead == '*') {
                advance(lexer);
                depth++;
            }
        } else if (lexer->lookahead == '*') {
            advance(lexer);
            if (lexer->lookahead == '/') {
                advance(lexer);
                if (--depth == 0) {
                    return;
                }
            }
        } else {
            advance(lexer);
        }
    }
}

static bool scan_block_comment(TSLexer* lexer) {
    if (lexer->lookahead != '/') {
        return false;
    }
    advance(lexer);
    if (lexer->lookahead != '*') {
        return false;
    }
    advance(lexer);
    finish_block_comment(lexer);
    lexer->mark_end(lexer);
    lexer->result_symbol = BLOCK_COMMENT;
    return true;
}

static bool scan_string_content(TSLexer* lexer, bool multiline) {
    bool has_content = false;
    for (;;) {
        int32_t c = lexer->lookahead;
        if (lexer->eof(lexer) || c == '\\' || c == '{' || (!multiline && c == '\n')) {
            break;
        }
        if (c == '"') {
            if (!multiline) {
                break;
            }
            lexer->mark_end(lexer);
            advance(lexer);
            if (lexer->lookahead == '"') {
                advance(lexer);
                if (lexer->lookahead == '"') {
                    lexer->result_symbol = MULTILINE_STRING_CONTENT;
                    return has_content;
                }
            }
            has_content = true;
            continue;
        }
        advance(lexer);
        has_content = true;
    }
    lexer->mark_end(lexer);
    lexer->result_symbol = multiline ? MULTILINE_STRING_CONTENT : STRING_CONTENT;
    return has_content;
}

static bool skip_comment_in_lookahead(TSLexer* lexer) {
    advance(lexer);
    if (lexer->lookahead == '/') {
        while (!lexer->eof(lexer) && lexer->lookahead != '\n') {
            advance(lexer);
        }
        return true;
    }
    if (lexer->lookahead == '*') {
        advance(lexer);
        finish_block_comment(lexer);
        return true;
    }
    return false;
}

// After a newline: does the next line continue the current expression?
static bool continues_expression(TSLexer* lexer) {
    switch (lexer->lookahead) {
    case '.':
        advance(lexer);
        return lexer->lookahead != '.';
    case '?':
        advance(lexer);
        if (lexer->lookahead == '?') {
            return true;
        }
        if (lexer->lookahead == '.') {
            advance(lexer);
            return !is_digit(lexer->lookahead);
        }
        return false;
    case '&':
        advance(lexer);
        return lexer->lookahead == '&';
    case '|':
        advance(lexer);
        return lexer->lookahead == '|';
    case 'e': {
        const char* rest = "else";
        for (const char* p = rest; *p != '\0'; p++) {
            if (lexer->lookahead != *p) {
                return false;
            }
            advance(lexer);
        }
        return !is_ident_char(lexer->lookahead);
    }
    default:
        return false;
    }
}

static bool scan_automatic_semicolon(TSLexer* lexer, bool* saw_comment) {
    while (lexer->lookahead == ' ' || lexer->lookahead == '\t' || lexer->lookahead == '\r') {
        skip(lexer);
    }
    lexer->mark_end(lexer);
    lexer->result_symbol = AUTOMATIC_SEMICOLON;
    if (lexer->eof(lexer) || lexer->lookahead == '}') {
        return true;
    }
    if (lexer->lookahead != '\n') {
        *saw_comment = lexer->lookahead == '/';
        return false;
    }
    for (;;) {
        int32_t c = lexer->lookahead;
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            advance(lexer);
        } else if (c == '/') {
            if (!skip_comment_in_lookahead(lexer)) {
                return true;
            }
        } else {
            break;
        }
    }
    if (lexer->eof(lexer)) {
        return true;
    }
    return !continues_expression(lexer);
}

bool tree_sitter_quartz_external_scanner_scan(void* payload, TSLexer* lexer, const bool* valid_symbols) {
    (void)payload;
    if (valid_symbols[ERROR_SENTINEL]) {
        while (lexer->lookahead == ' ' || lexer->lookahead == '\t' || lexer->lookahead == '\r' ||
               lexer->lookahead == '\n') {
            skip(lexer);
        }
        return valid_symbols[BLOCK_COMMENT] && scan_block_comment(lexer);
    }
    if (valid_symbols[STRING_CONTENT] || valid_symbols[MULTILINE_STRING_CONTENT]) {
        return scan_string_content(lexer, valid_symbols[MULTILINE_STRING_CONTENT]);
    }
    if (valid_symbols[AUTOMATIC_SEMICOLON]) {
        bool at_slash = false;
        if (scan_automatic_semicolon(lexer, &at_slash)) {
            return true;
        }
        if (!at_slash) {
            return false;
        }
        return valid_symbols[BLOCK_COMMENT] && scan_block_comment(lexer);
    }
    while (lexer->lookahead == ' ' || lexer->lookahead == '\t' || lexer->lookahead == '\r' ||
           lexer->lookahead == '\n') {
        skip(lexer);
    }
    return valid_symbols[BLOCK_COMMENT] && scan_block_comment(lexer);
}
