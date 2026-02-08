#ifndef AST_H
#define AST_H

#include <stddef.h>
#include "lexer.h"

typedef enum {
    AST_PROGRAM,
    AST_FUNCTION,
    AST_BLOCK,
    AST_RETURN,
    AST_INTEGER,
    AST_IDENTIFIER,
    AST_BINARY_EXPR,
    AST_VAR_DECL,
    AST_ASSIGN,
    AST_IF,
    AST_WHILE,
    AST_CALL,
    AST_UNKNOWN
} ASTNodeType;

typedef struct ASTNode {
    ASTNodeType type;
    struct ASTNode **children;
    size_t children_count;
    
    // For specific nodes
    union {
        struct {
            char *name;
            struct ASTNode *body;
        } function;
        struct {
            int value;
        } integer;
        struct {
            char *name;
        } identifier;
        struct {
            struct ASTNode *body;
        } program;
        struct {
            struct ASTNode *expression;
        } return_stmt;
        struct {
            TokenType op;
            struct ASTNode *left;
            struct ASTNode *right;
        } binary_expr;
        struct {
            char *name;
            struct ASTNode *initializer;
        } var_decl;
        struct {
            char *name;
            struct ASTNode *value;
        } assign;
        struct {
            struct ASTNode *condition;
            struct ASTNode *then_branch;
            struct ASTNode *else_branch;
        } if_stmt;
        struct {
            struct ASTNode *condition;
            struct ASTNode *body;
        } while_stmt;
        struct {
            char *name;
            // args will be in children
        } call;
    } data;
} ASTNode;

ASTNode *ast_create_node(ASTNodeType type);
void ast_add_child(ASTNode *parent, ASTNode *child);
void ast_print(ASTNode *node, int indent);

#endif // AST_H
