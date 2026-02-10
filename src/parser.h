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
    TypedefEntry typedefs[1024];
    int typedefs_count;
    EnumConstant enum_constants[1024];
    int enum_constants_count;
    Type *structs[1024];
    int structs_count;
    
    struct {
        char *name;
        struct Type *type;
    } locals[256];
    int locals_count;

    struct {
        char *name;
        struct Type *type;
    } globals[1024];
    int globals_count;

    int packing_stack[16];
    int packing_ptr;
} Parser;

void parser_init(Parser *parser, Lexer *lexer);
ASTNode *parser_parse(Parser *parser);

#endif // PARSER_H
