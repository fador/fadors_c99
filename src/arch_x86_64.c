#include "arch_x86_64.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static FILE *out;
static int label_count = 0;
static CodegenSyntax current_syntax = SYNTAX_ATT;

static void gen_function(ASTNode *node);
static void gen_statement(ASTNode *node);

typedef enum {
    OP_REG,
    OP_IMM,
    OP_MEM,
    OP_LABEL
} OperandType;

typedef struct {
    OperandType type;
    union {
        const char *reg;
        int imm;
        struct {
            const char *base;
            int offset;
        } mem;
        const char *label;
    } data;
} Operand;

static void emit_label_def(const char *name) {
    if (current_syntax == SYNTAX_INTEL && name[0] == '.') {
        fprintf(out, "%s:\n", name + 1);
    } else {
        fprintf(out, "%s:\n", name);
    }
}

static Operand op_label(const char *label) {
    Operand op; 
    op.type = OP_LABEL; 
    if (current_syntax == SYNTAX_INTEL && label[0] == '.') {
        op.data.label = label + 1;
    } else {
        op.data.label = label; 
    }
    return op;
}

void arch_x86_64_init(FILE *output) {
    out = output;
    if (current_syntax == SYNTAX_INTEL) {
        fprintf(out, "_TEXT SEGMENT\n");
    }
}

void arch_x86_64_set_syntax(CodegenSyntax syntax) {
    current_syntax = syntax;
}

void arch_x86_64_generate(ASTNode *program) {
    for (size_t i = 0; i < program->children_count; i++) {
        ASTNode *child = program->children[i];
        if (child->type == AST_FUNCTION) {
            gen_function(child);
        }
    }
    if (current_syntax == SYNTAX_INTEL) {
        fprintf(out, "_TEXT ENDS\n");
        fprintf(out, "END\n");
    }
}

