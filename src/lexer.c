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
    if (check_keyword(start, length, "sizeof", TOKEN_KEYWORD_SIZEOF) != TOKEN_IDENTIFIER) return TOKEN_KEYWORD_SIZEOF;
    if (check_keyword(start, length, "const", TOKEN_KEYWORD_CONST) != TOKEN_IDENTIFIER) return TOKEN_KEYWORD_CONST;
    if (check_keyword(start, length, "static", TOKEN_KEYWORD_STATIC) != TOKEN_IDENTIFIER) return TOKEN_KEYWORD_STATIC;
    if (check_keyword(start, length, "unsigned", TOKEN_KEYWORD_UNSIGNED) != TOKEN_IDENTIFIER) return TOKEN_KEYWORD_UNSIGNED;
    if (check_keyword(start, length, "long", TOKEN_KEYWORD_LONG) != TOKEN_IDENTIFIER) return TOKEN_KEYWORD_LONG;
    if (check_keyword(start, length, "__pragma_pack_push", TOKEN_PRAGMA_PACK_PUSH) != TOKEN_IDENTIFIER) return TOKEN_PRAGMA_PACK_PUSH;
    if (check_keyword(start, length, "__pragma_pack_pop", TOKEN_PRAGMA_PACK_POP) != TOKEN_IDENTIFIER) return TOKEN_PRAGMA_PACK_POP;
    if (check_keyword(start, length, "__pragma_pack", TOKEN_PRAGMA_PACK_SET) != TOKEN_IDENTIFIER) return TOKEN_PRAGMA_PACK_SET;
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
        
        // Hex literal: 0x... or 0X...
        // c is the peeked '0', not yet consumed - advance past it first
        if (c == '0') {
            advance(lexer); // consume '0'
            if (peek(lexer) == 'x' || peek(lexer) == 'X') {
                advance(lexer); // consume 'x'/'X'
                while (isdigit(peek(lexer)) || (peek(lexer) >= 'a' && peek(lexer) <= 'f') || (peek(lexer) >= 'A' && peek(lexer) <= 'F')) {
                    advance(lexer);
                }
                // Consume optional integer suffixes (U, L, UL, etc.)
                while (peek(lexer) == 'u' || peek(lexer) == 'U' || peek(lexer) == 'l' || peek(lexer) == 'L') {
                    advance(lexer);
                }
                token.type = TOKEN_NUMBER;
                token.length = &lexer->source[lexer->position] - token.start;
                return token;
            }
            // Just '0' followed by more digits (octal) or nothing
        }
        
        while (isdigit(peek(lexer))) {
            advance(lexer);
        }
        
        // Fraction part?
        if (peek(lexer) == '.') {
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
                advance(lexer);
            }
        }
        
        // Long suffix (e.g. 0L, 1UL) - just consume and ignore
        while (peek(lexer) == 'u' || peek(lexer) == 'U' || peek(lexer) == 'l' || peek(lexer) == 'L') {
            advance(lexer);
        }

        token.type = is_float ? TOKEN_FLOAT : TOKEN_NUMBER;
        token.length = &lexer->source[lexer->position] - token.start;
        return token;
    }
    
    // Character literal: 'a', '\n', '\0', '\\', etc.
    if (c == '\'') {
        advance(lexer); // consume opening quote
        if (peek(lexer) == '\\') {
            advance(lexer); // consume backslash
            advance(lexer); // consume escape char (n, 0, t, \\, ', etc.)
        } else {
            advance(lexer); // consume the char
        }
        if (peek(lexer) == '\'') {
            advance(lexer); // consume closing quote
        }
        token.type = TOKEN_NUMBER; // char literals are integers
        token.length = &lexer->source[lexer->position] - token.start;
        return token;
    }
    
    if (c == '"') {
        advance(lexer); // Consume opening quote
        while (peek(lexer) != '"' && peek(lexer) != '\0') {
            if (peek(lexer) == '\\') {
                advance(lexer); // Consume backslash
                if (peek(lexer) != '\0') advance(lexer); // Consume escaped char
            } else {
                if (peek(lexer) == '\n') lexer->line++;
                advance(lexer);
            }
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
        case '.':
            if (peek(lexer) == '.' && lexer->source[lexer->position + 1] == '.') {
                advance(lexer);
                advance(lexer);
                token.type = TOKEN_ELLIPSIS;
                token.length = 3;
            } else {
                token.type = TOKEN_DOT;
            }
            break;
        case '+':
            if (match(lexer, '+')) {
                token.type = TOKEN_PLUS_PLUS;
                token.length = 2;
            } else if (match(lexer, '=')) {
                token.type = TOKEN_PLUS_EQUAL;
                token.length = 2;
            } else {
                token.type = TOKEN_PLUS;
            }
            break;
        case '-':
            if (match(lexer, '>')) {
                token.type = TOKEN_ARROW;
                token.length = 2;
            } else if (match(lexer, '-')) {
                token.type = TOKEN_MINUS_MINUS;
                token.length = 2;
            } else if (match(lexer, '=')) {
                token.type = TOKEN_MINUS_EQUAL;
                token.length = 2;
            } else {
                token.type = TOKEN_MINUS;
            }
            break;
        case '%':
            if (match(lexer, '=')) {
                token.type = TOKEN_PERCENT_EQUAL;
                token.length = 2;
            } else {
                token.type = TOKEN_PERCENT;
            }
            break;
        case '*':
            if (match(lexer, '=')) {
                token.type = TOKEN_STAR_EQUAL;
                token.length = 2;
            } else {
                token.type = TOKEN_STAR;
            }
            break;
        case '/':
            if (match(lexer, '=')) {
                token.type = TOKEN_SLASH_EQUAL;
                token.length = 2;
            } else {
                token.type = TOKEN_SLASH;
            }
            break;
        case '&':
            if (match(lexer, '&')) {
                token.type = TOKEN_AMPERSAND_AMPERSAND;
                token.length = 2;
            } else if (match(lexer, '=')) {
                token.type = TOKEN_AMPERSAND_EQUAL;
                token.length = 2;
            } else {
                token.type = TOKEN_AMPERSAND;
            }
            break;
        case '|':
            if (match(lexer, '|')) {
                token.type = TOKEN_PIPE_PIPE;
                token.length = 2;
            } else if (match(lexer, '=')) {
                token.type = TOKEN_PIPE_EQUAL;
                token.length = 2;
            } else {
                token.type = TOKEN_PIPE;
            }
            break;
        case '^':
            if (match(lexer, '=')) {
                token.type = TOKEN_CARET_EQUAL;
                token.length = 2;
            } else {
                token.type = TOKEN_CARET;
            }
            break;
        case '?': token.type = TOKEN_QUESTION; break;
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
                if (match(lexer, '=')) {
                    token.type = TOKEN_LESS_LESS_EQUAL;
                    token.length = 3;
                } else {
                    token.type = TOKEN_LESS_LESS;
                    token.length = 2;
                }
            } else if (match(lexer, '=')) {
                token.type = TOKEN_LESS_EQUAL;
                token.length = 2;
            } else {
                token.type = TOKEN_LESS;
            }
            break;
        case '>':
            if (match(lexer, '>')) {
                if (match(lexer, '=')) {
                    token.type = TOKEN_GREATER_GREATER_EQUAL;
                    token.length = 3;
                } else {
                    token.type = TOKEN_GREATER_GREATER;
                    token.length = 2;
                }
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
