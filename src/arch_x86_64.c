#define _CRT_SECURE_NO_WARNINGS
#include "arch_x86_64.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "encoder.h"

typedef enum {
    SECTION_TEXT,
    SECTION_DATA
} Section;

static FILE *out;
static COFFWriter *obj_writer = NULL;
static int label_count = 0;
static CodegenSyntax current_syntax = SYNTAX_ATT;
static Section current_section = SECTION_TEXT;

static void gen_function(ASTNode *node);
static void gen_statement(ASTNode *node);

typedef struct {
    char *label;
    char *value;
} StringLiteral;

static StringLiteral string_literals[100];
static int string_literals_count = 0;

void arch_x86_64_set_writer(COFFWriter *writer) {
    obj_writer = writer;
}

static void emit_label_def(const char *name) {
    if (obj_writer) {
        uint8_t storage_class = IMAGE_SYM_CLASS_EXTERNAL;
        if (name[0] == '.') storage_class = IMAGE_SYM_CLASS_STATIC;
        
        int16_t section_num = (current_section == SECTION_TEXT) ? 1 : 2;
        uint32_t offset = (current_section == SECTION_TEXT) ? (uint32_t)obj_writer->text_section.size : (uint32_t)obj_writer->data_section.size;
        
        coff_writer_add_symbol(obj_writer, name, offset, section_num, storage_class);
        return;
    }
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
    if (out && !obj_writer && current_syntax == SYNTAX_INTEL) {
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
    if (!obj_writer && current_syntax == SYNTAX_INTEL) {
        if (string_literals_count > 0) {
            fprintf(out, "_TEXT ENDS\n");
            fprintf(out, "_DATA SEGMENT\n");
            for (int i = 0; i < string_literals_count; i++) {
                if (string_literals[i].label[0] == '.') {
                    fprintf(out, "%s:\n", string_literals[i].label + 1);
                } else {
                    fprintf(out, "%s:\n", string_literals[i].label);
                }
                fprintf(out, "    DB \"%s\", 0\n", string_literals[i].value);
            }
            fprintf(out, "_DATA ENDS\n");
            fprintf(out, "END\n");
        } else {
            fprintf(out, "_TEXT ENDS\n");
            fprintf(out, "END\n");
        }
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
    if (!out) return;
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
    if (obj_writer) {
        encode_inst0(&obj_writer->text_section, mnemonic);
        return;
    }
    const char *m = mnemonic;
    if (current_syntax == SYNTAX_INTEL) {
        if (strcmp(mnemonic, "cqto") == 0) m = "cqo";
        else if (strcmp(mnemonic, "leave") == 0) m = "leave";
        else if (strcmp(mnemonic, "ret") == 0) m = "ret";
    }
    fprintf(out, "    %s\n", m);
}

static void emit_inst1(const char *mnemonic, Operand op1) {
    if (obj_writer) {
        if (op1.type == OP_LABEL) {
            // Relocation for JMP/JE/etc starts AFTER the opcode
            uint32_t offset = 1;
            if (strcmp(mnemonic, "je") == 0 || strcmp(mnemonic, "jne") == 0) offset = 2; // 0F 84 / 0F 85
            else if (strcmp(mnemonic, "call") == 0) offset = 1; // E8
            
            uint32_t sym_idx = coff_writer_add_symbol(obj_writer, op1.data.label, 0, 0, IMAGE_SYM_CLASS_STATIC);
            coff_writer_add_reloc(obj_writer, (uint32_t)obj_writer->text_section.size + offset, sym_idx, IMAGE_REL_AMD64_REL32);
        }
        encode_inst1(&obj_writer->text_section, mnemonic, op1);
        return;
    }
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
    if (obj_writer) {
        if (src.type == OP_LABEL) {
            // LEA label, reg -> LEA label(%RIP), reg
            // Relocation starts at offset 3 (REX + 8D + ModRM)
            uint32_t sym_idx = coff_writer_add_symbol(obj_writer, src.data.label, 0, 0, IMAGE_SYM_CLASS_STATIC);
            coff_writer_add_reloc(obj_writer, (uint32_t)obj_writer->text_section.size + 3, sym_idx, IMAGE_REL_AMD64_REL32);
        }
        encode_inst2(&obj_writer->text_section, mnemonic, src, dest);
        return;
    }
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
        if (strcmp(mnemonic, "movzbq") == 0 && src.type == OP_MEM) {
            fprintf(out, "byte ptr ");
        }
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
            node->resolved_type = type_ptr(get_local_type(node->data.identifier.name));
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
            node->resolved_type = get_local_type(node->data.identifier.name);
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
        Type *t = node->data.unary.expression->resolved_type;
        if (t && t->kind == TYPE_PTR && t->data.ptr_to->kind == TYPE_CHAR) {
            emit_inst2("movzbq", op_mem("rax", 0), op_reg("rax"));
            node->resolved_type = t->data.ptr_to;
        } else {
            emit_inst2("movq", op_mem("rax", 0), op_reg("rax"));
            if (t && t->kind == TYPE_PTR) node->resolved_type = t->data.ptr_to;
        }
    } else if (node->type == AST_ADDR_OF) {
        gen_addr(node->data.unary.expression);
    } else if (node->type == AST_MEMBER_ACCESS) {
        gen_addr(node);
        emit_inst2("movq", op_mem("rax", 0), op_reg("rax"));
    } else if (node->type == AST_CALL) {
        const char *arg_regs[] = {"rcx", "rdx", "r8", "r9"};
        for (size_t i = 0; i < node->children_count; i++) {
            gen_expression(node->children[i]);
            emit_inst1("pushq", op_reg("rax"));
        }
        
        for (int i = (int)node->children_count - 1; i >= 0; i--) {
            if (i < 4) {
                emit_inst1("popq", op_reg(arg_regs[i]));
            } else {
                // For i >= 4, arguments are already on stack.
                // But they are below the shadow space if we just sub RSP, 32.
                // For now, only 4 args are fully supported.
            }
        }
        
        // Win64 requires 32 bytes of shadow space
        emit_inst2("subq", op_imm(32), op_reg("rsp"));
        emit_inst1("call", op_label(node->data.call.name));
        emit_inst2("addq", op_imm(32), op_reg("rsp"));
    } else if (node->type == AST_STRING) {
        char label[32];
        sprintf(label, ".LC%d", label_count++);
        
        if (obj_writer) {
            Section old_section = current_section;
            current_section = SECTION_DATA;
            emit_label_def(label);
            buffer_write_bytes(&obj_writer->data_section, node->data.string.value, strlen(node->data.string.value) + 1);
            current_section = old_section;
        } else {
            string_literals[string_literals_count].label = strdup(label);
            string_literals[string_literals_count].value = strdup(node->data.string.value);
            string_literals_count++;
        }
        
        // Load address of the string
        emit_inst2("leaq", op_label(label), op_reg("rax"));
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
    if (node->data.function.body == NULL) {
        if (obj_writer) {
            coff_writer_add_symbol(obj_writer, node->data.function.name, 0, 0, IMAGE_SYM_CLASS_EXTERNAL);
        } else if (current_syntax == SYNTAX_INTEL) {
            fprintf(out, "EXTERN %s:PROC\n", node->data.function.name);
        } else {
            fprintf(out, ".extern %s\n", node->data.function.name);
        }
        return;
    }
    current_function_end_label = label_count++;
    
    if (current_syntax == SYNTAX_ATT) {
        if (out) fprintf(out, ".globl %s\n", node->data.function.name);
        emit_label_def(node->data.function.name);
    } else {
        if (out) {
            fprintf(out, "PUBLIC %s\n", node->data.function.name);
            fprintf(out, "%s PROC\n", node->data.function.name);
        }
    }
    
    // Prologue
    emit_inst1("pushq", op_reg("rbp"));
    emit_inst2("movq", op_reg("rsp"), op_reg("rbp"));
    
    locals_count = 0;
    stack_offset = 0;
    
    // Handle parameters (Win64 ABI)
    const char *arg_regs[] = {"rcx", "rdx", "r8", "r9"};
    for (size_t i = 0; i < node->children_count; i++) {
        ASTNode *param = node->children[i];
        if (param->type == AST_VAR_DECL) {
            int size = param->resolved_type ? param->resolved_type->size : 8;
            stack_offset -= size;
            
            locals[locals_count].name = param->data.var_decl.name;
            locals[locals_count].offset = stack_offset;
            locals[locals_count].type = param->resolved_type;
            locals_count++;
            
            if (i < 4U) {
                emit_inst1("pushq", op_reg(arg_regs[i]));
            }
        }
    }
    
    gen_statement(node->data.function.body);
    
    // Epilogue label
    char label_buffer[32];
    sprintf(label_buffer, ".Lend_%d", current_function_end_label);
    emit_label_def(label_buffer);
    
    emit_inst0("leave");
    emit_inst0("ret");
    
    if (out && current_syntax == SYNTAX_INTEL) {
        fprintf(out, "%s ENDP\n", node->data.function.name);
    }
}