typedef struct {
    char *name;
    int offset;
    Type *type;
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

static Type *get_local_type(const char *name) {
    for (int i = 0; i < locals_count; i++) {
        if (strcmp(locals[i].name, name) == 0) {
            return locals[i].type;
        }
    }
    return NULL;
}


static Operand op_reg(const char *reg) {
    Operand op; op.type = OP_REG; op.data.reg = reg; return op;
}

static Operand op_imm(int imm) {
    Operand op; op.type = OP_IMM; op.data.imm = imm; return op;
}

static Operand op_mem(const char *base, int offset) {
    Operand op; op.type = OP_MEM; op.data.mem.base = base; op.data.mem.offset = offset; return op;
}


static void print_operand(Operand op) {
    if (op.type == OP_REG) {
        if (current_syntax == SYNTAX_ATT) fprintf(out, "%%%s", op.data.reg);
        else fprintf(out, "%s", op.data.reg);
    } else if (op.type == OP_IMM) {
        if (current_syntax == SYNTAX_ATT) fprintf(out, "$%d", op.data.imm);
        else fprintf(out, "%d", op.data.imm);
    } else if (op.type == OP_MEM) {
        if (current_syntax == SYNTAX_ATT) {
            if (op.data.mem.offset != 0) fprintf(out, "%d", op.data.mem.offset);
            fprintf(out, "(%%%s)", op.data.mem.base);
        } else {
            fprintf(out, "[%s", op.data.mem.base);
            if (op.data.mem.offset > 0) fprintf(out, "+%d", op.data.mem.offset);
            else if (op.data.mem.offset < 0) fprintf(out, "%d", op.data.mem.offset);
            fprintf(out, "]");
        }
    } else if (op.type == OP_LABEL) {
        fprintf(out, "%s", op.data.label);
    }
}

static void emit_inst0(const char *mnemonic) {
    const char *m = mnemonic;
    if (current_syntax == SYNTAX_INTEL) {
        if (strcmp(mnemonic, "cqto") == 0) m = "cqo";
        else if (strcmp(mnemonic, "leave") == 0) m = "leave";
        else if (strcmp(mnemonic, "ret") == 0) m = "ret";
    }
    fprintf(out, "    %s\n", m);
}

static void emit_inst1(const char *mnemonic, Operand op1) {
    const char *m = mnemonic;
    if (current_syntax == SYNTAX_INTEL) {
        if (strcmp(mnemonic, "idivq") == 0) m = "idiv";
        else if (strcmp(mnemonic, "pushq") == 0) m = "push";
        else if (strcmp(mnemonic, "popq") == 0) m = "pop";
        else if (strcmp(mnemonic, "call") == 0) m = "call";
        else if (strcmp(mnemonic, "jmp") == 0) m = "jmp";
        else if (strcmp(mnemonic, "je") == 0) m = "je";
        else if (strcmp(mnemonic, "sete") == 0) m = "sete"; 
        else if (strcmp(mnemonic, "setne") == 0) m = "setne";
        else if (strcmp(mnemonic, "setl") == 0) m = "setl";
        else if (strcmp(mnemonic, "setg") == 0) m = "setg";
        else if (strcmp(mnemonic, "setle") == 0) m = "setle";
        else if (strcmp(mnemonic, "setge") == 0) m = "setge";
        else if (strcmp(mnemonic, "neg") == 0) m = "neg";
    }

    fprintf(out, "    %s ", m);
    print_operand(op1);
    fprintf(out, "\n");
}

static void emit_inst2(const char *mnemonic, Operand src, Operand dest) {
    const char *m = mnemonic;
    if (current_syntax == SYNTAX_INTEL) {
        if (strcmp(mnemonic, "movq") == 0) m = "mov";
        else if (strcmp(mnemonic, "addq") == 0) m = "add";
        else if (strcmp(mnemonic, "subq") == 0) m = "sub";
        else if (strcmp(mnemonic, "imulq") == 0) m = "imul";
        else if (strcmp(mnemonic, "cmpq") == 0) m = "cmp";
        else if (strcmp(mnemonic, "leaq") == 0) m = "lea";
        else if (strcmp(mnemonic, "movzbq") == 0) m = "movzx";
    }

    fprintf(out, "    %s ", m);
    if (current_syntax == SYNTAX_ATT) {
        print_operand(src);
        fprintf(out, ", ");
        print_operand(dest);
    } else {
        print_operand(dest);
        fprintf(out, ", ");
        print_operand(src);
    }
    fprintf(out, "\n");
}


static Type *get_expr_type(ASTNode *node) {
    if (node->type == AST_IDENTIFIER) {
        return get_local_type(node->data.identifier.name);
    } else if (node->type == AST_DEREF) {
        Type *t = get_expr_type(node->data.unary.expression);
        return t ? t->data.ptr_to : NULL;
    } else if (node->type == AST_ADDR_OF) {
        Type *t = get_expr_type(node->data.unary.expression);
        return type_ptr(t);
    } else if (node->type == AST_MEMBER_ACCESS) {
        Type *st = get_expr_type(node->data.member_access.struct_expr);
        if (node->data.member_access.is_arrow && st && st->kind == TYPE_PTR) {
            st = st->data.ptr_to;
        }
        if (st && st->kind == TYPE_STRUCT) {
            for (int i = 0; i < st->data.struct_data.members_count; i++) {
                if (strcmp(st->data.struct_data.members[i].name, node->data.member_access.member_name) == 0) {
                    return st->data.struct_data.members[i].type;
                }
            }
        }
    }
    return NULL;
}

static void gen_expression(ASTNode *node);

static void gen_addr(ASTNode *node) {
    if (node->type == AST_IDENTIFIER) {
        int offset = get_local_offset(node->data.identifier.name);
        if (offset != 0) {
            emit_inst2("leaq", op_mem("rbp", offset), op_reg("rax"));
        }
    } else if (node->type == AST_DEREF) {
        gen_expression(node->data.unary.expression);
    } else if (node->type == AST_MEMBER_ACCESS) {
        Type *st = get_expr_type(node->data.member_access.struct_expr);
        if (node->data.member_access.is_arrow) {
            gen_expression(node->data.member_access.struct_expr);
            st = st->data.ptr_to;
        } else {
            gen_addr(node->data.member_access.struct_expr);
        }
        
        if (st && st->kind == TYPE_STRUCT) {
            for (int i = 0; i < st->data.struct_data.members_count; i++) {
                if (strcmp(st->data.struct_data.members[i].name, node->data.member_access.member_name) == 0) {
                    emit_inst2("addq", op_imm(st->data.struct_data.members[i].offset), op_reg("rax"));
                    break;
                }
            }
        }
    }
}

static void gen_binary_expr(ASTNode *node) {
    gen_expression(node->data.binary_expr.right);
    emit_inst1("pushq", op_reg("rax"));
    gen_expression(node->data.binary_expr.left);
    emit_inst1("popq", op_reg("rcx"));
    
    switch (node->data.binary_expr.op) {
        case TOKEN_PLUS:  emit_inst2("addq", op_reg("rcx"), op_reg("rax")); break;
        case TOKEN_MINUS: emit_inst2("subq", op_reg("rcx"), op_reg("rax")); break;
        case TOKEN_STAR:  emit_inst2("imulq", op_reg("rcx"), op_reg("rax")); break;
        case TOKEN_SLASH:
            emit_inst0("cqto");
            emit_inst1("idivq", op_reg("rcx"));
            break;
        case TOKEN_EQUAL_EQUAL:
            emit_inst2("cmpq", op_reg("rcx"), op_reg("rax"));
            emit_inst1("sete", op_reg("al"));
            emit_inst2("movzbq", op_reg("al"), op_reg("rax"));
            break;
        case TOKEN_BANG_EQUAL:
            emit_inst2("cmpq", op_reg("rcx"), op_reg("rax"));
            emit_inst1("setne", op_reg("al"));
            emit_inst2("movzbq", op_reg("al"), op_reg("rax"));
            break;
        case TOKEN_LESS:
            emit_inst2("cmpq", op_reg("rcx"), op_reg("rax"));
            emit_inst1("setl", op_reg("al"));
            emit_inst2("movzbq", op_reg("al"), op_reg("rax"));
            break;
        case TOKEN_GREATER:
            emit_inst2("cmpq", op_reg("rcx"), op_reg("rax"));
            emit_inst1("setg", op_reg("al"));
            emit_inst2("movzbq", op_reg("al"), op_reg("rax"));
            break;
        case TOKEN_LESS_EQUAL:
            emit_inst2("cmpq", op_reg("rcx"), op_reg("rax"));
            emit_inst1("setle", op_reg("al"));
            emit_inst2("movzbq", op_reg("al"), op_reg("rax"));
            break;
        case TOKEN_GREATER_EQUAL:
            emit_inst2("cmpq", op_reg("rcx"), op_reg("rax"));
            emit_inst1("setge", op_reg("al"));
            emit_inst2("movzbq", op_reg("al"), op_reg("rax"));
            break;
        default: break;
    }
}

static void gen_expression(ASTNode *node) {
    if (node->type == AST_INTEGER) {
        emit_inst2("movq", op_imm(node->data.integer.value), op_reg("rax"));
    } else if (node->type == AST_IDENTIFIER) {
        int offset = get_local_offset(node->data.identifier.name);
        if (offset != 0) {
            emit_inst2("movq", op_mem("rbp", offset), op_reg("rax"));
        }
    } else if (node->type == AST_BINARY_EXPR) {
        gen_binary_expr(node);
    } else if (node->type == AST_ASSIGN) {
        gen_expression(node->data.assign.value);
        if (node->data.assign.left->type == AST_IDENTIFIER) {
            int offset = get_local_offset(node->data.assign.left->data.identifier.name);
            if (offset != 0) {
                emit_inst2("movq", op_reg("rax"), op_mem("rbp", offset));
            }
        } else {
            emit_inst1("pushq", op_reg("rax"));
            gen_addr(node->data.assign.left);
            emit_inst1("popq", op_reg("rcx"));
            emit_inst2("movq", op_reg("rcx"), op_mem("rax", 0));
            emit_inst2("movq", op_reg("rcx"), op_reg("rax")); // Result is the value
        }
    } else if (node->type == AST_DEREF) {
        gen_expression(node->data.unary.expression);
        emit_inst2("movq", op_mem("rax", 0), op_reg("rax"));
    } else if (node->type == AST_ADDR_OF) {
        gen_addr(node->data.unary.expression);
    } else if (node->type == AST_MEMBER_ACCESS) {
        gen_addr(node);
        emit_inst2("movq", op_mem("rax", 0), op_reg("rax"));
    } else if (node->type == AST_CALL) {
        const char *arg_regs[] = {"rdi", "rsi", "rdx", "rcx", "r8", "r9"};
        for (size_t i = 0; i < node->children_count && i < 6; i++) {
            gen_expression(node->children[i]);
            emit_inst2("movq", op_reg("rax"), op_reg(arg_regs[i]));
        }
        // Align stack to 16 bytes for ABI compliance (Simplified)
        emit_inst1("call", op_label(node->data.call.name));
    }
}

static int current_function_end_label = 0;

static void gen_statement(ASTNode *node) {
    if (node->type == AST_RETURN) {
        if (node->data.return_stmt.expression) {
            gen_expression(node->data.return_stmt.expression);
        }
        char dest_label[32];
        sprintf(dest_label, ".Lend_%d", current_function_end_label);
        emit_inst1("jmp", op_label(dest_label));
    } else if (node->type == AST_VAR_DECL) {
        if (node->data.var_decl.initializer) {
            gen_expression(node->data.var_decl.initializer);
        } else {
            emit_inst2("movq", op_imm(0), op_reg("rax"));
        }
        
        int size = node->resolved_type ? node->resolved_type->size : 8;
        stack_offset -= size;
        
        locals[locals_count].name = node->data.var_decl.name;
        locals[locals_count].offset = stack_offset;
        locals[locals_count].type = node->resolved_type;
        locals_count++;
        
        if (node->resolved_type && node->resolved_type->kind == TYPE_STRUCT) {
            emit_inst2("subq", op_imm(size), op_reg("rsp"));
        } else {
            emit_inst1("pushq", op_reg("rax"));
        }
    } else if (node->type == AST_IF) {
        int label_else = label_count++;
        int label_end = label_count++;
        char l_else[32], l_end[32];
        sprintf(l_else, ".L%d", label_else);
        sprintf(l_end, ".L%d", label_end);
        
        gen_expression(node->data.if_stmt.condition);
        emit_inst2("cmpq", op_imm(0), op_reg("rax"));
        emit_inst1("je", op_label(l_else));
        
        gen_statement(node->data.if_stmt.then_branch);
        emit_inst1("jmp", op_label(l_end));
        
        emit_label_def(l_else);
        if (node->data.if_stmt.else_branch) {
            gen_statement(node->data.if_stmt.else_branch);
        }
        emit_label_def(l_end);
    } else if (node->type == AST_WHILE) {
        int label_start = label_count++;
        int label_end = label_count++;
        char l_start[32], l_end[32];
        sprintf(l_start, ".L%d", label_start);
        sprintf(l_end, ".L%d", label_end);
        
        emit_label_def(l_start);
        gen_expression(node->data.while_stmt.condition);
        emit_inst2("cmpq", op_imm(0), op_reg("rax"));
        emit_inst1("je", op_label(l_end));
        
        gen_statement(node->data.while_stmt.body);
        emit_inst1("jmp", op_label(l_start));
        
        emit_label_def(l_end);
    } else if (node->type == AST_BLOCK) {
        for (size_t i = 0; i < node->children_count; i++) {
            gen_statement(node->children[i]);
        }
    } else {
        gen_expression(node);
    }
}

static void gen_function(ASTNode *node) {
    current_function_end_label = label_count++;
    
    if (current_syntax == SYNTAX_ATT) {
        fprintf(out, ".globl %s\n", node->data.function.name);
        emit_label_def(node->data.function.name);
    } else {
        fprintf(out, "PUBLIC %s\n", node->data.function.name);
        fprintf(out, "%s PROC\n", node->data.function.name);
    }
    
    // Prologue
    emit_inst1("pushq", op_reg("rbp"));
    emit_inst2("movq", op_reg("rsp"), op_reg("rbp"));
    
    locals_count = 0;
    stack_offset = 0;
    
    gen_statement(node->data.function.body);
    
    // Epilogue label
    char label_buffer[32];
    sprintf(label_buffer, ".Lend_%d", current_function_end_label);
    emit_label_def(label_buffer);
    
    emit_inst0("leave");
    emit_inst0("ret");
    
    if (current_syntax == SYNTAX_INTEL) {
        fprintf(out, "%s ENDP\n", node->data.function.name);
    }
}
