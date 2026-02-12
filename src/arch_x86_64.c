#define _CRT_SECURE_NO_WARNINGS
#include "arch_x86_64.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _MSC_VER
#define _strdup strdup
#endif

#include "encoder.h"

typedef enum {
    SECTION_TEXT,
    SECTION_DATA
} Section;

static FILE *out;
static COFFWriter *obj_writer = NULL;
static ASTNode *current_program = NULL;
static int label_count = 0;
static CodegenSyntax current_syntax = SYNTAX_ATT;
static Section current_section = SECTION_TEXT;
static Type *current_func_return_type = NULL;
static char *current_func_name = NULL;
static int static_label_count = 0;

// ABI register parameter arrays
static const char *g_arg_regs[6];
static const char *g_xmm_arg_regs[8];
static int g_max_reg_args = 4;       // 4 for Win64, 6 for SysV
static int g_use_shadow_space = 1;   // 1 for Win64, 0 for SysV

static void gen_function(ASTNode *node);
static void gen_statement(ASTNode *node);
static void gen_global_decl(ASTNode *node);
static void emit_inst0(const char *mnemonic);
static void emit_inst1(const char *mnemonic, Operand *op1);
static void emit_inst2(const char *mnemonic, Operand *op1, Operand *op2);
static Type *get_expr_type(ASTNode *node);
static int is_float_type(Type *t);

static Operand _op_pool[16];
static int _op_idx = 0;

static Operand *op_reg(const char *reg) {
    Operand *op = &_op_pool[_op_idx++ & 15];
    op->type = OP_REG; op->data.reg = reg; return op;
}
static Operand *op_imm(int imm) {
    Operand *op = &_op_pool[_op_idx++ & 15];
    op->type = OP_IMM; op->data.imm = imm; return op;
}
static Operand *op_mem(const char *base, int offset) {
    Operand *op = &_op_pool[_op_idx++ & 15];
    op->type = OP_MEM; op->data.mem.base = base; op->data.mem.offset = offset; return op;
}
static Operand *op_label(const char *label) {
    Operand *op = &_op_pool[_op_idx++ & 15];
    op->type = OP_LABEL;
    if (current_syntax == SYNTAX_INTEL && label[0] == '.') {
        op->data.label = label + 1;
    } else {
        op->data.label = label;
    }
    return op;
}

typedef struct {
    char *label;
    char *value;
    int length;
} StringLiteral;

static StringLiteral string_literals[8192];
static int string_literals_count = 0;

void arch_x86_64_set_writer(COFFWriter *writer) {
    obj_writer = writer;
    encoder_set_writer(writer);
}

static void emit_label_def(const char *name) {
    if (obj_writer) {
        uint8_t storage_class = IMAGE_SYM_CLASS_EXTERNAL;
        if (name[0] == '.') storage_class = IMAGE_SYM_CLASS_STATIC;
        
        int16_t section_num;
        if (current_section == SECTION_TEXT) section_num = 1; else section_num = 2;
        uint32_t offset;
        if (current_section == SECTION_TEXT) offset = (uint32_t)obj_writer->text_section.size; else offset = (uint32_t)obj_writer->data_section.size;
        
        uint16_t type = 0;
        if (current_section == SECTION_TEXT && storage_class == IMAGE_SYM_CLASS_EXTERNAL) type = 0x20;
        
        coff_writer_add_symbol(obj_writer, name, offset, section_num, type, storage_class);
        return;
    }
    if (current_syntax == SYNTAX_INTEL && name[0] == '.') {
        fprintf(out, "%s:\n", name + 1);
    } else {
        fprintf(out, "%s:\n", name);
    }
}

// op_label is now defined above with the other op_* functions

void arch_x86_64_init(FILE *output) {
    out = output;
#ifdef _WIN32
    // Win64 ABI
    g_arg_regs[0] = "rcx";
    g_arg_regs[1] = "rdx";
    g_arg_regs[2] = "r8";
    g_arg_regs[3] = "r9";
    g_xmm_arg_regs[0] = "xmm0";
    g_xmm_arg_regs[1] = "xmm1";
    g_xmm_arg_regs[2] = "xmm2";
    g_xmm_arg_regs[3] = "xmm3";
    g_max_reg_args = 4;
    g_use_shadow_space = 1;
#else
    // System V AMD64 ABI (Linux/macOS)
    g_arg_regs[0] = "rdi";
    g_arg_regs[1] = "rsi";
    g_arg_regs[2] = "rdx";
    g_arg_regs[3] = "rcx";
    g_arg_regs[4] = "r8";
    g_arg_regs[5] = "r9";
    g_xmm_arg_regs[0] = "xmm0";
    g_xmm_arg_regs[1] = "xmm1";
    g_xmm_arg_regs[2] = "xmm2";
    g_xmm_arg_regs[3] = "xmm3";
    g_xmm_arg_regs[4] = "xmm4";
    g_xmm_arg_regs[5] = "xmm5";
    g_xmm_arg_regs[6] = "xmm6";
    g_xmm_arg_regs[7] = "xmm7";
    g_max_reg_args = 6;
    g_use_shadow_space = 0;
#endif
    if (out && !obj_writer && current_syntax == SYNTAX_INTEL) {
        fprintf(out, "_TEXT SEGMENT\n");
    }
}

void arch_x86_64_set_syntax(CodegenSyntax syntax) {
    current_syntax = syntax;
}

void arch_x86_64_generate(ASTNode *program) {
    current_program = program;
    for (size_t i = 0; i < program->children_count; i++) {
        ASTNode *child = program->children[i];
        if (child->type == AST_FUNCTION) {
            gen_function(child);
        } else if (child->type == AST_VAR_DECL) {
            gen_global_decl(child);
        }
    }
    if (obj_writer) {
         // Emit string literals for COFF
         Section old_section = current_section;
         current_section = SECTION_DATA;
         
         for (int i = 0; i < string_literals_count; i++) {
             // Define symbol for the label
             // Labels like .Lstr0 are static
             uint32_t offset = (uint32_t)obj_writer->data_section.size;
             coff_writer_add_symbol(obj_writer, string_literals[i].label, offset, 2 /* .data */, 0, IMAGE_SYM_CLASS_STATIC);
             
             // Write string data
             buffer_write_bytes(&obj_writer->data_section, string_literals[i].value, string_literals[i].length);
             uint8_t zero = 0;
             buffer_write_bytes(&obj_writer->data_section, &zero, 1);
         }
         current_section = old_section;
    } else {
        if (string_literals_count > 0) {
            if (current_syntax == SYNTAX_INTEL) {
                fprintf(out, "_TEXT ENDS\n_DATA SEGMENT\n");
                for (int i = 0; i < string_literals_count; i++) {
                    const char *label = string_literals[i].label;
                    if (label[0] == '.') fprintf(out, "%s:\n", label + 1);
                    else fprintf(out, "%s:\n", label);
                    for (int j = 0; j < string_literals[i].length; j++) {
                        fprintf(out, "    DB %d\n", (unsigned char)string_literals[i].value[j]);
                    }
                    fprintf(out, "    DB 0\n");
                }
                fprintf(out, "_DATA ENDS\nEND\n");
            } else {
                fprintf(out, ".data\n");
                for (int i = 0; i < string_literals_count; i++) {
                    fprintf(out, "%s:\n", string_literals[i].label);
                    for (int j = 0; j < string_literals[i].length; j++) {
                        fprintf(out, "    .byte %d\n", (unsigned char)string_literals[i].value[j]);
                    }
                    fprintf(out, "    .byte 0\n");
                }
                fprintf(out, ".text\n");
            }
        } else if (current_syntax == SYNTAX_INTEL) {
            fprintf(out, "_TEXT ENDS\nEND\n");
        }
        // Emit GNU-stack note to prevent executable stack warning on Linux
        if (out && current_syntax == SYNTAX_ATT) {
            fprintf(out, ".section .note.GNU-stack,\"\",@progbits\n");
        }
    }
}

typedef struct {
    char *name;
    int offset;
    char *label;
    Type *type;
} LocalVar;

static LocalVar locals[8192];
static int locals_count = 0;
static int stack_offset = 0;

static int get_local_offset(const char *name) {
    if (!name) return 0;
    for (int i = locals_count - 1; i >= 0; i--) {
        if (locals[i].name && strcmp(locals[i].name, name) == 0) {
            if (locals[i].label) return 0; // It's static, no stack offset
            return locals[i].offset;
        }
    }
    return 0;
}

static const char *get_local_label(const char *name) {
    if (!name) return NULL;
    for (int i = locals_count - 1; i >= 0; i--) {
        if (locals[i].name && strcmp(locals[i].name, name) == 0) {
            return locals[i].label;
        }
    }
    return NULL;
}

static Type *get_local_type(const char *name) {
    if (!name) return NULL;
    for (int i = locals_count - 1; i >= 0; i--) {
        if (locals[i].name && strcmp(locals[i].name, name) == 0) {
            return locals[i].type;
        }
    }
    return NULL;
}

typedef struct {
    char *name;
    Type *type;
} GlobalVar;

static GlobalVar globals[8192];
static int globals_count = 0;

static Type *get_global_type(const char *name) {
    for (int i = 0; i < globals_count; i++) {
        if (globals[i].name && name && strcmp(globals[i].name, name) == 0) {
            return globals[i].type;
        }
    }
    return NULL;
}

