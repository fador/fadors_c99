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
static ASTNode *current_program = NULL;
static int label_count = 0;
static CodegenSyntax current_syntax = SYNTAX_ATT;
static Section current_section = SECTION_TEXT;
static Type *current_func_return_type = NULL;
static char *current_func_name = NULL;
static int static_label_count = 0;

static void gen_function(ASTNode *node);
static void gen_statement(ASTNode *node);
static void gen_global_decl(ASTNode *node);
static void emit_inst0(const char *mnemonic);
static void emit_inst1(const char *mnemonic, Operand op1);
static void emit_inst2(const char *mnemonic, Operand op1, Operand op2);
static Type *get_expr_type(ASTNode *node);
static int is_float_type(Type *t);
static Operand op_reg(const char *reg);
static Operand op_imm(int imm);
static Operand op_mem(const char *base, int offset);
static Operand op_label(const char *label);

typedef struct {
    char *label;
    char *value;
    int length;
} StringLiteral;

static StringLiteral string_literals[8192];
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
    current_program = program;
    for (size_t i = 0; i < program->children_count; i++) {
        ASTNode *child = program->children[i];
        if (child->type == AST_FUNCTION) {
            if (child->data.function.name) printf("Codegen: Function %s\n", child->data.function.name);
            gen_function(child);
        } else if (child->type == AST_VAR_DECL) {
            if (child->data.var_decl.name) printf("Codegen: Global %s\n", child->data.var_decl.name);
            gen_global_decl(child);
        }
    }
    if (!obj_writer) {
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
        if (node->data.var_decl.initializer && node->data.var_decl.initializer->type == AST_INTEGER) {
            int val = node->data.var_decl.initializer->data.integer.value;
            buffer_write_bytes(&obj_writer->data_section, &val, size);
        } else if (node->data.var_decl.initializer && node->data.var_decl.initializer->type == AST_FLOAT) {
            double val = node->data.var_decl.initializer->data.float_val.value;
            if (size == 4) {
                float f = (float)val;
                buffer_write_bytes(&obj_writer->data_section, &f, 4);
            } else {
                buffer_write_bytes(&obj_writer->data_section, &val, 8);
            }
        } else {
            // Uninitialized or zero-initialized
            buffer_pad(&obj_writer->data_section, size);
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
            
            if (node->data.var_decl.initializer && node->data.var_decl.initializer->type == AST_INTEGER) {
                fprintf(out, "%s %d\n", directive, node->data.var_decl.initializer->data.integer.value);
            } else if (node->data.var_decl.initializer && node->data.var_decl.initializer->type == AST_FLOAT) {
                fprintf(out, "%s %f\n", directive, node->data.var_decl.initializer->data.float_val.value);
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
             if (node->data.var_decl.initializer && node->data.var_decl.initializer->type == AST_INTEGER) {
                 int val = node->data.var_decl.initializer->data.integer.value;
                 int size = node->resolved_type ? node->resolved_type->size : 4;
                 if (size == 1) fprintf(out, "    .byte %d\n", val);
                 else if (size == 4) fprintf(out, "    .long %d\n", val);
                 else if (size == 8) fprintf(out, "    .quad %d\n", val);
             } else if (node->data.var_decl.initializer && node->data.var_decl.initializer->type == AST_FLOAT) {
                 int size = node->resolved_type ? node->resolved_type->size : 4;
                 if (size == 4) fprintf(out, "    .float %f\n", node->data.var_decl.initializer->data.float_val.value);
                 else fprintf(out, "    .double %f\n", node->data.var_decl.initializer->data.float_val.value);
             } else {
                 int size = node->resolved_type ? node->resolved_type->size : 4;
                 fprintf(out, "    .zero %d\n", size);
             }
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
}

static void emit_pop_xmm(const char *reg) {
    emit_inst2("movsd", op_mem("rsp", 0), op_reg(reg));
    emit_inst2("add", op_imm(8), op_reg("rsp"));
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
        const char *lbl = op.data.label ? op.data.label : "null_label";
        if (current_syntax == SYNTAX_ATT) fprintf(out, "%s(%%rip)", lbl);
        else fprintf(out, "[%s]", lbl);
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
            uint8_t storage_class = (op1.data.label[0] == '.') ? IMAGE_SYM_CLASS_STATIC : IMAGE_SYM_CLASS_EXTERNAL;
            uint16_t type = (strcmp(mnemonic, "call") == 0) ? 0x20 : 0;
            uint32_t sym_idx = coff_writer_add_symbol(obj_writer, op1.data.label, 0, 0, type, storage_class);
            
            uint32_t offset = 1;
            if (strcmp(mnemonic, "je") == 0 || strcmp(mnemonic, "jne") == 0 || 
                strcmp(mnemonic, "jz") == 0 || strcmp(mnemonic, "jnz") == 0 ||
                strcmp(mnemonic, "jb") == 0 || strcmp(mnemonic, "jbe") == 0 ||
                strcmp(mnemonic, "ja") == 0 || strcmp(mnemonic, "jae") == 0) offset = 2;
            else if (strcmp(mnemonic, "call") == 0 || strcmp(mnemonic, "jmp") == 0) offset = 1;
            
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
        else if (strcmp(mnemonic, "not") == 0) m = "not";
    }

    fprintf(out, "    %s ", m);
    print_operand(op1);
    fprintf(out, "\n");
}

static void emit_inst2(const char *mnemonic, Operand op1, Operand op2) {
    if (obj_writer) {
        int offset_in_inst = 3; // REX + Opcode + ModRM (general)
        if (strcmp(mnemonic, "movss") == 0 || strcmp(mnemonic, "movsd") == 0) {
            int reg_id = -1;
            if (op1.type == OP_REG) reg_id = get_reg_id(op1.data.reg);
            else if (op2.type == OP_REG) reg_id = get_reg_id(op2.data.reg);
            
            // F3/F2 (1) + REX (if reg >= 8) (1) + 0F 10/11 (2) + ModRM (1)
            offset_in_inst = 4;
            if (reg_id >= 8) offset_in_inst = 5;
        }

        if (op1.type == OP_LABEL) {
            uint8_t storage_class = (op1.data.label[0] == '.') ? IMAGE_SYM_CLASS_STATIC : IMAGE_SYM_CLASS_EXTERNAL;
            uint32_t sym_idx = coff_writer_add_symbol(obj_writer, op1.data.label, 0, 0, 0, storage_class);
            coff_writer_add_reloc(obj_writer, (uint32_t)obj_writer->text_section.size + offset_in_inst, sym_idx, IMAGE_REL_AMD64_REL32);
        }
        if (op2.type == OP_LABEL) {
            uint8_t storage_class = (op2.data.label[0] == '.') ? IMAGE_SYM_CLASS_STATIC : IMAGE_SYM_CLASS_EXTERNAL;
            uint32_t sym_idx = coff_writer_add_symbol(obj_writer, op2.data.label, 0, 0, 0, storage_class);
            coff_writer_add_reloc(obj_writer, (uint32_t)obj_writer->text_section.size + offset_in_inst, sym_idx, IMAGE_REL_AMD64_REL32);
        }
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
        if (strcmp(mnemonic, "movzbq") == 0 && op1.type == OP_MEM) {
            fprintf(out, "byte ptr ");
        }
        print_operand(op1);
    }
    fprintf(out, "\n");
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
            else { fprintf(stderr, "      Member: arrow on non-ptr!\n"); return; }
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
            fprintf(stderr, "      Member: incomplete struct type!\n");
        }
    } else if (node->type == AST_ARRAY_ACCESS) {
        if (!node->data.array_access.array || !node->data.array_access.index) { fprintf(stderr, "      Array: NULL child!\n"); return; }
        gen_expression(node->data.array_access.array);
        emit_inst1("pushq", op_reg("rax"));
        gen_expression(node->data.array_access.index);
        
        // Element size calculation
        Type *array_type = (node->data.array_access.array) ? node->data.array_access.array->resolved_type : NULL;
        if (!array_type && node->data.array_access.array) array_type = get_expr_type(node->data.array_access.array);

        int element_size = 8;
        if (array_type) {
             if (array_type->kind == TYPE_PTR || array_type->kind == TYPE_ARRAY) {
                 if (array_type->data.ptr_to) element_size = array_type->data.ptr_to->size;
             }
        }
        
        emit_inst2("imul", op_imm(element_size), op_reg("rax"));
        emit_inst1("popq", op_reg("rcx"));
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
    gen_expression(node->data.binary_expr.left);
    emit_inst1("popq", op_reg("rcx"));
    
    Type *left_type = node->data.binary_expr.left->resolved_type;
    Type *right_type = node->data.binary_expr.right->resolved_type;
    
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
            emit_inst2("add", op_reg("rcx"), op_reg("rax")); 
            break;
        case TOKEN_MINUS: 
            if ((left_type && (left_type->kind == TYPE_PTR || left_type->kind == TYPE_ARRAY)) && 
                (right_type && (right_type->kind == TYPE_INT || right_type->kind == TYPE_CHAR))) {
                if (size > 1) emit_inst2("imul", op_imm(size), op_reg("rcx"));
                emit_inst2("sub", op_reg("rcx"), op_reg("rax"));
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
                emit_inst2("sub", op_reg("rcx"), op_reg("rax"));
                node->resolved_type = left_type;
            }
            break;
        case TOKEN_STAR:  
            emit_inst2("imul", op_reg("rcx"), op_reg("rax")); 
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
            emit_inst2("and", op_reg("rcx"), op_reg("rax"));
            node->resolved_type = left_type;
            break;
        case TOKEN_PIPE:
            emit_inst2("or", op_reg("rcx"), op_reg("rax"));
            node->resolved_type = left_type;
            break;
        case TOKEN_CARET:
            emit_inst2("xor", op_reg("rcx"), op_reg("rax"));
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
            emit_inst2("cmp", op_reg("rcx"), op_reg("rax"));
            emit_inst1("sete", op_reg("al"));
            emit_inst2("movzbq", op_reg("al"), op_reg("rax"));
            break;
        case TOKEN_BANG_EQUAL:
            emit_inst2("cmp", op_reg("rcx"), op_reg("rax"));
            emit_inst1("setne", op_reg("al"));
            emit_inst2("movzbq", op_reg("al"), op_reg("rax"));
            break;
        case TOKEN_LESS:
            emit_inst2("cmp", op_reg("rcx"), op_reg("rax"));
            emit_inst1("setl", op_reg("al"));
            emit_inst2("movzbq", op_reg("al"), op_reg("rax"));
            break;
        case TOKEN_GREATER:
            emit_inst2("cmp", op_reg("rcx"), op_reg("rax"));
            emit_inst1("setg", op_reg("al"));
            emit_inst2("movzbq", op_reg("al"), op_reg("rax"));
            break;
        case TOKEN_LESS_EQUAL:
            emit_inst2("cmp", op_reg("rcx"), op_reg("rax"));
            emit_inst1("setle", op_reg("al"));
            emit_inst2("movzbq", op_reg("al"), op_reg("rax"));
            break;
        case TOKEN_GREATER_EQUAL:
            emit_inst2("cmp", op_reg("rcx"), op_reg("rax"));
            emit_inst1("setge", op_reg("al"));
            emit_inst2("movzbq", op_reg("al"), op_reg("rax"));
            break;
        default: break;
    }
}

