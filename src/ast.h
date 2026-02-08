#ifndef AST_H
#define AST_H

#include <stddef.h>

typedef enum {
    AST_PROGRAM,
    AST_FUNCTION,
    AST_BLOCK,
    AST_RETURN,
    AST_INTEGER,
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
            struct ASTNode *body;
        } program;
        struct {
            struct ASTNode *expression;
        } return_stmt;
    } data;
} ASTNode;

ASTNode *ast_create_node(ASTNodeType type);
void ast_add_child(ASTNode *parent, ASTNode *child);
void ast_print(ASTNode *node, int indent);

#endif // AST_H