static void gen_global_decl(ASTNode *node) {
    if (globals_count >= 8192) { fprintf(stderr, "Error: Too many globals\n"); exit(1); }
    globals[globals_count].name = node->data.var_decl.name;
    globals[globals_count].type = node->resolved_type;
    globals_count++;

    if (node->data.var_decl.is_extern) return;

    if (obj_writer) {
        // Switch to .data section
        Section old_section = current_section;
        current_section = SECTION_DATA;
        
        // Define symbol
        uint32_t offset = (uint32_t)obj_writer->data_section.size;
        int16_t section_num = 2; // .data
        uint8_t storage_class = node->data.var_decl.is_static ? IMAGE_SYM_CLASS_STATIC : IMAGE_SYM_CLASS_EXTERNAL;
        
        coff_writer_add_symbol(obj_writer, node->data.var_decl.name, offset, section_num, 0, storage_class);
        
        // Write initial value
        int size = node->resolved_type ? node->resolved_type->size : 4;
        ASTNode *init_node = node->data.var_decl.initializer;
        if (init_node && init_node->type == AST_INTEGER) {
            int val = init_node->data.integer.value;
            buffer_write_bytes(&obj_writer->data_section, &val, size);
        } else if (init_node && init_node->type == AST_FLOAT) {
            double val = init_node->data.float_val.value;
            if (size == 4) {
                float f = (float)val;
                buffer_write_bytes(&obj_writer->data_section, &f, 4);
            } else {
                buffer_write_bytes(&obj_writer->data_section, &val, 8);
            }
        } else {
            ASTNode *init = node->data.var_decl.initializer;
            int handled = 0;
            if (init && init->type == AST_ADDR_OF) {
                 ASTNode *target = init->data.unary.expression;
                 if (target && target->type == AST_IDENTIFIER) {
                     char *target_name = target->data.identifier.name;
                     int16_t target_section_num = 0;
                     uint8_t target_storage_class = IMAGE_SYM_CLASS_EXTERNAL;
                     uint32_t sym_idx = coff_writer_add_symbol(obj_writer, target_name, 0, target_section_num, 0, target_storage_class);
                     uint32_t reloc_offset = (uint32_t)obj_writer->data_section.size;
                     coff_writer_add_reloc(obj_writer, reloc_offset, sym_idx, 1, 2);
                     uint64_t zero = 0;
                     buffer_write_bytes(&obj_writer->data_section, &zero, 8);
                     handled = 1;
                 }
            }
            if (!handled) {
                // Write 'size' zero bytes for uninitialized / default-zero global data
                for (int zi = 0; zi < size; zi++) {
                    buffer_write_byte(&obj_writer->data_section, 0);
                }
            }
        }
        
        current_section = old_section;
    } else {
        if (current_syntax == SYNTAX_INTEL) {
            fprintf(out, "_DATA SEGMENT\n");
            fprintf(out, "PUBLIC %s\n", node->data.var_decl.name);
            fprintf(out, "%s ", node->data.var_decl.name);
            
            int size = node->resolved_type ? node->resolved_type->size : 4;
            const char *directive = "DD";
            if (size == 1) directive = "DB";
            else if (size == 8) directive = "DQ";
            
            ASTNode *init_intel = node->data.var_decl.initializer;
            if (init_intel && init_intel->type == AST_INTEGER) {
                fprintf(out, "%s %d\n", directive, init_intel->data.integer.value);
            } else if (init_intel && init_intel->type == AST_FLOAT) {
                fprintf(out, "%s %f\n", directive, init_intel->data.float_val.value);
            } else {
                fprintf(out, "%s 0\n", directive);
                if (size > 8) {
                     fprintf(out, "DB %d DUP(0)\n", size - (size > 1 ? (size > 4 ? 8 : 4) : 1)); // This logic was simplified before, let's just make it zero fill
                }
            }
            fprintf(out, "_DATA ENDS\n");
        } else {
            fprintf(out, ".data\n");
            fprintf(out, ".globl %s\n", node->data.var_decl.name);
            fprintf(out, "%s:\n", node->data.var_decl.name);
             ASTNode *init_att = node->data.var_decl.initializer;
             if (init_att && init_att->type == AST_INTEGER) {
                 int val = init_att->data.integer.value;
                 int size = node->resolved_type ? node->resolved_type->size : 4;
                 if (size == 1) fprintf(out, "    .byte %d\n", val);
                 else if (size == 4) fprintf(out, "    .long %d\n", val);
                 else if (size == 8) fprintf(out, "    .quad %d\n", val);
             } else if (init_att && init_att->type == AST_FLOAT) {
                 int size = node->resolved_type ? node->resolved_type->size : 4;
                 if (size == 4) fprintf(out, "    .float %f\n", init_att->data.float_val.value);
                 else fprintf(out, "    .double %f\n", init_att->data.float_val.value);
             } else {
                 int size = node->resolved_type ? node->resolved_type->size : 4;
                 fprintf(out, "    .zero %d\n", size);
             }
            fprintf(out, ".text\n");
        }
    }
    
    if (globals_count >= 8192) { fprintf(stderr, "Error: Too many globals\n"); exit(1); }
    globals[globals_count].name = node->data.var_decl.name;
    globals[globals_count].type = node->resolved_type;
    globals_count++;
}

static void emit_push_xmm(const char *reg) {
    emit_inst2("sub", op_imm(8), op_reg("rsp"));
    emit_inst2("movsd", op_reg(reg), op_mem("rsp", 0));
    stack_offset -= 8;
}

static void emit_pop_xmm(const char *reg) {
    emit_inst2("movsd", op_mem("rsp", 0), op_reg(reg));
    emit_inst2("add", op_imm(8), op_reg("rsp"));
    stack_offset += 8;
}


// op_reg, op_imm, op_mem are now defined above with the other op_* functions


static void print_operand(Operand *op) {
    if (!out) return;
    if (op->type == OP_REG) {
        if (current_syntax == SYNTAX_ATT) fprintf(out, "%%%s", op->data.reg);
        else fprintf(out, "%s", op->data.reg);
    } else if (op->type == OP_IMM) {
        if (current_syntax == SYNTAX_ATT) fprintf(out, "$%d", op->data.imm);
        else fprintf(out, "%d", op->data.imm);
    } else if (op->type == OP_MEM) {
        if (current_syntax == SYNTAX_ATT) {
            if (op->data.mem.offset != 0) fprintf(out, "%d", op->data.mem.offset);
            fprintf(out, "(%%%s)", op->data.mem.base);
        } else {
            fprintf(out, "[%s", op->data.mem.base);
            if (op->data.mem.offset > 0) fprintf(out, "+%d", op->data.mem.offset);
            else if (op->data.mem.offset < 0) fprintf(out, "%d", op->data.mem.offset);
            fprintf(out, "]");
        }
    } else if (op->type == OP_LABEL) {
        const char *lbl = op->data.label ? op->data.label : "null_label";
        if (current_syntax == SYNTAX_ATT) fprintf(out, "%s(%%rip)", lbl);
        else fprintf(out, "[%s]", lbl);
    }
}

