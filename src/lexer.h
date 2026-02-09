#ifndef LEXER_H
#define LEXER_H

#include <stddef.h>

typedef enum {
    TOKEN_EOF,
    TOKEN_IDENTIFIER,
    TOKEN_NUMBER,
    TOKEN_KEYWORD_INT,
    TOKEN_KEYWORD_RETURN,
    TOKEN_KEYWORD_IF,
    TOKEN_KEYWORD_ELSE,
    TOKEN_KEYWORD_WHILE,
    TOKEN_KEYWORD_FOR,
    TOKEN_KEYWORD_VOID,
    TOKEN_KEYWORD_CHAR,
    TOKEN_KEYWORD_STRUCT,
    TOKEN_KEYWORD_TYPEDEF,
    TOKEN_KEYWORD_EXTERN,
    TOKEN_KEYWORD_SWITCH,
    TOKEN_KEYWORD_CASE,
    TOKEN_KEYWORD_DEFAULT,
    TOKEN_KEYWORD_BREAK,
    TOKEN_KEYWORD_ENUM,
    TOKEN_KEYWORD_UNION,
    TOKEN_KEYWORD_FLOAT,
    TOKEN_KEYWORD_DOUBLE,
    TOKEN_KEYWORD_SIZEOF,
    TOKEN_KEYWORD_CONST,
    TOKEN_KEYWORD_STATIC,
    TOKEN_KEYWORD_UNSIGNED,
    TOKEN_LPAREN,       // (
    TOKEN_RPAREN,       // )
    TOKEN_BANG,         // !
    TOKEN_LBRACE,       // {
    TOKEN_RBRACE,       // }
    TOKEN_LBRACKET,     // [
    TOKEN_RBRACKET,     // ]
    TOKEN_SEMICOLON,    // ;
    TOKEN_COLON,        // :
    TOKEN_COMMA,        // ,
    TOKEN_DOT,          // .
    TOKEN_ARROW,        // ->
    TOKEN_PLUS,         // +
    TOKEN_MINUS,        // -
    TOKEN_STAR,         // *
    TOKEN_SLASH,        // /
    TOKEN_PERCENT,      // %
    TOKEN_AMPERSAND,    // &
    TOKEN_PIPE,         // |
    TOKEN_CARET,        // ^
    TOKEN_AMPERSAND_AMPERSAND, // &&
    TOKEN_PIPE_PIPE,    // ||
    TOKEN_LESS_LESS,    // <<
    TOKEN_GREATER_GREATER, // >>
    TOKEN_EQUAL,        // =
    TOKEN_EQUAL_EQUAL,  // ==
    TOKEN_BANG_EQUAL,   // !=
    TOKEN_LESS,         // <
    TOKEN_GREATER,      // >
    TOKEN_LESS_EQUAL,   // <=
    TOKEN_GREATER_EQUAL, // >=
    TOKEN_PLUS_PLUS,    // ++
    TOKEN_MINUS_MINUS,  // --
    TOKEN_ELLIPSIS,     // ...
    TOKEN_FLOAT,       // e.g. 3.14
    TOKEN_STRING,       // "string"
    TOKEN_UNKNOWN
} TokenType;

typedef struct {
    TokenType type;
    const char *start;
    size_t length;
    int line;
} Token;

typedef struct {
    const char *source;
    size_t position;
    int line;
} Lexer;

void lexer_init(Lexer *lexer, const char *source);
Token lexer_next_token(Lexer *lexer);
Token lexer_peek_token(Lexer *lexer);

#endif // LEXER_H