static void gen_expression(ASTNode *node) {
    if (!node) return;
    // fprintf(stderr, "      GenExpr type=%d\n", node->type);
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
        fprintf(stderr, "      Ident: %s\n", node->data.identifier.name);
        const char *label = get_local_label(node->data.identifier.name);
        if (label) {
            fprintf(stderr, "      Ident: local label %s\n", label);
            Type *t = get_local_type(node->data.identifier.name);
            if (t && t->kind == TYPE_ARRAY) {
                emit_inst2("lea", op_label(label), op_reg("rax"));
            } else if (is_float_type(t)) {
                if (t->kind == TYPE_FLOAT) emit_inst2("movss", op_label(label), op_reg("xmm0"));
                else emit_inst2("movsd", op_label(label), op_reg("xmm0"));
            } else {
                emit_inst2("mov", op_label(label), op_reg("rax"));
            }
            node->resolved_type = t;
            return;
        }
        int offset = get_local_offset(node->data.identifier.name);
        if (offset != 0) {
            fprintf(stderr, "      Ident: local offset %d\n", offset);
            Type *t = get_local_type(node->data.identifier.name);
            fprintf(stderr, "      Ident: type %p\n", (void*)t);
            if (t && t->kind == TYPE_ARRAY) {
                // Array decays to pointer
                emit_inst2("lea", op_mem("rbp", offset), op_reg("rax"));
            } else if (is_float_type(t)) {
                fprintf(stderr, "      Ident: float type kind=%d\n", t->kind);
                if (t->kind == TYPE_FLOAT) emit_inst2("movss", op_mem("rbp", offset), op_reg("xmm0"));
                else emit_inst2("movsd", op_mem("rbp", offset), op_reg("xmm0"));
            } else {
                emit_inst2("mov", op_mem("rbp", offset), op_reg("rax"));
            }
            node->resolved_type = t;
        } else {
            // Global
            fprintf(stderr, "      Ident: global %s\n", node->data.identifier.name);
            Type *t = get_global_type(node->data.identifier.name);
            fprintf(stderr, "      Ident: global type %p\n", (void*)t);
            if (t && t->kind == TYPE_ARRAY) {
                emit_inst2("lea", op_label(node->data.identifier.name), op_reg("rax"));
            } else if (is_float_type(t)) {
                if (t->kind == TYPE_FLOAT) emit_inst2("movss", op_label(node->data.identifier.name), op_reg("xmm0"));
                else emit_inst2("movsd", op_label(node->data.identifier.name), op_reg("xmm0"));
            } else {
                emit_inst2("mov", op_label(node->data.identifier.name), op_reg("rax"));
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
        } else {
            emit_inst2("mov", op_mem("rax", 0), op_reg("rcx"));
        }
        
        if (!is_pre) {
            emit_inst1("pushq", op_reg("rcx")); // Save original value
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
        } else {
            emit_inst2("mov", op_reg("rcx"), op_mem("rax", 0));
        }
        
        if (!is_pre) {
            emit_inst1("popq", op_reg("rax")); // Restore original value to result register
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
        Type *t = get_expr_type(node->data.assign.left);
        if (node->data.assign.left->type == AST_IDENTIFIER) {
            fprintf(stderr, "      Assign Ident: %s\n", node->data.assign.left->data.identifier.name ? node->data.assign.left->data.identifier.name : "NULL");
            const char *label = get_local_label(node->data.assign.left->data.identifier.name);
            if (label) {
                if (is_float_type(t)) {
                    if (t && t->kind == TYPE_FLOAT) emit_inst2("movss", op_reg("xmm0"), op_label(label));
                    else emit_inst2("movsd", op_reg("xmm0"), op_label(label));
                } else {
                    emit_inst2("mov", op_reg("rax"), op_label(label));
                }
                return;
            }
            int offset = get_local_offset(node->data.assign.left->data.identifier.name);
            if (offset != 0) {
                if (is_float_type(t)) {
                    if (t && t->kind == TYPE_FLOAT) emit_inst2("movss", op_reg("xmm0"), op_mem("rbp", offset));
                    else emit_inst2("movsd", op_reg("xmm0"), op_mem("rbp", offset));
                } else {
                    emit_inst2("mov", op_reg("rax"), op_mem("rbp", offset));
                }
            } else if (node->data.assign.left->data.identifier.name) {
                // Global
                if (is_float_type(t)) {
                    if (t && t->kind == TYPE_FLOAT) emit_inst2("movss", op_reg("xmm0"), op_label(node->data.assign.left->data.identifier.name));
                    else emit_inst2("movsd", op_reg("xmm0"), op_label(node->data.assign.left->data.identifier.name));
                } else {
                    emit_inst2("mov", op_reg("rax"), op_label(node->data.assign.left->data.identifier.name));
                }
            }
        } else {
            fprintf(stderr, "      Assign complex L-value\n");
            if (is_float_type(t)) {
                emit_push_xmm("xmm0");
                gen_addr(node->data.assign.left);
                emit_pop_xmm("xmm1");
                if (t && t->kind == TYPE_FLOAT) emit_inst2("movss", op_reg("xmm1"), op_mem("rax", 0));
                else emit_inst2("movsd", op_reg("xmm1"), op_mem("rax", 0));
                if (t && t->kind == TYPE_FLOAT) emit_inst2("movss", op_reg("xmm1"), op_reg("xmm0"));
                else emit_inst2("movsd", op_reg("xmm1"), op_reg("xmm0"));
            } else {
                emit_inst1("pushq", op_reg("rax"));
                gen_addr(node->data.assign.left);
                emit_inst1("popq", op_reg("rcx"));
                emit_inst2("mov", op_reg("rcx"), op_mem("rax", 0));
                emit_inst2("mov", op_reg("rcx"), op_reg("rax"));
            }
        }
        node->resolved_type = t;
    } else if (node->type == AST_DEREF) {
        gen_expression(node->data.unary.expression);
        Type *t = node->data.unary.expression->resolved_type;
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
        node->resolved_type = node->data.unary.expression->resolved_type;
    } else if (node->type == AST_MEMBER_ACCESS) {
        gen_addr(node);
        emit_inst2("mov", op_mem("rax", 0), op_reg("rax"));
    } else if (node->type == AST_CALL) {
        // Save initial stack offset to restore later
        int initial_stack_offset = stack_offset;
        
        const char *arg_regs[] = {"rcx", "rdx", "r8", "r9"};
        const char *xmm_arg_regs[] = {"xmm0", "xmm1", "xmm2", "xmm3"};
        
        int num_args = (int)node->children_count;
        int extra_args = num_args > 4 ? num_args - 4 : 0;
        
        // Calculate padding based on CURRENT stack depth (including any pushed args from outer calls)
        int current_stack_depth = abs(stack_offset);
        int padding = (16 - ((current_stack_depth + extra_args * 8 + 32) % 16)) % 16;
        
        if (padding > 0) {
            emit_inst2("sub", op_imm(padding), op_reg("rsp"));
            stack_offset -= padding;
        }

        for (size_t i = 0; i < node->children_count; i++) {
            gen_expression(node->children[i]);
            if (is_float_type(node->children[i]->resolved_type)) {
                emit_push_xmm("xmm0");
            } else {
                emit_inst1("pushq", op_reg("rax"));
            }
            stack_offset -= 8; // Update stack offset for nested calls
        }
        
        for (int i = 0; i < (int)num_args && i < 4; i++) {
            if (is_float_type(node->children[i]->resolved_type)) {
                emit_pop_xmm(xmm_arg_regs[i]);
            } else {
                emit_inst1("popq", op_reg(arg_regs[i]));
            }
        }
        
        // Win64 requires 32 bytes of shadow space
        emit_inst2("sub", op_imm(32), op_reg("rsp"));
        emit_inst1("call", op_label(node->data.call.name));
        if (node->resolved_type == NULL) node->resolved_type = get_expr_type(node);
        
        // Clean up shadow space + extra args + padding
        emit_inst2("add", op_imm(32 + extra_args * 8 + padding), op_reg("rsp"));
        
        // Restore stack offset
        stack_offset = initial_stack_offset;
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

static void collect_cases(ASTNode *node, ASTNode **cases, int *case_count, ASTNode **default_node) {
    if (!node) return;
    if (node->type == AST_CASE) {
        cases[(*case_count)++] = node;
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
                if (node->data.var_decl.initializer && node->data.var_decl.initializer->type == AST_INTEGER) {
                    val = node->data.var_decl.initializer->data.integer.value;
                }
                buffer_write_bytes(&obj_writer->data_section, &val, size);
            } else {
                if (current_syntax == SYNTAX_INTEL) {
                    fprintf(out, "_TEXT ENDS\n_DATA SEGMENT\n");
                    emit_label_def(slabel);
                    int size = node->resolved_type ? node->resolved_type->size : 8;
                    int val = 0;
                    if (node->data.var_decl.initializer && node->data.var_decl.initializer->type == AST_INTEGER) {
                        val = node->data.var_decl.initializer->data.integer.value;
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
                    if (node->data.var_decl.initializer && node->data.var_decl.initializer->type == AST_INTEGER) {
                        val = node->data.var_decl.initializer->data.integer.value;
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
        
        if (node->data.var_decl.initializer && node->data.var_decl.initializer->type == AST_INIT_LIST) {
            // Initializer list: {expr, expr, ...}
            ASTNode *init_list = node->data.var_decl.initializer;
            
            if (size < 8 && node->resolved_type && node->resolved_type->kind != TYPE_STRUCT && node->resolved_type->kind != TYPE_ARRAY) {
                size = 8;
            }
            stack_offset -= size;
            
            locals[locals_count].name = node->data.var_decl.name;
            locals[locals_count].offset = stack_offset;
            locals[locals_count].label = NULL;
            locals[locals_count].type = node->resolved_type;
            locals_count++;
            
            // Allocate space on stack
            emit_inst2("sub", op_imm(size), op_reg("rsp"));
            
            // Zero-initialize with qword stores
            {
                int off;
                for (off = 0; off + 8 <= size; off += 8) {
                    emit_inst2("movq", op_imm(0), op_mem("rbp", stack_offset + off));
                }
                if (off + 4 <= size) {
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
            
            if (size < 8 && node->resolved_type && node->resolved_type->kind != TYPE_STRUCT && node->resolved_type->kind != TYPE_ARRAY) {
                size = 8;
            }
            stack_offset -= size;
            fprintf(stderr, "    Var: %s offset=%d size=%d\n", node->data.var_decl.name ? node->data.var_decl.name : "NULL", stack_offset, size);
            
            if (locals_count >= 8192) { fprintf(stderr, "Error: Too many locals\n"); exit(1); }
            locals[locals_count].name = node->data.var_decl.name;
            locals[locals_count].offset = stack_offset;
            locals[locals_count].label = NULL;
            locals[locals_count].type = node->resolved_type;
            locals_count++;
            
            if (is_float_type(node->resolved_type)) {
                emit_push_xmm("xmm0");
            } else if (node->resolved_type && (node->resolved_type->kind == TYPE_STRUCT || node->resolved_type->kind == TYPE_ARRAY)) {
                emit_inst2("sub", op_imm(size), op_reg("rsp"));
            } else {
                emit_inst1("pushq", op_reg("rax"));
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
        
        // Generate then branch
        if (node->data.if_stmt.then_branch) {
            gen_statement(node->data.if_stmt.then_branch);
        }
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
        emit_inst2("cmp", op_imm(0), op_reg("rax"));
        emit_inst1("je", op_label(l_end));
        
        break_label_stack[break_label_ptr++] = label_end;
        continue_label_stack[continue_label_ptr++] = label_start;
        gen_statement(node->data.while_stmt.body);
        continue_label_ptr--;
        break_label_ptr--;
        
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
        
        break_label_stack[break_label_ptr++] = label_end;
        continue_label_stack[continue_label_ptr++] = label_continue;
        gen_statement(node->data.while_stmt.body);
        continue_label_ptr--;
        break_label_ptr--;

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
        
        break_label_stack[break_label_ptr++] = label_end;
        continue_label_stack[continue_label_ptr++] = label_continue;
        gen_statement(node->data.for_stmt.body);
        continue_label_ptr--;
        break_label_ptr--;
        
        emit_label_def(l_cont);
        if (node->data.for_stmt.increment) {
            gen_expression(node->data.for_stmt.increment);
        }
        emit_inst1("jmp", op_label(l_start));
        
        emit_label_def(l_end);
    } else if (node->type == AST_BREAK) {
        if (break_label_ptr > 0) {
            char l_break[32];
            sprintf(l_break, ".L%d", break_label_stack[break_label_ptr - 1]);
            emit_inst1("jmp", op_label(l_break));
        } else {
            fprintf(stderr, "Error: 'break' outside of loop or switch\n");
        }
    } else if (node->type == AST_CONTINUE) {
        if (continue_label_ptr > 0) {
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

        break_label_stack[break_label_ptr++] = label_end;
        gen_statement(node->data.switch_stmt.body);
        break_label_ptr--;

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
    
    // Handle parameters (Win64 ABI)
    const char *arg_regs[] = {"rcx", "rdx", "r8", "r9"};
    const char *xmm_arg_regs[] = {"xmm0", "xmm1", "xmm2", "xmm3"};
    for (size_t i = 0; i < node->children_count; i++) {
        ASTNode *param = node->children[i];
        if (param->type == AST_VAR_DECL) {
            int size = param->resolved_type ? param->resolved_type->size : 8;
            if (size < 8 && param->resolved_type && param->resolved_type->kind != TYPE_STRUCT && param->resolved_type->kind != TYPE_ARRAY) {
                size = 8;
            }
            stack_offset -= size;
            
            if (locals_count >= 8192) { fprintf(stderr, "Error: Too many locals\n"); exit(1); }
            locals[locals_count].name = param->data.var_decl.name;
            locals[locals_count].offset = stack_offset;
            locals[locals_count].label = NULL;
            locals[locals_count].type = param->resolved_type;
            locals_count++;
            
            if (i < 4U) {
                if (is_float_type(param->resolved_type)) {
                    emit_push_xmm(xmm_arg_regs[i]);
                } else {
                    emit_inst1("pushq", op_reg(arg_regs[i]));
                }
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