// Print operand for jump/call targets (no RIP-relative for ATT)
static void print_operand_jmp(Operand *op) {
    if (!out) return;
    if (op->type == OP_LABEL) {
        const char *lbl = op->data.label ? op->data.label : "null_label";
        if (current_syntax == SYNTAX_ATT) fprintf(out, "%s", lbl);
        else fprintf(out, "%s", lbl);
    } else {
        print_operand(op);
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

static void emit_inst1(const char *mnemonic, Operand *op1) {
    if (obj_writer) {
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
        else if (strcmp(mnemonic, "not") == 0) m = "not";
    }

    fprintf(out, "    %s ", m);
    // Use jump-style label printing for jmp/jcc/call (no RIP-relative)
    if (op1->type == OP_LABEL &&
        (m[0] == 'j' || strcmp(m, "call") == 0)) {
        print_operand_jmp(op1);
    } else {
        print_operand(op1);
    }
    fprintf(out, "\n");
}

static void emit_inst2(const char *mnemonic, Operand *op1, Operand *op2) {
    if (obj_writer) {
        encode_inst2(&obj_writer->text_section, mnemonic, op1, op2);
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
        print_operand(op1);
        fprintf(out, ", ");
        print_operand(op2);
    } else {
        print_operand(op2);
        fprintf(out, ", ");
        if (strcmp(mnemonic, "movzbq") == 0 && op1->type == OP_MEM) {
            fprintf(out, "byte ptr ");
        }
        print_operand(op1);
    }
    fprintf(out, "\n");
}

static const char *get_reg_32(const char *reg64) {
    if (strcmp(reg64, "rax") == 0) return "eax";
    if (strcmp(reg64, "rcx") == 0) return "ecx";
    if (strcmp(reg64, "rdx") == 0) return "edx";
    if (strcmp(reg64, "rbx") == 0) return "ebx";
    if (strcmp(reg64, "rsi") == 0) return "esi";
    if (strcmp(reg64, "rdi") == 0) return "edi";
    if (strcmp(reg64, "r8") == 0) return "r8d";
    if (strcmp(reg64, "r9") == 0) return "r9d";
    return reg64;
}

static const char *get_reg_16(const char *reg64) {
    if (strcmp(reg64, "rax") == 0) return "ax";
    if (strcmp(reg64, "rcx") == 0) return "cx";
    if (strcmp(reg64, "rdx") == 0) return "dx";
    if (strcmp(reg64, "rbx") == 0) return "bx";
    if (strcmp(reg64, "rsi") == 0) return "si";
    if (strcmp(reg64, "rdi") == 0) return "di";
    if (strcmp(reg64, "r8") == 0) return "r8w";
    if (strcmp(reg64, "r9") == 0) return "r9w";
    return reg64;
}

static const char *get_reg_8(const char *reg64) {
    if (strcmp(reg64, "rax") == 0) return "al";
    if (strcmp(reg64, "rcx") == 0) return "cl";
    if (strcmp(reg64, "rdx") == 0) return "dl";
    if (strcmp(reg64, "rbx") == 0) return "bl";
    if (strcmp(reg64, "rsi") == 0) return "sil";
    if (strcmp(reg64, "rdi") == 0) return "dil";
    if (strcmp(reg64, "r8") == 0) return "r8b";
    if (strcmp(reg64, "r9") == 0) return "r9b";
    return reg64;
}


static int is_float_type(Type *t) {
    return t && (t->kind == TYPE_FLOAT || t->kind == TYPE_DOUBLE);
}

static Type *get_expr_type(ASTNode *node) {
    if (!node) return NULL;
    if (node->type == AST_INTEGER) return type_int();
    if (node->type == AST_FLOAT) return node->resolved_type ? node->resolved_type : type_double();
    if (node->type == AST_IDENTIFIER) {
        Type *t = get_local_type(node->data.identifier.name);
        if (!t) t = get_global_type(node->data.identifier.name);
        return t;
    } else if (node->type == AST_DEREF) {
        Type *t = get_expr_type(node->data.unary.expression);
        return t ? t->data.ptr_to : NULL;
    } else if (node->type == AST_ADDR_OF) {
        Type *t = get_expr_type(node->data.unary.expression);
        return type_ptr(t);
    } else if (node->type == AST_CALL) {
        if (current_program) {
            for (size_t i = 0; i < current_program->children_count; i++) {
                ASTNode *child = current_program->children[i];
                if (child->type == AST_FUNCTION && child->data.function.name && node->data.call.name && 
                    strcmp(child->data.function.name, node->data.call.name) == 0) {
                    return child->resolved_type;
                }
            }
        }
        return type_int(); // Default
    } else if (node->type == AST_MEMBER_ACCESS) {
        Type *st = get_expr_type(node->data.member_access.struct_expr);
        if (node->data.member_access.is_arrow && st && st->kind == TYPE_PTR) {
            st = st->data.ptr_to;
        }
        if (st && (st->kind == TYPE_STRUCT || st->kind == TYPE_UNION)) {
            for (int i = 0; i < st->data.struct_data.members_count; i++) {
                if (st->data.struct_data.members[i].name && node->data.member_access.member_name &&
                    strcmp(st->data.struct_data.members[i].name, node->data.member_access.member_name) == 0) {
                    return st->data.struct_data.members[i].type;
                }
            }
        }
    } else if (node->type == AST_BINARY_EXPR) {
        TokenType op = node->data.binary_expr.op;
        if (op == TOKEN_EQUAL_EQUAL || op == TOKEN_BANG_EQUAL ||
            op == TOKEN_LESS || op == TOKEN_GREATER ||
            op == TOKEN_LESS_EQUAL || op == TOKEN_GREATER_EQUAL ||
            op == TOKEN_AMPERSAND_AMPERSAND || op == TOKEN_PIPE_PIPE) {
            return type_int();
        }
        Type *lt = get_expr_type(node->data.binary_expr.left);
        Type *rt = get_expr_type(node->data.binary_expr.right);
        if (is_float_type(lt) || is_float_type(rt)) {
            if (lt && lt->kind == TYPE_DOUBLE) return lt;
            if (rt && rt->kind == TYPE_DOUBLE) return rt;
            if (is_float_type(lt)) return lt;
            return rt;
        }
        return lt ? lt : rt;
    } else if (node->type == AST_NEG || node->type == AST_PRE_INC || node->type == AST_PRE_DEC || 
               node->type == AST_POST_INC || node->type == AST_POST_DEC || node->type == AST_BITWISE_NOT) {
        return get_expr_type(node->data.unary.expression);
    } else if (node->type == AST_NOT) {
        return type_int();
    } else if (node->type == AST_CAST) {
        return node->data.cast.target_type;
    } else if (node->type == AST_ARRAY_ACCESS) {
        Type *arr = get_expr_type(node->data.array_access.array);
        if (arr && (arr->kind == TYPE_PTR || arr->kind == TYPE_ARRAY)) {
            return arr->data.ptr_to;
        }
        return NULL;
    }
    return NULL;
}

static void gen_expression(ASTNode *node);

static void gen_addr(ASTNode *node) {
    if (node->type == AST_IDENTIFIER) {
        const char *label = get_local_label(node->data.identifier.name);
        if (label) {
            emit_inst2("lea", op_label(label), op_reg("rax"));
            node->resolved_type = type_ptr(get_local_type(node->data.identifier.name));
            return;
        }
        int offset = get_local_offset(node->data.identifier.name);
        if (offset != 0) {
            emit_inst2("lea", op_mem("rbp", offset), op_reg("rax"));
            node->resolved_type = type_ptr(get_local_type(node->data.identifier.name));
        } else {
            // Global
            emit_inst2("lea", op_label(node->data.identifier.name), op_reg("rax"));
            node->resolved_type = type_ptr(get_global_type(node->data.identifier.name));
        }
    } else if (node->type == AST_DEREF) {
        gen_expression(node->data.unary.expression);
    } else if (node->type == AST_MEMBER_ACCESS) {
        if (!node->data.member_access.struct_expr) { fprintf(stderr, "      Member: NULL struct_expr!\n"); return; }
        Type *st = get_expr_type(node->data.member_access.struct_expr);
        if (node->data.member_access.is_arrow) {
            gen_expression(node->data.member_access.struct_expr);
            if (st && st->kind == TYPE_PTR) st = st->data.ptr_to;
            else { fprintf(stderr, "      Member: arrow on non-ptr! st=%p\n", (void*)st); return; }
        } else {
            gen_addr(node->data.member_access.struct_expr);
        }
        
        if (st && (st->kind == TYPE_STRUCT || st->kind == TYPE_UNION)) {
            for (int i = 0; i < st->data.struct_data.members_count; i++) {
                if (st->data.struct_data.members[i].name && node->data.member_access.member_name &&
                    strcmp(st->data.struct_data.members[i].name, node->data.member_access.member_name) == 0) {
                    emit_inst2("add", op_imm(st->data.struct_data.members[i].offset), op_reg("rax"));
                    break;
                }
            }
        } else {
        }
    } else if (node->type == AST_ARRAY_ACCESS) {
        if (!node->data.array_access.array || !node->data.array_access.index) { fprintf(stderr, "      Array: NULL child!\n"); return; }
        gen_expression(node->data.array_access.array);
        emit_inst1("pushq", op_reg("rax"));
        stack_offset -= 8;
        
        gen_expression(node->data.array_access.index);
        
        // Element size calculation - use get_expr_type to avoid chained deref issues
        Type *array_type = get_expr_type(node->data.array_access.array);

        int element_size = 8;
        if (array_type) {
             if (array_type->kind == TYPE_PTR || array_type->kind == TYPE_ARRAY) {
                 if (array_type->data.ptr_to) element_size = array_type->data.ptr_to->size;
             }
        }
        
        emit_inst2("imul", op_imm(element_size), op_reg("rax"));
        emit_inst1("popq", op_reg("rcx"));
        stack_offset += 8;
        emit_inst2("add", op_reg("rcx"), op_reg("rax"));
    }
}

static void gen_binary_expr(ASTNode *node) {
    if (node->data.binary_expr.op == TOKEN_AMPERSAND_AMPERSAND || node->data.binary_expr.op == TOKEN_PIPE_PIPE) {
        int is_and = (node->data.binary_expr.op == TOKEN_AMPERSAND_AMPERSAND);
        int l_short = label_count++;
        int l_end = label_count++;
        char sl[32], el[32];
        sprintf(sl, ".L%d", l_short);
        sprintf(el, ".L%d", l_end);

        gen_expression(node->data.binary_expr.left);
        Type *lt = get_expr_type(node->data.binary_expr.left);
        if (is_float_type(lt)) {
             emit_inst2("xor", op_reg("rax"), op_reg("rax"));
             if (lt->kind == TYPE_FLOAT) {
                 emit_inst2("cvtsi2ss", op_reg("rax"), op_reg("xmm1"));
                 emit_inst2("ucomiss", op_reg("xmm1"), op_reg("xmm0"));
             } else {
                 emit_inst2("cvtsi2sd", op_reg("rax"), op_reg("xmm1"));
                 emit_inst2("ucomisd", op_reg("xmm1"), op_reg("xmm0"));
             }
             emit_inst1(is_and ? "jz" : "jnz", op_label(sl));
        } else {
             emit_inst2("test", op_reg("rax"), op_reg("rax"));
             emit_inst1(is_and ? "jz" : "jnz", op_label(sl));
        }

        gen_expression(node->data.binary_expr.right);
        Type *rt = get_expr_type(node->data.binary_expr.right);
        if (is_float_type(rt)) {
             emit_inst2("xor", op_reg("rax"), op_reg("rax"));
             if (rt->kind == TYPE_FLOAT) {
                 emit_inst2("cvtsi2ss", op_reg("rax"), op_reg("xmm1"));
                 emit_inst2("ucomiss", op_reg("xmm1"), op_reg("xmm0"));
             } else {
                 emit_inst2("cvtsi2sd", op_reg("rax"), op_reg("xmm1"));
                 emit_inst2("ucomisd", op_reg("xmm1"), op_reg("xmm0"));
             }
             emit_inst1(is_and ? "jz" : "jnz", op_label(sl));
        } else {
             emit_inst2("test", op_reg("rax"), op_reg("rax"));
             emit_inst1(is_and ? "jz" : "jnz", op_label(sl));
        }

        emit_inst2("mov", op_imm(is_and ? 1 : 0), op_reg("rax"));
        emit_inst1("jmp", op_label(el));
        emit_label_def(sl);
        emit_inst2("mov", op_imm(is_and ? 0 : 1), op_reg("rax"));
        emit_label_def(el);
        node->resolved_type = type_int();
        return;
    }

    Type *lt = get_expr_type(node->data.binary_expr.left);
    Type *rt = get_expr_type(node->data.binary_expr.right);
    int is_fp = is_float_type(lt) || is_float_type(rt);

    if (is_fp) {
        int is_double = (lt && lt->kind == TYPE_DOUBLE) || (rt && rt->kind == TYPE_DOUBLE);

        gen_expression(node->data.binary_expr.right);
        // If it was int, convert to float/double
        if (!is_float_type(rt)) {
             emit_inst2(is_double ? "cvtsi2sd" : "cvtsi2ss", op_reg("rax"), op_reg("xmm0"));
        } else if (is_double && rt->kind == TYPE_FLOAT) {
             emit_inst2("cvtss2sd", op_reg("xmm0"), op_reg("xmm0"));
        }
        emit_push_xmm("xmm0");
        
        gen_expression(node->data.binary_expr.left);
        if (!is_float_type(lt)) {
             emit_inst2(is_double ? "cvtsi2sd" : "cvtsi2ss", op_reg("rax"), op_reg("xmm0"));
        } else if (is_double && lt->kind == TYPE_FLOAT) {
             emit_inst2("cvtss2sd", op_reg("xmm0"), op_reg("xmm0"));
        }
        emit_pop_xmm("xmm1");
        
        // At this point: left in xmm0, right in xmm1
        
        switch (node->data.binary_expr.op) {
            case TOKEN_PLUS: emit_inst2(is_double ? "addsd" : "addss", op_reg("xmm1"), op_reg("xmm0")); break;
            case TOKEN_MINUS: emit_inst2(is_double ? "subsd" : "subss", op_reg("xmm1"), op_reg("xmm0")); break;
            case TOKEN_STAR: emit_inst2(is_double ? "mulsd" : "mulss", op_reg("xmm1"), op_reg("xmm0")); break;
            case TOKEN_SLASH: emit_inst2(is_double ? "divsd" : "divss", op_reg("xmm1"), op_reg("xmm0")); break;
            case TOKEN_EQUAL_EQUAL:
            case TOKEN_BANG_EQUAL:
            case TOKEN_LESS:
            case TOKEN_GREATER:
            case TOKEN_LESS_EQUAL:
            case TOKEN_GREATER_EQUAL:
                emit_inst2(is_double ? "ucomisd" : "ucomiss", op_reg("xmm1"), op_reg("xmm0"));
                if (node->data.binary_expr.op == TOKEN_EQUAL_EQUAL) emit_inst1("sete", op_reg("al"));
                else if (node->data.binary_expr.op == TOKEN_BANG_EQUAL) emit_inst1("setne", op_reg("al"));
                else if (node->data.binary_expr.op == TOKEN_LESS) emit_inst1("setb", op_reg("al")); // below
                else if (node->data.binary_expr.op == TOKEN_LESS_EQUAL) emit_inst1("setbe", op_reg("al")); // below or equal
                else if (node->data.binary_expr.op == TOKEN_GREATER) emit_inst1("seta", op_reg("al")); // above
                else if (node->data.binary_expr.op == TOKEN_GREATER_EQUAL) emit_inst1("setae", op_reg("al")); // above or equal
                emit_inst2("movzbq", op_reg("al"), op_reg("rax"));
                node->resolved_type = type_int();
                return;
            default: break;
        }
        node->resolved_type = is_double ? type_double() : type_float();
        return;
    }

    // Integer branch
    gen_expression(node->data.binary_expr.right);
    emit_inst1("pushq", op_reg("rax"));
    stack_offset -= 8;
    gen_expression(node->data.binary_expr.left);
    emit_inst1("popq", op_reg("rcx"));
    stack_offset += 8;
    
    Type *left_type = get_expr_type(node->data.binary_expr.left);
    Type *right_type = get_expr_type(node->data.binary_expr.right);
    
    // Helper to get element size
    int size = 1;
    if (left_type && (left_type->kind == TYPE_PTR || left_type->kind == TYPE_ARRAY) && left_type->data.ptr_to) {
        size = left_type->data.ptr_to->size;
    } else if (right_type && (right_type->kind == TYPE_PTR || right_type->kind == TYPE_ARRAY) && right_type->data.ptr_to) {
        size = right_type->data.ptr_to->size;
    }
    
    switch (node->data.binary_expr.op) {
        case TOKEN_PLUS:
            if ((left_type && (left_type->kind == TYPE_PTR || left_type->kind == TYPE_ARRAY)) && 
                (right_type && (right_type->kind == TYPE_INT || right_type->kind == TYPE_CHAR))) {
                if (size > 1) emit_inst2("imul", op_imm(size), op_reg("rcx"));
                node->resolved_type = left_type;
            } else if ((left_type && (left_type->kind == TYPE_INT || left_type->kind == TYPE_CHAR)) && 
                       (right_type && (right_type->kind == TYPE_PTR || right_type->kind == TYPE_ARRAY))) {
                if (size > 1) emit_inst2("imul", op_imm(size), op_reg("rax"));
                node->resolved_type = right_type;
            } else {
                node->resolved_type = left_type ? left_type : right_type;
            }
            if (node->resolved_type && node->resolved_type->size == 4) emit_inst2("addl", op_reg("ecx"), op_reg("eax"));
            else emit_inst2("add", op_reg("rcx"), op_reg("rax"));
            break;
        case TOKEN_MINUS: 
            if ((left_type && (left_type->kind == TYPE_PTR || left_type->kind == TYPE_ARRAY)) && 
                (right_type && (right_type->kind == TYPE_INT || right_type->kind == TYPE_CHAR))) {
                if (size > 1) emit_inst2("imul", op_imm(size), op_reg("rcx"));
                emit_inst2("sub", op_reg("rcx"), op_reg("rax")); // Pointers are 64-bit
                node->resolved_type = left_type;
            } else if ((left_type && (left_type->kind == TYPE_PTR || left_type->kind == TYPE_ARRAY)) && 
                       (right_type && (right_type->kind == TYPE_PTR || right_type->kind == TYPE_ARRAY))) {
                emit_inst2("sub", op_reg("rcx"), op_reg("rax"));
                if (size > 1) {
                    emit_inst0("cqo");
                    emit_inst2("mov", op_imm(size), op_reg("rcx"));
                    emit_inst1("idiv", op_reg("rcx"));
                }
                node->resolved_type = type_int();
            } else {
                if (node->resolved_type && node->resolved_type->size == 4) emit_inst2("subl", op_reg("ecx"), op_reg("eax"));
                else emit_inst2("sub", op_reg("rcx"), op_reg("rax"));
                node->resolved_type = left_type;
            }
            break;
        case TOKEN_STAR:  
            if (node->resolved_type && node->resolved_type->size == 4) emit_inst2("imull", op_reg("ecx"), op_reg("eax"));
            else emit_inst2("imul", op_reg("rcx"), op_reg("rax")); 
            node->resolved_type = left_type;
            break;
        case TOKEN_SLASH:
        case TOKEN_PERCENT:
            emit_inst0("cqo");
            emit_inst1("idiv", op_reg("rcx"));
            if (node->data.binary_expr.op == TOKEN_PERCENT) {
                emit_inst2("mov", op_reg("rdx"), op_reg("rax"));
            }
            node->resolved_type = left_type;
            break;
        case TOKEN_AMPERSAND:
            if (node->resolved_type && node->resolved_type->size == 4) emit_inst2("andl", op_reg("ecx"), op_reg("eax"));
            else emit_inst2("and", op_reg("rcx"), op_reg("rax"));
            node->resolved_type = left_type;
            break;
        case TOKEN_PIPE:
            if (node->resolved_type && node->resolved_type->size == 4) emit_inst2("orl", op_reg("ecx"), op_reg("eax"));
            else emit_inst2("or", op_reg("rcx"), op_reg("rax"));
            node->resolved_type = left_type;
            break;
        case TOKEN_CARET:
            if (node->resolved_type && node->resolved_type->size == 4) emit_inst2("xorl", op_reg("ecx"), op_reg("eax"));
            else emit_inst2("xor", op_reg("rcx"), op_reg("rax"));
            node->resolved_type = left_type;
            break;
        case TOKEN_LESS_LESS:
            emit_inst2("shl", op_reg("cl"), op_reg("rax"));
            node->resolved_type = left_type;
            break;
        case TOKEN_GREATER_GREATER:
            emit_inst2("sar", op_reg("cl"), op_reg("rax"));
            node->resolved_type = left_type;
            break;
        case TOKEN_EQUAL_EQUAL:
        case TOKEN_BANG_EQUAL:
        case TOKEN_LESS:
        case TOKEN_GREATER:
        case TOKEN_LESS_EQUAL:
        case TOKEN_GREATER_EQUAL: {
            Type *cmp_type = left_type ? left_type : right_type;
            if (cmp_type && cmp_type->size == 4) emit_inst2("cmpl", op_reg("ecx"), op_reg("eax"));
            else emit_inst2("cmp", op_reg("rcx"), op_reg("rax"));
            
            if (node->data.binary_expr.op == TOKEN_EQUAL_EQUAL) emit_inst1("sete", op_reg("al"));
            else if (node->data.binary_expr.op == TOKEN_BANG_EQUAL) emit_inst1("setne", op_reg("al"));
            else if (node->data.binary_expr.op == TOKEN_LESS) emit_inst1("setl", op_reg("al"));
            else if (node->data.binary_expr.op == TOKEN_GREATER) emit_inst1("setg", op_reg("al"));
            else if (node->data.binary_expr.op == TOKEN_LESS_EQUAL) emit_inst1("setle", op_reg("al"));
            else if (node->data.binary_expr.op == TOKEN_GREATER_EQUAL) emit_inst1("setge", op_reg("al"));
            
            emit_inst2("movzbq", op_reg("al"), op_reg("rax"));
            break;
        }
        default: break;
    }
}

static void gen_expression(ASTNode *node) {
    if (!node) return;
    if (node->resolved_type == NULL) {
        node->resolved_type = get_expr_type(node);
    }
    if (node->type == AST_INTEGER) {
        emit_inst2("mov", op_imm(node->data.integer.value), op_reg("rax"));
        node->resolved_type = type_int();
    } else if (node->type == AST_FLOAT) {
        char label[32];
        sprintf(label, ".LF%d", label_count++);
        node->resolved_type = node->resolved_type ? node->resolved_type : type_double();
        
        if (obj_writer) {
            Section old_section = current_section;
            current_section = SECTION_DATA;
            emit_label_def(label);
            if (node->resolved_type->kind == TYPE_FLOAT) {
                float f = (float)node->data.float_val.value;
                buffer_write_bytes(&obj_writer->data_section, &f, 4);
            } else {
                double d = node->data.float_val.value;
                buffer_write_bytes(&obj_writer->data_section, &d, 8);
            }
            current_section = old_section;
        } else {
            if (current_syntax == SYNTAX_INTEL) {
                fprintf(out, "_TEXT ENDS\n_DATA SEGMENT\n%s ", label + 1);
                if (node->resolved_type->kind == TYPE_FLOAT) {
                    fprintf(out, "DD %f\n", node->data.float_val.value);
                } else {
                    fprintf(out, "DQ %f\n", node->data.float_val.value);
                }
                fprintf(out, "_DATA ENDS\n_TEXT SEGMENT\n");
            } else {
                fprintf(out, ".data\n%s:\n", label);
                if (node->resolved_type->kind == TYPE_FLOAT) {
                    fprintf(out, "    .float %f\n", node->data.float_val.value);
                } else {
                    fprintf(out, "    .double %f\n", node->data.float_val.value);
                }
                fprintf(out, ".text\n");
            }
        }
        
        if (node->resolved_type->kind == TYPE_FLOAT) {
            emit_inst2("movss", op_label(label), op_reg("xmm0"));
        } else {
            emit_inst2("movsd", op_label(label), op_reg("xmm0"));
        }
    } else if (node->type == AST_IDENTIFIER) {
        if (!node->data.identifier.name) { fprintf(stderr, "      Ident: NULL NAME!\n"); return; }
        const char *label = get_local_label(node->data.identifier.name);
        if (label) {
            Type *t = get_local_type(node->data.identifier.name);
            if (t && t->kind == TYPE_ARRAY) {
                emit_inst2("lea", op_label(label), op_reg("rax"));
            } else if (is_float_type(t)) {
                if (t->kind == TYPE_FLOAT) emit_inst2("movss", op_label(label), op_reg("xmm0"));
                else emit_inst2("movsd", op_label(label), op_reg("xmm0"));
            } else {
                if (t && t->size == 1) emit_inst2("movzbq", op_label(label), op_reg("rax"));
                else if (t && t->size == 2) emit_inst2("movzwq", op_label(label), op_reg("rax"));
                else if (t && t->size == 4) emit_inst2("movl", op_label(label), op_reg("eax"));
                else emit_inst2("mov", op_label(label), op_reg("rax"));
            }
            node->resolved_type = t;
            return;
        }
        int offset = get_local_offset(node->data.identifier.name);
        if (offset != 0) {
            Type *t = get_local_type(node->data.identifier.name);
            if (t && t->kind == TYPE_ARRAY) {
                // Array decays to pointer
                emit_inst2("lea", op_mem("rbp", offset), op_reg("rax"));
            } else if (is_float_type(t)) {
                if (t->kind == TYPE_FLOAT) emit_inst2("movss", op_mem("rbp", offset), op_reg("xmm0"));
                else emit_inst2("movsd", op_mem("rbp", offset), op_reg("xmm0"));
            } else {
                if (t && t->size == 1) emit_inst2("movzbq", op_mem("rbp", offset), op_reg("rax"));
                else if (t && t->size == 2) emit_inst2("movzwq", op_mem("rbp", offset), op_reg("rax"));
                else if (t && t->size == 4) emit_inst2("movl", op_mem("rbp", offset), op_reg("eax"));
                else emit_inst2("mov", op_mem("rbp", offset), op_reg("rax"));
            }
            node->resolved_type = t;
        } else {
            // Global
            Type *t = get_global_type(node->data.identifier.name);
            if (t && t->kind == TYPE_ARRAY) {
                emit_inst2("lea", op_label(node->data.identifier.name), op_reg("rax"));
            } else if (is_float_type(t)) {
                if (t->kind == TYPE_FLOAT) emit_inst2("movss", op_label(node->data.identifier.name), op_reg("xmm0"));
                else emit_inst2("movsd", op_label(node->data.identifier.name), op_reg("xmm0"));
            } else {
                if (t && t->size == 1) emit_inst2("movzbq", op_label(node->data.identifier.name), op_reg("rax"));
                else if (t && t->size == 2) emit_inst2("movzwq", op_label(node->data.identifier.name), op_reg("rax"));
                else if (t && t->size == 4) emit_inst2("movl", op_label(node->data.identifier.name), op_reg("eax"));
                else emit_inst2("mov", op_label(node->data.identifier.name), op_reg("rax"));
            }
            node->resolved_type = t;
        }
    } else if (node->type == AST_ARRAY_ACCESS) {
        gen_addr(node);
        Type *t = node->resolved_type; // Element type
        if (is_float_type(t)) {
            if (t->size == 4) emit_inst2("movss", op_mem("rax", 0), op_reg("xmm0"));
            else emit_inst2("movsd", op_mem("rax", 0), op_reg("xmm0"));
        } else if (t && t->size == 1) {
            emit_inst2("movzbq", op_mem("rax", 0), op_reg("rax"));
        } else if (t && t->size == 2) {
            emit_inst2("movzwq", op_mem("rax", 0), op_reg("rax"));
        } else if (t && t->size == 4) {
            emit_inst2("movl", op_mem("rax", 0), op_reg("eax"));
        } else {
            emit_inst2("mov", op_mem("rax", 0), op_reg("rax"));
        }
    } else if (node->type == AST_BINARY_EXPR) {
        gen_binary_expr(node);
    } else if (node->type == AST_PRE_INC || node->type == AST_PRE_DEC || 
               node->type == AST_POST_INC || node->type == AST_POST_DEC) {
        int is_inc = (node->type == AST_PRE_INC || node->type == AST_POST_INC);
        int is_pre = (node->type == AST_PRE_INC || node->type == AST_PRE_DEC);
        
        Type *t = get_expr_type(node->data.unary.expression);
        gen_addr(node->data.unary.expression);
        // Address is in RAX. 
        
        int size = 1;
        if (t && (t->kind == TYPE_PTR || t->kind == TYPE_ARRAY) && t->data.ptr_to) {
             size = t->data.ptr_to->size;
        }
        
        // Load value to RCX
        if (t && t->size == 1) {
            emit_inst2("movzbq", op_mem("rax", 0), op_reg("rcx"));
        } else if (t && t->size <= 4) {
            emit_inst2("movslq", op_mem("rax", 0), op_reg("rcx"));
        } else {
            emit_inst2("mov", op_mem("rax", 0), op_reg("rcx"));
        }
        
        if (!is_pre) {
            emit_inst1("pushq", op_reg("rcx")); // Save original value
            stack_offset -= 8;
        }
        
        // Modify RCX
        if (is_inc) {
            emit_inst2("add", op_imm(size), op_reg("rcx"));
        } else {
            emit_inst2("sub", op_imm(size), op_reg("rcx"));
        }
        
        // Store back
        if (t && t->size == 1) {
            emit_inst2("mov", op_reg("cl"), op_mem("rax", 0));
        } else if (t && t->size <= 4) {
            emit_inst2("movl", op_reg("ecx"), op_mem("rax", 0));
        } else {
            emit_inst2("mov", op_reg("rcx"), op_mem("rax", 0));
        }
        
        if (!is_pre) {
            emit_inst1("popq", op_reg("rax")); // Restore original value to result register
            stack_offset += 8;
        } else {
            emit_inst2("mov", op_reg("rcx"), op_reg("rax")); // Result is new value
        }
        
        node->resolved_type = t;
    } else if (node->type == AST_CAST) {
        gen_expression(node->data.cast.expression);
        Type *src = get_expr_type(node->data.cast.expression);
        Type *dst = node->data.cast.target_type;
        
        if (is_float_type(src) && is_float_type(dst)) {
            // Float <-> Double
            if (src->kind == TYPE_FLOAT && dst->kind == TYPE_DOUBLE) {
                emit_inst2("cvtss2sd", op_reg("xmm0"), op_reg("xmm0"));
            } else if (src->kind == TYPE_DOUBLE && dst->kind == TYPE_FLOAT) {
                emit_inst2("cvtsd2ss", op_reg("xmm0"), op_reg("xmm0"));
            }
        } else if (is_float_type(src) && !is_float_type(dst)) {
            // Float -> Int
            if (src->kind == TYPE_FLOAT) {
                emit_inst2("cvttss2si", op_reg("xmm0"), op_reg("rax"));
            } else {
                emit_inst2("cvttsd2si", op_reg("xmm0"), op_reg("rax"));
            }
        } else if (!is_float_type(src) && is_float_type(dst)) {
            // Int -> Float
            if (dst->kind == TYPE_FLOAT) {
                emit_inst2("cvtsi2ss", op_reg("rax"), op_reg("xmm0"));
            } else {
                emit_inst2("cvtsi2sd", op_reg("rax"), op_reg("xmm0"));
            }
        } else {
            // Int -> Int / Ptr
            if (dst->kind == TYPE_CHAR) {
                 // Cast to char: Truncate and sign-extend to 64-bit for consistency
                 emit_inst2("movsbq", op_reg("al"), op_reg("rax"));
            } 
            // Other int-to-int casts usually no-op for same size or smaller-to-larger (implicit)
            // Or larger-to-smaller (truncate, handled by using lower reg parts if needed)
            // But if we cast int (32/64) to char, we explicitly truncated above.
        }
        node->resolved_type = dst;
    } else if (node->type == AST_ASSIGN) {
        if (!node->data.assign.left || !node->data.assign.value) { fprintf(stderr, "      Assign: NULL child!\n"); return; }
        gen_expression(node->data.assign.value);
        ASTNode *left_node = node->data.assign.left;
        Type *t = get_expr_type(left_node);
        if (left_node->type == AST_IDENTIFIER) {
            const char *ident_name = left_node->data.identifier.name;
            const char *label = get_local_label(ident_name);
            if (label) {
                if (is_float_type(t)) {
                    if (t && t->kind == TYPE_FLOAT) emit_inst2("movss", op_reg("xmm0"), op_label(label));
                    else emit_inst2("movsd", op_reg("xmm0"), op_label(label));
                } else {
                    if (t && t->size == 1) emit_inst2("movb", op_reg("al"), op_label(label));
                    else if (t && t->size == 2) emit_inst2("movw", op_reg("ax"), op_label(label));
                    else if (t && t->size == 4) emit_inst2("movl", op_reg("eax"), op_label(label));
                    else emit_inst2("mov", op_reg("rax"), op_label(label));
                }
                return;
            }
            int offset = get_local_offset(ident_name);
            if (offset != 0) {
                if (is_float_type(t)) {
                    if (t && t->kind == TYPE_FLOAT) emit_inst2("movss", op_reg("xmm0"), op_mem("rbp", offset));
                    else emit_inst2("movsd", op_reg("xmm0"), op_mem("rbp", offset));
                } else {
                    if (t && t->size == 1) emit_inst2("movb", op_reg("al"), op_mem("rbp", offset));
                    else if (t && t->size == 2) emit_inst2("movw", op_reg("ax"), op_mem("rbp", offset));
                    else if (t && t->size == 4) emit_inst2("movl", op_reg("eax"), op_mem("rbp", offset));
                    else emit_inst2("mov", op_reg("rax"), op_mem("rbp", offset));
                }
            } else if (ident_name) {
                // Global
                if (is_float_type(t)) {
                    if (t && t->kind == TYPE_FLOAT) emit_inst2("movss", op_reg("xmm0"), op_label(ident_name));
                    else emit_inst2("movsd", op_reg("xmm0"), op_label(ident_name));
                } else {
                    if (t && t->size == 1) emit_inst2("movb", op_reg("al"), op_label(ident_name));
                    else if (t && t->size == 2) emit_inst2("movw", op_reg("ax"), op_label(ident_name));
                    else if (t && t->size == 4) emit_inst2("movl", op_reg("eax"), op_label(ident_name));
                    else emit_inst2("mov", op_reg("rax"), op_label(ident_name));
                }
            }
        } else {
            if (is_float_type(t)) {
                emit_push_xmm("xmm0");
                gen_addr(left_node);
                emit_pop_xmm("xmm1");
                if (t && t->kind == TYPE_FLOAT) emit_inst2("movss", op_reg("xmm1"), op_mem("rax", 0));
                else emit_inst2("movsd", op_reg("xmm1"), op_mem("rax", 0));
                if (t && t->kind == TYPE_FLOAT) emit_inst2("movss", op_reg("xmm1"), op_reg("xmm0"));
                else emit_inst2("movsd", op_reg("xmm1"), op_reg("xmm0"));
            } else {
                emit_inst1("pushq", op_reg("rax"));
                gen_addr(left_node);
                emit_inst1("popq", op_reg("rcx"));
                if (t && t->size == 1) emit_inst2("movb", op_reg("cl"), op_mem("rax", 0));
                else if (t && t->size == 2) emit_inst2("movw", op_reg("cx"), op_mem("rax", 0));
                else if (t && t->size == 4) emit_inst2("movl", op_reg("ecx"), op_mem("rax", 0));
                else emit_inst2("mov", op_reg("rcx"), op_mem("rax", 0));
                emit_inst2("mov", op_reg("rcx"), op_reg("rax"));
            }
        }
        node->resolved_type = t;
    } else if (node->type == AST_DEREF) {
        gen_expression(node->data.unary.expression);
        Type *t = get_expr_type(node->data.unary.expression);
        Type *ptr_to = (t && t->kind == TYPE_PTR) ? t->data.ptr_to : NULL;
        if (is_float_type(ptr_to)) {
            if (ptr_to->size == 4) emit_inst2("movss", op_mem("rax", 0), op_reg("xmm0"));
            else emit_inst2("movsd", op_mem("rax", 0), op_reg("xmm0"));
        } else if (ptr_to && ptr_to->kind == TYPE_CHAR) {
            emit_inst2("movzbq", op_mem("rax", 0), op_reg("rax"));
        } else {
            emit_inst2("mov", op_mem("rax", 0), op_reg("rax"));
        }
        node->resolved_type = ptr_to;
    } else if (node->type == AST_ADDR_OF) {
        gen_addr(node->data.unary.expression);
    } else if (node->type == AST_NEG) {
        gen_expression(node->data.unary.expression);
        Type *t = get_expr_type(node->data.unary.expression);
        if (is_float_type(t)) {
            if (t->kind == TYPE_FLOAT) {
                emit_inst2("xor", op_reg("rax"), op_reg("rax"));
                emit_inst2("cvtsi2ss", op_reg("rax"), op_reg("xmm1")); // xmm1 = 0.0
                emit_inst2("subss", op_reg("xmm0"), op_reg("xmm1"));  // xmm1 = 0.0 - xmm0
                emit_inst2("movss", op_reg("xmm1"), op_reg("xmm0"));
            } else {
                emit_inst2("xor", op_reg("rax"), op_reg("rax"));
                emit_inst2("cvtsi2sd", op_reg("rax"), op_reg("xmm1"));
                emit_inst2("subsd", op_reg("xmm0"), op_reg("xmm1"));
                emit_inst2("movsd", op_reg("xmm1"), op_reg("xmm0"));
            }
        } else {
            emit_inst1("neg", op_reg("rax"));
        }
        node->resolved_type = t;
    } else if (node->type == AST_NOT) {
        gen_expression(node->data.unary.expression);
        Type *t = get_expr_type(node->data.unary.expression);
        if (is_float_type(t)) {
            emit_inst2("xor", op_reg("rax"), op_reg("rax"));
            if (t->kind == TYPE_FLOAT) {
                emit_inst2("cvtsi2ss", op_reg("rax"), op_reg("xmm1"));
                emit_inst2("ucomiss", op_reg("xmm1"), op_reg("xmm0"));
            } else {
                emit_inst2("cvtsi2sd", op_reg("rax"), op_reg("xmm1"));
                emit_inst2("ucomisd", op_reg("xmm1"), op_reg("xmm0"));
            }
            emit_inst1("setz", op_reg("al"));
            emit_inst2("movzbq", op_reg("al"), op_reg("rax"));
        } else {
            emit_inst2("test", op_reg("rax"), op_reg("rax"));
            emit_inst1("setz", op_reg("al"));
            emit_inst2("movzbq", op_reg("al"), op_reg("rax"));
        }
        node->resolved_type = type_int();
    } else if (node->type == AST_BITWISE_NOT) {
        gen_expression(node->data.unary.expression);
        emit_inst1("not", op_reg("rax"));
        node->resolved_type = get_expr_type(node->data.unary.expression);
    } else if (node->type == AST_MEMBER_ACCESS) {
        gen_addr(node);
        Type *mt = get_expr_type(node);
        if (mt && mt->kind == TYPE_ARRAY) {
            // Array member decays to pointer - address is already the value
            node->resolved_type = mt;
        } else if (is_float_type(mt)) {
            if (mt->kind == TYPE_FLOAT) emit_inst2("movss", op_mem("rax", 0), op_reg("xmm0"));
            else emit_inst2("movsd", op_mem("rax", 0), op_reg("xmm0"));
            node->resolved_type = mt;
        } else if (mt && mt->size == 1) {
            emit_inst2("movzbq", op_mem("rax", 0), op_reg("rax"));
        } else if (mt && mt->size == 2) {
            emit_inst2("movzwq", op_mem("rax", 0), op_reg("rax"));
        } else if (mt && mt->size == 4) {
            emit_inst2("movl", op_mem("rax", 0), op_reg("eax"));
        } else {
            emit_inst2("mov", op_mem("rax", 0), op_reg("rax"));
        }
    } else if (node->type == AST_CALL) {
        // Save initial stack offset to restore later
        int initial_stack_offset = stack_offset;
        
        const char **arg_regs = g_arg_regs;
        const char **xmm_arg_regs = g_xmm_arg_regs;
        int max_reg = g_max_reg_args;
        int shadow = g_use_shadow_space ? 32 : 0;
        
        int num_args = (int)node->children_count;
        int extra_args = num_args > max_reg ? num_args - max_reg : 0;
        
        // Calculate padding based on CURRENT stack depth (including any pushed args from outer calls)
        int current_stack_depth = abs(stack_offset);
        int padding = (16 - ((current_stack_depth + extra_args * 8 + shadow) % 16)) % 16;
        
        if (padding > 0) {
            emit_inst2("sub", op_imm(padding), op_reg("rsp"));
            stack_offset -= padding;
        }

        for (int i = (int)node->children_count - 1; i >= 0; i--) {
            ASTNode *child = node->children[i];
            gen_expression(child);
            Type *arg_type = get_expr_type(child);
            if (is_float_type(arg_type)) {
                emit_push_xmm("xmm0");
            } else {
                emit_inst1("pushq", op_reg("rax"));
            }
            stack_offset -= 8; // Update stack offset for nested calls
        }
        
        for (int i = 0; i < (int)num_args && i < max_reg; i++) {
            ASTNode *pop_child = node->children[i];
            Type *pop_type = get_expr_type(pop_child);
            if (is_float_type(pop_type)) {
                emit_pop_xmm(xmm_arg_regs[i]);
            } else {
                emit_inst1("popq", op_reg(arg_regs[i]));
                stack_offset += 8;
            }
        }
        
        // Shadow space (Win64 only)
        if (shadow > 0) {
            emit_inst2("sub", op_imm(shadow), op_reg("rsp"));
        }
        // System V ABI: set al to number of vector (XMM) args for variadic functions
        if (!g_use_shadow_space) {
            int xmm_count = 0;
            for (int i = 0; i < num_args && i < max_reg; i++) {
                Type *at = get_expr_type(node->children[i]);
                if (is_float_type(at)) xmm_count++;
            }
            emit_inst2("mov", op_imm(xmm_count), op_reg("eax"));
        }
        emit_inst1("call", op_label(node->data.call.name));
        if (node->resolved_type == NULL) node->resolved_type = get_expr_type(node);
        
        // Clean up shadow space + extra args + padding
        int cleanup = shadow + extra_args * 8 + padding;
        if (cleanup > 0) {
            emit_inst2("add", op_imm(cleanup), op_reg("rsp"));
        }
        
        // Restore stack offset
        stack_offset = initial_stack_offset;
    } else if (node->type == AST_IF) {
        // Ternary expression: condition ? then_expr : else_expr
        int label_else = label_count++;
        int label_end = label_count++;
        char l_else[32], l_end[32];
        sprintf(l_else, ".L%d", label_else);
        sprintf(l_end, ".L%d", label_end);
        
        gen_expression(node->data.if_stmt.condition);
        emit_inst2("cmp", op_imm(0), op_reg("rax"));
        emit_inst1("je", op_label(l_else));
        
        gen_expression(node->data.if_stmt.then_branch);
        emit_inst1("jmp", op_label(l_end));
        
        emit_label_def(l_else);
        gen_expression(node->data.if_stmt.else_branch);
        
        emit_label_def(l_end);
    } else if (node->type == AST_STRING) {
        char label[32];
        sprintf(label, ".LC%d", label_count++);
        
        int len = node->data.string.length;
        
        if (obj_writer) {
            Section old_section = current_section;
            current_section = SECTION_DATA;
            emit_label_def(label);
            buffer_write_bytes(&obj_writer->data_section, node->data.string.value, len + 1);
            current_section = old_section;
        } else {
            if (string_literals_count >= 8192) { fprintf(stderr, "Error: Too many string literals\n"); exit(1); }
            string_literals[string_literals_count].label = _strdup(label);
            string_literals[string_literals_count].value = malloc(len + 1);
            memcpy(string_literals[string_literals_count].value, node->data.string.value, len + 1);
            string_literals[string_literals_count].length = len;
            string_literals_count++;
        }
        
        // Load address of the string
        emit_inst2("lea", op_label(label), op_reg("rax"));
    }
}

static int current_function_end_label = 0;

static int break_label_stack[32];
static int break_label_ptr = 0;
static int continue_label_stack[32];
static int continue_label_ptr = 0;
static int loop_saved_stack_offset[32];
static int loop_saved_stack_ptr = 0;

static void collect_cases(ASTNode *node, ASTNode **cases, int *case_count, ASTNode **default_node) {
    if (!node) return;
    if (node->type == AST_CASE) {
        int cc_idx = *case_count;
        cases[cc_idx] = node;
        *case_count = cc_idx + 1;
    } else if (node->type == AST_DEFAULT) {
        *default_node = node;
    }
    
    if (node->type == AST_BLOCK) {
        for (size_t i = 0; i < node->children_count; i++) {
            collect_cases(node->children[i], cases, case_count, default_node);
        }
    } else if (node->type == AST_SWITCH) {
        // Don't descend into nested switches for *this* switch's cases
        return;
    } else {
        // For other statements (if/while), we might need to descend if labels can be there
        // C allows this (Duff's Device!), but we'll stick to blocks for now.
        // Actually, let's just descend into all children except nested switches.
        for (size_t i = 0; i < node->children_count; i++) {
            collect_cases(node->children[i], cases, case_count, default_node);
        }
    }
}

static void gen_statement(ASTNode *node) {
    if (!node) return;
    if (node->type == AST_RETURN) {
        if (node->data.return_stmt.expression) {
            gen_expression(node->data.return_stmt.expression);
            Type *expr_type = get_expr_type(node->data.return_stmt.expression);
            
            // Convert to function return type if needed
            if (current_func_return_type && expr_type) {
                if (is_float_type(current_func_return_type) && !is_float_type(expr_type)) {
                    // int -> float/double
                    if (current_func_return_type->kind == TYPE_FLOAT) emit_inst2("cvtsi2ss", op_reg("rax"), op_reg("xmm0"));
                    else emit_inst2("cvtsi2sd", op_reg("rax"), op_reg("xmm0"));
                } else if (!is_float_type(current_func_return_type) && is_float_type(expr_type)) {
                    // float/double -> int
                    if (expr_type->kind == TYPE_FLOAT) emit_inst2("cvttss2si", op_reg("xmm0"), op_reg("rax"));
                    else emit_inst2("cvttsd2si", op_reg("xmm0"), op_reg("rax"));
                } else if (is_float_type(current_func_return_type) && is_float_type(expr_type)) {
                    // float <-> double
                    if (current_func_return_type->kind == TYPE_DOUBLE && expr_type->kind == TYPE_FLOAT) {
                        emit_inst2("cvtss2sd", op_reg("xmm0"), op_reg("xmm0"));
                    } else if (current_func_return_type->kind == TYPE_FLOAT && expr_type->kind == TYPE_DOUBLE) {
                        emit_inst2("cvtsd2ss", op_reg("xmm0"), op_reg("xmm0"));
                    }
                }
            }
        }
        char dest_label[32];
        sprintf(dest_label, ".Lend_%d", current_function_end_label);
        emit_inst1("jmp", op_label(dest_label));
    } else if (node->type == AST_VAR_DECL) {
        if (node->data.var_decl.is_extern) {
             locals[locals_count].name = node->data.var_decl.name;
             locals[locals_count].label = node->data.var_decl.name;
             locals[locals_count].offset = 0;
             locals[locals_count].type = node->resolved_type;
             locals_count++;
             return;
        }
        if (node->data.var_decl.is_static) {
            // Static local variable
            char slabel[64];
            sprintf(slabel, "_S_%s_%s_%d", current_func_name ? current_func_name : "global", node->data.var_decl.name, static_label_count++);
            
            Section old_section = current_section;
            current_section = SECTION_DATA;
            
            if (obj_writer) {
                emit_label_def(slabel);
                int size = node->resolved_type ? node->resolved_type->size : 8;
                int val = 0;
                ASTNode *vinit1 = node->data.var_decl.initializer;
                if (vinit1 && vinit1->type == AST_INTEGER) {
                    val = vinit1->data.integer.value;
                }
                buffer_write_bytes(&obj_writer->data_section, &val, size);
            } else {
                if (current_syntax == SYNTAX_INTEL) {
                    fprintf(out, "_TEXT ENDS\n_DATA SEGMENT\n");
                    emit_label_def(slabel);
                    int size = node->resolved_type ? node->resolved_type->size : 8;
                    int val = 0;
                    ASTNode *vinit2 = node->data.var_decl.initializer;
                    if (vinit2 && vinit2->type == AST_INTEGER) {
                        val = vinit2->data.integer.value;
                    }
                    if (size == 1) fprintf(out, "DB %d\n", val);
                    else if (size == 4) fprintf(out, "DD %d\n", val);
                    else fprintf(out, "DQ %d\n", val);
                    fprintf(out, "_DATA ENDS\n_TEXT SEGMENT\n");
                } else {
                    fprintf(out, ".data\n");
                    emit_label_def(slabel);
                    int size = node->resolved_type ? node->resolved_type->size : 8;
                    int val = 0;
                    ASTNode *vinit3 = node->data.var_decl.initializer;
                    if (vinit3 && vinit3->type == AST_INTEGER) {
                        val = vinit3->data.integer.value;
                    }
                    if (size == 1) fprintf(out, ".byte %d\n", val);
                    else if (size == 4) fprintf(out, ".long %d\n", val);
                    else fprintf(out, ".quad %d\n", val);
                    fprintf(out, ".text\n");
                }
            }
            current_section = old_section;
            
            locals[locals_count].name = node->data.var_decl.name;
            locals[locals_count].label = _strdup(slabel);
            locals[locals_count].offset = 0;
            locals[locals_count].type = node->resolved_type;
            locals_count++;
            return;
        }
        int size = node->resolved_type ? node->resolved_type->size : 8;
        int alloc_size = size;
        if (alloc_size < 8 && node->resolved_type && node->resolved_type->kind != TYPE_STRUCT && node->resolved_type->kind != TYPE_ARRAY) {
            alloc_size = 8;
        }
        
        ASTNode *init_list = node->data.var_decl.initializer;
        if (init_list && init_list->type == AST_INIT_LIST) {
            // Initializer list: {expr, expr, ...}
            
            stack_offset -= alloc_size;
            
            locals[locals_count].name = node->data.var_decl.name;
            locals[locals_count].offset = stack_offset;
            locals[locals_count].label = NULL;
            locals[locals_count].type = node->resolved_type;
            locals_count++;
            
            // Allocate space on stack
            emit_inst2("sub", op_imm(alloc_size), op_reg("rsp"));
            
            // Zero-initialize with qword stores
            {
                int off;
                for (off = 0; off + 8 <= alloc_size; off += 8) {
                    emit_inst2("movq", op_imm(0), op_mem("rbp", stack_offset + off));
                }
                if (off + 4 <= alloc_size) {
                    emit_inst2("movl", op_imm(0), op_mem("rbp", stack_offset + off));
                }
            }
            
            // Determine element size for arrays
            int elem_size = 8;
            if (node->resolved_type && node->resolved_type->kind == TYPE_ARRAY && node->resolved_type->data.ptr_to) {
                elem_size = node->resolved_type->data.ptr_to->size;
                if (elem_size < 4) elem_size = 1;
                else if (elem_size < 8) elem_size = 4;
            }
            
            if (node->resolved_type && node->resolved_type->kind == TYPE_STRUCT) {
                // Struct init: store to each member offset
                size_t i;
                for (i = 0; i < init_list->children_count; i++) {
                    gen_expression(init_list->children[i]);
                    if (node->resolved_type->data.struct_data.members && (int)i < node->resolved_type->data.struct_data.members_count) {
                        int mem_offset = node->resolved_type->data.struct_data.members[i].offset;
                        int mem_size = node->resolved_type->data.struct_data.members[i].type ? node->resolved_type->data.struct_data.members[i].type->size : 8;
                        if (mem_size == 1) {
                            emit_inst2("movb", op_reg("al"), op_mem("rbp", stack_offset + mem_offset));
                        } else if (mem_size == 4) {
                            emit_inst2("movl", op_reg("eax"), op_mem("rbp", stack_offset + mem_offset));
                        } else {
                            emit_inst2("mov", op_reg("rax"), op_mem("rbp", stack_offset + mem_offset));
                        }
                    }
                }
            } else {
                // Array init: store to each element index
                size_t i;
                for (i = 0; i < init_list->children_count; i++) {
                    gen_expression(init_list->children[i]);
                    int el_offset = stack_offset + (int)(i * elem_size);
                    if (elem_size == 1) {
                        emit_inst2("movb", op_reg("al"), op_mem("rbp", el_offset));
                    } else if (elem_size == 4) {
                        emit_inst2("movl", op_reg("eax"), op_mem("rbp", el_offset));
                    } else {
                        emit_inst2("mov", op_reg("rax"), op_mem("rbp", el_offset));
                    }
                }
            }
        } else {
            // Scalar initializer (original path)
            if (node->data.var_decl.initializer) {
                gen_expression(node->data.var_decl.initializer);
            } else {
                if (is_float_type(node->resolved_type)) {
                    emit_inst2("xor", op_reg("rax"), op_reg("rax"));
                    if (node->resolved_type->kind == TYPE_FLOAT) emit_inst2("cvtsi2ss", op_reg("rax"), op_reg("xmm0"));
                    else emit_inst2("cvtsi2sd", op_reg("rax"), op_reg("xmm0"));
                } else {
                    emit_inst2("mov", op_imm(0), op_reg("rax"));
                }
            }
            
            stack_offset -= alloc_size;
            
            if (locals_count >= 8192) { fprintf(stderr, "Error: Too many locals\n"); exit(1); }
            locals[locals_count].name = node->data.var_decl.name;
            locals[locals_count].offset = stack_offset;
            locals[locals_count].label = NULL;
            locals[locals_count].type = node->resolved_type;
            locals_count++;
            
            if (is_float_type(node->resolved_type)) {
                emit_push_xmm("xmm0"); 
            } else {
                emit_inst2("sub", op_imm(alloc_size), op_reg("rsp"));
                if (node->resolved_type && node->resolved_type->kind != TYPE_STRUCT && node->resolved_type->kind != TYPE_ARRAY) {
                    // Scalar store
                    if (size == 1) emit_inst2("movb", op_reg("al"), op_mem("rsp", 0));
                    else if (size == 2) emit_inst2("movw", op_reg("ax"), op_mem("rsp", 0));
                    else if (size == 4) emit_inst2("movl", op_reg("eax"), op_mem("rsp", 0));
                    else emit_inst2("mov", op_reg("rax"), op_mem("rsp", 0));
                }
            }
        }
    } else if (node->type == AST_IF) {
        int label_else = label_count++;
        int label_end = label_count++;
        char l_else[32], l_end[32];
        sprintf(l_else, ".L%d", label_else);
        sprintf(l_end, ".L%d", label_end);
        
        gen_expression(node->data.if_stmt.condition);
        emit_inst2("cmp", op_imm(0), op_reg("rax"));
        emit_inst1("je", op_label(l_else));
        
        // Save stack state before then-branch
        int saved_stack_offset = stack_offset;
        int saved_locals_count = locals_count;
        
        // Generate then branch
        if (node->data.if_stmt.then_branch) {
            gen_statement(node->data.if_stmt.then_branch);
        }
        
        // Restore RSP to pre-branch value (variables go out of scope)
        if (stack_offset != saved_stack_offset) {
            emit_inst2("lea", op_mem("rbp", saved_stack_offset), op_reg("rsp"));
        }
        stack_offset = saved_stack_offset;
        locals_count = saved_locals_count;
        
        emit_inst1("jmp", op_label(l_end));
        
        emit_label_def(l_else);
        if (node->data.if_stmt.else_branch) {
            gen_statement(node->data.if_stmt.else_branch);
            // Restore RSP after else-branch too
            if (stack_offset != saved_stack_offset) {
                emit_inst2("lea", op_mem("rbp", saved_stack_offset), op_reg("rsp"));
            }
            stack_offset = saved_stack_offset;
            locals_count = saved_locals_count;
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
        emit_inst2("cmp", op_imm(0), op_reg("rax"));
        emit_inst1("je", op_label(l_end));
        
        int saved_stack_offset = stack_offset;
        int saved_locals_count = locals_count;
        
        int lsp_idx = loop_saved_stack_ptr;
        loop_saved_stack_offset[lsp_idx] = saved_stack_offset;
        loop_saved_stack_ptr = lsp_idx + 1;
        int blp_idx = break_label_ptr;
        break_label_stack[blp_idx] = label_end;
        break_label_ptr = blp_idx + 1;
        int clp_idx = continue_label_ptr;
        continue_label_stack[clp_idx] = label_start;
        continue_label_ptr = clp_idx + 1;
        gen_statement(node->data.while_stmt.body);
        break_label_ptr--;
        continue_label_ptr--;
        loop_saved_stack_ptr--;
        
        // Restore RSP to loop entry value using rbp-relative addressing
        // This correctly handles all runtime paths (if/else branches with different var decls)
        if (saved_stack_offset != stack_offset) {
            emit_inst2("lea", op_mem("rbp", saved_stack_offset), op_reg("rsp"));
        }
        stack_offset = saved_stack_offset;
        locals_count = saved_locals_count;
        
        emit_inst1("jmp", op_label(l_start));
        
        emit_label_def(l_end);
    } else if (node->type == AST_DO_WHILE) {
        int label_start = label_count++;
        int label_continue = label_count++; // condition check
        int label_end = label_count++;
        char l_start[32], l_cont[32], l_end[32];
        sprintf(l_start, ".L%d", label_start);
        sprintf(l_cont, ".L%d", label_continue);
        sprintf(l_end, ".L%d", label_end);
        
        emit_label_def(l_start);
        
        int saved_stack_offset_dw = stack_offset;
        int saved_locals_count_dw = locals_count;
        
        { int i0 = loop_saved_stack_ptr; loop_saved_stack_offset[i0] = saved_stack_offset_dw; loop_saved_stack_ptr = i0 + 1; }
        { int i1 = break_label_ptr; break_label_stack[i1] = label_end; break_label_ptr = i1 + 1; }
        { int i2 = continue_label_ptr; continue_label_stack[i2] = label_continue; continue_label_ptr = i2 + 1; }
        gen_statement(node->data.while_stmt.body);
        continue_label_ptr--;
        break_label_ptr--;
        loop_saved_stack_ptr--;

        // Restore RSP to loop entry value
        if (saved_stack_offset_dw != stack_offset) {
            emit_inst2("lea", op_mem("rbp", saved_stack_offset_dw), op_reg("rsp"));
        }
        stack_offset = saved_stack_offset_dw;
        locals_count = saved_locals_count_dw;

        emit_label_def(l_cont);
        gen_expression(node->data.while_stmt.condition);
        emit_inst2("cmp", op_imm(0), op_reg("rax"));
        emit_inst1("jne", op_label(l_start));
        
        emit_label_def(l_end);
    } else if (node->type == AST_FOR) {
        int label_start = label_count++;
        int label_continue = label_count++; // increment
        int label_end = label_count++;
        char l_start[32], l_cont[32], l_end[32];
        sprintf(l_start, ".L%d", label_start);
        sprintf(l_cont, ".L%d", label_continue);
        sprintf(l_end, ".L%d", label_end);
        
        if (node->data.for_stmt.init) {
            gen_statement(node->data.for_stmt.init);
        }
        
        emit_label_def(l_start);
        if (node->data.for_stmt.condition) {
            gen_expression(node->data.for_stmt.condition);
            emit_inst2("cmp", op_imm(0), op_reg("rax"));
            emit_inst1("je", op_label(l_end));
        }
        
        int saved_stack_offset_for = stack_offset;
        int saved_locals_count_for = locals_count;
        
        { int i0 = loop_saved_stack_ptr; loop_saved_stack_offset[i0] = saved_stack_offset_for; loop_saved_stack_ptr = i0 + 1; }
        { int i1 = break_label_ptr; break_label_stack[i1] = label_end; break_label_ptr = i1 + 1; }
        { int i2 = continue_label_ptr; continue_label_stack[i2] = label_continue; continue_label_ptr = i2 + 1; }
        gen_statement(node->data.for_stmt.body);
        continue_label_ptr--;
        break_label_ptr--;
        loop_saved_stack_ptr--;
        
        // Restore RSP to loop entry value
        if (saved_stack_offset_for != stack_offset) {
            emit_inst2("lea", op_mem("rbp", saved_stack_offset_for), op_reg("rsp"));
        }
        stack_offset = saved_stack_offset_for;
        locals_count = saved_locals_count_for;
        
        emit_label_def(l_cont);
        if (node->data.for_stmt.increment) {
            gen_expression(node->data.for_stmt.increment);
        }
        emit_inst1("jmp", op_label(l_start));
        
        emit_label_def(l_end);
    } else if (node->type == AST_BREAK) {
        if (break_label_ptr > 0) {
            // Restore RSP to loop entry value before breaking
            if (loop_saved_stack_ptr > 0) {
                int saved = loop_saved_stack_offset[loop_saved_stack_ptr - 1];
                if (saved != stack_offset) {
                    emit_inst2("lea", op_mem("rbp", saved), op_reg("rsp"));
                }
            }
            char l_break[32];
            sprintf(l_break, ".L%d", break_label_stack[break_label_ptr - 1]);
            emit_inst1("jmp", op_label(l_break));
        } else {
            fprintf(stderr, "Error: 'break' outside of loop or switch\n");
        }
    } else if (node->type == AST_CONTINUE) {
        if (continue_label_ptr > 0) {
            // Restore RSP to loop entry value before continuing
            if (loop_saved_stack_ptr > 0) {
                int saved = loop_saved_stack_offset[loop_saved_stack_ptr - 1];
                if (saved != stack_offset) {
                    emit_inst2("lea", op_mem("rbp", saved), op_reg("rsp"));
                }
            }
            char l_cont[32];
            sprintf(l_cont, ".L%d", continue_label_stack[continue_label_ptr - 1]);
            emit_inst1("jmp", op_label(l_cont));
        } else {
            fprintf(stderr, "Error: 'continue' outside of loop\n");
        }
    } else if (node->type == AST_GOTO) {
        emit_inst1("jmp", op_label(node->data.goto_stmt.label));
    } else if (node->type == AST_LABEL) {
        emit_label_def(node->data.label_stmt.name);
    } else if (node->type == AST_SWITCH) {
        gen_expression(node->data.switch_stmt.condition);

        int label_end = label_count++;
        char l_end[32];
        sprintf(l_end, ".L%d", label_end);

        ASTNode *cases[1024];
        int case_count = 0;
        ASTNode *default_node = NULL;
        collect_cases(node->data.switch_stmt.body, cases, &case_count, &default_node);

        char case_labels[1024][32];
        for (int i = 0; i < case_count; i++) {
            sprintf(case_labels[i], ".L%d", label_count++);
            emit_inst2("cmp", op_imm(cases[i]->data.case_stmt.value), op_reg("rax"));
            emit_inst1("je", op_label(case_labels[i]));
            cases[i]->resolved_type = (Type *)_strdup(case_labels[i]);
        }

        if (default_node) {
            char default_label[32];
            sprintf(default_label, ".L%d", label_count++);
            default_node->resolved_type = (Type *)_strdup(default_label);
            emit_inst1("jmp", op_label(default_label));
        } else {
            emit_inst1("jmp", op_label(l_end));
        }

        { int i0 = break_label_ptr; break_label_stack[i0] = label_end; break_label_ptr = i0 + 1; }
        { int i1 = loop_saved_stack_ptr; loop_saved_stack_offset[i1] = stack_offset; loop_saved_stack_ptr = i1 + 1; }
        gen_statement(node->data.switch_stmt.body);
        break_label_ptr--;
        loop_saved_stack_ptr--;

        emit_label_def(l_end);
    } else if (node->type == AST_CASE) {
        if (node->resolved_type) {
            emit_label_def((char *)node->resolved_type);
        }
    } else if (node->type == AST_DEFAULT) {
        if (node->resolved_type) {
            emit_label_def((char *)node->resolved_type);
        }
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
            coff_writer_add_symbol(obj_writer, node->data.function.name, 0, 0, 0x20, IMAGE_SYM_CLASS_EXTERNAL);
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
    emit_inst2("mov", op_reg("rsp"), op_reg("rbp"));
    
    locals_count = 0;
    current_func_return_type = node->resolved_type;
    current_func_name = node->data.function.name;
    stack_offset = 0;
    
    // Handle parameters (platform ABI)
    const char **arg_regs = g_arg_regs;
    const char **xmm_arg_regs = g_xmm_arg_regs;
    int max_reg = g_max_reg_args;
    for (size_t i = 0; i < node->children_count; i++) {
        ASTNode *param = node->children[i];
        if (param->type == AST_VAR_DECL) {
            int size = param->resolved_type ? param->resolved_type->size : 8;
            int alloc_size = size;
            if (alloc_size < 8 && param->resolved_type && param->resolved_type->kind != TYPE_STRUCT && param->resolved_type->kind != TYPE_ARRAY) {
                alloc_size = 8;
            }
            
            if (locals_count >= 8192) { fprintf(stderr, "Error: Too many locals\n"); exit(1); }
            locals[locals_count].name = param->data.var_decl.name;
            locals[locals_count].label = NULL;
            locals[locals_count].type = param->resolved_type;
            
            if ((int)i < max_reg) {
                // Register params: allocate on local stack
                stack_offset -= alloc_size;
                locals[locals_count].offset = stack_offset;
                locals_count++;
                if (is_float_type(param->resolved_type)) {
                    emit_push_xmm(xmm_arg_regs[i]);
                } else {
                    emit_inst2("sub", op_imm(alloc_size), op_reg("rsp"));
                    if (size == 1) emit_inst2("movb", op_reg(get_reg_8(arg_regs[i])), op_mem("rsp", 0));
                    else if (size == 2) emit_inst2("movw", op_reg(get_reg_16(arg_regs[i])), op_mem("rsp", 0));
                    else if (size == 4) emit_inst2("movl", op_reg(get_reg_32(arg_regs[i])), op_mem("rsp", 0));
                    else emit_inst2("mov", op_reg(arg_regs[i]), op_mem("rsp", 0));
                }
            } else {
                // Stack params: already on caller's stack at positive rbp offset
                // Win64: [rbp+16] = shadow[0], [rbp+48] = param5, ...
                // SysV:  [rbp+16] = param7, [rbp+24] = param8, ...
                int param_offset;
                if (g_use_shadow_space) {
                    param_offset = 48 + ((int)i - max_reg) * 8; // Win64
                } else {
                    param_offset = 16 + ((int)i - max_reg) * 8; // SysV
                }
                locals[locals_count].offset = param_offset;
                locals_count++;
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
