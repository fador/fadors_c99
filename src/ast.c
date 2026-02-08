#include "ast.h"
#include <stdlib.h>
#include <stdio.h>

ASTNode *ast_create_node(ASTNodeType type) {
    ASTNode *node = (ASTNode *)malloc(sizeof(ASTNode));
    if (node) {
        node->type = type;
        node->children = NULL;
        node->children_count = 0;
    }
    return node;
}

void ast_add_child(ASTNode *parent, ASTNode *child) {
    if (!parent || !child) return;
    
    parent->children_count++;
    parent->children = (ASTNode **)realloc(parent->children, sizeof(ASTNode *) * parent->children_count);
    parent->children[parent->children_count - 1] = child;
}

void ast_print(ASTNode *node, int indent) {
    if (!node) return;
    
    for (int i = 0; i < indent; i++) printf("  ");
    
    switch (node->type) {
        case AST_PROGRAM: printf("Program\n"); break;
        case AST_FUNCTION: printf("Function: %s\n", node->data.function.name); break;
        case AST_BLOCK: printf("Block\n"); break;
        case AST_RETURN: printf("Return\n"); break;
        case AST_INTEGER: printf("Integer: %d\n", node->data.integer.value); break;
        case AST_UNKNOWN: printf("Unknown\n"); break;
    }
    
    for (size_t i = 0; i < node->children_count; i++) {
        ast_print(node->children[i], indent + 1);
    }
}
