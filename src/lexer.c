#include "lexer.h"
#include <ctype.h>
#include <string.h>

void lexer_init(Lexer *lexer, const char *source) {
    lexer->source = source;
    lexer->position = 0;
    lexer->line = 1;
}

static char peek(Lexer *lexer) {
    return lexer->source[lexer->position];
}

static char advance(Lexer *lexer) {
    char c = lexer->source[lexer->position];
    if (c != '\0') {
        lexer->position++;
        if (c == '\n') {
            lexer->line++;
        }
    }
    return c;
}

static void skip_whitespace(Lexer *lexer) {
    while (isspace(peek(lexer))) {
        advance(lexer);
    }
}

static TokenType check_keyword(const char *start, size_t length, const char *keyword, TokenType type) {
    if (strlen(keyword) == length && strncmp(start, keyword, length) == 0) {
        return type;
    }
    return TOKEN_IDENTIFIER;
}

static TokenType identifier_type(const char *start, size_t length) {
    if (check_keyword(start, length, "int", TOKEN_KEYWORD_INT) != TOKEN_IDENTIFIER) return TOKEN_KEYWORD_INT;
    if (check_keyword(start, length, "return", TOKEN_KEYWORD_RETURN) != TOKEN_IDENTIFIER) return TOKEN_KEYWORD_RETURN;
    return TOKEN_IDENTIFIER;
}

Token lexer_next_token(Lexer *lexer) {
    skip_whitespace(lexer);

    Token token;
    token.start = &lexer->source[lexer->position];
    token.line = lexer->line;

    char c = peek(lexer);

    if (c == '\0') {
        token.type = TOKEN_EOF;
        token.length = 0;
        return token;
    }

    if (isalpha(c) || c == '_') {
        while (isalnum(peek(lexer)) || peek(lexer) == '_') {
            advance(lexer);
        }
        token.length = &lexer->source[lexer->position] - token.start;
        token.type = identifier_type(token.start, token.length);
        return token;
    }

    if (isdigit(c)) {
        while (isdigit(peek(lexer))) {
            advance(lexer);
        }
        token.type = TOKEN_NUMBER;
        token.length = &lexer->source[lexer->position] - token.start;
        return token;
    }

    advance(lexer);
    token.length = 1;

    switch (c) {
        case '(': token.type = TOKEN_LPAREN; break;
        case ')': token.type = TOKEN_RPAREN; break;
        case '{': token.type = TOKEN_LBRACE; break;
        case '}': token.type = TOKEN_RBRACE; break;
        case ';': token.type = TOKEN_SEMICOLON; break;
        default: token.type = TOKEN_UNKNOWN; break;
    }

    return token;
}
