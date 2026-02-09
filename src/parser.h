#ifndef PARSER_H
#define PARSER_H

#include "lexer.h"
#include "ast.h"

typedef struct {
    char *name;
    Type *type;
} TypedefEntry;

typedef struct {
    char *name;
    int value;
} EnumConstant;

typedef struct {
    Lexer *lexer;
    Token current_token;
    TypedefEntry typedefs[200];
    int typedefs_count;
    EnumConstant enum_constants[200];
    int enum_constants_count;
    Type *structs[100];
    int structs_count;
    
    struct {
        char *name;
        struct Type *type;
    } locals[100];
    int locals_count;

    struct {
        char *name;
        struct Type *type;
    } globals[100];
    int globals_count;
} Parser;

void parser_init(Parser *parser, Lexer *lexer);
ASTNode *parser_parse(Parser *parser);

#endif // PARSER_H
