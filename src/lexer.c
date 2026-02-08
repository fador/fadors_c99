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
    while (1) {
        char c = peek(lexer);
        if (isspace(c)) {
            advance(lexer);
        } else if (c == '/' && lexer->source[lexer->position + 1] == '/') {
            while (peek(lexer) != '\n' && peek(lexer) != '\0') {
                advance(lexer);
            }
        } else {
            break;
        }
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
    if (check_keyword(start, length, "if", TOKEN_KEYWORD_IF) != TOKEN_IDENTIFIER) return TOKEN_KEYWORD_IF;
    if (check_keyword(start, length, "else", TOKEN_KEYWORD_ELSE) != TOKEN_IDENTIFIER) return TOKEN_KEYWORD_ELSE;
    if (check_keyword(start, length, "while", TOKEN_KEYWORD_WHILE) != TOKEN_IDENTIFIER) return TOKEN_KEYWORD_WHILE;
    if (check_keyword(start, length, "for", TOKEN_KEYWORD_FOR) != TOKEN_IDENTIFIER) return TOKEN_KEYWORD_FOR;
    if (check_keyword(start, length, "void", TOKEN_KEYWORD_VOID) != TOKEN_IDENTIFIER) return TOKEN_KEYWORD_VOID;
    if (check_keyword(start, length, "char", TOKEN_KEYWORD_CHAR) != TOKEN_IDENTIFIER) return TOKEN_KEYWORD_CHAR;
    return TOKEN_IDENTIFIER;
}

static int match(Lexer *lexer, char expected) {
    if (peek(lexer) == expected) {
        advance(lexer);
        return 1;
    }
    return 0;
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
        case ',': token.type = TOKEN_COMMA; break;
        case '+': token.type = TOKEN_PLUS; break;
        case '-': token.type = TOKEN_MINUS; break;
        case '*': token.type = TOKEN_STAR; break;
        case '/': token.type = TOKEN_SLASH; break;
        case '=':
            if (match(lexer, '=')) {
                token.type = TOKEN_EQUAL_EQUAL;
                token.length = 2;
            } else {
                token.type = TOKEN_EQUAL;
            }
            break;
        case '!':
            if (match(lexer, '=')) {
                token.type = TOKEN_BANG_EQUAL;
                token.length = 2;
            } else {
                token.type = TOKEN_UNKNOWN;
            }
            break;
        case '<':
            if (match(lexer, '=')) {
                token.type = TOKEN_LESS_EQUAL;
                token.length = 2;
            } else {
                token.type = TOKEN_LESS;
            }
            break;
        case '>':
            if (match(lexer, '=')) {
                token.type = TOKEN_GREATER_EQUAL;
                token.length = 2;
            } else {
                token.type = TOKEN_GREATER;
            }
            break;
        default: token.type = TOKEN_UNKNOWN; break;
    }

    return token;
}

Token lexer_peek_token(Lexer *lexer) {
    size_t pos = lexer->position;
    int line = lexer->line;
    Token token = lexer_next_token(lexer);
    lexer->position = pos;
    lexer->line = line;
    return token;
}
