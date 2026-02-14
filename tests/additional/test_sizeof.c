int printf(const char *fmt, ...);
int fflush(void *stream);
void *__acrt_iob_func(int idx);
void *malloc(long long size);

/* Replicate the relevant struct definitions */
typedef long long size_t;

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
    TOKEN_KEYWORD_ENUM,
    TOKEN_KEYWORD_UNION,
    TOKEN_KEYWORD_FLOAT,
    TOKEN_KEYWORD_DOUBLE,
    TOKEN_KEYWORD_SIZEOF,
    TOKEN_KEYWORD_INLINE,
    TOKEN_KEYWORD_RESTRICT,
    TOKEN_KEYWORD_VOLATILE,
    TOKEN_KEYWORD_SWITCH,
    TOKEN_KEYWORD_CASE,
    TOKEN_KEYWORD_DEFAULT,
    TOKEN_KEYWORD_BREAK,
    TOKEN_KEYWORD_CONTINUE,
    TOKEN_KEYWORD_DO,
    TOKEN_KEYWORD_GOTO,
    TOKEN_KEYWORD_CONST,
    TOKEN_KEYWORD_STATIC,
    TOKEN_KEYWORD_UNSIGNED,
    TOKEN_KEYWORD_LONG,
    TOKEN_KEYWORD_AUTO,
    TOKEN_KEYWORD_REGISTER,
    TOKEN_KEYWORD_SHORT,
    TOKEN_LPAREN,
    TOKEN_RPAREN,
    TOKEN_BANG,
    TOKEN_LBRACE,
    TOKEN_RBRACE,
    TOKEN_LBRACKET,
    TOKEN_RBRACKET,
    TOKEN_SEMICOLON,
    TOKEN_COLON,
    TOKEN_COMMA,
    TOKEN_DOT,
    TOKEN_ARROW,
    TOKEN_PLUS,
    TOKEN_MINUS,
    TOKEN_STAR,
    TOKEN_SLASH,
    TOKEN_PERCENT,
    TOKEN_AMPERSAND,
    TOKEN_PIPE,
    TOKEN_CARET,
    TOKEN_AMPERSAND_AMPERSAND,
    TOKEN_PIPE_PIPE,
    TOKEN_LESS_LESS,
    TOKEN_GREATER_GREATER,
    TOKEN_BITWISE_NOT,
    TOKEN_EQUAL,
    TOKEN_EQUAL_EQUAL,
    TOKEN_BANG_EQUAL,
    TOKEN_LESS,
    TOKEN_GREATER,
    TOKEN_LESS_EQUAL,
    TOKEN_GREATER_EQUAL,
    TOKEN_PLUS_PLUS,
    TOKEN_MINUS_MINUS,
    TOKEN_ELLIPSIS,
    TOKEN_FLOAT_LIT,
    TOKEN_STRING,
    TOKEN_PLUS_EQUAL,
    TOKEN_MINUS_EQUAL,
    TOKEN_STAR_EQUAL,
    TOKEN_SLASH_EQUAL,
    TOKEN_PERCENT_EQUAL,
    TOKEN_PIPE_EQUAL,
    TOKEN_AMPERSAND_EQUAL,
    TOKEN_CARET_EQUAL,
    TOKEN_LESS_LESS_EQUAL,
    TOKEN_GREATER_GREATER_EQUAL,
    TOKEN_QUESTION,
    TOKEN_PRAGMA_PACK_PUSH,
    TOKEN_PRAGMA_PACK_POP,
    TOKEN_PRAGMA_PACK_SET,
    TOKEN_UNKNOWN
} TokenType;

typedef struct {
    TokenType type;
    const char *start;
    size_t length;
    int line;
} Token;

int main() {
    printf("sizeof(Token) = %d\n", (int)sizeof(Token));
    printf("sizeof(int) = %d\n", (int)sizeof(int));
    printf("sizeof(char*) = %d\n", (int)sizeof(char*));
    printf("sizeof(size_t) = %d\n", (int)sizeof(size_t));
    printf("sizeof(TokenType) = %d\n", (int)sizeof(TokenType));
    
    Token t;
    char *base = (char*)&t;
    long long off_type = (char*)&t.type - base;
    long long off_start = (char*)&t.start - base;
    long long off_length = (char*)&t.length - base;
    long long off_line = (char*)&t.line - base;
    printf("Token.type offset = %d\n", (int)off_type);
    printf("Token.start offset = %d\n", (int)off_start);
    printf("Token.length offset = %d\n", (int)off_length);
    printf("Token.line offset = %d\n", (int)off_line);
    return 0;
}
