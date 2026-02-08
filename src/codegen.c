#include "codegen.h"
#include <stdio.h>
#include <stdlib.h>

static FILE *out;

void codegen_init(FILE *output) {
    out = output;
}

static void gen_expression(ASTNode *node) {
    if (node->type == AST_INTEGER) {
        fprintf(out, "    movq $%d, %%rax\n", node->data.integer.value);
    }
}

static void gen_statement(ASTNode *node) {
    if (node->type == AST_RETURN) {
        gen_expression(node->data.return_stmt.expression);
        fprintf(out, "    popq %%rbp\n");
        fprintf(out, "    ret\n");
    }
}

static void gen_block(ASTNode *node) {
    for (size_t i = 0; i < node->children_count; i++) {
        gen_statement(node->children[i]);
    }
}

static void gen_function(ASTNode *node) {
    fprintf(out, ".globl %s\n", node->data.function.name);
    fprintf(out, "%s:\n", node->data.function.name);
    
    // Prologue
    fprintf(out, "    pushq %%rbp\n");
    fprintf(out, "    movq %%rsp, %%rbp\n");
    
    gen_block(node->data.function.body);
    
    // Epilogue (in case there's no return statement) (Not implemented yet, assume return exists)
    // fprintf(out, "    popq %%rbp\n");
    // fprintf(out, "    ret\n");
}

void codegen_generate(ASTNode *program) {
    for (size_t i = 0; i < program->children_count; i++) {
        ASTNode *child = program->children[i];
        if (child->type == AST_FUNCTION) {
            gen_function(child);
        }
    }
}
