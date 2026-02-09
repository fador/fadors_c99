#include "ast.h"
#include <stdlib.h>
#include <stdio.h>

ASTNode *ast_create_node(ASTNodeType type) {
    ASTNode *node = (ASTNode *)malloc(sizeof(ASTNode));
    if (node) {
        node->type = type;
        node->children = NULL;
        node->children_count = 0;
        node->resolved_type = NULL;
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
        case AST_IDENTIFIER: printf("Identifier: %s\n", node->data.identifier.name); break;
        case AST_BINARY_EXPR: printf("BinaryExpr (op: %d)\n", node->data.binary_expr.op); break;
        case AST_VAR_DECL: printf("VarDecl: %s\n", node->data.var_decl.name); break;
        case AST_ASSIGN: printf("Assign\n"); break;
        case AST_IF: printf("If\n"); break;
        case AST_WHILE: printf("While\n"); break;
        case AST_CALL: printf("Call: %s\n", node->data.call.name); break;
        case AST_STRUCT_DEF: printf("StructDef: %s\n", node->data.struct_def.name); break;
        case AST_MEMBER_ACCESS: printf("MemberAccess: %s%s\n", node->data.member_access.is_arrow ? "->" : ".", node->data.member_access.member_name); break;
        case AST_DEREF: printf("Deref\n"); break;
        case AST_ADDR_OF: printf("AddrOf\n"); break;
        case AST_STRING: printf("String: \"%s\"\n", node->data.string.value); break;
        case AST_NEG: printf("Neg\n"); break;
        case AST_NOT: printf("Not\n"); break;
        case AST_PRE_INC: printf("PreInc\n"); break;
        case AST_PRE_DEC: printf("PreDec\n"); break;
        case AST_POST_INC: printf("PostInc\n"); break;
        case AST_POST_DEC: printf("PostDec\n"); break;
        case AST_CAST: printf("Cast\n"); break;
        case AST_UNKNOWN: printf("Unknown\n"); break;
    }
    
    if (node->type == AST_DEREF || node->type == AST_ADDR_OF || node->type == AST_NEG || node->type == AST_NOT ||
        node->type == AST_PRE_INC || node->type == AST_PRE_DEC || node->type == AST_POST_INC || node->type == AST_POST_DEC ||
        node->type == AST_CAST) {
        if (node->type == AST_CAST) ast_print(node->data.cast.expression, indent + 1);
        else ast_print(node->data.unary.expression, indent + 1);
    }
    
    if (node->type == AST_ASSIGN) {
        ast_print(node->data.assign.left, indent + 1);
        ast_print(node->data.assign.value, indent + 1);
    }
    
    for (size_t i = 0; i < node->children_count; i++) {
        ast_print(node->children[i], indent + 1);
    }
}
