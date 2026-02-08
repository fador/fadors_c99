#include "codegen.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static FILE *out;
static int label_count = 0;

typedef struct {
    char *name;
    int offset;
} LocalVar;

static LocalVar locals[100];
static int locals_count = 0;
static int stack_offset = 0;

static int get_local_offset(const char *name) {
    for (int i = 0; i < locals_count; i++) {
        if (strcmp(locals[i].name, name) == 0) {
            return locals[i].offset;
        }
    }
    return 0;
}

void codegen_init(FILE *output) {
    out = output;
}

static void gen_expression(ASTNode *node);

static void gen_binary_expr(ASTNode *node) {
    gen_expression(node->data.binary_expr.right);
    fprintf(out, "    pushq %%rax\n");
    gen_expression(node->data.binary_expr.left);
    fprintf(out, "    popq %%rcx\n");
    
    switch (node->data.binary_expr.op) {
        case TOKEN_PLUS:  fprintf(out, "    addq %%rcx, %%rax\n"); break;
        case TOKEN_MINUS: fprintf(out, "    subq %%rcx, %%rax\n"); break;
        case TOKEN_STAR:  fprintf(out, "    imulq %%rcx, %%rax\n"); break;
        case TOKEN_SLASH:
            fprintf(out, "    cqto\n");
            fprintf(out, "    idivq %%rcx\n");
            break;
        case TOKEN_EQUAL_EQUAL:
            fprintf(out, "    cmpq %%rcx, %%rax\n");
            fprintf(out, "    sete %%al\n");
            fprintf(out, "    movzbq %%al, %%rax\n");
            break;
        case TOKEN_BANG_EQUAL:
            fprintf(out, "    cmpq %%rcx, %%rax\n");
            fprintf(out, "    setne %%al\n");
            fprintf(out, "    movzbq %%al, %%rax\n");
            break;
        case TOKEN_LESS:
            fprintf(out, "    cmpq %%rcx, %%rax\n");
            fprintf(out, "    setl %%al\n");
            fprintf(out, "    movzbq %%al, %%rax\n");
            break;
        case TOKEN_GREATER:
            fprintf(out, "    cmpq %%rcx, %%rax\n");
            fprintf(out, "    setg %%al\n");
            fprintf(out, "    movzbq %%al, %%rax\n");
            break;
        case TOKEN_LESS_EQUAL:
            fprintf(out, "    cmpq %%rcx, %%rax\n");
            fprintf(out, "    setle %%al\n");
            fprintf(out, "    movzbq %%al, %%rax\n");
            break;
        case TOKEN_GREATER_EQUAL:
            fprintf(out, "    cmpq %%rcx, %%rax\n");
            fprintf(out, "    setge %%al\n");
            fprintf(out, "    movzbq %%al, %%rax\n");
            break;
        default: break;
    }
}

static void gen_expression(ASTNode *node) {
    if (node->type == AST_INTEGER) {
        fprintf(out, "    movq $%d, %%rax\n", node->data.integer.value);
    } else if (node->type == AST_IDENTIFIER) {
        int offset = get_local_offset(node->data.identifier.name);
        if (offset != 0) {
            fprintf(out, "    movq %d(%%rbp), %%rax\n", offset);
        }
    } else if (node->type == AST_BINARY_EXPR) {
        gen_binary_expr(node);
    } else if (node->type == AST_ASSIGN) {
        gen_expression(node->data.assign.value);
        int offset = get_local_offset(node->data.assign.name);
        if (offset != 0) {
            fprintf(out, "    movq %%rax, %d(%%rbp)\n", offset);
        }
    } else if (node->type == AST_CALL) {
        const char *arg_regs[] = {"%rdi", "%rsi", "%rdx", "%rcx", "%r8", "%r9"};
        for (size_t i = 0; i < node->children_count && i < 6; i++) {
            gen_expression(node->children[i]);
            fprintf(out, "    movq %%rax, %s\n", arg_regs[i]);
        }
        // Align stack to 16 bytes for ABI compliance (Simplified)
        fprintf(out, "    call %s\n", node->data.call.name);
    }
}

static void gen_statement(ASTNode *node) {
    if (node->type == AST_RETURN) {
        gen_expression(node->data.return_stmt.expression);
        fprintf(out, "    leave\n");
        fprintf(out, "    ret\n");
    } else if (node->type == AST_VAR_DECL) {
        if (node->data.var_decl.initializer) {
            gen_expression(node->data.var_decl.initializer);
        } else {
            fprintf(out, "    movq $0, %%rax\n");
        }
        stack_offset -= 8;
        locals[locals_count].name = node->data.var_decl.name;
        locals[locals_count].offset = stack_offset;
        locals_count++;
        fprintf(out, "    pushq %%rax\n");
    } else if (node->type == AST_IF) {
        int label_else = label_count++;
        int label_end = label_count++;
        
        gen_expression(node->data.if_stmt.condition);
        fprintf(out, "    cmpq $0, %%rax\n");
        fprintf(out, "    je .L%d\n", label_else);
        
        gen_statement(node->data.if_stmt.then_branch);
        fprintf(out, "    jmp .L%d\n", label_end);
        
        fprintf(out, ".L%d:\n", label_else);
        if (node->data.if_stmt.else_branch) {
            gen_statement(node->data.if_stmt.else_branch);
        }
        fprintf(out, ".L%d:\n", label_end);
    } else if (node->type == AST_WHILE) {
        int label_start = label_count++;
        int label_end = label_count++;
        
        fprintf(out, ".L%d:\n", label_start);
        gen_expression(node->data.while_stmt.condition);
        fprintf(out, "    cmpq $0, %%rax\n");
        fprintf(out, "    je .L%d\n", label_end);
        
        gen_statement(node->data.while_stmt.body);
        fprintf(out, "    jmp .L%d\n", label_start);
        
        fprintf(out, ".L%d:\n", label_end);
    } else if (node->type == AST_BLOCK) {
        for (size_t i = 0; i < node->children_count; i++) {
            gen_statement(node->children[i]);
        }
    } else {
        gen_expression(node);
    }
}

static void gen_function(ASTNode *node) {
    fprintf(out, ".globl %s\n", node->data.function.name);
    fprintf(out, "%s:\n", node->data.function.name);
    
    // Prologue
    fprintf(out, "    pushq %%rbp\n");
    fprintf(out, "    movq %%rsp, %%rbp\n");
    
    locals_count = 0;
    stack_offset = 0;
    
    gen_statement(node->data.function.body);
    
    // Default epilogue
    fprintf(out, "    leave\n");
    fprintf(out, "    ret\n");
}

void codegen_generate(ASTNode *program) {
    for (size_t i = 0; i < program->children_count; i++) {
        ASTNode *child = program->children[i];
        if (child->type == AST_FUNCTION) {
            gen_function(child);
        }
    }
}
