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
    if (check_keyword(start, length, "struct", TOKEN_KEYWORD_STRUCT) != TOKEN_IDENTIFIER) return TOKEN_KEYWORD_STRUCT;
    if (check_keyword(start, length, "typedef", TOKEN_KEYWORD_TYPEDEF) != TOKEN_IDENTIFIER) return TOKEN_KEYWORD_TYPEDEF;
    if (check_keyword(start, length, "extern", TOKEN_KEYWORD_EXTERN) != TOKEN_IDENTIFIER) return TOKEN_KEYWORD_EXTERN;
    if (check_keyword(start, length, "switch", TOKEN_KEYWORD_SWITCH) != TOKEN_IDENTIFIER) return TOKEN_KEYWORD_SWITCH;
    if (check_keyword(start, length, "case", TOKEN_KEYWORD_CASE) != TOKEN_IDENTIFIER) return TOKEN_KEYWORD_CASE;
    if (check_keyword(start, length, "default", TOKEN_KEYWORD_DEFAULT) != TOKEN_IDENTIFIER) return TOKEN_KEYWORD_DEFAULT;
    if (check_keyword(start, length, "break", TOKEN_KEYWORD_BREAK) != TOKEN_IDENTIFIER) return TOKEN_KEYWORD_BREAK;
    if (check_keyword(start, length, "enum", TOKEN_KEYWORD_ENUM) != TOKEN_IDENTIFIER) return TOKEN_KEYWORD_ENUM;
    if (check_keyword(start, length, "union", TOKEN_KEYWORD_UNION) != TOKEN_IDENTIFIER) return TOKEN_KEYWORD_UNION;
    if (check_keyword(start, length, "float", TOKEN_KEYWORD_FLOAT) != TOKEN_IDENTIFIER) return TOKEN_KEYWORD_FLOAT;
    if (check_keyword(start, length, "double", TOKEN_KEYWORD_DOUBLE) != TOKEN_IDENTIFIER) return TOKEN_KEYWORD_DOUBLE;
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

    if (isdigit(c) != 0) {
        int is_float = 0;
        
        while (isdigit(peek(lexer))) {
            advance(lexer);
        }
        
        // Fraction part?
        if (peek(lexer) == '.') {
            // Need to distinguish struct member access vs float. e.g. "student.name".
            // But if we already parsed digits, "1" is a number. "1." is a float.
            // Standard C says "1." is a float.
            is_float = 1;
            advance(lexer);
            while (isdigit(peek(lexer))) {
                advance(lexer);
            }
        }
        
        // Exponent part?
        if (peek(lexer) == 'e' || peek(lexer) == 'E') {
            is_float = 1;
            advance(lexer);
            if (peek(lexer) == '+' || peek(lexer) == '-') {
                advance(lexer);
            }
            while (isdigit(peek(lexer))) {
                advance(lexer);
            }
        }
        
        // Float suffix?
        if (peek(lexer) == 'f' || peek(lexer) == 'F') {
            if (is_float) {
                // If it is already float-like (has . or e), consume 'f'.
                // If it's just '1f', standard compliance says invalid.
                // But for now, let's allow 'f' to make it a float if we want `1f` to be float.
                // Standard C: `1.0f` is float. `1f` is error.
                // Let's stick to standardish: consume 'f' only if is_float is true OR if we treat `1f` as error/float.
                // Actually `1f` is often a typo for `1.f`.
                // Let's only consume 'f' if we saw '.', 'e', or if we decide `1f` is float.
                // GCC: `1f` -> invalid suffix "f" on integer constant
                // So valid floats MUST have `.` or `e`.
                advance(lexer);
            }
        }

        token.type = is_float ? TOKEN_FLOAT : TOKEN_NUMBER;
        token.length = &lexer->source[lexer->position] - token.start;
        return token;
    }
    if (c == '"') {
        advance(lexer); // Consume opening quote
        while (peek(lexer) != '"' && peek(lexer) != '\0') {
            if (peek(lexer) == '\n') lexer->line++;
            advance(lexer);
        }
        if (peek(lexer) == '"') {
            advance(lexer); // Consume closing quote
            token.type = TOKEN_STRING;
            // token.start points to opening quote, position is after closing quote
            token.length = &lexer->source[lexer->position] - token.start - 2;
            token.start++; // skip opening quote
            return token;
        }
    }

    advance(lexer);
    token.length = 1;

    switch (c) {
        case '(': token.type = TOKEN_LPAREN; break;
        case ')': token.type = TOKEN_RPAREN; break;
        case '{': token.type = TOKEN_LBRACE; break;
        case '}': token.type = TOKEN_RBRACE; break;
        case '[': token.type = TOKEN_LBRACKET; break;
        case ']': token.type = TOKEN_RBRACKET; break;
        case ';': token.type = TOKEN_SEMICOLON; break;
        case ':': token.type = TOKEN_COLON; break;
        case ',': token.type = TOKEN_COMMA; break;
        case '.': token.type = TOKEN_DOT; break;
        case '+': token.type = TOKEN_PLUS; break;
        case '-':
            if (match(lexer, '>')) {
                token.type = TOKEN_ARROW;
                token.length = 2;
            } else {
                token.type = TOKEN_MINUS;
            }
            break;
        case '%': token.type = TOKEN_PERCENT; break;
        case '*': token.type = TOKEN_STAR; break;
        case '/': token.type = TOKEN_SLASH; break;
        case '&':
            if (match(lexer, '&')) {
                token.type = TOKEN_AMPERSAND_AMPERSAND;
                token.length = 2;
            } else {
                token.type = TOKEN_AMPERSAND;
            }
            break;
        case '|':
            if (match(lexer, '|')) {
                token.type = TOKEN_PIPE_PIPE;
                token.length = 2;
            } else {
                token.type = TOKEN_PIPE;
            }
            break;
        case '^': token.type = TOKEN_CARET; break;
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
                token.type = TOKEN_BANG;
            }
            break;
        case '<':
            if (match(lexer, '<')) {
                token.type = TOKEN_LESS_LESS;
                token.length = 2;
            } else if (match(lexer, '=')) {
                token.type = TOKEN_LESS_EQUAL;
                token.length = 2;
            } else {
                token.type = TOKEN_LESS;
            }
            break;
        case '>':
            if (match(lexer, '>')) {
                token.type = TOKEN_GREATER_GREATER;
                token.length = 2;
            } else if (match(lexer, '=')) {
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
