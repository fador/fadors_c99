#define _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#define _CRT_NONSTDC_NO_DEPRECATE
#include "arch_x86.h"
#include "codegen.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>



#include "encoder.h"

/* ---- PGO instrumentation tracking ---- */
#define PGO_MAX_PROBES 4096
#define PGO_NAME_LEN   64

typedef struct {
    char name[PGO_NAME_LEN]; /* function name or "func:BnT"/"func:BnN" */
} PGOProbeInfo;

static PGOProbeInfo pgo_probes[PGO_MAX_PROBES];
static int pgo_probe_count = 0;
static int pgo_func_branch_id = 0; /* per-function branch counter */

/* Allocate a new PGO probe and return its index */
static int pgo_alloc_probe(const char *name) {
    if (pgo_probe_count >= PGO_MAX_PROBES) return -1;
    int id = pgo_probe_count++;
    strncpy(pgo_probes[id].name, name, PGO_NAME_LEN - 1);
    pgo_probes[id].name[PGO_NAME_LEN - 1] = '\0';
    return id;
}
/* ---- end PGO tracking ---- */

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
static int debug_last_line = 0;  /* last line emitted for debug tracking */
static int sret_offset = 0;      /* stack offset where hidden return pointer is saved (struct returns) */

/* Check if a type requires struct-return ABI (hidden pointer) */
static int is_struct_return(Type *t) {
    return t && (t->kind == TYPE_STRUCT || t->kind == TYPE_UNION);
}

/* Record a debug line entry if -g is active and the source line changed */
static void debug_record_line(ASTNode *node) {
    if (!obj_writer || !g_compiler_options.debug_info) return;
    if (!node || node->line <= 0) return;
    if ((int)node->line == debug_last_line) return;
    debug_last_line = (int)node->line;
    coff_writer_add_debug_line(obj_writer,
                               (uint32_t)obj_writer->text_section.size,
                               (uint32_t)node->line, 1);
}

/* Map a Type* to a DebugTypeKind for the debug info pipeline */
static uint8_t debug_type_kind(Type *t) {
    if (!t) return DBG_TYPE_VOID;
    switch (t->kind) {
        case TYPE_VOID:      return DBG_TYPE_VOID;
        case TYPE_CHAR:      return DBG_TYPE_CHAR;
        case TYPE_SHORT:     return DBG_TYPE_SHORT;
        case TYPE_INT:       return DBG_TYPE_INT;
        case TYPE_LONG:      return DBG_TYPE_LONG;
        case TYPE_LONG_LONG: return DBG_TYPE_LONGLONG;
        case TYPE_FLOAT:     return DBG_TYPE_FLOAT;
        case TYPE_DOUBLE:    return DBG_TYPE_DOUBLE;
        case TYPE_PTR:       return DBG_TYPE_PTR;
        case TYPE_ARRAY:     return DBG_TYPE_ARRAY;
        case TYPE_STRUCT:    return DBG_TYPE_STRUCT;
        case TYPE_UNION:     return DBG_TYPE_UNION;
        case TYPE_ENUM:      return DBG_TYPE_ENUM;
        default:             return DBG_TYPE_INT;
    }
}

/* Get the struct/union/enum name from a Type*, or NULL */
static const char *debug_type_name(Type *t) {
    if (!t) return NULL;
    if (t->kind == TYPE_STRUCT || t->kind == TYPE_UNION || t->kind == TYPE_ENUM)
        return t->data.struct_data.name;
    return NULL;
}

/* Record a local variable or parameter for debug info */
static void debug_record_var(const char *name, int ebp_offset, int is_param, Type *t) {
    if (!obj_writer || !g_compiler_options.debug_info) return;
    if (!name) return;
    coff_writer_add_debug_var(obj_writer, name, (int32_t)ebp_offset,
                               (uint8_t)is_param, debug_type_kind(t),
                               t ? (int32_t)t->size : 0,
                               debug_type_name(t));
}

// ABI register parameter arrays
static const char *g_arg_regs[6];
static const char *g_xmm_arg_regs[8];
static int g_max_reg_args = 4;       // 4 for Win64, 6 for SysV
static int g_use_shadow_space = 1;   // 1 for Win64, 0 for SysV

static TargetPlatform g_target =
#ifdef _WIN32
    TARGET_WINDOWS;
#else
    TARGET_LINUX;
#endif

static void gen_function(ASTNode *node);
static void gen_statement(ASTNode *node);
static void gen_global_decl(ASTNode *node);
static void emit_inst0(const char *mnemonic);
static void emit_inst1(const char *mnemonic, Operand *op1);
static void emit_inst2(const char *mnemonic, Operand *op1, Operand *op2);
static void emit_inst3(const char *mnemonic, Operand *op1, Operand *op2, Operand *op3);
static Type *get_expr_type(ASTNode *node);
static int is_float_type(Type *t);
static void last_value_clear(void);

// Peephole optimization state: eliminates redundant jumps and dead code
static int peep_unreachable = 0;     // 1 after unconditional jmp (suppress dead code)
static int peep_pending_jmp = 0;     // 1 if there's a buffered jmp to emit
static char peep_jmp_target[64];     // target label of the buffered jmp
static int peep_in_flush = 0;        // prevent recuesion during flush

// Peephole: buffer push for push/pop → mov optimization
static int peep_pending_push = 0;    // 1 if there's a buffered push to emit
static char peep_push_reg[16];       // register name of the buffered push

// Branch optimization: buffer jcc for jcc-over-jmp pattern
// Detects: jcc L1; jmp L2; L1: → inverted-jcc L2
// State machine: buffer jcc, then if jmp follows, store both as a candidate pair.
// Only invert when emit_label_def sees L1 as the very next label.
static int peep_pending_jcc = 0;     // 1 if there's a buffered conditional jump
static char peep_jcc_mnemonic[16];   // mnemonic of the buffered jcc (e.g., "je")
static char peep_jcc_target[64];     // target label of the buffered jcc
// Combined jcc-over-jmp candidate pair:
static int peep_jcc_jmp_pair = 0;    // 1 if we have a jcc+jmp pair waiting for label confirmation
static char peep_pair_jcc_mn[16];    // jcc mnemonic in the pair
static char peep_pair_jcc_tgt[64];   // jcc target (L1 — must match next label)
static char peep_pair_jmp_tgt[64];   // jmp target (L2 — becomes inverted jcc target)

// Peephole: setcc + movzbl + test + jcc → direct jcc
// Eliminates 3 redundant instructions per boolean comparison branch.
// State: 0=idle, 1=saw "setCC %al", 2=also saw "movzbl %al,%eax", 3=also saw "test %eax,%eax"
static int peep_setcc_state = 0;
static char peep_setcc_cond[16];     // condition suffix (e.g., "e", "ne", "l", "ge")

static const char *peep_invert_jcc(const char *jcc) {
    if (strcmp(jcc, "je")  == 0) return "jne";
    if (strcmp(jcc, "jne") == 0) return "je";
    if (strcmp(jcc, "jl")  == 0) return "jge";
    if (strcmp(jcc, "jge") == 0) return "jl";
    if (strcmp(jcc, "jg")  == 0) return "jle";
    if (strcmp(jcc, "jle") == 0) return "jg";
    if (strcmp(jcc, "ja")  == 0) return "jbe";
    if (strcmp(jcc, "jbe") == 0) return "ja";
    if (strcmp(jcc, "jae") == 0) return "jb";
    if (strcmp(jcc, "jb")  == 0) return "jae";
    if (strcmp(jcc, "jz")  == 0) return "jnz";
    if (strcmp(jcc, "jnz") == 0) return "jz";
    return NULL;  // unknown — can't invert
}

static void peep_flush_jcc(void) {
    if (peep_pending_jcc) {
        peep_pending_jcc = 0;
        peep_in_flush = 1;
        Operand flush_op;
        flush_op.type = OP_LABEL;
        flush_op.data.label = peep_jcc_target;
        emit_inst1(peep_jcc_mnemonic, &flush_op);
        peep_in_flush = 0;
    }
}

/* Flush a pending jcc+jmp pair without optimization (pattern didn't match) */
static void peep_flush_pair(void) {
    if (peep_jcc_jmp_pair) {
        peep_jcc_jmp_pair = 0;
        peep_in_flush = 1;
        Operand op1;
        op1.type = OP_LABEL;
        op1.data.label = peep_pair_jcc_tgt;
        emit_inst1(peep_pair_jcc_mn, &op1);
        Operand op2;
        op2.type = OP_LABEL;
        op2.data.label = peep_pair_jmp_tgt;
        emit_inst1("jmp", &op2);
        peep_in_flush = 0;
    }
}

static void peep_flush_jmp(void) {
    if (peep_pending_jmp) {
        peep_pending_jmp = 0;
        peep_in_flush = 1;
        Operand flush_op;
        flush_op.type = OP_LABEL;
        flush_op.data.label = peep_jmp_target;
        emit_inst1("jmp", &flush_op);
        peep_in_flush = 0;
    }
}

static Operand _op_pool[16];
static int _op_idx = 0;

static Operand *op_reg(const char *reg) {
    Operand *op = &_op_pool[_op_idx++ & 15];
    op->type = OP_REG; op->data.reg = reg; return op;
}
static Operand *op_imm(long long imm) {
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

static Operand *op_sib(const char *base, const char *index, int scale, int disp) {
    Operand *op = &_op_pool[_op_idx++ & 15];
    op->type = OP_MEM_SIB;
    op->data.sib.base = base;
    op->data.sib.index = index;
    op->data.sib.scale = scale;
    op->data.sib.disp = disp;
    return op;
}

/* Map 32-bit register name to 64-bit equivalent for SIB addressing */
static const char *dos_reg_to_64bit(const char *reg) {
    if (strcmp(reg, "eax") == 0) return "eax";
    if (strcmp(reg, "ecx") == 0) return "ecx";
    if (strcmp(reg, "edx") == 0) return "edx";
    if (strcmp(reg, "ebx") == 0) return "ebx";
    if (strcmp(reg, "esi") == 0) return "esi";
    if (strcmp(reg, "edi") == 0) return "edi";
    if (strcmp(reg, "esp") == 0) return "esp";
    if (strcmp(reg, "ebp") == 0) return "ebp";
    /* r8d-r15d → r8-r15 */
    if (reg[0] == 'r' && reg[1] >= '0' && reg[1] <= '9') {
        static char buf[8];
        int i = 0;
        while (reg[i] && reg[i] != 'd' && reg[i] != 'w' && reg[i] != 'b' && i < 6) {
            buf[i] = reg[i]; i++;
        }
        buf[i] = '\0';
        return buf;
    }
    return reg; /* already 64-bit */
}

/* Flush a pending push without optimization (no matching pop followed) */
static void peep_flush_push(void) {
    if (peep_pending_push) {
        peep_pending_push = 0;
        peep_in_flush = 1;
        emit_inst1("push", op_reg(peep_push_reg));
        peep_in_flush = 0;
    }
}

/* Flush a pending setcc chain without optimization */
static void peep_flush_setcc(void) {
    if (peep_setcc_state == 0) return;
    int saved_state = peep_setcc_state;
    peep_setcc_state = 0;
    peep_in_flush = 1;
    char mn[16];
    snprintf(mn, sizeof(mn), "set%s", peep_setcc_cond);
    emit_inst1(mn, op_reg("al"));
    if (saved_state >= 2)
        emit_inst2("movzbl", op_reg("al"), op_reg("eax"));
    if (saved_state >= 3)
        emit_inst2("test", op_reg("eax"), op_reg("eax"));
    peep_in_flush = 0;
}

typedef struct {
    char *label;
    char *value;
    int length;
} StringLiteral;

static StringLiteral string_literals[8192];
static int string_literals_count = 0;

void arch_x86_set_writer(COFFWriter *writer) {
    obj_writer = writer;
    coff_writer_set_machine(writer, IMAGE_FILE_MACHINE_I386);
    encoder_set_writer(writer);
}

static void emit_label_def_ex(const char *name, int is_static);

static void emit_label_def(const char *name) {
    emit_label_def_ex(name, name[0] == '.');
}

static void emit_label_def_ex(const char *name, int is_static) {
    // Peephole: only apply to text section labels
    if (current_section == SECTION_TEXT) {
        peep_flush_setcc();
        peep_flush_push();
        // Resolve pending jcc+jmp pair: jcc L1; jmp L2; L1: → j!cc L2
        if (peep_jcc_jmp_pair) {
            const char *cmp_name = name;
            if (current_syntax == SYNTAX_INTEL && name[0] == '.') cmp_name = name + 1;
            if (strcmp(cmp_name, peep_pair_jcc_tgt) == 0) {
                /* Pattern confirmed: jcc L1; jmp L2; L1: → emit inverted-jcc L2 */
                const char *inv = peep_invert_jcc(peep_pair_jcc_mn);
                peep_jcc_jmp_pair = 0;
                if (inv) {
                    peep_in_flush = 1;
                    Operand inv_op;
                    inv_op.type = OP_LABEL;
                    inv_op.data.label = peep_pair_jmp_tgt;
                    emit_inst1(inv, &inv_op);
                    peep_in_flush = 0;
                } else {
                    /* Can't invert — flush both original instructions */
                    peep_in_flush = 1;
                    Operand op1;
                    op1.type = OP_LABEL;
                    op1.data.label = peep_pair_jcc_tgt;
                    emit_inst1(peep_pair_jcc_mn, &op1);
                    Operand op2;
                    op2.type = OP_LABEL;
                    op2.data.label = peep_pair_jmp_tgt;
                    emit_inst1("jmp", &op2);
                    peep_in_flush = 0;
                }
            } else {
                peep_flush_pair();  // different label — pattern didn't match, flush both
            }
        }
        // Flush or cancel pending conditional jump
        if (peep_pending_jcc) {
            const char *cmp_name = name;
            if (current_syntax == SYNTAX_INTEL && name[0] == '.') cmp_name = name + 1;
            if (strcmp(cmp_name, peep_jcc_target) == 0) {
                peep_pending_jcc = 0;  // jcc to next instruction — always taken or no-op, cancel it
            } else {
                peep_flush_jcc();      // different label — flush the pending jcc
            }
        }
        if (peep_pending_jmp) {
            const char *cmp_name = name;
            if (current_syntax == SYNTAX_INTEL && name[0] == '.') cmp_name = name + 1;
            if (strcmp(cmp_name, peep_jmp_target) == 0) {
                peep_pending_jmp = 0;  // jmp to next instruction — redundant, cancel it
            } else {
                peep_flush_jmp();      // different label — flush the pending jmp
            }
        }
        peep_unreachable = 0;  // label is a potential jump target, code is reachable again
        last_value_clear();
    }

    if (obj_writer) {
        uint8_t storage_class = is_static ? IMAGE_SYM_CLASS_STATIC : IMAGE_SYM_CLASS_EXTERNAL;
        
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

void arch_x86_init(FILE *output) {
    out = output;
    if (g_target == TARGET_DOS) {
        // MS-DOS 32-bit (djgpp/watcom style flat model)
        // cdecl: args on stack
        g_max_reg_args = 0;
        g_use_shadow_space = 0;
    } else if (g_target == TARGET_WINDOWS) {
        // Win64 ABI
        g_arg_regs[0] = "ecx";
        g_arg_regs[1] = "edx";
        g_arg_regs[2] = "r8";
        g_arg_regs[3] = "r9";
        g_xmm_arg_regs[0] = "xmm0";
        g_xmm_arg_regs[1] = "xmm1";
        g_xmm_arg_regs[2] = "xmm2";
        g_xmm_arg_regs[3] = "xmm3";
        g_max_reg_args = 4;
        g_use_shadow_space = 1;
    } else {
        // System V AMD64 ABI (Linux/macOS)
        g_arg_regs[0] = "edi";
        g_arg_regs[1] = "esi";
        g_arg_regs[2] = "edx";
        g_arg_regs[3] = "ecx";
        /* 32-bit only has 4 arg regs usually? actually fastcall/regparm might use eax/edx/ecx.
           SysV i386 ABI passes args on STACK.
           Watcom/fastcall passes in registers.
           We are defining our own internal ABI or following a standard?
           If we use internal ABI, we can use whatever.
           But standard C on DOS (e.g. DJGPP) uses stack (cdecl).
           If we want to link with libc, we need cdecl.
           Let's switch to CDECL (stack) for DOS by default?
           Or support `__cdecl`.
           If `g_arg_regs` are defined, `gen_function` will use them for parameters?
           No, `gen_function` checks `current_func_type->attr`?
           Actually `assign_arg_regs` uses these.
           For 32-bit x86, usually ALL args are on stack (except maybe fastcall).
           If we set g_arg_regs to empty, maybe it uses stack?
           But for now lets keep using registers if our codegen supports it internally, 
           but external calls might break.
           Wait, `main.c` doesn't set ABI.
           If we want standard DOS object files, we should probably stick to CDECL.
           So `g_arg_regs` should be EMPTY?
           Let's comment them out or set to NULL?
           Actually, let's keep EAX/EDX/ECX for scratch, but args on stack.
           Wait, `assign_arg_regs` logic:
           for (int i=0; i<num_args; i++) { if (i < max_reg_args) assign reg... }
           So if I set `g_max_reg_args = 0`, it will put everything on stack.
           
           Let's look at `arch_x86_init`:
           g_max_reg_args = 6; (copied from x64)
           I should change this to 0 for 32-bit default (stack passing).
           Or 2/3 for fastcall.
           Let's use 0 for compatibility with standard C utils on DOS.
        */
        g_arg_regs[0] = NULL;
        g_arg_regs[1] = NULL;
        g_arg_regs[2] = NULL;
        g_arg_regs[3] = NULL;
        g_arg_regs[4] = NULL;
        g_arg_regs[5] = NULL;
        g_xmm_arg_regs[0] = "xmm0";
        g_xmm_arg_regs[1] = "xmm1";
        g_xmm_arg_regs[2] = "xmm2";
        g_xmm_arg_regs[3] = "xmm3";
        g_xmm_arg_regs[4] = "xmm4";
        g_xmm_arg_regs[5] = "xmm5";
        g_xmm_arg_regs[6] = "xmm6";
        g_xmm_arg_regs[7] = "xmm7";
        // For 32-bit DOS, default to CDECL (stack args)
    }
    
    if (g_target == TARGET_DOS) {
        encoder_set_bitness(16);
    } else {
        encoder_set_bitness(32);
    }

    if (out && !obj_writer && current_syntax == SYNTAX_INTEL) {
        fprintf(out, "_TEXT SEGMENT\n");
    }
}

void arch_x86_set_syntax(CodegenSyntax syntax) {
    current_syntax = syntax;
}

void arch_x86_set_target(TargetPlatform target) {
    g_target = target;
}

void arch_x86_generate(ASTNode *program) {
    current_program = program;
    pgo_probe_count = 0; /* reset PGO probes for this compilation unit */
    for (size_t i = 0; i < program->children_count; i++) {
        ASTNode *child = program->children[i];
        if (child->type == AST_FUNCTION) {
            gen_function(child);
        } else if (child->type == AST_VAR_DECL) {
            gen_global_decl(child);
        }
    }

    /* ---- PGO: Emit __pgo_dump function and data sections ---- */
    /* PGO is disabled for 32-bit DOS target for now (requires syscalls) */
#if 0
    if (g_compiler_options.pgo_generate && pgo_probe_count > 0) {
        /* Reset peephole state for the synthetic function */
        peep_unreachable = 0;
        peep_pending_jmp = 0;
        peep_pending_push = 0;
        peep_pending_jcc = 0;
        peep_jcc_jmp_pair = 0;
        peep_setcc_state = 0;

        /* Emit __pgo_dump function */
        if (obj_writer) {
            coff_writer_add_symbol(obj_writer, "__pgo_dump",
                (uint32_t)obj_writer->text_section.size, 1, 0x20, IMAGE_SYM_CLASS_STATIC);
        }
        if (out && current_syntax == SYNTAX_ATT) {
            fprintf(out, "\n__pgo_dump:\n");
        }
        emit_inst1("push", op_reg("ebp"));
        emit_inst2("mov", op_reg("esp"), op_reg("ebp"));
    emit_inst1("push", op_reg("esi"));
    emit_inst1("push", op_reg("edi"));

        /* Open file: sys_open(filename, O_WRONLY|O_CREAT|O_TRUNC, 0644) */
        emit_inst2("mov", op_imm(2), op_reg("eax"));
        emit_inst2("lea", op_label("__pgo_filename"), op_reg("edi"));
        emit_inst2("mov", op_imm(577), op_reg("esi"));  /* 0x241 */
        emit_inst2("mov", op_imm(420), op_reg("edx"));  /* 0644 */
        emit_inst0("syscall");
        emit_inst2("test", op_reg("eax"), op_reg("eax"));
        int lbl_done = label_count++;
        char l_done[32];
        sprintf(l_done, ".L%d", lbl_done);
        emit_inst1("jl", op_label(l_done));
        emit_inst2("mov", op_reg("eax"), op_reg("r12")); /* save fd */

        /* Write header: 8 bytes = "PGO1" + uint32 count */
        emit_inst2("mov", op_imm(1), op_reg("eax"));
        emit_inst2("mov", op_reg("r12"), op_reg("edi"));
        emit_inst2("lea", op_label("__pgo_header"), op_reg("esi"));
        emit_inst2("mov", op_imm(8), op_reg("edx"));
        emit_inst0("syscall");

        /* Load pointers for the write loop */
        emit_inst2("lea", op_label("__pgo_names"), op_reg("esi"));
        emit_inst2("lea", op_label("__pgo_counters"), op_reg("edi"));
        // ebx as counter
        emit_inst2("mov", op_imm(pgo_probe_count), op_reg("ebx"));

        int lbl_loop = label_count++;
        int lbl_close = label_count++;
        char l_loop[32], l_close[32];
        sprintf(l_loop, ".L%d", lbl_loop);
        sprintf(l_close, ".L%d", lbl_close);

        emit_label_def(l_loop);
        emit_inst2("test", op_reg("ebx"), op_reg("ebx"));
        emit_inst1("jz", op_label(l_close));

        emit_inst2("mov", op_imm(1), op_reg("eax"));
        // write(fd, name, len)
        // fd is in r12 -> but r12 is not 32-bit. 
        // We need a callee-saved reg for fd. Let's use local stack or another reg.
        // Actually, let's assume we saved fd in a local var or just use stack.
        // For now, let's use a stack slot for fd? Or assume we have enough regs. 
        // 32-bit registers are scarce.
        // Let's use stack [ebp-4] for fd.
        emit_inst2("mov", op_reg("r12"), op_reg("eax")); // WAIT r12 is gone
        // We need to fix the fd save/restore. 
        // In 32-bit, we push args for syscall? No, syscalls on Linux/DOS are different.
        // DOS doesn't use syscalls nicely like this. PGO on DOS might be hard.
        // I will disable PGO logic for DOS or comment it out?
        // Detailed PGO is low prio. I will keep it but fix registers blindly for now.
        // fd -> [ebp-something]?
        // Let's just use `ebx` (if available) but I used ebx for counter.
        // Let's use `push` / `pop` to preserve fd?
        emit_inst2("mov", op_reg("esi"), op_reg("ecx")); // name ptr
        emit_inst2("mov", op_imm(PGO_NAME_LEN), op_reg("edx"));
        emit_inst0("syscall");

        /* Write 8 bytes of counter */
        emit_inst2("mov", op_imm(1), op_reg("eax"));
        emit_inst2("mov", op_reg("r12"), op_reg("edi"));
        emit_inst2("mov", op_reg("r15"), op_reg("esi"));
        emit_inst2("mov", op_imm(8), op_reg("edx"));
        emit_inst0("syscall");

        /* Advance pointers and decrement counter */
        emit_inst2("add", op_imm(PGO_NAME_LEN), op_reg("r14"));
        emit_inst2("add", op_imm(8), op_reg("r15"));
        emit_inst2("sub", op_imm(1), op_reg("r13"));
        emit_inst1("jmp", op_label(l_loop));

        emit_label_def(l_close);
        /* Close file: sys_close(fd) */
        emit_inst2("mov", op_imm(3), op_reg("eax"));
        emit_inst2("mov", op_reg("r12"), op_reg("edi"));
        emit_inst0("syscall");

        emit_label_def(l_done);
        emit_inst1("pop", op_reg("r15"));
        emit_inst1("pop", op_reg("r14"));
        emit_inst1("pop", op_reg("r13"));
        emit_inst1("pop", op_reg("r12"));
        emit_inst0("leave");
        emit_inst0("ret");

        /* ---- PGO data sections ---- */
        if (obj_writer) {
            Section old_section = current_section;
            current_section = SECTION_DATA;
            uint32_t off;

            /* __pgo_header: "PGO1" + uint32 num_entries */
            off = (uint32_t)obj_writer->data_section.size;
            coff_writer_add_symbol(obj_writer, "__pgo_header", off, 2, 0, IMAGE_SYM_CLASS_STATIC);
            buffer_write_bytes(&obj_writer->data_section, "PGO1", 4);
            buffer_write_dword(&obj_writer->data_section, (uint32_t)pgo_probe_count);

            /* __pgo_filename: "default.profdata\0" */
            off = (uint32_t)obj_writer->data_section.size;
            coff_writer_add_symbol(obj_writer, "__pgo_filename", off, 2, 0, IMAGE_SYM_CLASS_STATIC);
            buffer_write_bytes(&obj_writer->data_section, "default.profdata", 17);

            /* __pgo_names: 64-byte padded name entries */
            off = (uint32_t)obj_writer->data_section.size;
            coff_writer_add_symbol(obj_writer, "__pgo_names", off, 2, 0, IMAGE_SYM_CLASS_STATIC);
            for (int pi = 0; pi < pgo_probe_count; pi++) {
                char padded[PGO_NAME_LEN];
                memset(padded, 0, PGO_NAME_LEN);
                strncpy(padded, pgo_probes[pi].name, PGO_NAME_LEN - 1);
                buffer_write_bytes(&obj_writer->data_section, padded, PGO_NAME_LEN);
            }

            /* __pgo_counters + individual __pgo_cnt_N symbols: 8 bytes each */
            off = (uint32_t)obj_writer->data_section.size;
            coff_writer_add_symbol(obj_writer, "__pgo_counters", off, 2, 0, IMAGE_SYM_CLASS_STATIC);
            for (int pi = 0; pi < pgo_probe_count; pi++) {
                char cnt_sym[32];
                sprintf(cnt_sym, "__pgo_cnt_%d", pi);
                off = (uint32_t)obj_writer->data_section.size;
                coff_writer_add_symbol(obj_writer, cnt_sym, off, 2, 0, IMAGE_SYM_CLASS_STATIC);
                uint64_t zero = 0;
                buffer_write_bytes(&obj_writer->data_section, &zero, 8);
            }

            current_section = old_section;
        } else if (out && current_syntax == SYNTAX_ATT) {
            /* Text assembly path */
            fprintf(out, "\n.data\n");

            fprintf(out, "__pgo_header:\n");
            fprintf(out, "    .byte 0x50, 0x47, 0x4f, 0x31\n"); /* "PGO1" */
            fprintf(out, "    .long %d\n", pgo_probe_count);

            fprintf(out, "__pgo_filename:\n");
            fprintf(out, "    .asciz \"default.profdata\"\n");

            fprintf(out, "__pgo_names:\n");
            for (int pi = 0; pi < pgo_probe_count; pi++) {
                fprintf(out, "    .ascii \"");
                int len = (int)strlen(pgo_probes[pi].name);
                for (int ci = 0; ci < PGO_NAME_LEN; ci++) {
                    if (ci < len) fprintf(out, "%c", pgo_probes[pi].name[ci]);
                    else fprintf(out, "\\0");
                }
                fprintf(out, "\"\n");
            }

            fprintf(out, "\n.data\n");
            fprintf(out, "__pgo_counters:\n");
            for (int pi = 0; pi < pgo_probe_count; pi++) {
                fprintf(out, "__pgo_cnt_%d:\n", pi);
                fprintf(out, "    .quad 0\n");
            }
            fprintf(out, ".text\n");
        }
    }
#endif
    /* ---- End PGO instrumentation ---- */
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
    const char *reg;   /* Non-NULL if variable lives in a register (e.g. "ebx") */
} LocalVar;

static LocalVar locals[8192];
static int locals_count = 0;
static int stack_offset = 0;

typedef enum {
    LASTVAL_NONE,
    LASTVAL_STACK,
    LASTVAL_LABEL,
    LASTVAL_REG
} LastValueKind;

typedef struct {
    LastValueKind kind;
    int offset;
    const char *name;
    const char *reg;
    int size;
} LastValueCache;

static LastValueCache last_value = { LASTVAL_NONE, 0, NULL, NULL, 0 };

static int last_value_can_track(Type *t) {
    if (!t) return 0;
    if (is_float_type(t)) return 0;
    if (t->kind == TYPE_ARRAY || t->kind == TYPE_STRUCT || t->kind == TYPE_UNION) return 0;
    if (t->size <= 0 || t->size > 8) return 0;
    return 1;
}

static void last_value_clear(void) {
    last_value.kind = LASTVAL_NONE;
    last_value.offset = 0;
    last_value.name = NULL;
    last_value.reg = NULL;
    last_value.size = 0;
}

static void last_value_set_stack(int offset, Type *t) {
    if (!last_value_can_track(t)) {
        last_value_clear();
        return;
    }
    last_value.kind = LASTVAL_STACK;
    last_value.offset = offset;
    last_value.name = NULL;
    last_value.reg = NULL;
    last_value.size = t ? t->size : 0;
}

static void last_value_set_label(const char *name, Type *t) {
    if (!last_value_can_track(t)) {
        last_value_clear();
        return;
    }
    last_value.kind = LASTVAL_LABEL;
    last_value.offset = 0;
    last_value.name = name;
    last_value.reg = NULL;
    last_value.size = t ? t->size : 0;
}

static void last_value_set_reg(const char *reg, Type *t) {
    if (!last_value_can_track(t)) {
        last_value_clear();
        return;
    }
    last_value.kind = LASTVAL_REG;
    last_value.offset = 0;
    last_value.name = NULL;
    last_value.reg = reg;
    last_value.size = t ? t->size : 0;
}

static int last_value_match_stack(int offset, Type *t) {
    return last_value.kind == LASTVAL_STACK && last_value.offset == offset &&
           last_value_can_track(t) && last_value.size == (t ? t->size : 0);
}

static int last_value_match_label(const char *name, Type *t) {
    return last_value.kind == LASTVAL_LABEL && name && last_value.name &&
           strcmp(last_value.name, name) == 0 && last_value_can_track(t) &&
           last_value.size == (t ? t->size : 0);
}

static int last_value_match_reg(const char *reg, Type *t) {
    return last_value.kind == LASTVAL_REG && reg && last_value.reg &&
           strcmp(last_value.reg, reg) == 0 && last_value_can_track(t) &&
           last_value.size == (t ? t->size : 0);
}

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

/* --- Register Allocator (Phase 7a) ---
 * Assigns frequently-used scalar integer locals to callee-saved registers
 * (%ebx, %r12-%r15) to eliminate memory round-trips in tight loops.
 * Activated at -O2 and above. */

#define REGALLOC_MAX_REGS  3   /* %ebx, %esi, %edi */
#define REGALLOC_MAX_VARS  256

static const char *regalloc_callee_regs[REGALLOC_MAX_REGS] = {
    "ebx", "esi", "edi"
};
static const char *regalloc_callee_regs_32[REGALLOC_MAX_REGS] = {
    "ebx", "esi", "edi"
};
static const char *regalloc_callee_regs_16[REGALLOC_MAX_REGS] = {
    "bx", "si", "di"
};

static const char *regalloc_callee_regs_8[REGALLOC_MAX_REGS] = {
    "bl" /* Only bl is available as 8-bit part of callee-saved (ebx) in 32-bit mode. si/di have no 8-bit parts. */
};

/* Pre-scan data: collected from the AST before codegen */
typedef struct {
    const char *name;
    Type *type;
    int is_addr_taken;  /* 1 if &var appears in the function body */
    int is_param;       /* 1 if this is a function parameter */
    int use_count;      /* approximate number of uses for priority */
} RegScanVar;

static RegScanVar regalloc_scan_vars[REGALLOC_MAX_VARS];
static int regalloc_scan_count = 0;

/* Per-function register assignments */
typedef struct {
    const char *var_name;
    const char *reg64;   /* 64-bit name: "ebx", "r12", ... */
    const char *reg32;   /* 32-bit name: "ebx", "r12d", ... */
    const char *reg16;   /* 16-bit name */
    const char *reg8;    /* 8-bit name */
    int save_offset;     /* ebp-relative offset where we saved the original value */
} RegAssignment;

static RegAssignment regalloc_assignments[REGALLOC_MAX_REGS];
static int regalloc_assignment_count = 0;

/* Look up the register assigned to a local variable (NULL if on stack) */
static const char *get_local_reg(const char *name) {
    if (!name || regalloc_assignment_count == 0) return NULL;
    for (int i = locals_count - 1; i >= 0; i--) {
        if (locals[i].name && strcmp(locals[i].name, name) == 0) {
            return locals[i].reg;
        }
    }
    return NULL;
}

/* Get the 32-bit veesion of a register-allocated variable's register */
static const char *get_local_reg32(const char *name) {
    if (!name || regalloc_assignment_count == 0) return NULL;
    const char *reg64 = get_local_reg(name);
    if (!reg64) return NULL;
    for (int i = 0; i < regalloc_assignment_count; i++) {
        if (strcmp(regalloc_assignments[i].reg64, reg64) == 0)
            return regalloc_assignments[i].reg32;
    }
    return NULL;
}

/* Get the 8-bit veesion of a register-allocated variable's register */
static const char *get_local_reg8(const char *name) {
    if (!name || regalloc_assignment_count == 0) return NULL;
    const char *reg64 = get_local_reg(name);
    if (!reg64) return NULL;
    for (int i = 0; i < regalloc_assignment_count; i++) {
        if (strcmp(regalloc_assignments[i].reg64, reg64) == 0)
            return regalloc_assignments[i].reg8;
    }
    return NULL;
}

/* Get the 16-bit veesion of a register-allocated variable's register */
static const char *get_local_reg16(const char *name) {
    if (!name || regalloc_assignment_count == 0) return NULL;
    const char *reg64 = get_local_reg(name);
    if (!reg64) return NULL;
    for (int i = 0; i < regalloc_assignment_count; i++) {
        if (strcmp(regalloc_assignments[i].reg64, reg64) == 0)
            return regalloc_assignments[i].reg16;
    }
    return NULL;
}

/* Pre-scan: record a variable declaration */
static void regalloc_scan_record_var(const char *name, Type *type, int is_param) {
    if (!name || regalloc_scan_count >= REGALLOC_MAX_VARS) return;
    /* Check for duplicate (variable shadowing — skip for safety) */
    for (int i = 0; i < regalloc_scan_count; i++) {
        if (strcmp(regalloc_scan_vars[i].name, name) == 0) {
            /* Mark as address-taken to prevent register allocation */
            regalloc_scan_vars[i].is_addr_taken = 1;
            return;
        }
    }
    regalloc_scan_vars[regalloc_scan_count].name = name;
    regalloc_scan_vars[regalloc_scan_count].type = type;
    regalloc_scan_vars[regalloc_scan_count].is_addr_taken = 0;
    regalloc_scan_vars[regalloc_scan_count].is_param = is_param;
    regalloc_scan_vars[regalloc_scan_count].use_count = 0;
    regalloc_scan_count++;
}

/* Pre-scan: recuesively walk AST to collect variable info */
static void regalloc_scan_ast(ASTNode *node) {
    if (!node) return;

    /* Record variable declarations */
    if (node->type == AST_VAR_DECL && node->data.var_decl.name) {
        regalloc_scan_record_var(node->data.var_decl.name, node->resolved_type, 0);
    }

    /* Detect address-taken variables: &identifier */
    if (node->type == AST_ADDR_OF && node->data.unary.expression &&
        node->data.unary.expression->type == AST_IDENTIFIER) {
        const char *taken_name = node->data.unary.expression->data.identifier.name;
        if (taken_name) {
            for (int i = 0; i < regalloc_scan_count; i++) {
                if (strcmp(regalloc_scan_vars[i].name, taken_name) == 0) {
                    regalloc_scan_vars[i].is_addr_taken = 1;
                    break;
                }
            }
        }
    }

    /* Count variable uses */
    if (node->type == AST_IDENTIFIER && node->data.identifier.name) {
        for (int i = 0; i < regalloc_scan_count; i++) {
            if (strcmp(regalloc_scan_vars[i].name, node->data.identifier.name) == 0) {
                regalloc_scan_vars[i].use_count++;
                break;
            }
        }
    }

    /* Recurse into children */
    if (node->children) {
        for (size_t i = 0; i < node->children_count; i++) {
            regalloc_scan_ast(node->children[i]);
        }
    }

    /* Recurse into specific AST node fields */
    switch (node->type) {
        case AST_FUNCTION:
            regalloc_scan_ast(node->data.function.body);
            break;
        case AST_VAR_DECL:
            regalloc_scan_ast(node->data.var_decl.initializer);
            break;
        case AST_ASSIGN:
            regalloc_scan_ast(node->data.assign.left);
            regalloc_scan_ast(node->data.assign.value);
            break;
        case AST_BINARY_EXPR:
            regalloc_scan_ast(node->data.binary_expr.left);
            regalloc_scan_ast(node->data.binary_expr.right);
            break;
        case AST_IF:
            regalloc_scan_ast(node->data.if_stmt.condition);
            regalloc_scan_ast(node->data.if_stmt.then_branch);
            regalloc_scan_ast(node->data.if_stmt.else_branch);
            break;
        case AST_WHILE:
        case AST_DO_WHILE:
            regalloc_scan_ast(node->data.while_stmt.condition);
            regalloc_scan_ast(node->data.while_stmt.body);
            break;
        case AST_FOR:
            regalloc_scan_ast(node->data.for_stmt.init);
            regalloc_scan_ast(node->data.for_stmt.condition);
            regalloc_scan_ast(node->data.for_stmt.increment);
            regalloc_scan_ast(node->data.for_stmt.body);
            break;
        case AST_RETURN:
            regalloc_scan_ast(node->data.return_stmt.expression);
            break;
        case AST_CALL:
            /* arguments are in children[], already handled above */
            break;
        case AST_CAST:
            regalloc_scan_ast(node->data.cast.expression);
            break;
        case AST_DEREF:
        case AST_ADDR_OF:
        case AST_NEG:
        case AST_NOT:
        case AST_BITWISE_NOT:
        case AST_PRE_INC:
        case AST_PRE_DEC:
        case AST_POST_INC:
        case AST_POST_DEC:
            regalloc_scan_ast(node->data.unary.expression);
            break;
        case AST_MEMBER_ACCESS:
            regalloc_scan_ast(node->data.member_access.struct_expr);
            break;
        case AST_ARRAY_ACCESS:
            regalloc_scan_ast(node->data.array_access.array);
            regalloc_scan_ast(node->data.array_access.index);
            break;
        case AST_SWITCH:
            regalloc_scan_ast(node->data.switch_stmt.condition);
            regalloc_scan_ast(node->data.switch_stmt.body);
            break;
        case AST_ASSERT:
            regalloc_scan_ast(node->data.assert_stmt.condition);
            break;
        default:
            break;
    }
}

/* Determine if a variable is eligible for register allocation */
static int regalloc_is_eligible(RegScanVar *sv) {
    if (sv->is_addr_taken) return 0;
    if (!sv->type) return 0;
    /* Only scalar integer types (size 1-8) */
    if (sv->type->kind == TYPE_ARRAY || sv->type->kind == TYPE_STRUCT ||
        sv->type->kind == TYPE_UNION) return 0;
    if (sv->type->kind == TYPE_FLOAT || sv->type->kind == TYPE_DOUBLE) return 0;
    if (sv->type->size > 8) return 0;
    return 1;
}

/* Assign registers to eligible variables.  Called before parameter handling.
 * Phase 1 (regalloc_analyze): pre-scan AST and determine assignments.
 * Phase 2 (regalloc_emit_saves): emit push for callee-saved registers.
 * func_node is the AST_FUNCTION node. */
static void regalloc_analyze(ASTNode *func_node) {
    regalloc_scan_count = 0;
    regalloc_assignment_count = 0;

    if (!OPT_AT_LEAST(OPT_O2)) return;

    /* Pre-scan function parameters */
    if (func_node->children) {
        for (size_t i = 0; i < func_node->children_count; i++) {
            ASTNode *param = func_node->children[i];
            if (param && param->type == AST_VAR_DECL && param->data.var_decl.name) {
                regalloc_scan_record_var(param->data.var_decl.name, param->resolved_type, 1);
            }
        }
    }

    /* Pre-scan the function body */
    regalloc_scan_ast(func_node->data.function.body);

    /* Sort eligible variables by use count (descending) — simple selection */
    /* Collect eligible indices */
    int eligible[REGALLOC_MAX_VARS];
    int eligible_count = 0;
    for (int i = 0; i < regalloc_scan_count && eligible_count < REGALLOC_MAX_VARS; i++) {
        if (regalloc_is_eligible(&regalloc_scan_vars[i])) {
            eligible[eligible_count++] = i;
        }
    }

    /* Simple sort by use_count descending (selection sort is fine for small N) */
    for (int i = 0; i < eligible_count - 1; i++) {
        int max_idx = i;
        for (int j = i + 1; j < eligible_count; j++) {
            if (regalloc_scan_vars[eligible[j]].use_count >
                regalloc_scan_vars[eligible[max_idx]].use_count) {
                max_idx = j;
            }
        }
        if (max_idx != i) {
            int tmp = eligible[i];
            eligible[i] = eligible[max_idx];
            eligible[max_idx] = tmp;
        }
    }

    /* Assign registers to the top N eligible variables (no codegen yet) */
    int num_assign = eligible_count < REGALLOC_MAX_REGS ? eligible_count : REGALLOC_MAX_REGS;
    for (int i = 0; i < num_assign; i++) {
        RegScanVar *sv = &regalloc_scan_vars[eligible[i]];
        regalloc_assignments[i].var_name = sv->name;
        regalloc_assignments[i].reg64 = regalloc_callee_regs[i];
        regalloc_assignments[i].reg32 = regalloc_callee_regs_32[i];
        regalloc_assignments[i].reg16 = regalloc_callee_regs_16[i];
        regalloc_assignments[i].reg8 = regalloc_callee_regs_8[i];
        regalloc_assignments[i].save_offset = 0; /* set during emit_saves */
    }
    regalloc_assignment_count = num_assign;
}

/* Emit push instructions to save callee-saved registers that we'll use.
 * Must be called after prologue but before parameter handling / body codegen. */
static void regalloc_emit_saves(void) {
    for (int i = 0; i < regalloc_assignment_count; i++) {
        emit_inst1("push", op_reg(regalloc_assignments[i].reg64));
        stack_offset -= 8;
        regalloc_assignments[i].save_offset = stack_offset;
    }
}

/* Restore callee-saved registers before function epilogue.
 * Uses ebp-relative addressing so stack pointer position doesn't matter. */
static void regalloc_restore_registers(void) {
    for (int i = 0; i < regalloc_assignment_count; i++) {
        emit_inst2("mov", op_mem("ebp", regalloc_assignments[i].save_offset),
                    op_reg(regalloc_assignments[i].reg64));
    }
}

/* Check if a variable name is assigned to a register and return the
 * register assignment index, or -1 if not register-allocated. */
static int regalloc_find_assignment(const char *name) {
    if (!name) return -1;
    for (int i = 0; i < regalloc_assignment_count; i++) {
        if (strcmp(regalloc_assignments[i].var_name, name) == 0)
            return i;
    }
    return -1;
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
            int64_t val = (int64_t)init_node->data.integer.value;
            buffer_write_bytes(&obj_writer->data_section, &val, size);
        } else if (init_node && init_node->type == AST_FLOAT) {
            double val = init_node->data.float_val.value;
            if (size == 4) {
                float f = (float)val;
                buffer_write_bytes(&obj_writer->data_section, &f, 4);
            } else {
                buffer_write_bytes(&obj_writer->data_section, &val, 8);
            }
        } else if (init_node && init_node->type == AST_INIT_LIST) {
            /* Array / struct initializer list: emit each element to .data */
            int elem_size = 1;
            if (node->resolved_type && node->resolved_type->kind == TYPE_ARRAY && node->resolved_type->data.ptr_to) {
                elem_size = node->resolved_type->data.ptr_to->size;
            }
            size_t gi;
            int total_written = 0;
            for (gi = 0; gi < init_node->children_count; gi++) {
                ASTNode *elem = init_node->children[gi];
                if (elem && elem->type == AST_INTEGER) {
                    if (elem_size == 1) {
                        uint8_t b = (uint8_t)elem->data.integer.value;
                        buffer_write_byte(&obj_writer->data_section, b);
                    } else if (elem_size == 2) {
                        uint16_t w = (uint16_t)elem->data.integer.value;
                        buffer_write_word(&obj_writer->data_section, w);
                    } else if (elem_size == 4) {
                        int32_t d = (int32_t)elem->data.integer.value;
                        buffer_write_dword(&obj_writer->data_section, (uint32_t)d);
                    } else {
                        int64_t q = (int64_t)elem->data.integer.value;
                        buffer_write_qword(&obj_writer->data_section, (uint64_t)q);
                    }
                } else if (elem && elem->type == AST_FLOAT) {
                    if (elem_size == 4) {
                        float f = (float)elem->data.float_val.value;
                        buffer_write_bytes(&obj_writer->data_section, &f, 4);
                    } else {
                        double d = elem->data.float_val.value;
                        buffer_write_bytes(&obj_writer->data_section, &d, 8);
                    }
                } else if (elem && elem->type == AST_STRING) {
                    /* String literal in init list: store string data and add relocation */
                    char slabel[32];
                    sprintf(slabel, ".LC%d", label_count++);
                    int slen = elem->data.string.length;
                    /* Emit the string data in .data section */
                    uint32_t str_offset = (uint32_t)obj_writer->data_section.size;
                    /* We need to emit a placeholder and relocation, but since this
                     * is a pointer within the same data section, we store the string
                     * after all init list data, and record a self-relocation.
                     * For now, emit the string into the string_literals table for 
                     * deferred emission, and write a zero placeholder. */
                    if (string_literals_count < 8192) {
                        string_literals[string_literals_count].label = strdup(slabel);
                        string_literals[string_literals_count].value = malloc(slen + 1);
                        memcpy(string_literals[string_literals_count].value, elem->data.string.value, slen + 1);
                        string_literals[string_literals_count].length = slen;
                        string_literals_count++;
                    }
                    /* Write symbol and relocation for the string pointer */
                    uint32_t sym_idx = coff_writer_add_symbol(obj_writer, slabel, 0, 0, 0, IMAGE_SYM_CLASS_EXTERNAL);
                    uint32_t reloc_off = (uint32_t)obj_writer->data_section.size;
                    coff_writer_add_reloc(obj_writer, reloc_off, sym_idx, 1, 2);
                    uint64_t zero = 0;
                    buffer_write_bytes(&obj_writer->data_section, &zero, 8);
                } else {
                    /* Unknown element — zero fill */
                    int zi; for (zi = 0; zi < elem_size; zi++)
                        buffer_write_byte(&obj_writer->data_section, 0);
                }
                total_written += elem_size;
            }
            /* Zero-fill remaining bytes if array larger than init list */
            while (total_written < size) {
                buffer_write_byte(&obj_writer->data_section, 0);
                total_written++;
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
                fprintf(out, "%s %I64d\n", directive, init_intel->data.integer.value);
            } else if (init_intel && init_intel->type == AST_FLOAT) {
                fprintf(out, "%s %f\n", directive, init_intel->data.float_val.value);
            } else if (init_intel && init_intel->type == AST_INIT_LIST) {
                /* Array / struct initializer list */
                int elem_size = 1;
                if (node->resolved_type && node->resolved_type->kind == TYPE_ARRAY && node->resolved_type->data.ptr_to) {
                    elem_size = node->resolved_type->data.ptr_to->size;
                }
                const char *edirective = "DB";
                if (elem_size == 4) edirective = "DD";
                else if (elem_size >= 8) edirective = "DQ";
                size_t ii;
                int total_written = 0;
                for (ii = 0; ii < init_intel->children_count; ii++) {
                    ASTNode *elem = init_intel->children[ii];
                    if (elem && elem->type == AST_INTEGER) {
                        fprintf(out, "%s %I64d\n", edirective, elem->data.integer.value);
                    } else if (elem && elem->type == AST_FLOAT) {
                        fprintf(out, "%s %f\n", edirective, elem->data.float_val.value);
                    } else if (elem && elem->type == AST_STRING) {
                        char slabel[32];
                        sprintf(slabel, ".LC%d", label_count++);
                        int slen = elem->data.string.length;
                        if (string_literals_count < 8192) {
                            string_literals[string_literals_count].label = strdup(slabel);
                            string_literals[string_literals_count].value = malloc(slen + 1);
                            memcpy(string_literals[string_literals_count].value, elem->data.string.value, slen + 1);
                            string_literals[string_literals_count].length = slen;
                            string_literals_count++;
                        }
                        fprintf(out, "DQ OFFSET %s\n", slabel);
                    } else {
                        fprintf(out, "%s 0\n", edirective);
                    }
                    total_written += elem_size;
                }
                if (total_written < size) {
                    fprintf(out, "DB %d DUP(0)\n", size - total_written);
                }
            } else {
                fprintf(out, "%s 0\n", directive);
                if (size > 8) {
                     fprintf(out, "DB %d DUP(0)\n", size - (size > 1 ? (size > 4 ? 8 : 4) : 1));
                }
            }
            fprintf(out, "_DATA ENDS\n");
        } else {
            fprintf(out, ".data\n");
            if (!node->data.var_decl.is_static) fprintf(out, ".globl %s\n", node->data.var_decl.name);
            fprintf(out, "%s:\n", node->data.var_decl.name);
             ASTNode *init_att = node->data.var_decl.initializer;
             if (init_att && init_att->type == AST_INTEGER) {
                 long long val = init_att->data.integer.value;
                 int size = node->resolved_type ? node->resolved_type->size : 4;
                 if (size == 1) fprintf(out, "    .byte %I64d\n", val);
                 else if (size == 2) fprintf(out, "    .word %I64d\n", val);
                 else if (size == 4) fprintf(out, "    .long %I64d\n", val);
                 else if (size == 8) fprintf(out, "    .quad %I64d\n", val);
                 else fprintf(out, "    .long %I64d\n", val);
             } else if (init_att && init_att->type == AST_FLOAT) {
                 int size = node->resolved_type ? node->resolved_type->size : 4;
                 if (size == 4) fprintf(out, "    .float %f\n", init_att->data.float_val.value);
                 else fprintf(out, "    .double %f\n", init_att->data.float_val.value);
             } else if (init_att && init_att->type == AST_INIT_LIST) {
                 /* Array / struct initializer list */
                 int elem_size = 1;
                 if (node->resolved_type && node->resolved_type->kind == TYPE_ARRAY && node->resolved_type->data.ptr_to) {
                     elem_size = node->resolved_type->data.ptr_to->size;
                 }
                 size_t ai;
                 int total_written = 0;
                 for (ai = 0; ai < init_att->children_count; ai++) {
                     ASTNode *elem = init_att->children[ai];
                     if (elem && elem->type == AST_INTEGER) {
                         if (elem_size == 1) fprintf(out, "    .byte %I64d\n", elem->data.integer.value);
                         else if (elem_size == 4) fprintf(out, "    .long %I64d\n", elem->data.integer.value);
                         else if (elem_size == 8) fprintf(out, "    .quad %I64d\n", elem->data.integer.value);
                         else fprintf(out, "    .long %I64d\n", elem->data.integer.value);
                     } else if (elem && elem->type == AST_FLOAT) {
                         if (elem_size == 4) fprintf(out, "    .float %f\n", elem->data.float_val.value);
                         else fprintf(out, "    .double %f\n", elem->data.float_val.value);
                     } else if (elem && elem->type == AST_STRING) {
                         /* String literal in init list: store pointer to string data */
                         char slabel[32];
                         sprintf(slabel, ".LC%d", label_count++);
                         int slen = elem->data.string.length;
                         if (string_literals_count < 8192) {
                             string_literals[string_literals_count].label = strdup(slabel);
                             string_literals[string_literals_count].value = malloc(slen + 1);
                             memcpy(string_literals[string_literals_count].value, elem->data.string.value, slen + 1);
                             string_literals[string_literals_count].length = slen;
                             string_literals_count++;
                         }
                         if (g_target == TARGET_DOS) fprintf(out, "    .long %s\n", slabel); else fprintf(out, "    .quad %s\n", slabel);
                     } else {
                         int zi; for (zi = 0; zi < elem_size; zi++) fprintf(out, "    .byte 0\n");
                     }
                     total_written += elem_size;
                 }
                 { int zsize = node->resolved_type ? node->resolved_type->size : 4;
                   if (total_written < zsize) fprintf(out, "    .zero %d\n", zsize - total_written); }
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
    emit_inst2("sub", op_imm(8), op_reg("esp"));
    emit_inst2("movsd", op_reg(reg), op_mem("esp", 0));
    stack_offset -= 8;
}

static void emit_pop_xmm(const char *reg) {
    emit_inst2("movsd", op_mem("esp", 0), op_reg(reg));
    emit_inst2("add", op_imm(8), op_reg("esp"));
    stack_offset += 8;
}


// op_reg, op_imm, op_mem are now defined above with the other op_* functions


static void print_operand(Operand *op) {
    if (!out) return;
    if (op->type == OP_REG) {
        if (current_syntax == SYNTAX_ATT) fprintf(out, "%%%s", op->data.reg);
        else fprintf(out, "%s", op->data.reg);
    } else if (op->type == OP_IMM) {
        if (current_syntax == SYNTAX_ATT) fprintf(out, "$%lld", op->data.imm);
        else fprintf(out, "%lld", op->data.imm);
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
        if (current_syntax == SYNTAX_ATT) fprintf(out, "%s", lbl);
        else fprintf(out, "[%s]", lbl);
    } else if (op->type == OP_MEM_SIB) {
        /* SIB addressing: disp(base, index, scale) in AT&T; [base + index*scale + disp] in Intel */
        if (current_syntax == SYNTAX_ATT) {
            if (op->data.sib.disp != 0) fprintf(out, "%d", op->data.sib.disp);
            fprintf(out, "(%%%s,%%%s,%d)", op->data.sib.base, op->data.sib.index, op->data.sib.scale);
        } else {
            fprintf(out, "[%s+%s*%d", op->data.sib.base, op->data.sib.index, op->data.sib.scale);
            if (op->data.sib.disp > 0) fprintf(out, "+%d", op->data.sib.disp);
            else if (op->data.sib.disp < 0) fprintf(out, "%d", op->data.sib.disp);
            fprintf(out, "]");
        }
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
    // Peephole: suppress dead code after unconditional jmp
    if (!peep_in_flush && current_section == SECTION_TEXT) {
        peep_flush_setcc();
        peep_flush_push();
        peep_flush_jcc();
        peep_flush_pair();
        peep_flush_jmp();
        if (peep_unreachable) return;
    }

    if (obj_writer) {
        encode_inst0(&obj_writer->text_section, mnemonic);
        return;
    }
    const char *m = mnemonic;
    if (current_syntax == SYNTAX_INTEL) {
        if (strcmp(mnemonic, "cqto") == 0) m = "cdq";
        else if (strcmp(mnemonic, "leave") == 0) m = "leave";
        else if (strcmp(mnemonic, "ret") == 0) m = "ret";
    }
    fprintf(out, "    %s\n", m);
}

static void emit_inst1(const char *mnemonic, Operand *op1) {
    /* ---- Peephole: setcc + movzbl + test + jcc → direct jcc ----
     * When state==3, a jcc completes the pattern. Compute the direct
     * jump condition from the original setCC and the branch direction.
     * setCC %al; movzbl %al,%eax; test %eax,%eax; je L  → j(inv CC) L
     * setCC %al; movzbl %al,%eax; test %eax,%eax; jne L → jCC L       */
    if (!peep_in_flush && current_section == SECTION_TEXT &&
        OPT_AT_LEAST(OPT_O1) &&
        peep_setcc_state == 3 &&
        op1->type == OP_LABEL &&
        mnemonic[0] == 'j' && strcmp(mnemonic, "jmp") != 0) {
        /* Build the jcc that corresponds to the original setCC */
        static char setcc_jcc[16];
        snprintf(setcc_jcc, sizeof(setcc_jcc), "j%s", peep_setcc_cond);
        if (strcmp(mnemonic, "je") == 0 || strcmp(mnemonic, "jz") == 0) {
            /* test+je: branch when setCC result was 0 → condition false → invert */
            const char *inv = peep_invert_jcc(setcc_jcc);
            if (inv) {
                peep_setcc_state = 0;
                mnemonic = inv;
                /* fall through to normal jcc handling with the new mnemonic */
            } else {
                peep_flush_setcc();
                /* fall through with original mnemonic */
            }
        } else if (strcmp(mnemonic, "jne") == 0 || strcmp(mnemonic, "jnz") == 0) {
            /* test+jne: branch when setCC result was 1 → condition true → keep */
            peep_setcc_state = 0;
            mnemonic = setcc_jcc;
            /* fall through to normal jcc handling */
        } else {
            /* Other conditional jump — can't optimize, flush */
            peep_flush_setcc();
        }
    }
    /* If we have a pending setcc chain and this is NOT a matching jcc, flush */
    else if (!peep_in_flush && peep_setcc_state > 0 &&
             !(mnemonic[0] == 's' && mnemonic[1] == 'e' && mnemonic[2] == 't' &&
               op1->type == OP_REG && strcmp(op1->data.reg, "al") == 0)) {
        peep_flush_setcc();
    }

    /* ---- Peephole: buffer setCC %al ---- */
    if (!peep_in_flush && current_section == SECTION_TEXT &&
        OPT_AT_LEAST(OPT_O1) &&
        mnemonic[0] == 's' && mnemonic[1] == 'e' && mnemonic[2] == 't' &&
        op1->type == OP_REG && strcmp(op1->data.reg, "al") == 0) {
        peep_flush_push();
        peep_flush_jcc();
        peep_flush_pair();
        peep_flush_jmp();
        if (peep_unreachable) return;
        peep_setcc_state = 1;
        strncpy(peep_setcc_cond, mnemonic + 3, 15);
        peep_setcc_cond[15] = '\0';
        return;
    }

    // Peephole: intercept unconditional jmp for buffering/dead-code elimination
    if (!peep_in_flush && current_section == SECTION_TEXT &&
        strcmp(mnemonic, "jmp") == 0 && op1->type == OP_LABEL) {
        if (peep_unreachable) return;  // dead code after previous jmp
        peep_flush_setcc();
        peep_flush_push();
        
        /* Branch optimization: jcc L1; jmp L2 → candidate pair
         * Don't invert yet — defer to emit_label_def to confirm L1 is next. */
        if (OPT_AT_LEAST(OPT_O1) && peep_pending_jcc) {
            const char *inv = peep_invert_jcc(peep_jcc_mnemonic);
            if (inv) {
                /* Store as a candidate pair; emit_label_def will resolve */
                peep_jcc_jmp_pair = 1;
                strncpy(peep_pair_jcc_mn, peep_jcc_mnemonic, 15);
                peep_pair_jcc_mn[15] = '\0';
                strncpy(peep_pair_jcc_tgt, peep_jcc_target, 63);
                peep_pair_jcc_tgt[63] = '\0';
                strncpy(peep_pair_jmp_tgt, op1->data.label, 63);
                peep_pair_jmp_tgt[63] = '\0';
                peep_pending_jcc = 0;
                peep_unreachable = 1;  // code after jmp is unreachable
                return;
            }
        }
        
        peep_flush_jcc();
        peep_flush_pair();
        peep_pending_jmp = 1;
        strncpy(peep_jmp_target, op1->data.label, 63);
        peep_jmp_target[63] = '\0';
        peep_unreachable = 1;
        return;
    }
    
    /* Peephole: buffer conditional jumps at -O1 for jcc-over-jmp detection */
    if (!peep_in_flush && current_section == SECTION_TEXT &&
        OPT_AT_LEAST(OPT_O1) &&
        op1->type == OP_LABEL &&
        mnemonic[0] == 'j' && strcmp(mnemonic, "jmp") != 0) {
        peep_flush_setcc();
        peep_flush_push();
        peep_flush_jcc();  /* flush any previous buffered jcc first */
        peep_flush_pair();
        peep_flush_jmp();
        if (peep_unreachable) return;
        peep_pending_jcc = 1;
        strncpy(peep_jcc_mnemonic, mnemonic, 15);
        peep_jcc_mnemonic[15] = '\0';
        strncpy(peep_jcc_target, op1->data.label, 63);
        peep_jcc_target[63] = '\0';
        return;
    }

    /* Peephole: push %reg → buffer it; if next is pop %reg2, emit mov instead.
     * push %eax; pop %edi → mov %eax, %edi (saves 2 memory ops) */
    if (!peep_in_flush && current_section == SECTION_TEXT &&
        OPT_AT_LEAST(OPT_O1) &&
        strcmp(mnemonic, "push") == 0 && op1->type == OP_REG) {
        peep_flush_setcc();
        peep_flush_push();  /* flush any previous buffered push */
        peep_flush_jcc();
        peep_flush_pair();
        peep_flush_jmp();
        if (peep_unreachable) return;
        peep_pending_push = 1;
        strncpy(peep_push_reg, op1->data.reg, 15);
        peep_push_reg[15] = '\0';
        return;
    }

    /* Peephole: pop %reg after buffered push %reg2 → mov %reg2, %reg */
    if (!peep_in_flush && current_section == SECTION_TEXT &&
        OPT_AT_LEAST(OPT_O1) &&
        strcmp(mnemonic, "pop") == 0 && op1->type == OP_REG &&
        peep_pending_push) {
        peep_pending_push = 0;
        /* push %X; pop %X → eliminate both (identity) */
        if (strcmp(peep_push_reg, op1->data.reg) == 0) {
            return;
        }
        /* push %X; pop %Y → mov %X, %Y */
        peep_in_flush = 1;
        emit_inst2("mov", op_reg(peep_push_reg), op_reg(op1->data.reg));
        peep_in_flush = 0;
        return;
    }
    
    // Peephole: suppress dead code after unconditional jmp
    if (!peep_in_flush && current_section == SECTION_TEXT) {
        peep_flush_setcc();
        peep_flush_push();
        peep_flush_jcc();
        peep_flush_pair();
        peep_flush_jmp();
        if (peep_unreachable) return;
    }

    if (obj_writer) {
        encode_inst1(&obj_writer->text_section, mnemonic, op1);
        return;
    }
    const char *m = mnemonic;
    if (current_syntax == SYNTAX_INTEL) {
        if (strcmp(mnemonic, "idivq") == 0) m = "idiv";
        else if (strcmp(mnemonic, "push") == 0) m = "push";
        else if (strcmp(mnemonic, "pop") == 0) m = "pop";
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
    /* ---- Peephole: setcc state transitions for 2-operand instructions ---- */
    if (!peep_in_flush && current_section == SECTION_TEXT &&
        OPT_AT_LEAST(OPT_O1) && peep_setcc_state > 0) {
        /* State 1 → 2: movzbl %al, %eax */
        if (peep_setcc_state == 1 &&
            strcmp(mnemonic, "movzbl") == 0 &&
            op1->type == OP_REG && strcmp(op1->data.reg, "al") == 0 &&
            op2->type == OP_REG && strcmp(op2->data.reg, "eax") == 0) {
            peep_setcc_state = 2;
            return;
        }
        /* State 2 → 3: test %eax, %eax */
        if (peep_setcc_state == 2 &&
            strcmp(mnemonic, "test") == 0 &&
            op1->type == OP_REG && strcmp(op1->data.reg, "eax") == 0 &&
            op2->type == OP_REG && strcmp(op2->data.reg, "eax") == 0) {
            peep_setcc_state = 3;
            return;
        }
        /* Unexpected instruction — flush and continue normally */
        peep_flush_setcc();
    }

    // Peephole: suppress dead code after unconditional jmp
    if (!peep_in_flush && current_section == SECTION_TEXT) {
        peep_flush_push();
        peep_flush_jcc();
        peep_flush_pair();
        peep_flush_jmp();
        if (peep_unreachable) return;

        /* Peephole: eliminate no-op instructions at -O1+ */
        if (OPT_AT_LEAST(OPT_O1)) {
            /* add $0, %reg → nop */
            if ((strcmp(mnemonic, "add") == 0 || strcmp(mnemonic, "addl") == 0 ||
                 strcmp(mnemonic, "sub") == 0 || strcmp(mnemonic, "subl") == 0) &&
                op1->type == OP_IMM && op1->data.imm == 0 && op2->type == OP_REG) {
                return;
            }
            /* imul $1, %reg → nop */
            if ((strcmp(mnemonic, "imul") == 0 || strcmp(mnemonic, "imull") == 0) &&
                op1->type == OP_IMM && op1->data.imm == 1 && op2->type == OP_REG) {
                return;
            }
            /* imul $0, %reg → xor %reg32, %reg32 */
            if ((strcmp(mnemonic, "imul") == 0 || strcmp(mnemonic, "imull") == 0) &&
                op1->type == OP_IMM && op1->data.imm == 0 && op2->type == OP_REG) {
                peep_in_flush = 1;
                emit_inst2("xor", op_reg("eax"), op_reg("eax"));
                peep_in_flush = 0;
                return;
            }
            /* imul $const, %reg → lea/shl chains (1-cycle LEA vs 3-cycle imul) */
            if ((strcmp(mnemonic, "imul") == 0 || strcmp(mnemonic, "imull") == 0) &&
                op1->type == OP_IMM && op2->type == OP_REG) {
                long long val = op1->data.imm;
                int is_32 = (strcmp(mnemonic, "imull") == 0);
                const char *reg64 = dos_reg_to_64bit(op2->data.reg);
                const char *lea_mn = is_32 ? "leal" : "lea";
                /* Single-instruction: x*N where (N-1) is a valid SIB scale */
                int scale = 0;
                if (val == 3) scale = 2;
                else if (val == 5) scale = 4;
                else if (val == 9) scale = 8;
                if (scale > 0) {
                    peep_in_flush = 1;
                    emit_inst2(lea_mn, op_sib(reg64, reg64, scale, 0), op_reg(op2->data.reg));
                    peep_in_flush = 0;
                    return;
                }
                /* Single-instruction: x*2 → add %r, %r */
                if (val == 2) {
                    const char *add_mn = is_32 ? "addl" : "add";
                    peep_in_flush = 1;
                    emit_inst2(add_mn, op_reg(op2->data.reg), op_reg(op2->data.reg));
                    peep_in_flush = 0;
                    return;
                }
                /* Single-instruction: x*4 → shl $2 */
                if (val == 4) {
                    peep_in_flush = 1;
                    emit_inst2(is_32 ? "shll" : "shl", op_imm(2), op_reg(op2->data.reg));
                    peep_in_flush = 0;
                    return;
                }
                /* Single-instruction: x*8 → shl $3 */
                if (val == 8) {
                    peep_in_flush = 1;
                    emit_inst2(is_32 ? "shll" : "shl", op_imm(3), op_reg(op2->data.reg));
                    peep_in_flush = 0;
                    return;
                }
                /* Multi-instruction LEA chains at -O2+ (not -Os: trades size for speed) */
                if (OPT_AT_LEAST(OPT_O2) && !OPT_SIZE_MODE && (val == 6 || val == 7)) {
                    const char *s64 = (strcmp(reg64, "r11") == 0) ? "r10" : "r11";
                    const char *sreg = is_32 ? (strcmp(s64, "r11") == 0 ? "r11d" : "r10d") : s64;
                    peep_in_flush = 1;
                    if (val == 6) {
                        /* x*6: lea (%r,%r,2), %tmp → 3x; lea (%tmp,%tmp,1), %r → 6x */
                        emit_inst2(lea_mn, op_sib(reg64, reg64, 2, 0), op_reg(sreg));
                        emit_inst2(lea_mn, op_sib(s64, s64, 1, 0), op_reg(op2->data.reg));
                    } else {
                        /* x*7: lea (%r,%r,2), %tmp → 3x; lea (%r,%tmp,2), %r → x+6x=7x */
                        emit_inst2(lea_mn, op_sib(reg64, reg64, 2, 0), op_reg(sreg));
                        emit_inst2(lea_mn, op_sib(reg64, s64, 2, 0), op_reg(op2->data.reg));
                    }
                    peep_in_flush = 0;
                    return;
                }
            }
            /* cmp $0, %reg → test %reg, %reg (shorter encoding, same flags) */
            if ((strcmp(mnemonic, "cmp") == 0 || strcmp(mnemonic, "cmpl") == 0) &&
                op1->type == OP_IMM && op1->data.imm == 0 && op2->type == OP_REG) {
                int is_32 = (strcmp(mnemonic, "cmpl") == 0);
                peep_in_flush = 1;
                emit_inst2(is_32 ? "testl" : "test",
                           op_reg(op2->data.reg), op_reg(op2->data.reg));
                peep_in_flush = 0;
                return;
            }
            /* mov %reg, %reg → nop (self-move) */
            if (strcmp(mnemonic, "mov") == 0 &&
                op1->type == OP_REG && op2->type == OP_REG &&
                strcmp(op1->data.reg, op2->data.reg) == 0) {
                return;
            }
        }
    }

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
        else if (strcmp(mnemonic, "leal") == 0) m = "lea";
        else if (strcmp(mnemonic, "testl") == 0) m = "test";
        else if (strcmp(mnemonic, "movzbl") == 0) m = "movzx";
    }

    fprintf(out, "    %s ", m);
    if (current_syntax == SYNTAX_ATT) {
        print_operand(op1);
        fprintf(out, ", ");
        print_operand(op2);
    } else {
        print_operand(op2);
        fprintf(out, ", ");
        if (strcmp(mnemonic, "movzbl") == 0 && op1->type == OP_MEM) {
            fprintf(out, "byte ptr ");
        }
        print_operand(op1);
    }
    fprintf(out, "\n");
}

/* 3-operand AVX instruction: emit_inst3("vaddps", src1, src2, dest) */
static void emit_inst3(const char *mnemonic, Operand *op1, Operand *op2, Operand *op3) {
    if (!peep_in_flush && current_section == SECTION_TEXT) {
        peep_flush_setcc();
        peep_flush_push();
        peep_flush_jcc();
        peep_flush_pair();
        peep_flush_jmp();
        if (peep_unreachable) return;
    }

    if (obj_writer) {
        encode_inst3(&obj_writer->text_section, mnemonic, op1, op2, op3);
        return;
    }
    /* Text assembly output */
    fprintf(out, "    %s ", mnemonic);
    if (current_syntax == SYNTAX_ATT) {
        /* AT&T: src1, src2, dest */
        print_operand(op1);
        fprintf(out, ", ");
        print_operand(op2);
        fprintf(out, ", ");
        print_operand(op3);
    } else {
        /* Intel: dest, src2, src1 */
        print_operand(op3);
        fprintf(out, ", ");
        print_operand(op2);
        fprintf(out, ", ");
        print_operand(op1);
    }
    fprintf(out, "\n");
}

static const char *get_reg_32(const char *reg64) {
    if (strcmp(reg64, "eax") == 0) return "eax";
    if (strcmp(reg64, "ecx") == 0) return "ecx";
    if (strcmp(reg64, "edx") == 0) return "edx";
    if (strcmp(reg64, "ebx") == 0) return "ebx";
    if (strcmp(reg64, "esi") == 0) return "esi";
    if (strcmp(reg64, "edi") == 0) return "edi";
    if (strcmp(reg64, "r8") == 0) return "r8d";
    if (strcmp(reg64, "r9") == 0) return "r9d";
    return reg64;
}

static const char *get_reg_16(const char *reg64) {
    if (strcmp(reg64, "eax") == 0) return "ax";
    if (strcmp(reg64, "ecx") == 0) return "cx";
    if (strcmp(reg64, "edx") == 0) return "dx";
    if (strcmp(reg64, "ebx") == 0) return "bx";
    if (strcmp(reg64, "esi") == 0) return "si";
    if (strcmp(reg64, "edi") == 0) return "di";
    if (strcmp(reg64, "r8") == 0) return "r8w";
    if (strcmp(reg64, "r9") == 0) return "r9w";
    return reg64;
}

static const char *get_reg_8(const char *reg64) {
    if (strcmp(reg64, "eax") == 0) return "al";
    if (strcmp(reg64, "ecx") == 0) return "cl";
    if (strcmp(reg64, "edx") == 0) return "dl";
    if (strcmp(reg64, "ebx") == 0) return "bl";
    if (strcmp(reg64, "esi") == 0) return "sil";
    if (strcmp(reg64, "edi") == 0) return "dil";
    if (strcmp(reg64, "r8") == 0) return "r8b";
    if (strcmp(reg64, "r9") == 0) return "r9b";
    return reg64;
}


static int is_float_type(Type *t) {
    return t && (t->kind == TYPE_FLOAT || t->kind == TYPE_DOUBLE);
}

/* Check if an expression is simple enough for cmov (no side effects, single instruction) */
static int is_simple_scalar_expr(ASTNode *node) {
    if (!node) return 0;
    if (node->type == AST_INTEGER) return 1;
    if (node->type == AST_IDENTIFIER) {
        Type *t = get_expr_type(node);
        if (!t) return 1; /* unknown type, assume scalar */
        if (is_float_type(t)) return 0;
        if (t->kind == TYPE_STRUCT || t->kind == TYPE_UNION || t->kind == TYPE_ARRAY) return 0;
        return 1;
    }
    return 0;
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
        /* Comma operator: type is the type of the right operand */
        if (op == TOKEN_COMMA) {
            return get_expr_type(node->data.binary_expr.right);
        }
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
            emit_inst2("lea", op_label(label), op_reg("eax"));
            node->resolved_type = type_ptr(get_local_type(node->data.identifier.name));
            return;
        }
        int offset = get_local_offset(node->data.identifier.name);
        if (offset != 0) {
            emit_inst2("lea", op_mem("ebp", offset), op_reg("eax"));
            node->resolved_type = type_ptr(get_local_type(node->data.identifier.name));
        } else {
            // Global
            emit_inst2("lea", op_label(node->data.identifier.name), op_reg("eax"));
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
                    emit_inst2("add", op_imm(st->data.struct_data.members[i].offset), op_reg("eax"));
                    break;
                }
            }
        } else {
        }
    } else if (node->type == AST_ARRAY_ACCESS) {
        if (!node->data.array_access.array || !node->data.array_access.index) { fprintf(stderr, "      Array: NULL child!\n"); return; }
        gen_expression(node->data.array_access.array);
        emit_inst1("push", op_reg("eax"));
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
        
        emit_inst2("imul", op_imm(element_size), op_reg("eax"));
        emit_inst1("pop", op_reg("ecx"));
        stack_offset += 8;
        emit_inst2("add", op_reg("ecx"), op_reg("eax"));
    } else if (node->type == AST_CALL) {
        /* For struct-returning calls, gen_expression returns a pointer in %eax */
        gen_expression(node);
    }
}

/* Instruction scheduling helper: check if an expression, when generated,
 * only uses %eax/%eax (and not %ecx or the stack).  When true, the binary
 * expression codegen can schedule the independent loads closer together
 * by using "mov %eax, %ecx" instead of "push %eax ... pop %ecx".
 * This eliminates 2 memory operations and improves ILP. */
static int gen_expr_is_eax_only(ASTNode *node) {
    if (!node) return 0;
    switch (node->type) {
        case AST_INTEGER:
        case AST_IDENTIFIER:
            return 1;
        case AST_NEG:
        case AST_NOT:
        case AST_BITWISE_NOT:
        case AST_ADDR_OF:
            return gen_expr_is_eax_only(node->data.unary.expression);
        case AST_CAST:
            return gen_expr_is_eax_only(node->data.cast.expression);
        case AST_DEREF:
            return gen_expr_is_eax_only(node->data.unary.expression);
        default:
            return 0;
    }
}

static void gen_binary_expr(ASTNode *node) {
    /* Comma operator: evaluate left for side effects, result is right */
    if (node->data.binary_expr.op == TOKEN_COMMA) {
        gen_expression(node->data.binary_expr.left);
        gen_expression(node->data.binary_expr.right);
        node->resolved_type = get_expr_type(node->data.binary_expr.right);
        return;
    }

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
             emit_inst2("xor", op_reg("eax"), op_reg("eax"));
             if (lt->kind == TYPE_FLOAT) {
                 emit_inst2("cvtsi2ss", op_reg("eax"), op_reg("xmm1"));
                 emit_inst2("ucomiss", op_reg("xmm1"), op_reg("xmm0"));
             } else {
                 emit_inst2("cvtsi2sd", op_reg("eax"), op_reg("xmm1"));
                 emit_inst2("ucomisd", op_reg("xmm1"), op_reg("xmm0"));
             }
             emit_inst1(is_and ? "jz" : "jnz", op_label(sl));
        } else {
             emit_inst2("test", op_reg("eax"), op_reg("eax"));
             emit_inst1(is_and ? "jz" : "jnz", op_label(sl));
        }

        gen_expression(node->data.binary_expr.right);
        Type *rt = get_expr_type(node->data.binary_expr.right);
        if (is_float_type(rt)) {
             emit_inst2("xor", op_reg("eax"), op_reg("eax"));
             if (rt->kind == TYPE_FLOAT) {
                 emit_inst2("cvtsi2ss", op_reg("eax"), op_reg("xmm1"));
                 emit_inst2("ucomiss", op_reg("xmm1"), op_reg("xmm0"));
             } else {
                 emit_inst2("cvtsi2sd", op_reg("eax"), op_reg("xmm1"));
                 emit_inst2("ucomisd", op_reg("xmm1"), op_reg("xmm0"));
             }
             emit_inst1(is_and ? "jz" : "jnz", op_label(sl));
        } else {
             emit_inst2("test", op_reg("eax"), op_reg("eax"));
             emit_inst1(is_and ? "jz" : "jnz", op_label(sl));
        }

        emit_inst2("mov", op_imm(is_and ? 1 : 0), op_reg("eax"));
        emit_inst1("jmp", op_label(el));
        emit_label_def(sl);
        emit_inst2("mov", op_imm(is_and ? 0 : 1), op_reg("eax"));
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
             emit_inst2(is_double ? "cvtsi2sd" : "cvtsi2ss", op_reg("eax"), op_reg("xmm0"));
        } else if (is_double && rt->kind == TYPE_FLOAT) {
             emit_inst2("cvtss2sd", op_reg("xmm0"), op_reg("xmm0"));
        }
        emit_push_xmm("xmm0");
        
        gen_expression(node->data.binary_expr.left);
        if (!is_float_type(lt)) {
             emit_inst2(is_double ? "cvtsi2sd" : "cvtsi2ss", op_reg("eax"), op_reg("xmm0"));
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
                emit_inst2("movzbl", op_reg("al"), op_reg("eax"));
                node->resolved_type = type_int();
                last_value_clear();
                return;
            default: break;
        }
        node->resolved_type = is_double ? type_double() : type_float();
        last_value_clear();
        return;
    }

    // Integer branch

    /* -O1: Immediate operand optimization.
     * When the right operand is a small integer constant, avoid push/pop
     * and use the immediate directly with the instruction:
     *   Before: mov $K,%eax; push %eax; gen(left); pop %ecx; OP %ecx,%eax
     *   After:  gen(left); OP $K,%eax
     * This saves 2 memory ops + several bytes per binary expression with a constant.
     * Only for ops that support immediate operands (not div/mod, which need %ecx).
     */
    ASTNode *right_node = node->data.binary_expr.right;
    if (OPT_AT_LEAST(OPT_O1) &&
        right_node->type == AST_INTEGER &&
        node->data.binary_expr.op != TOKEN_SLASH &&
        node->data.binary_expr.op != TOKEN_PERCENT &&
        node->data.binary_expr.op != TOKEN_AMPERSAND_AMPERSAND &&
        node->data.binary_expr.op != TOKEN_PIPE_PIPE) {
        
        long long imm = right_node->data.integer.value;
        Type *left_type = get_expr_type(node->data.binary_expr.left);
        Type *right_type = get_expr_type(right_node);
        
        int use_32bit = 0;
        if (left_type && right_type &&
            left_type->kind != TYPE_PTR && left_type->kind != TYPE_ARRAY &&
            right_type->kind != TYPE_PTR && right_type->kind != TYPE_ARRAY &&
            left_type->size <= 4 && right_type->size <= 4) {
            use_32bit = 1;
        }
        
        gen_expression(node->data.binary_expr.left);
        
        /* If 64-bit mode and immediate doesn't fit in signed 32-bit,
         * load it into %r10 first, then use %r10 as the operand.
         * x86-64 ALU instructions only accept sign-extended 32-bit immediates. */
        int imm_needs_reg = (!use_32bit && (imm > 0x7FFFFFFFLL || imm < -0x80000000LL));
        if (imm_needs_reg) {
            emit_inst2("movabs", op_imm(imm), op_reg("r10"));
        }
        
        switch (node->data.binary_expr.op) {
            case TOKEN_PLUS:
                if (left_type && (left_type->kind == TYPE_PTR || left_type->kind == TYPE_ARRAY)) {
                    int psize = 1;
                    if (left_type->data.ptr_to) psize = left_type->data.ptr_to->size;
                    if (psize > 1) imm *= psize;
                    node->resolved_type = left_type;
                } else {
                    node->resolved_type = left_type ? left_type : right_type;
                }
                if (use_32bit) emit_inst2("addl", op_imm(imm), op_reg("eax"));
                else if (imm_needs_reg) emit_inst2("add", op_reg("r10"), op_reg("eax"));
                else emit_inst2("add", op_imm(imm), op_reg("eax"));
                last_value_clear();
                return;
            case TOKEN_MINUS:
                if (left_type && (left_type->kind == TYPE_PTR || left_type->kind == TYPE_ARRAY)) {
                    int psize = 1;
                    if (left_type->data.ptr_to) psize = left_type->data.ptr_to->size;
                    if (psize > 1) imm *= psize;
                    node->resolved_type = left_type;
                } else {
                    node->resolved_type = left_type;
                }
                if (use_32bit) emit_inst2("subl", op_imm(imm), op_reg("eax"));
                else if (imm_needs_reg) emit_inst2("sub", op_reg("r10"), op_reg("eax"));
                else emit_inst2("sub", op_imm(imm), op_reg("eax"));
                last_value_clear();
                return;
            case TOKEN_STAR:
                if (use_32bit) emit_inst2("imull", op_imm(imm), op_reg("eax"));
                else if (imm_needs_reg) emit_inst2("imul", op_reg("r10"), op_reg("eax"));
                else emit_inst2("imul", op_imm(imm), op_reg("eax"));
                node->resolved_type = left_type;
                last_value_clear();
                return;
            case TOKEN_AMPERSAND:
                if (use_32bit) emit_inst2("andl", op_imm(imm), op_reg("eax"));
                else if (imm_needs_reg) emit_inst2("and", op_reg("r10"), op_reg("eax"));
                else emit_inst2("and", op_imm(imm), op_reg("eax"));
                node->resolved_type = left_type;
                last_value_clear();
                return;
            case TOKEN_PIPE:
                if (use_32bit) emit_inst2("orl", op_imm(imm), op_reg("eax"));
                else if (imm_needs_reg) emit_inst2("or", op_reg("r10"), op_reg("eax"));
                else emit_inst2("or", op_imm(imm), op_reg("eax"));
                node->resolved_type = left_type;
                last_value_clear();
                return;
            case TOKEN_CARET:
                if (use_32bit) emit_inst2("xorl", op_imm(imm), op_reg("eax"));
                else if (imm_needs_reg) emit_inst2("xor", op_reg("r10"), op_reg("eax"));
                else emit_inst2("xor", op_imm(imm), op_reg("eax"));
                node->resolved_type = left_type;
                last_value_clear();
                return;
            case TOKEN_LESS_LESS:
                emit_inst2("shl", op_imm(imm), op_reg("eax"));
                node->resolved_type = left_type;
                last_value_clear();
                return;
            case TOKEN_GREATER_GREATER:
                emit_inst2("sar", op_imm(imm), op_reg("eax"));
                node->resolved_type = left_type;
                last_value_clear();
                return;
            case TOKEN_EQUAL_EQUAL:
            case TOKEN_BANG_EQUAL:
            case TOKEN_LESS:
            case TOKEN_GREATER:
            case TOKEN_LESS_EQUAL:
            case TOKEN_GREATER_EQUAL: {
                Type *cmp_type = left_type ? left_type : right_type;
                if (cmp_type && cmp_type->size == 4) emit_inst2("cmpl", op_imm(imm), op_reg("eax"));
                else if (imm_needs_reg) emit_inst2("cmp", op_reg("r10"), op_reg("eax"));
                else emit_inst2("cmp", op_imm(imm), op_reg("eax"));
                
                if (node->data.binary_expr.op == TOKEN_EQUAL_EQUAL) emit_inst1("sete", op_reg("al"));
                else if (node->data.binary_expr.op == TOKEN_BANG_EQUAL) emit_inst1("setne", op_reg("al"));
                else if (node->data.binary_expr.op == TOKEN_LESS) emit_inst1("setl", op_reg("al"));
                else if (node->data.binary_expr.op == TOKEN_GREATER) emit_inst1("setg", op_reg("al"));
                else if (node->data.binary_expr.op == TOKEN_LESS_EQUAL) emit_inst1("setle", op_reg("al"));
                else if (node->data.binary_expr.op == TOKEN_GREATER_EQUAL) emit_inst1("setge", op_reg("al"));
                
                emit_inst2("movzbl", op_reg("al"), op_reg("eax"));
                node->resolved_type = type_int();
                last_value_clear();
                return;
            }
            default: break;  /* fall through to general path */
        }
    }

    gen_expression(node->data.binary_expr.right);

    /* Instruction scheduling: when the left operand is a simple expression
     * (only uses %eax), use "mov %eax, %ecx" instead of push/pop to save
     * the right operand.  This eliminates 2 memory operations (push write +
     * pop read) and schedules independent loads closer together for better
     * instruction-level parallelism on modern out-of-order CPUs.
     *   Before: gen(R); push %eax; gen(L); pop %ecx
     *   After:  gen(R); mov %eax,%ecx; gen(L)                */
    if (OPT_AT_LEAST(OPT_O2) &&
        gen_expr_is_eax_only(node->data.binary_expr.left)) {
        emit_inst2("mov", op_reg("eax"), op_reg("ecx"));
        gen_expression(node->data.binary_expr.left);
    } else {
        emit_inst1("push", op_reg("eax"));
        stack_offset -= 8;
        gen_expression(node->data.binary_expr.left);
        emit_inst1("pop", op_reg("ecx"));
        stack_offset += 8;
    }
    
    Type *left_type = get_expr_type(node->data.binary_expr.left);
    Type *right_type = get_expr_type(node->data.binary_expr.right);
    
    // Helper to get element size
    int size = 1;
    if (left_type && (left_type->kind == TYPE_PTR || left_type->kind == TYPE_ARRAY) && left_type->data.ptr_to) {
        size = left_type->data.ptr_to->size;
    } else if (right_type && (right_type->kind == TYPE_PTR || right_type->kind == TYPE_ARRAY) && right_type->data.ptr_to) {
        size = right_type->data.ptr_to->size;
    }
    
    // Determine if we should use 32-bit or 64-bit integer operations
    // Use 32-bit only when both operands are <= 4 bytes and neither is a pointer
    int use_32bit = 0;
    if (left_type && right_type &&
        left_type->kind != TYPE_PTR && left_type->kind != TYPE_ARRAY &&
        right_type->kind != TYPE_PTR && right_type->kind != TYPE_ARRAY &&
        left_type->size <= 4 && right_type->size <= 4) {
        use_32bit = 1;
    }

    switch (node->data.binary_expr.op) {
        case TOKEN_PLUS:
            if ((left_type && (left_type->kind == TYPE_PTR || left_type->kind == TYPE_ARRAY)) && 
                (right_type && (right_type->kind == TYPE_INT || right_type->kind == TYPE_CHAR))) {
                if (size > 1) emit_inst2("imul", op_imm(size), op_reg("ecx"));
                node->resolved_type = left_type;
            } else if ((left_type && (left_type->kind == TYPE_INT || left_type->kind == TYPE_CHAR)) && 
                       (right_type && (right_type->kind == TYPE_PTR || right_type->kind == TYPE_ARRAY))) {
                if (size > 1) emit_inst2("imul", op_imm(size), op_reg("eax"));
                node->resolved_type = right_type;
            } else {
                node->resolved_type = left_type ? left_type : right_type;
            }
            if (use_32bit) emit_inst2("addl", op_reg("ecx"), op_reg("eax"));
            else emit_inst2("add", op_reg("ecx"), op_reg("eax"));
            break;
        case TOKEN_MINUS: 
            if ((left_type && (left_type->kind == TYPE_PTR || left_type->kind == TYPE_ARRAY)) && 
                (right_type && (right_type->kind == TYPE_INT || right_type->kind == TYPE_CHAR))) {
                if (size > 1) emit_inst2("imul", op_imm(size), op_reg("ecx"));
                emit_inst2("sub", op_reg("ecx"), op_reg("eax")); // Pointers are 64-bit
                node->resolved_type = left_type;
            } else if ((left_type && (left_type->kind == TYPE_PTR || left_type->kind == TYPE_ARRAY)) && 
                       (right_type && (right_type->kind == TYPE_PTR || right_type->kind == TYPE_ARRAY))) {
                emit_inst2("sub", op_reg("ecx"), op_reg("eax"));
                if (size > 1) {
                    emit_inst0("cdq");
                    emit_inst2("mov", op_imm(size), op_reg("ecx"));
                    emit_inst1("idiv", op_reg("ecx"));
                }
                node->resolved_type = type_int();
            } else {
                if (use_32bit) emit_inst2("subl", op_reg("ecx"), op_reg("eax"));
                else emit_inst2("sub", op_reg("ecx"), op_reg("eax"));
                node->resolved_type = left_type;
            }
            break;
        case TOKEN_STAR:  
            if (use_32bit) emit_inst2("imull", op_reg("ecx"), op_reg("eax"));
            else emit_inst2("imul", op_reg("ecx"), op_reg("eax")); 
            node->resolved_type = left_type;
            break;
        case TOKEN_SLASH:
        case TOKEN_PERCENT:
            emit_inst0("cdq");
            emit_inst1("idiv", op_reg("ecx"));
            if (node->data.binary_expr.op == TOKEN_PERCENT) {
                emit_inst2("mov", op_reg("edx"), op_reg("eax"));
            }
            node->resolved_type = left_type;
            break;
        case TOKEN_AMPERSAND:
            if (use_32bit) emit_inst2("andl", op_reg("ecx"), op_reg("eax"));
            else emit_inst2("and", op_reg("ecx"), op_reg("eax"));
            node->resolved_type = left_type;
            break;
        case TOKEN_PIPE:
            if (use_32bit) emit_inst2("orl", op_reg("ecx"), op_reg("eax"));
            else emit_inst2("or", op_reg("ecx"), op_reg("eax"));
            node->resolved_type = left_type;
            break;
        case TOKEN_CARET:
            if (use_32bit) emit_inst2("xorl", op_reg("ecx"), op_reg("eax"));
            else emit_inst2("xor", op_reg("ecx"), op_reg("eax"));
            node->resolved_type = left_type;
            break;
        case TOKEN_LESS_LESS:
            emit_inst2("shl", op_reg("cl"), op_reg("eax"));
            node->resolved_type = left_type;
            break;
        case TOKEN_GREATER_GREATER:
            emit_inst2("sar", op_reg("cl"), op_reg("eax"));
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
            else emit_inst2("cmp", op_reg("ecx"), op_reg("eax"));
            
            if (node->data.binary_expr.op == TOKEN_EQUAL_EQUAL) emit_inst1("sete", op_reg("al"));
            else if (node->data.binary_expr.op == TOKEN_BANG_EQUAL) emit_inst1("setne", op_reg("al"));
            else if (node->data.binary_expr.op == TOKEN_LESS) emit_inst1("setl", op_reg("al"));
            else if (node->data.binary_expr.op == TOKEN_GREATER) emit_inst1("setg", op_reg("al"));
            else if (node->data.binary_expr.op == TOKEN_LESS_EQUAL) emit_inst1("setle", op_reg("al"));
            else if (node->data.binary_expr.op == TOKEN_GREATER_EQUAL) emit_inst1("setge", op_reg("al"));
            
            emit_inst2("movzbl", op_reg("al"), op_reg("eax"));
            break;
        }
        default: break;
    }
    last_value_clear();
}

static void gen_expression(ASTNode *node) {
    if (!node) return;
    if (node->resolved_type == NULL) {
        node->resolved_type = get_expr_type(node);
    }
    if (node->type == AST_INTEGER) {
        /* -O1: mov $0 → xor (smaller, faster; also breaks false dependencies) */
        if (OPT_AT_LEAST(OPT_O1) && node->data.integer.value == 0) {
            emit_inst2("xor", op_reg("eax"), op_reg("eax"));
        } else {
            emit_inst2("mov", op_imm(node->data.integer.value), op_reg("eax"));
        }
        node->resolved_type = type_int();
        last_value_clear();
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
        last_value_clear();
    } else if (node->type == AST_IDENTIFIER) {
        if (!node->data.identifier.name) { fprintf(stderr, "      Ident: NULL NAME!\n"); return; }
        /* Register allocator: check if variable lives in a register */
        const char *ra_reg = get_local_reg(node->data.identifier.name);
        if (ra_reg) {
            Type *t = get_local_type(node->data.identifier.name);
            if (last_value_match_reg(ra_reg, t)) {
                node->resolved_type = t;
                return;
            }
            /* Variable is in a register — just move to %eax */
            if (t && t->size == 4) {
                /* 32-bit: use movl for zero-extension */
                const char *r32 = get_local_reg32(node->data.identifier.name);
                if (r32) emit_inst2("movl", op_reg(r32), op_reg("eax"));
                else emit_inst2("mov", op_reg(ra_reg), op_reg("eax"));
            } else if (t && t->size == 1) {
                const char *r8 = get_local_reg8(node->data.identifier.name);
                if (r8) emit_inst2("movzbl", op_reg(r8), op_reg("eax"));
                else emit_inst2("mov", op_reg(ra_reg), op_reg("eax"));
            } else {
                emit_inst2("mov", op_reg(ra_reg), op_reg("eax"));
            }
            node->resolved_type = t;
            last_value_set_reg(ra_reg, t);
            return;
        }
        const char *label = get_local_label(node->data.identifier.name);
        if (label) {
            Type *t = get_local_type(node->data.identifier.name);
            if (t && (t->kind == TYPE_ARRAY || t->kind == TYPE_STRUCT || t->kind == TYPE_UNION)) {
                emit_inst2("lea", op_label(label), op_reg("eax"));
            } else if (is_float_type(t)) {
                if (t->kind == TYPE_FLOAT) emit_inst2("movss", op_label(label), op_reg("xmm0"));
                else emit_inst2("movsd", op_label(label), op_reg("xmm0"));
            } else {
                if (last_value_match_label(label, t)) {
                    node->resolved_type = t;
                    return;
                }
                if (t && t->size == 1) emit_inst2("movzbl", op_label(label), op_reg("eax"));
                else if (t && t->size == 2) emit_inst2("movzwl", op_label(label), op_reg("eax"));
                else if (t && t->size == 4) emit_inst2("movl", op_label(label), op_reg("eax"));
                else emit_inst2("mov", op_label(label), op_reg("eax"));
            }
            node->resolved_type = t;
            if (last_value_can_track(t)) last_value_set_label(label, t);
            else last_value_clear();
            return;
        }
        int offset = get_local_offset(node->data.identifier.name);
        if (offset != 0) {
            Type *t = get_local_type(node->data.identifier.name);
            if (t && (t->kind == TYPE_ARRAY || t->kind == TYPE_STRUCT || t->kind == TYPE_UNION)) {
                // Array/struct/union decays to pointer (address)
                emit_inst2("lea", op_mem("ebp", offset), op_reg("eax"));
            } else if (is_float_type(t)) {
                if (t->kind == TYPE_FLOAT) emit_inst2("movss", op_mem("ebp", offset), op_reg("xmm0"));
                else emit_inst2("movsd", op_mem("ebp", offset), op_reg("xmm0"));
            } else {
                if (last_value_match_stack(offset, t)) {
                    node->resolved_type = t;
                    return;
                }
                if (t && t->size == 1) emit_inst2("movzbl", op_mem("ebp", offset), op_reg("eax"));
                else if (t && t->size == 2) emit_inst2("movzwl", op_mem("ebp", offset), op_reg("eax"));
                else if (t && t->size == 4) emit_inst2("movl", op_mem("ebp", offset), op_reg("eax"));
                else emit_inst2("mov", op_mem("ebp", offset), op_reg("eax"));
            }
            node->resolved_type = t;
            if (last_value_can_track(t)) last_value_set_stack(offset, t);
            else last_value_clear();
        } else {
            // Global
            Type *t = get_global_type(node->data.identifier.name);
            if (t && (t->kind == TYPE_ARRAY || t->kind == TYPE_STRUCT || t->kind == TYPE_UNION)) {
                emit_inst2("lea", op_label(node->data.identifier.name), op_reg("eax"));
            } else if (is_float_type(t)) {
                if (t->kind == TYPE_FLOAT) emit_inst2("movss", op_label(node->data.identifier.name), op_reg("xmm0"));
                else emit_inst2("movsd", op_label(node->data.identifier.name), op_reg("xmm0"));
            } else {
                if (last_value_match_label(node->data.identifier.name, t)) {
                    node->resolved_type = t;
                    return;
                }
                if (t && t->size == 1) emit_inst2("movzbl", op_label(node->data.identifier.name), op_reg("eax"));
                else if (t && t->size == 2) emit_inst2("movzwl", op_label(node->data.identifier.name), op_reg("eax"));
                else if (t && t->size == 4) emit_inst2("movl", op_label(node->data.identifier.name), op_reg("eax"));
                else emit_inst2("mov", op_label(node->data.identifier.name), op_reg("eax"));
            }
            node->resolved_type = t;
            if (last_value_can_track(t)) last_value_set_label(node->data.identifier.name, t);
            else last_value_clear();
        }
    } else if (node->type == AST_ARRAY_ACCESS) {
        gen_addr(node);
        Type *t = node->resolved_type; // Element type
        if (t && t->kind == TYPE_ARRAY) {
            // Array element decays to pointer - address is already the value
        } else if (t && (t->kind == TYPE_STRUCT || t->kind == TYPE_UNION)) {
            // Struct/union element: address is the value (passed by reference)
        } else if (is_float_type(t)) {
            if (t->size == 4) emit_inst2("movss", op_mem("eax", 0), op_reg("xmm0"));
            else emit_inst2("movsd", op_mem("eax", 0), op_reg("xmm0"));
        } else if (t && t->size == 1) {
            emit_inst2("movzbl", op_mem("eax", 0), op_reg("eax"));
        } else if (t && t->size == 2) {
            emit_inst2("movzwl", op_mem("eax", 0), op_reg("eax"));
        } else if (t && t->size == 4) {
            emit_inst2("movl", op_mem("eax", 0), op_reg("eax"));
        } else {
            emit_inst2("mov", op_mem("eax", 0), op_reg("eax"));
        }
        last_value_clear();
    } else if (node->type == AST_BINARY_EXPR) {
        gen_binary_expr(node);
    } else if (node->type == AST_PRE_INC || node->type == AST_PRE_DEC || 
               node->type == AST_POST_INC || node->type == AST_POST_DEC) {
        int is_inc = (node->type == AST_PRE_INC || node->type == AST_POST_INC);
        int is_pre = (node->type == AST_PRE_INC || node->type == AST_PRE_DEC);
        
        Type *t = get_expr_type(node->data.unary.expression);

        const char *ident_name = NULL;
        if (node->data.unary.expression && node->data.unary.expression->type == AST_IDENTIFIER) {
            ident_name = node->data.unary.expression->data.identifier.name;
        }

        /* Register allocator fast path: if the operand is an identifier in a register,
         * directly increment/decrement the register without going through memory. */
        if (ident_name) {
            const char *ra_reg = get_local_reg(ident_name);
            if (ra_reg) {
                int step = 1;
                if (t && (t->kind == TYPE_PTR || t->kind == TYPE_ARRAY) && t->data.ptr_to) {
                    step = t->data.ptr_to->size;
                }
                if (!is_pre) {
                    /* POST: result is old value */
                    emit_inst2("mov", op_reg(ra_reg), op_reg("eax"));
                }
                if (is_inc) {
                    emit_inst2("add", op_imm(step), op_reg(ra_reg));
                } else {
                    emit_inst2("sub", op_imm(step), op_reg(ra_reg));
                }
                if (is_pre) {
                    /* PRE: result is new value */
                    emit_inst2("mov", op_reg(ra_reg), op_reg("eax"));
                }
                node->resolved_type = t;
                if (is_pre) {
                    last_value_set_reg(ra_reg, t);
                } else {
                    /* POST: eax has old value, ra_reg has new — cache would be stale */
                    last_value_clear();
                }
                return;
            }
        }
        
        gen_addr(node->data.unary.expression);
        // Address is in RAX. 
        
        int size = 1;
        if (t && (t->kind == TYPE_PTR || t->kind == TYPE_ARRAY) && t->data.ptr_to) {
             size = t->data.ptr_to->size;
        }
        
        // Load value to RCX
        if (t && t->size == 1) {
            emit_inst2("movzbl", op_mem("eax", 0), op_reg("ecx"));
        } else if (t && t->size <= 4) {
            emit_inst2("mov", op_mem("eax", 0), op_reg("ecx"));
        } else {
            emit_inst2("mov", op_mem("eax", 0), op_reg("ecx"));
        }
        
        if (!is_pre) {
            emit_inst1("push", op_reg("ecx")); // Save original value
            stack_offset -= 8;
        }
        
        // Modify RCX
        if (is_inc) {
            emit_inst2("add", op_imm(size), op_reg("ecx"));
        } else {
            emit_inst2("sub", op_imm(size), op_reg("ecx"));
        }
        
        // Store back
        if (t && t->size == 1) {
            emit_inst2("mov", op_reg("cl"), op_mem("eax", 0));
        } else if (t && t->size <= 4) {
            emit_inst2("movl", op_reg("ecx"), op_mem("eax", 0));
        } else {
            emit_inst2("mov", op_reg("ecx"), op_mem("eax", 0));
        }
        
        if (!is_pre) {
            emit_inst1("pop", op_reg("eax")); // Restore original value to result register
            stack_offset += 8;
        } else {
            emit_inst2("mov", op_reg("ecx"), op_reg("eax")); // Result is new value
        }
        
        node->resolved_type = t;
        if (is_pre && ident_name) {
            /* PRE: eax holds the new value which matches memory — safe to cache */
            const char *label = get_local_label(ident_name);
            if (label) {
                last_value_set_label(label, t);
            } else {
                int offset = get_local_offset(ident_name);
                if (offset != 0) last_value_set_stack(offset, t);
                else last_value_set_label(ident_name, t);
            }
        } else {
            /* POST: eax holds the OLD pre-increment value — cache would be stale */
            last_value_clear();
        }
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
                emit_inst2("cvttss2si", op_reg("xmm0"), op_reg("eax"));
            } else {
                emit_inst2("cvttsd2si", op_reg("xmm0"), op_reg("eax"));
            }
        } else if (!is_float_type(src) && is_float_type(dst)) {
            // Int -> Float
            if (dst->kind == TYPE_FLOAT) {
                emit_inst2("cvtsi2ss", op_reg("eax"), op_reg("xmm0"));
            } else {
                emit_inst2("cvtsi2sd", op_reg("eax"), op_reg("xmm0"));
            }
        } else {
            // Int -> Int / Ptr
            if (dst->kind == TYPE_CHAR) {
                 // Cast to char: Truncate and sign-extend to 64-bit for consistency
                 emit_inst2("movsbq", op_reg("al"), op_reg("eax"));
            } 
            // Other int-to-int casts usually no-op for same size or smaller-to-larger (implicit)
            // Or larger-to-smaller (truncate, handled by using lower reg parts if needed)
            // But if we cast int (32/64) to char, we explicitly truncated above.
        }
        node->resolved_type = dst;
        last_value_clear();
    } else if (node->type == AST_ASSIGN) {
        if (!node->data.assign.left || !node->data.assign.value) { fprintf(stderr, "      Assign: NULL child!\n"); return; }
        ASTNode *left_node = node->data.assign.left;
        Type *t = get_expr_type(left_node);

        /* --- Struct / large-type assignment: use memcpy ----------- */
        if (t && t->size > 8) {
            /* Save stack_offset before the struct assignment so we can
             * fully restore it afterwards.  gen_addr(value) for struct-returning
             * calls permanently allocates an sret buffer on the stack;
             * after memcpy the buffer is no longer needed. */
            int pre_assign_stack_offset = stack_offset;
            /* Reserve a stack slot for saving the source address.
             * We must do this BEFORE gen_addr(value) because for struct-returning
             * calls, the sret buffer sits at the bottom of the stack frame.
             * A push after the call would overwrite the sret buffer contents. */
            emit_inst2("sub", op_imm(8), op_reg("esp"));
            stack_offset -= 8;
            int src_save_offset = stack_offset;
            /* Get source address */
            gen_addr(node->data.assign.value);
            /* Save source address into the reserved slot (above sret buffer) */
            emit_inst2("mov", op_reg("eax"), op_mem("ebp", src_save_offset));
            /* Get dest address → edi */
            gen_addr(left_node);
            emit_inst2("mov", op_reg("eax"), op_reg("edi"));
            /* source → esi (from saved slot) */
            emit_inst2("mov", op_mem("ebp", src_save_offset), op_reg("esi"));
            /* size → edx */
            emit_inst2("mov", op_imm(t->size), op_reg("edx"));
            /* Ensure 16-byte alignment before calling memcpy */
            int cur_depth = abs(stack_offset);
            int memcpy_pad = (16 - (cur_depth % 16)) % 16;
            if (memcpy_pad > 0) {
                emit_inst2("sub", op_imm(memcpy_pad), op_reg("esp"));
                stack_offset -= memcpy_pad;
            }
            /* call memcpy */
            emit_inst2("xor", op_reg("eax"), op_reg("eax"));
            emit_inst0("call memcpy");
            /* Restore stack fully (save slot + sret buffer + alignment padding) */
            int total_restore = abs(pre_assign_stack_offset - stack_offset);
            if (total_restore > 0) {
                emit_inst2("add", op_imm(total_restore), op_reg("esp"));
            }
            stack_offset = pre_assign_stack_offset;
            node->resolved_type = t;
            last_value_clear();
            return;
        }

        gen_expression(node->data.assign.value);
        if (left_node->type == AST_IDENTIFIER) {
            const char *ident_name = left_node->data.identifier.name;
            /* Register allocator: check if variable lives in a register */
            const char *ra_reg = get_local_reg(ident_name);
            if (ra_reg) {
                /* Store value from %eax to the register */
                emit_inst2("mov", op_reg("eax"), op_reg(ra_reg));
                node->resolved_type = t;
                last_value_set_reg(ra_reg, t);
                return;
            }
            const char *label = get_local_label(ident_name);
            if (label) {
                if (is_float_type(t)) {
                    if (t && t->kind == TYPE_FLOAT) emit_inst2("movss", op_reg("xmm0"), op_label(label));
                    else emit_inst2("movsd", op_reg("xmm0"), op_label(label));
                } else {
                    if (t && t->size == 1) emit_inst2("movb", op_reg("al"), op_label(label));
                    else if (t && t->size == 2) emit_inst2("movw", op_reg("ax"), op_label(label));
                    else if (t && t->size == 4) emit_inst2("movl", op_reg("eax"), op_label(label));
                    else emit_inst2("mov", op_reg("eax"), op_label(label));
                }
                if (!is_float_type(t)) last_value_set_label(label, t);
                else last_value_clear();
                return;
            }
            int offset = get_local_offset(ident_name);
            if (offset != 0) {
                if (is_float_type(t)) {
                    if (t && t->kind == TYPE_FLOAT) emit_inst2("movss", op_reg("xmm0"), op_mem("ebp", offset));
                    else emit_inst2("movsd", op_reg("xmm0"), op_mem("ebp", offset));
                } else {
                    if (t && t->size == 1) emit_inst2("movb", op_reg("al"), op_mem("ebp", offset));
                    else if (t && t->size == 2) emit_inst2("movw", op_reg("ax"), op_mem("ebp", offset));
                    else if (t && t->size == 4) emit_inst2("movl", op_reg("eax"), op_mem("ebp", offset));
                    else emit_inst2("mov", op_reg("eax"), op_mem("ebp", offset));
                }
                if (!is_float_type(t)) last_value_set_stack(offset, t);
                else last_value_clear();
            } else if (ident_name) {
                // Global
                if (is_float_type(t)) {
                    if (t && t->kind == TYPE_FLOAT) emit_inst2("movss", op_reg("xmm0"), op_label(ident_name));
                    else emit_inst2("movsd", op_reg("xmm0"), op_label(ident_name));
                } else {
                    if (t && t->size == 1) emit_inst2("movb", op_reg("al"), op_label(ident_name));
                    else if (t && t->size == 2) emit_inst2("movw", op_reg("ax"), op_label(ident_name));
                    else if (t && t->size == 4) emit_inst2("movl", op_reg("eax"), op_label(ident_name));
                    else emit_inst2("mov", op_reg("eax"), op_label(ident_name));
                }
                if (!is_float_type(t)) last_value_set_label(ident_name, t);
                else last_value_clear();
            }
        } else {
            if (is_float_type(t)) {
                emit_push_xmm("xmm0");
                gen_addr(left_node);
                emit_pop_xmm("xmm1");
                if (t && t->kind == TYPE_FLOAT) emit_inst2("movss", op_reg("xmm1"), op_mem("eax", 0));
                else emit_inst2("movsd", op_reg("xmm1"), op_mem("eax", 0));
                if (t && t->kind == TYPE_FLOAT) emit_inst2("movss", op_reg("xmm1"), op_reg("xmm0"));
                else emit_inst2("movsd", op_reg("xmm1"), op_reg("xmm0"));
                last_value_clear();
            } else {
                emit_inst1("push", op_reg("eax"));
                gen_addr(left_node);
                emit_inst1("pop", op_reg("ecx"));
                if (t && t->size == 1) emit_inst2("movb", op_reg("cl"), op_mem("eax", 0));
                else if (t && t->size == 2) emit_inst2("movw", op_reg("cx"), op_mem("eax", 0));
                else if (t && t->size == 4) emit_inst2("movl", op_reg("ecx"), op_mem("eax", 0));
                else emit_inst2("mov", op_reg("ecx"), op_mem("eax", 0));
                emit_inst2("mov", op_reg("ecx"), op_reg("eax"));
                last_value_clear();
            }
        }
        node->resolved_type = t;
    } else if (node->type == AST_DEREF) {
        gen_expression(node->data.unary.expression);
        Type *t = get_expr_type(node->data.unary.expression);
        Type *ptr_to = (t && t->kind == TYPE_PTR) ? t->data.ptr_to : NULL;
        if (is_float_type(ptr_to)) {
            if (ptr_to->size == 4) emit_inst2("movss", op_mem("eax", 0), op_reg("xmm0"));
            else emit_inst2("movsd", op_mem("eax", 0), op_reg("xmm0"));
        } else if (ptr_to && ptr_to->kind == TYPE_CHAR) {
            emit_inst2("movzbl", op_mem("eax", 0), op_reg("eax"));
        } else {
            emit_inst2("mov", op_mem("eax", 0), op_reg("eax"));
        }
        node->resolved_type = ptr_to;
        last_value_clear();
    } else if (node->type == AST_ADDR_OF) {
        gen_addr(node->data.unary.expression);
        last_value_clear();
    } else if (node->type == AST_NEG) {
        /* Constant-fold negation of integer literals to avoid double-negation
           when the literal overflows int32 in op_imm */
        if (node->data.unary.expression && node->data.unary.expression->type == AST_INTEGER) {
            long long v = -(long long)node->data.unary.expression->data.integer.value;
            emit_inst2("mov", op_imm(v), op_reg("eax"));
            node->resolved_type = type_int();
            last_value_clear();
            return;
        }
        gen_expression(node->data.unary.expression);
        Type *t = get_expr_type(node->data.unary.expression);
        if (is_float_type(t)) {
            if (t->kind == TYPE_FLOAT) {
                emit_inst2("xor", op_reg("eax"), op_reg("eax"));
                emit_inst2("cvtsi2ss", op_reg("eax"), op_reg("xmm1")); // xmm1 = 0.0
                emit_inst2("subss", op_reg("xmm0"), op_reg("xmm1"));  // xmm1 = 0.0 - xmm0
                emit_inst2("movss", op_reg("xmm1"), op_reg("xmm0"));
            } else {
                emit_inst2("xor", op_reg("eax"), op_reg("eax"));
                emit_inst2("cvtsi2sd", op_reg("eax"), op_reg("xmm1"));
                emit_inst2("subsd", op_reg("xmm0"), op_reg("xmm1"));
                emit_inst2("movsd", op_reg("xmm1"), op_reg("xmm0"));
            }
        } else {
            emit_inst1("neg", op_reg("eax"));
        }
        node->resolved_type = t;
        last_value_clear();
    } else if (node->type == AST_NOT) {
        gen_expression(node->data.unary.expression);
        Type *t = get_expr_type(node->data.unary.expression);
        if (is_float_type(t)) {
            emit_inst2("xor", op_reg("eax"), op_reg("eax"));
            if (t->kind == TYPE_FLOAT) {
                emit_inst2("cvtsi2ss", op_reg("eax"), op_reg("xmm1"));
                emit_inst2("ucomiss", op_reg("xmm1"), op_reg("xmm0"));
            } else {
                emit_inst2("cvtsi2sd", op_reg("eax"), op_reg("xmm1"));
                emit_inst2("ucomisd", op_reg("xmm1"), op_reg("xmm0"));
            }
            emit_inst1("setz", op_reg("al"));
            emit_inst2("movzbl", op_reg("al"), op_reg("eax"));
        } else {
            emit_inst2("test", op_reg("eax"), op_reg("eax"));
            emit_inst1("setz", op_reg("al"));
            emit_inst2("movzbl", op_reg("al"), op_reg("eax"));
        }
        node->resolved_type = type_int();
        last_value_clear();
    } else if (node->type == AST_BITWISE_NOT) {
        gen_expression(node->data.unary.expression);
        emit_inst1("not", op_reg("eax"));
        node->resolved_type = get_expr_type(node->data.unary.expression);
        last_value_clear();
    } else if (node->type == AST_MEMBER_ACCESS) {
        gen_addr(node);
        Type *mt = get_expr_type(node);
        if (mt && mt->kind == TYPE_ARRAY) {
            // Array member decays to pointer - address is already the value
            node->resolved_type = mt;
        } else if (mt && (mt->kind == TYPE_STRUCT || mt->kind == TYPE_UNION)) {
            // Struct/union member: address is the value (passed by reference)
            node->resolved_type = mt;
        } else if (is_float_type(mt)) {
            if (mt->kind == TYPE_FLOAT) emit_inst2("movss", op_mem("eax", 0), op_reg("xmm0"));
            else emit_inst2("movsd", op_mem("eax", 0), op_reg("xmm0"));
            node->resolved_type = mt;
        } else if (mt && mt->size == 1) {
            emit_inst2("movzbl", op_mem("eax", 0), op_reg("eax"));
        } else if (mt && mt->size == 2) {
            emit_inst2("movzwl", op_mem("eax", 0), op_reg("eax"));
        } else if (mt && mt->size == 4) {
            emit_inst2("movl", op_mem("eax", 0), op_reg("eax"));
        } else {
            emit_inst2("mov", op_mem("eax", 0), op_reg("eax"));
        }
        last_value_clear();
    } else if (node->type == AST_CALL) {
        // Save initial stack offset to restore later
        int initial_stack_offset = stack_offset;
        
        const char **arg_regs = g_arg_regs;
        const char **xmm_arg_regs = g_xmm_arg_regs;
        int max_reg = g_max_reg_args;
        int shadow = g_use_shadow_space ? 32 : 0;
        int arg_slot_size = (g_target == TARGET_DOS) ? 4 : 8;
        
        /* Check if the called function returns a struct (needs hidden pointer) */
        Type *call_ret_type = get_expr_type(node);
        int call_sret = is_struct_return(call_ret_type);
        int call_sret_size = call_sret ? call_ret_type->size : 0;
        int call_sret_shift = call_sret ? 1 : 0;
        
        int num_args = (int)node->children_count;
        int effective_args = num_args + call_sret_shift;
        int extra_args = effective_args > max_reg ? effective_args - max_reg : 0;
        
        // Calculate padding based on CURRENT stack depth (including any pushed args from outer calls)
        int current_stack_depth = abs(stack_offset);
        /* Also account for sret temp space if needed */
        int sret_alloc = 0;
        if (call_sret) {
            sret_alloc = (call_sret_size + 15) & ~15; /* align to 16 */
        }
        int padding = (16 - ((current_stack_depth + extra_args * arg_slot_size + shadow + sret_alloc) % 16)) % 16;
        
        if (padding > 0) {
            emit_inst2("sub", op_imm(padding), op_reg("esp"));
            stack_offset -= padding;
        }
        
        /* Allocate stack space for the struct return value */
        int sret_stack_offset = 0;
        if (call_sret) {
            emit_inst2("sub", op_imm(sret_alloc), op_reg("esp"));
            stack_offset -= sret_alloc;
            sret_stack_offset = stack_offset; /* remember where it is */
        }

        for (int i = (int)node->children_count - 1; i >= 0; i--) {
            ASTNode *child = node->children[i];
            gen_expression(child);
            Type *arg_type = get_expr_type(child);
            if (is_float_type(arg_type)) {
                emit_push_xmm("xmm0");
            } else {
                emit_inst1("push", op_reg("eax"));
            }
            stack_offset -= arg_slot_size; // Update stack offset for nested calls
        }
        
        /* Pop user args into registers, shifted by 1 if sret */
        for (int i = 0; i < num_args && (i + call_sret_shift) < max_reg; i++) {
            ASTNode *pop_child = node->children[i];
            Type *pop_type = get_expr_type(pop_child);
            if (is_float_type(pop_type)) {
                emit_pop_xmm(xmm_arg_regs[i]);
            } else {
                emit_inst1("pop", op_reg(arg_regs[i + call_sret_shift]));
                stack_offset += arg_slot_size;
            }
        }
        
        /* Load hidden return pointer into %edi (first arg) */
        if (call_sret) {
            emit_inst2("lea", op_mem("ebp", sret_stack_offset), op_reg("edi"));
        }
        
        // Shadow space (Win64 only)
        if (shadow > 0) {
            emit_inst2("sub", op_imm(shadow), op_reg("esp"));
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
        
        // Clean up shadow space + extra args + padding (but NOT sret space — caller needs %eax valid)
        int cleanup = shadow + extra_args * arg_slot_size + padding;
        if (cleanup > 0) {
            emit_inst2("add", op_imm(cleanup), op_reg("esp"));
        }
        
        // Restore stack offset (sret space remains allocated on stack, cleaned up by leave)
        stack_offset = initial_stack_offset - sret_alloc;
        last_value_clear();
    } else if (node->type == AST_IF) {
        // Ternary expression: condition ? then_expr : else_expr
        ASTNode *cond = node->data.if_stmt.condition;
        ASTNode *then_br = node->data.if_stmt.then_branch;
        ASTNode *else_br = node->data.if_stmt.else_branch;

        /* cmov optimization: if both branches are simple scalars, avoid branches */
        if (OPT_AT_LEAST(OPT_O2) && !OPT_DEBUG_MODE && else_br &&
            is_simple_scalar_expr(then_br) && is_simple_scalar_expr(else_br)) {
            /* Pattern: gen(cond) → save → gen(then) → save → gen(else) → test → cmovne */
            gen_expression(cond);
            emit_inst2("mov", op_reg("eax"), op_reg("r11"));  /* save condition */
            gen_expression(then_br);
            emit_inst2("mov", op_reg("eax"), op_reg("ecx"));  /* save then-value */
            gen_expression(else_br);                           /* else-value in eax */
            emit_inst2("test", op_reg("r11"), op_reg("r11")); /* test condition */
            emit_inst2("cmovne", op_reg("ecx"), op_reg("eax")); /* if true, use then-value */
            last_value_clear();
        } else {
            int label_else = label_count++;
            int label_end = label_count++;
            char l_else[32], l_end[32];
            sprintf(l_else, ".L%d", label_else);
            sprintf(l_end, ".L%d", label_end);

            gen_expression(cond);
            emit_inst2("test", op_reg("eax"), op_reg("eax"));
            emit_inst1("je", op_label(l_else));

            gen_expression(then_br);
            emit_inst1("jmp", op_label(l_end));

            emit_label_def(l_else);
            gen_expression(else_br);

            emit_label_def(l_end);
            last_value_clear();
        }
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
            string_literals[string_literals_count].label = strdup(label);
            string_literals[string_literals_count].value = malloc(len + 1);
            memcpy(string_literals[string_literals_count].value, node->data.string.value, len + 1);
            string_literals[string_literals_count].length = len;
            string_literals_count++;
        }
        
        // Load address of the string
        emit_inst2("lea", op_label(label), op_reg("eax"));
        last_value_clear();
    }
}

static int current_function_end_label = 0;

static int break_label_stack[32];
static int break_label_ptr = 0;
static int continue_label_stack[32];
static int continue_label_ptr = 0;
static int loop_saved_stack_offset[32];
static int loop_saved_locals_count[32];
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

/* ------------------------------------------------------------------ */
/* Vectorized loop codegen — emits SSE packed instructions             */
/*                                                                     */
/* For a loop annotated with VecInfo (set by optimizer), generates:    */
/*   1. Prologue: save callee-saved regs, compute array base addrs    */
/*   2. Vector loop: process 4 elements per iteration with movups/    */
/*      movdqu + packed arithmetic                                    */
/*   3. Scalar remainder: process remaining 0-3 elements              */
/*   4. Epilogue: restore callee-saved regs                           */
/*                                                                     */
/* Uses ebx, r14, r15 as pointer registers (callee-saved, no SIB      */
/* encoding issues). ecx as element counter.                           */
/* ------------------------------------------------------------------ */
static void gen_vectorized_loop(ASTNode *node) {
    VecInfo *vi = node->vec_info;
    if (!vi) return;

    int lbl_vec = label_count++;
    int lbl_scalar = label_count++;
    int lbl_scalar_loop = label_count++;
    int lbl_done = label_count++;

    char l_vec[32], l_scalar[32], l_scalar_loop[32], l_done[32];
    sprintf(l_vec, ".L%d", lbl_vec);
    sprintf(l_scalar, ".L%d", lbl_scalar);
    sprintf(l_scalar_loop, ".L%d", lbl_scalar_loop);
    sprintf(l_done, ".L%d", lbl_done);

    /* Determine AVX vs SSE mode from vi->width */
    int use_avx = (vi->width == 8);  /* width=8 → AVX (256-bit), width=4 → SSE (128-bit) */
    int vec_elems = vi->width;       /* elements per vector iteration */
    int vec_bytes = vec_elems * vi->elem_size;  /* bytes per vector: 16 (SSE) or 32 (AVX) */

    /* Register prefix: ymm for AVX, xmm for SSE */
    const char *vreg0 = use_avx ? "ymm0" : "xmm0";
    const char *vreg1 = use_avx ? "ymm1" : "xmm1";

    /* Pick move/op mnemonics based on element type and AVX mode */
    const char *vec_mov  = NULL;
    const char *scl_mov  = vi->is_float ? "movss"  : "mov";
    const char *vec_op   = NULL;
    const char *scl_op   = NULL;

    if (use_avx) {
        /* AVX: VEX-encoded instructions, 3-operand form for arithmetic */
        vec_mov = vi->is_float ? "vmovups" : "vmovdqu";
        if (vi->is_float) {
            switch (vi->op) {
            case TOKEN_PLUS:  vec_op = "vaddps"; scl_op = "addss"; break;
            case TOKEN_MINUS: vec_op = "vsubps"; scl_op = "subss"; break;
            case TOKEN_STAR:  vec_op = "vmulps"; scl_op = "mulss"; break;
            case TOKEN_SLASH: vec_op = "vdivps"; scl_op = "divss"; break;
            default: return;
            }
        } else {
            switch (vi->op) {
            case TOKEN_PLUS:  vec_op = "vpaddd"; scl_op = "add"; break;
            case TOKEN_MINUS: vec_op = "vpsubd"; scl_op = "sub"; break;
            default: return;
            }
        }
    } else {
        /* SSE: legacy encoding, 2-operand destructive form */
        vec_mov = vi->is_float ? "movups" : "movdqu";
        if (vi->is_float) {
            switch (vi->op) {
            case TOKEN_PLUS:  vec_op = "addps"; scl_op = "addss"; break;
            case TOKEN_MINUS: vec_op = "subps"; scl_op = "subss"; break;
            case TOKEN_STAR:  vec_op = "mulps"; scl_op = "mulss"; break;
            case TOKEN_SLASH: vec_op = "divps"; scl_op = "divss"; break;
            default: return;
            }
        } else {
            switch (vi->op) {
            case TOKEN_PLUS:  vec_op = "paddd"; scl_op = "add"; break;
            case TOKEN_MINUS: vec_op = "psubd"; scl_op = "sub"; break;
            default: return;
            }
        }
    }

    /* --- Prologue: save callee-saved registers --- */
    emit_inst1("push", op_reg("ebx"));
    emit_inst1("push", op_reg("esi"));
    emit_inst1("push", op_reg("edi"));

    /* --- Compute base addresses of dst, src1, src2 --- */
    /* dst → ebx */
    int off_dst = get_local_offset(vi->dst);
    Type *t_dst = get_local_type(vi->dst);
    if (t_dst && t_dst->kind == TYPE_ARRAY) {
        emit_inst2("lea", op_mem("ebp", off_dst), op_reg("ebx"));
    } else {
        emit_inst2("mov", op_mem("ebp", off_dst), op_reg("ebx"));
    }

    /* src1 → esi */
    int off_s1 = get_local_offset(vi->src1);
    Type *t_s1 = get_local_type(vi->src1);
    if (t_s1 && t_s1->kind == TYPE_ARRAY) {
        emit_inst2("lea", op_mem("ebp", off_s1), op_reg("esi"));
    } else {
        emit_inst2("mov", op_mem("ebp", off_s1), op_reg("esi"));
    }

    /* src2 → edi */
    int off_s2 = get_local_offset(vi->src2);
    Type *t_s2 = get_local_type(vi->src2);
    if (t_s2 && t_s2->kind == TYPE_ARRAY) {
        emit_inst2("lea", op_mem("ebp", off_s2), op_reg("edi"));
    } else {
        emit_inst2("mov", op_mem("ebp", off_s2), op_reg("edi"));
    }

    /* --- Initialize counter --- */
    emit_inst2("xor", op_reg("ecx"), op_reg("ecx"));  /* ecx = 0 */

    int vec_limit = vi->iterations - (vec_elems - 1);  /* last valid counter for vector */

    /* --- Vector loop: process vec_elems elements per iteration --- */
    emit_label_def(l_vec);
    emit_inst2("cmp", op_imm(vec_limit), op_reg("ecx"));
    emit_inst1("jg", op_label(l_scalar));

    /* Load elements from src1 and src2 */
    emit_inst2(vec_mov, op_mem("esi", 0), op_reg(vreg0));
    emit_inst2(vec_mov, op_mem("edi", 0), op_reg(vreg1));

    /* Perform packed operation */
    if (use_avx) {
        /* AVX 3-operand: vreg0 = vreg1 OP vreg0 → emit_inst3(op, src1, src2, dest) */
        emit_inst3(vec_op, op_reg(vreg0), op_reg(vreg1), op_reg(vreg0));
    } else {
        /* SSE 2-operand: xmm0 = xmm0 OP xmm1 */
        emit_inst2(vec_op, op_reg(vreg1), op_reg(vreg0));
    }

    /* Store elements to dst */
    emit_inst2(vec_mov, op_reg(vreg0), op_mem("ebx", 0));

    /* Advance pointers and counter */
    emit_inst2("add", op_imm(vec_bytes), op_reg("ebx"));
    emit_inst2("add", op_imm(vec_bytes), op_reg("esi"));
    emit_inst2("add", op_imm(vec_bytes), op_reg("edi"));
    emit_inst2("add", op_imm(vec_elems), op_reg("ecx"));
    emit_inst1("jmp", op_label(l_vec));

    /* --- Scalar remainder loop --- */
    emit_label_def(l_scalar);

    /* AVX requires vzeroupper before transitioning to SSE/scalar code */
    if (use_avx) {
        emit_inst0("vzeroupper");
    }

    emit_inst2("cmp", op_imm(vi->iterations), op_reg("ecx"));
    emit_inst1("jge", op_label(l_done));

    emit_label_def(l_scalar_loop);

    if (vi->is_float) {
        /* Float scalar: use SSE scalar instructions */
        emit_inst2(scl_mov, op_mem("esi", 0), op_reg("xmm0"));
        emit_inst2(scl_mov, op_mem("edi", 0), op_reg("xmm1"));
        emit_inst2(scl_op, op_reg("xmm1"), op_reg("xmm0"));
        emit_inst2(scl_mov, op_reg("xmm0"), op_mem("ebx", 0));
    } else {
        /* Integer scalar: use GPR instructions */
        emit_inst2("mov", op_mem("esi", 0), op_reg("eax"));
        emit_inst2(scl_op, op_mem("edi", 0), op_reg("eax"));
        emit_inst2("mov", op_reg("eax"), op_mem("ebx", 0));
    }

    /* Advance pointers by 4 bytes (1 × 4-byte element) */
    emit_inst2("add", op_imm(4), op_reg("ebx"));
    emit_inst2("add", op_imm(4), op_reg("esi"));
    emit_inst2("add", op_imm(4), op_reg("edi"));
    emit_inst2("add", op_imm(1), op_reg("ecx"));
    emit_inst2("cmp", op_imm(vi->iterations), op_reg("ecx"));
    emit_inst1("jl", op_label(l_scalar_loop));

    /* --- Epilogue: restore callee-saved registers --- */
    emit_label_def(l_done);
    emit_inst1("pop", op_reg("edi"));
    emit_inst1("pop", op_reg("esi"));
    emit_inst1("pop", op_reg("ebx"));
}

/* ------------------------------------------------------------------ */
/* Vectorized while-loop codegen (reduction + init patterns)           */
/* ------------------------------------------------------------------ */
static void gen_vectorized_while_loop(ASTNode *node) {
    VecInfo *vi = node->vec_info;
    if (!vi) return;

    if (vi->vec_mode == 1) {
        /* ============================================================
         * Reduction: sum += arr[i]
         * Strategy: accumulate in xmm0 (4 × int32), then horizontal sum.
         *   xmm0 = [0,0,0,0]  (accumulator)
         *   loop: xmm1 = load arr[i..i+3]; xmm0 = paddd xmm0, xmm1
         *   horiz: pshufd + paddd to collapse 4 lanes → scalar
         *   store back to accum variable
         * ============================================================ */
        int lbl_vec = label_count++;
        int lbl_scalar = label_count++;
        int lbl_scalar_loop = label_count++;
        int lbl_done = label_count++;
        char l_vec[32], l_scalar[32], l_scalar_loop[32], l_done[32];
        sprintf(l_vec, ".L%d", lbl_vec);
        sprintf(l_scalar, ".L%d", lbl_scalar);
        sprintf(l_scalar_loop, ".L%d", lbl_scalar_loop);
        sprintf(l_done, ".L%d", lbl_done);

        int use_avx = (vi->width == 8);
        int vec_elems = vi->width;
        int vec_bytes = vec_elems * vi->elem_size;

        const char *vreg0 = use_avx ? "ymm0" : "xmm0";
        const char *vreg1 = use_avx ? "ymm1" : "xmm1";

        /* Pick move/add mnemonics */
        const char *vec_mov, *vec_add;
        if (use_avx) {
            vec_mov = vi->is_float ? "vmovups" : "vmovdqu";
            vec_add = vi->is_float ? "vaddps" : "vpaddd";
        } else {
            vec_mov = vi->is_float ? "movups" : "movdqu";
            vec_add = vi->is_float ? "addps" : "paddd";
        }

        /* Save callee-saved registers and load array base */
        emit_inst1("push", op_reg("esi"));

        const char *src_reg = get_local_reg(vi->src1);
        if (src_reg) {
            /* Array pointer is register-allocated */
            emit_inst2("mov", op_reg(src_reg), op_reg("esi"));
        } else {
            int off_src = get_local_offset(vi->src1);
            Type *t_src = get_local_type(vi->src1);
            if (t_src && t_src->kind == TYPE_ARRAY)
                emit_inst2("lea", op_mem("ebp", off_src), op_reg("esi"));
            else
                emit_inst2("mov", op_mem("ebp", off_src), op_reg("esi"));
        }

        /* Zero the accumulator vector register */
        if (use_avx) {
            /* vpxor ymm0, ymm0, ymm0 → zero YMM */
            emit_inst3("vpxor", op_reg(vreg0), op_reg(vreg0), op_reg(vreg0));
        } else {
            emit_inst2("pxor", op_reg("xmm0"), op_reg("xmm0"));
        }

        /* Counter in ecx */
        emit_inst2("xor", op_reg("ecx"), op_reg("ecx"));
        int vec_limit = vi->iterations - (vec_elems - 1);

        /* --- Vector loop --- */
        emit_label_def(l_vec);
        emit_inst2("cmp", op_imm(vec_limit), op_reg("ecx"));
        emit_inst1("jg", op_label(l_scalar));

        /* Load chunk from array */
        emit_inst2(vec_mov, op_mem("esi", 0), op_reg(vreg1));

        /* Accumulate: xmm0 += xmm1 */
        if (use_avx)
            emit_inst3(vec_add, op_reg(vreg1), op_reg(vreg0), op_reg(vreg0));
        else
            emit_inst2(vec_add, op_reg(vreg1), op_reg(vreg0));

        /* Advance pointer and counter */
        emit_inst2("add", op_imm(vec_bytes), op_reg("esi"));
        emit_inst2("add", op_imm(vec_elems), op_reg("ecx"));
        emit_inst1("jmp", op_label(l_vec));

        /* --- Scalar remainder --- */
        emit_label_def(l_scalar);

        if (use_avx) {
            /* Reduce YMM to XMM: extract high 128 bits and add to low */
            emit_inst3("vextracti128", op_imm(1), op_reg("ymm0"), op_reg("xmm1"));
            emit_inst2("paddd", op_reg("xmm1"), op_reg("xmm0"));
            emit_inst0("vzeroupper");
        }

        /* Horizontal reduction: xmm0 = [a, b, c, d] →
           pshufd xmm1, xmm0, 0x4E → xmm1 = [c, d, a, b]
           paddd  xmm0, xmm1       → xmm0 = [a+c, b+d, ...]
           pshufd xmm1, xmm0, 0xB1 → xmm1 = [b+d, a+c, ...]
           paddd  xmm0, xmm1       → xmm0 = [a+b+c+d, ...] */
        if (vi->is_float) {
            /* Float: haddps twice or movhlps+addps */
            emit_inst2("movhlps", op_reg("xmm0"), op_reg("xmm1"));
            emit_inst2("addps", op_reg("xmm1"), op_reg("xmm0"));
            emit_inst3("pshufd", op_imm(0x55), op_reg("xmm0"), op_reg("xmm1"));
            emit_inst2("addss", op_reg("xmm1"), op_reg("xmm0"));
        } else {
            /* Integer: pshufd + paddd */
            emit_inst3("pshufd", op_imm(0x4E), op_reg("xmm0"), op_reg("xmm1"));
            emit_inst2("paddd", op_reg("xmm1"), op_reg("xmm0"));
            emit_inst3("pshufd", op_imm(0xB1), op_reg("xmm0"), op_reg("xmm1"));
            emit_inst2("paddd", op_reg("xmm1"), op_reg("xmm0"));
        }

        /* Move scalar result to eax, add existing accumulator value */
        if (vi->is_float) {
            /* movss to accumulator variable */
            const char *acc_freg = get_local_reg(vi->accum_var);
            if (acc_freg) {
                /* accum is register-allocated — but floats in GPR need special handling:
                   movd gpr→xmm1, addss xmm1,xmm0, movd xmm0→gpr */
                const char *acc_freg32 = get_local_reg32(vi->accum_var);
                emit_inst2("movd", op_reg(acc_freg32), op_reg("xmm1"));
                emit_inst2("addss", op_reg("xmm1"), op_reg("xmm0"));
                emit_inst2("movd", op_reg("xmm0"), op_reg(acc_freg32));
            } else {
                int off_acc = get_local_offset(vi->accum_var);
                emit_inst2("addss", op_mem("ebp", off_acc), op_reg("xmm0"));
                emit_inst2("movss", op_reg("xmm0"), op_mem("ebp", off_acc));
            }
        } else {
            emit_inst2("movd", op_reg("xmm0"), op_reg("eax"));
            /* Check if accumulator is register-allocated */
            const char *acc_reg32 = get_local_reg32(vi->accum_var);
            if (acc_reg32) {
                emit_inst2("addl", op_reg(acc_reg32), op_reg("eax"));
            } else {
                int off_acc = get_local_offset(vi->accum_var);
                emit_inst2("addl", op_mem("ebp", off_acc), op_reg("eax"));
            }
            /* Handle scalar remainder elements */
            emit_inst2("cmp", op_imm(vi->iterations), op_reg("ecx"));
            emit_inst1("jge", op_label(l_done));
            emit_label_def(l_scalar_loop);
            emit_inst2("addl", op_mem("esi", 0), op_reg("eax"));
            emit_inst2("add", op_imm(4), op_reg("esi"));
            emit_inst2("add", op_imm(1), op_reg("ecx"));
            emit_inst2("cmp", op_imm(vi->iterations), op_reg("ecx"));
            emit_inst1("jl", op_label(l_scalar_loop));
            emit_label_def(l_done);
            /* Store final sum back */
            if (acc_reg32) {
                emit_inst2("movl", op_reg("eax"), op_reg(acc_reg32));
            } else {
                int off_acc = get_local_offset(vi->accum_var);
                emit_inst2("movl", op_reg("eax"), op_mem("ebp", off_acc));
            }
        }

        emit_inst1("pop", op_reg("esi"));

    } else if (vi->vec_mode == 2) {
        /* ============================================================
         * Init: arr[i] = i * scale + offset
         * Strategy:
         *   If scale == 0: broadcast offset, movdqu store
         *   If scale != 0: init vector = [0*s+o, 1*s+o, 2*s+o, 3*s+o],
         *     stride vector = [4*s, 4*s, 4*s, 4*s] (or 8*s for AVX2)
         *     Each iteration: store init vector, init += stride
         * ============================================================ */
        int lbl_vec = label_count++;
        int lbl_scalar = label_count++;
        int lbl_scalar_loop = label_count++;
        int lbl_done = label_count++;
        char l_vec[32], l_scalar[32], l_scalar_loop[32], l_done[32];
        sprintf(l_vec, ".L%d", lbl_vec);
        sprintf(l_scalar, ".L%d", lbl_scalar);
        sprintf(l_scalar_loop, ".L%d", lbl_scalar_loop);
        sprintf(l_done, ".L%d", lbl_done);

        int use_avx = (vi->width == 8);
        int vec_elems = vi->width;
        int vec_bytes = vec_elems * vi->elem_size;

        /* Save callee-saved registers and load array base */
        emit_inst1("push", op_reg("ebx"));

        const char *dst_reg = get_local_reg(vi->dst);
        if (dst_reg) {
            /* Array pointer is register-allocated */
            emit_inst2("mov", op_reg(dst_reg), op_reg("ebx"));
        } else {
            int off_dst = get_local_offset(vi->dst);
            Type *t_dst = get_local_type(vi->dst);
            if (t_dst && t_dst->kind == TYPE_ARRAY)
                emit_inst2("lea", op_mem("ebp", off_dst), op_reg("ebx"));
            else
                emit_inst2("mov", op_mem("ebp", off_dst), op_reg("ebx"));
        }

        long long scale = vi->init_scale;
        long long offset = vi->init_offset;

        if (scale == 0) {
            /* Constant init: broadcast offset into all lanes */
            /* Use: mov $offset, %eax; movd %eax, %xmm0; pshufd $0, %xmm0, %xmm0 */
            emit_inst2("mov", op_imm((int)offset), op_reg("eax"));
            emit_inst2("movd", op_reg("eax"), op_reg("xmm0"));
            emit_inst3("pshufd", op_imm(0), op_reg("xmm0"), op_reg("xmm0"));
            if (use_avx) {
                /* Broadcast xmm0 to ymm0: vinserti128 $1, %xmm0, %ymm0, %ymm0 */
                emit_inst3("vinserti128", op_imm(1), op_reg("xmm0"), op_reg("ymm0"));
            }
        } else {
            /* Strided init: build initial vector [0*s+o, 1*s+o, 2*s+o, 3*s+o]
               and stride vector [vec_elems*s, vec_elems*s, ...] */
            /* We need stack space for the initial values.
               Use sub esp to allocate temp space, store values, load vector. */
            int tmp_size = vec_bytes * 2;  /* init_vec + stride_vec */
            /* Align to 16/32 bytes */
            int align = use_avx ? 32 : 16;
            tmp_size = (tmp_size + align - 1) & ~(align - 1);
            emit_inst2("sub", op_imm(tmp_size), op_reg("esp"));

            /* Store initial values for init vector */
            for (int k = 0; k < vec_elems; k++) {
                long long val = (long long)k * scale + offset;
                emit_inst2("movl", op_imm((int)val), op_mem("esp", k * 4));
            }
            /* Store stride values */
            long long stride_val = (long long)vec_elems * scale;
            for (int k = 0; k < vec_elems; k++) {
                emit_inst2("movl", op_imm((int)stride_val), op_mem("esp", vec_bytes + k * 4));
            }

            /* Load init vector and stride vector */
            const char *vec_mov = use_avx ? "vmovdqu" : "movdqu";
            const char *vreg0 = use_avx ? "ymm0" : "xmm0";
            const char *vreg1 = use_avx ? "ymm1" : "xmm1";
            emit_inst2(vec_mov, op_mem("esp", 0), op_reg(vreg0));
            emit_inst2(vec_mov, op_mem("esp", vec_bytes), op_reg(vreg1));

            /* Free temp space */
            emit_inst2("add", op_imm(tmp_size), op_reg("esp"));
        }

        /* Counter in ecx */
        emit_inst2("xor", op_reg("ecx"), op_reg("ecx"));
        int vec_limit = vi->iterations - (vec_elems - 1);

        const char *vec_mov_store = use_avx ? "vmovdqu" : "movdqu";
        const char *vreg0 = use_avx ? "ymm0" : "xmm0";

        /* --- Vector loop --- */
        emit_label_def(l_vec);
        emit_inst2("cmp", op_imm(vec_limit), op_reg("ecx"));
        emit_inst1("jg", op_label(l_scalar));

        /* Store current vector to array */
        emit_inst2(vec_mov_store, op_reg(vreg0), op_mem("ebx", 0));

        if (scale != 0) {
            /* Advance: xmm0 += stride */
            const char *vreg1 = use_avx ? "ymm1" : "xmm1";
            if (use_avx)
                emit_inst3("vpaddd", op_reg(vreg1), op_reg(vreg0), op_reg(vreg0));
            else
                emit_inst2("paddd", op_reg(vreg1), op_reg(vreg0));
        }

        /* Advance pointer and counter */
        emit_inst2("add", op_imm(vec_bytes), op_reg("ebx"));
        emit_inst2("add", op_imm(vec_elems), op_reg("ecx"));
        emit_inst1("jmp", op_label(l_vec));

        /* --- Scalar remainder --- */
        emit_label_def(l_scalar);
        if (use_avx) emit_inst0("vzeroupper");

        emit_inst2("cmp", op_imm(vi->iterations), op_reg("ecx"));
        emit_inst1("jge", op_label(l_done));
        emit_label_def(l_scalar_loop);

        if (scale == 0) {
            /* Store constant */
            emit_inst2("movl", op_imm((int)offset), op_mem("ebx", 0));
        } else {
            /* Compute value: ecx * scale + offset */
            emit_inst2("mov", op_reg("ecx"), op_reg("eax"));
            if (scale != 1)
                emit_inst2("imull", op_imm((int)scale), op_reg("eax"));
            if (offset != 0)
                emit_inst2("addl", op_imm((int)offset), op_reg("eax"));
            emit_inst2("movl", op_reg("eax"), op_mem("ebx", 0));
        }

        emit_inst2("add", op_imm(4), op_reg("ebx"));
        emit_inst2("add", op_imm(1), op_reg("ecx"));
        emit_inst2("cmp", op_imm(vi->iterations), op_reg("ecx"));
        emit_inst1("jl", op_label(l_scalar_loop));

        /* --- Epilogue --- */
        emit_label_def(l_done);
        emit_inst1("pop", op_reg("ebx"));
    }
}

static void gen_statement(ASTNode *node) {
    if (!node) return;
    debug_record_line(node);
    if (node->type == AST_RETURN) {
        if (node->data.return_stmt.expression) {
            ASTNode *ret_expr = node->data.return_stmt.expression;

            /* ---- Tail call optimization (-O2+) ----
             * Pattern: return f(args...)
             * Replace: call f + epilogue  →  leave + jmp f
             * Constraints:
             *   - All args must fit in registers (no stack args)
             *   - Return types must be register-compatible (no int<->float conveesion)
             */
            if (OPT_AT_LEAST(OPT_O2) && !OPT_DEBUG_MODE && ret_expr->type == AST_CALL) {
                int num_args = (int)ret_expr->children_count;
                int max_reg = g_max_reg_args;

                if (num_args <= max_reg) {
                    /* Check return type compatibility */
                    Type *call_ret_type = get_expr_type(ret_expr);
                    int can_tco = 1;
                    /* Struct-returning calls use hidden sret pointer — TCO is
                     * not safe because the argument registers are shifted. */
                    if (is_struct_return(call_ret_type) || is_struct_return(current_func_return_type)) {
                        can_tco = 0;
                    }
                    if (current_func_return_type && call_ret_type) {
                        /* int <-> float mismatch: different return register */
                        if (is_float_type(current_func_return_type) != is_float_type(call_ret_type)) {
                            can_tco = 0;
                        }
                        /* float <-> double mismatch: needs conveesion */
                        else if (is_float_type(current_func_return_type) && is_float_type(call_ret_type)) {
                            if (current_func_return_type->kind != call_ret_type->kind) {
                                can_tco = 0;
                            }
                        }
                    }

                    if (can_tco) {
                        const char **arg_regs = g_arg_regs;
                        const char **xmm_arg_regs = g_xmm_arg_regs;

                        /* Evaluate all args (reverse order) and push to stack */
                        for (int i = num_args - 1; i >= 0; i--) {
                            gen_expression(ret_expr->children[i]);
                            Type *arg_type = get_expr_type(ret_expr->children[i]);
                            if (is_float_type(arg_type)) {
                                emit_push_xmm("xmm0");
                            } else {
                                emit_inst1("push", op_reg("eax"));
                            }
                        }

                        /* Pop args into correct argument registers */
                        for (int i = 0; i < num_args; i++) {
                            Type *arg_type = get_expr_type(ret_expr->children[i]);
                            if (is_float_type(arg_type)) {
                                emit_pop_xmm(xmm_arg_regs[i]);
                            } else {
                                emit_inst1("pop", op_reg(arg_regs[i]));
                            }
                        }

                        /* System V ABI: set al = number of XMM args (for variadics) */
                        if (!g_use_shadow_space) {
                            int xmm_count = 0;
                            for (int i = 0; i < num_args && i < max_reg; i++) {
                                Type *at = get_expr_type(ret_expr->children[i]);
                                if (is_float_type(at)) xmm_count++;
                            }
                            emit_inst2("mov", op_imm(xmm_count), op_reg("eax"));
                        }

                        /* Tear down current stack frame and jump to target */
                        regalloc_restore_registers();
                        emit_inst0("leave");
                        emit_inst1("jmp", op_label(ret_expr->data.call.name));
                        return; /* done — skip normal return path */
                    }
                }
            }
            /* ---- End tail call optimization ---- */

            /* ---- Struct return: copy to hidden sret pointer ---- */
            if (is_struct_return(current_func_return_type) && sret_offset != 0) {
                /* Get address of the struct being returned */
                if (ret_expr->type == AST_IDENTIFIER) {
                    gen_addr(ret_expr);
                } else {
                    /* For function calls returning structs, gen_expression
                     * already returns a pointer in %eax */
                    gen_expression(ret_expr);
                }
                /* eax = source address of struct data */
                /* memcpy(sret_ptr, eax, size) */
                emit_inst2("mov", op_reg("eax"), op_reg("esi")); /* source = eax */
                emit_inst2("mov", op_mem("ebp", sret_offset), op_reg("edi")); /* dest = hidden ptr */
                emit_inst2("mov", op_imm(current_func_return_type->size), op_reg("edx"));
                emit_inst2("xor", op_reg("eax"), op_reg("eax"));
                emit_inst0("call memcpy");
                /* return the hidden pointer in %eax */
                emit_inst2("mov", op_mem("ebp", sret_offset), op_reg("eax"));
            } else {
            gen_expression(ret_expr);
            Type *expr_type = get_expr_type(ret_expr);
            
            // Convert to function return type if needed
            if (current_func_return_type && expr_type) {
                if (is_float_type(current_func_return_type) && !is_float_type(expr_type)) {
                    // int -> float/double
                    if (current_func_return_type->kind == TYPE_FLOAT) emit_inst2("cvtsi2ss", op_reg("eax"), op_reg("xmm0"));
                    else emit_inst2("cvtsi2sd", op_reg("eax"), op_reg("xmm0"));
                } else if (!is_float_type(current_func_return_type) && is_float_type(expr_type)) {
                    // float/double -> int
                    if (expr_type->kind == TYPE_FLOAT) emit_inst2("cvttss2si", op_reg("xmm0"), op_reg("eax"));
                    else emit_inst2("cvttsd2si", op_reg("xmm0"), op_reg("eax"));
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
             locals[locals_count].reg = NULL;
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
                ASTNode *vinit1 = node->data.var_decl.initializer;
                if (vinit1 && vinit1->type == AST_INIT_LIST) {
                    int elem_size = 1;
                    if (node->resolved_type && node->resolved_type->kind == TYPE_ARRAY && node->resolved_type->data.ptr_to) {
                        elem_size = node->resolved_type->data.ptr_to->size;
                    }
                    size_t vi; int tw = 0;
                    for (vi = 0; vi < vinit1->children_count; vi++) {
                        ASTNode *elem = vinit1->children[vi];
                        if (elem && elem->type == AST_INTEGER) {
                            if (elem_size == 1) { uint8_t b = (uint8_t)elem->data.integer.value; buffer_write_byte(&obj_writer->data_section, b); }
                            else if (elem_size == 2) { uint16_t w = (uint16_t)elem->data.integer.value; buffer_write_word(&obj_writer->data_section, w); }
                            else if (elem_size == 4) { int32_t d = (int32_t)elem->data.integer.value; buffer_write_dword(&obj_writer->data_section, (uint32_t)d); }
                            else { int64_t q = (int64_t)elem->data.integer.value; buffer_write_qword(&obj_writer->data_section, (uint64_t)q); }
                        } else { int zi; for (zi = 0; zi < elem_size; zi++) buffer_write_byte(&obj_writer->data_section, 0); }
                        tw += elem_size;
                    }
                    while (tw < size) { buffer_write_byte(&obj_writer->data_section, 0); tw++; }
                } else {
                    long long val = 0;
                    if (vinit1 && vinit1->type == AST_INTEGER) {
                        val = vinit1->data.integer.value;
                    }
                    buffer_write_bytes(&obj_writer->data_section, &val, size);
                }
            } else {
                if (current_syntax == SYNTAX_INTEL) {
                    fprintf(out, "_TEXT ENDS\n_DATA SEGMENT\n");
                    emit_label_def(slabel);
                    int size = node->resolved_type ? node->resolved_type->size : 8;
                    ASTNode *vinit2 = node->data.var_decl.initializer;
                    if (vinit2 && vinit2->type == AST_INIT_LIST) {
                        int elem_size = 1;
                        if (node->resolved_type && node->resolved_type->kind == TYPE_ARRAY && node->resolved_type->data.ptr_to)
                            elem_size = node->resolved_type->data.ptr_to->size;
                        const char *edir = "DB";
                        if (elem_size == 4) edir = "DD"; else if (elem_size >= 8) edir = "DQ";
                        size_t vi; int tw = 0;
                        for (vi = 0; vi < vinit2->children_count; vi++) {
                            ASTNode *elem = vinit2->children[vi];
                            if (elem && elem->type == AST_INTEGER) fprintf(out, "%s %I64d\n", edir, elem->data.integer.value);
                            else fprintf(out, "%s 0\n", edir);
                            tw += elem_size;
                        }
                        if (tw < size) fprintf(out, "DB %d DUP(0)\n", size - tw);
                    } else {
                        long long val = 0;
                        if (vinit2 && vinit2->type == AST_INTEGER) val = vinit2->data.integer.value;
                        if (size == 1) fprintf(out, "DB %I64d\n", val);
                        else if (size == 4) fprintf(out, "DD %I64d\n", val);
                        else fprintf(out, "DQ %I64d\n", val);
                    }
                    fprintf(out, "_DATA ENDS\n_TEXT SEGMENT\n");
                } else {
                    fprintf(out, ".data\n");
                    emit_label_def(slabel);
                    int size = node->resolved_type ? node->resolved_type->size : 8;
                    ASTNode *vinit3 = node->data.var_decl.initializer;
                    if (vinit3 && vinit3->type == AST_INIT_LIST) {
                        int elem_size = 1;
                        if (node->resolved_type && node->resolved_type->kind == TYPE_ARRAY && node->resolved_type->data.ptr_to)
                            elem_size = node->resolved_type->data.ptr_to->size;
                        size_t vi; int tw = 0;
                        for (vi = 0; vi < vinit3->children_count; vi++) {
                            ASTNode *elem = vinit3->children[vi];
                            if (elem && elem->type == AST_INTEGER) {
                                if (elem_size == 1) fprintf(out, "    .byte %lld\n", elem->data.integer.value);
                                else if (elem_size == 2) fprintf(out, "    .word %lld\n", elem->data.integer.value);
                                else if (elem_size == 4) fprintf(out, "    .long %lld\n", elem->data.integer.value);
                                else if (elem_size == 8) fprintf(out, "    .quad %lld\n", elem->data.integer.value);
                                else fprintf(out, "    .long %lld\n", elem->data.integer.value);
                            } else {
                                int zi; for (zi = 0; zi < elem_size; zi++) fprintf(out, "    .byte 0\n");
                            }
                            tw += elem_size;
                        }
                        if (tw < size) fprintf(out, "    .zero %d\n", size - tw);
                    } else {
                        long long val = 0;
                        if (vinit3 && vinit3->type == AST_INTEGER) val = vinit3->data.integer.value;
                        if (size == 1) fprintf(out, "    .byte %I64d\n", val);
                        else if (size == 2) fprintf(out, "    .word %I64d\n", val);
                        else if (size == 4) fprintf(out, "    .long %I64d\n", val);
                        else if (size == 8) fprintf(out, "    .quad %I64d\n", val);
                        else fprintf(out, "    .long %I64d\n", val);

                    }
                    fprintf(out, ".text\n");
                }
            }
            current_section = old_section;
            
            locals[locals_count].name = node->data.var_decl.name;
            locals[locals_count].label = strdup(slabel);
            locals[locals_count].offset = 0;
            locals[locals_count].type = node->resolved_type;
            locals[locals_count].reg = NULL;
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
            locals[locals_count].reg = NULL;
            locals_count++;
            debug_record_var(node->data.var_decl.name, stack_offset, 0, node->resolved_type);
            
            // Allocate space on stack
            emit_inst2("sub", op_imm(alloc_size), op_reg("esp"));
            
            // Zero-initialize with qword stores
            {
                int off;
                for (off = 0; off + 8 <= alloc_size; off += 8) {
                    emit_inst2("movl", op_imm(0), op_mem("ebp", stack_offset + off));
emit_inst2("movl", op_imm(0), op_mem("ebp", stack_offset + off + 4));
                }
                if (off + 4 <= alloc_size) {
                    emit_inst2("movl", op_imm(0), op_mem("ebp", stack_offset + off));
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
                            emit_inst2("movb", op_reg("al"), op_mem("ebp", stack_offset + mem_offset));
                        } else if (mem_size == 4) {
                            emit_inst2("movl", op_reg("eax"), op_mem("ebp", stack_offset + mem_offset));
                        } else {
                            emit_inst2("mov", op_reg("eax"), op_mem("ebp", stack_offset + mem_offset));
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
                        emit_inst2("movb", op_reg("al"), op_mem("ebp", el_offset));
                    } else if (elem_size == 4) {
                        emit_inst2("movl", op_reg("eax"), op_mem("ebp", el_offset));
                    } else {
                        emit_inst2("mov", op_reg("eax"), op_mem("ebp", el_offset));
                    }
                }
            }
        } else {
            // Scalar initializer (original path)
            if (node->data.var_decl.initializer) {
                gen_expression(node->data.var_decl.initializer);
            } else {
                if (is_float_type(node->resolved_type)) {
                    emit_inst2("xor", op_reg("eax"), op_reg("eax"));
                    if (node->resolved_type->kind == TYPE_FLOAT) emit_inst2("cvtsi2ss", op_reg("eax"), op_reg("xmm0"));
                    else emit_inst2("cvtsi2sd", op_reg("eax"), op_reg("xmm0"));
                    last_value_clear();
                } else {
                    if (OPT_AT_LEAST(OPT_O1)) {
                        emit_inst2("xor", op_reg("eax"), op_reg("eax"));
                    } else {
                        emit_inst2("mov", op_imm(0), op_reg("eax"));
                    }
                }
            }
            
            /* Check if this variable should be register-allocated */
            int ra_idx = regalloc_find_assignment(node->data.var_decl.name);
            if (ra_idx >= 0 && !is_float_type(node->resolved_type)) {
                /* Register-allocated variable: store value in assigned register */
                if (locals_count >= 8192) { fprintf(stderr, "Error: Too many locals\n"); exit(1); }
                locals[locals_count].name = node->data.var_decl.name;
                locals[locals_count].offset = 0;
                locals[locals_count].label = NULL;
                locals[locals_count].type = node->resolved_type;
                locals[locals_count].reg = regalloc_assignments[ra_idx].reg64;
                locals_count++;
                /* Move value from %eax to the assigned register */
                emit_inst2("mov", op_reg("eax"), op_reg(regalloc_assignments[ra_idx].reg64));
                last_value_clear();
            } else {
                /* Stack-allocated variable (original path) */
                stack_offset -= alloc_size;
            
                if (locals_count >= 8192) { fprintf(stderr, "Error: Too many locals\n"); exit(1); }
                locals[locals_count].name = node->data.var_decl.name;
                locals[locals_count].offset = stack_offset;
                locals[locals_count].label = NULL;
                locals[locals_count].type = node->resolved_type;
                locals[locals_count].reg = NULL;
                locals_count++;
                debug_record_var(node->data.var_decl.name, stack_offset, 0, node->resolved_type);
            
                if (is_float_type(node->resolved_type)) {
                    // Don't use emit_push_xmm here - stack_offset already adjusted above
                    emit_inst2("sub", op_imm(alloc_size), op_reg("esp"));
                    emit_inst2("movsd", op_reg("xmm0"), op_mem("esp", 0));
                    last_value_clear();
                } else {
                    emit_inst2("sub", op_imm(alloc_size), op_reg("esp"));
                    if (node->resolved_type && node->resolved_type->kind != TYPE_STRUCT && node->resolved_type->kind != TYPE_ARRAY) {
                        // Scalar store
                        if (size == 1) emit_inst2("movb", op_reg("al"), op_mem("esp", 0));
                        else if (size == 2) emit_inst2("movw", op_reg("ax"), op_mem("esp", 0));
                        else if (size == 4) emit_inst2("movl", op_reg("eax"), op_mem("esp", 0));
                        else emit_inst2("mov", op_reg("eax"), op_mem("esp", 0));
                        /* Cache: eax holds the value just stored at stack_offset */
                        last_value_set_stack(stack_offset, node->resolved_type);
                    } else if (node->resolved_type && (node->resolved_type->kind == TYPE_STRUCT || node->resolved_type->kind == TYPE_ARRAY) && node->data.var_decl.initializer && node->data.var_decl.initializer->type != AST_INIT_LIST) {
                        /* Struct/array copy via memcpy: eax has source address from gen_expression */
                        emit_inst2("mov", op_reg("esp"), op_reg("edi"));
                        emit_inst2("mov", op_reg("eax"), op_reg("esi"));
                        emit_inst2("mov", op_imm(alloc_size), op_reg("edx"));
                        emit_inst2("xor", op_reg("eax"), op_reg("eax"));
                        emit_inst0("call memcpy");
                        last_value_clear();
                    } else {
                        /* Array/struct without initializer or non-scalar:
                           eax was clobbered by zero-init, invalidate cache */
                        last_value_clear();
                    }
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
        emit_inst2("test", op_reg("eax"), op_reg("eax"));
        emit_inst1("je", op_label(l_else));

        /* PGO: increment branch-taken counter */
        int pgo_branch_id_local = -1;
        if (g_compiler_options.pgo_generate && current_func_name) {
            pgo_branch_id_local = pgo_func_branch_id++;
            char probe_name[PGO_NAME_LEN];
            snprintf(probe_name, PGO_NAME_LEN, "%s:B%dT", current_func_name, pgo_branch_id_local);
            int pid = pgo_alloc_probe(probe_name);
            if (pid >= 0) {
                char cl[80];
                sprintf(cl, "__pgo_cnt_%d", pid);
                emit_inst1("incq", op_label(cl));
            }
        }
        
        // Save stack state before then-branch
        int saved_stack_offset = stack_offset;
        int saved_locals_count = locals_count;
        
        // Generate then branch
        if (node->data.if_stmt.then_branch) {
            gen_statement(node->data.if_stmt.then_branch);
        }
        
        // Restore RSP to pre-branch value (variables go out of scope)
        if (stack_offset != saved_stack_offset) {
            emit_inst2("lea", op_mem("ebp", saved_stack_offset), op_reg("esp"));
        }
        stack_offset = saved_stack_offset;
        locals_count = saved_locals_count;
        
        emit_inst1("jmp", op_label(l_end));
        
        emit_label_def(l_else);

        /* PGO: increment branch-not-taken counter */
        if (g_compiler_options.pgo_generate && current_func_name && pgo_branch_id_local >= 0) {
            char probe_name[PGO_NAME_LEN];
            snprintf(probe_name, PGO_NAME_LEN, "%s:B%dN", current_func_name, pgo_branch_id_local);
            int pid = pgo_alloc_probe(probe_name);
            if (pid >= 0) {
                char cl[80];
                sprintf(cl, "__pgo_cnt_%d", pid);
                emit_inst1("incq", op_label(cl));
            }
        }

        if (node->data.if_stmt.else_branch) {
            gen_statement(node->data.if_stmt.else_branch);
            // Restore RSP after else-branch too
            if (stack_offset != saved_stack_offset) {
                emit_inst2("lea", op_mem("ebp", saved_stack_offset), op_reg("esp"));
            }
            stack_offset = saved_stack_offset;
            locals_count = saved_locals_count;
        }
        emit_label_def(l_end);
        last_value_clear();
    } else if (node->type == AST_WHILE) {
        /* Vectorized while loop: emit SIMD packed instructions */
        if (node->vec_info) {
            gen_vectorized_while_loop(node);
            last_value_clear();
            return;
        }
        int label_start = label_count++;
        int label_end = label_count++;
        char l_start[32], l_end[32];
        sprintf(l_start, ".L%d", label_start);
        sprintf(l_end, ".L%d", label_end);

        if (OPT_AT_LEAST(OPT_O2)) {
            /* Loop rotation: while(cond){body} → if(cond) do{body}while(cond)
               Eliminates one unconditional jmp per iteration.
               The backward branch (jne .Lstart) is predicted taken by the CPU. */
            int label_continue = label_count++;
            char l_cont[32];
            sprintf(l_cont, ".L%d", label_continue);

            /* Guard: skip loop entirely if condition is false */
            gen_expression(node->data.while_stmt.condition);
            emit_inst2("test", op_reg("eax"), op_reg("eax"));
            emit_inst1("je", op_label(l_end));

            /* Loop body */
            emit_label_def(l_start);

            int saved_stack_offset = stack_offset;
            int saved_locals_count = locals_count;

            int lsp_idx = loop_saved_stack_ptr;
            loop_saved_stack_offset[lsp_idx] = saved_stack_offset;
            loop_saved_locals_count[lsp_idx] = saved_locals_count;
            loop_saved_stack_ptr = lsp_idx + 1;
            int blp_idx = break_label_ptr;
            break_label_stack[blp_idx] = label_end;
            break_label_ptr = blp_idx + 1;
            int clp_idx = continue_label_ptr;
            continue_label_stack[clp_idx] = label_continue;
            continue_label_ptr = clp_idx + 1;
            gen_statement(node->data.while_stmt.body);
            break_label_ptr--;
            continue_label_ptr--;
            loop_saved_stack_ptr--;

            if (saved_stack_offset != stack_offset) {
                emit_inst2("lea", op_mem("ebp", saved_stack_offset), op_reg("esp"));
            }
            stack_offset = saved_stack_offset;
            locals_count = saved_locals_count;

            /* Continue label + backward condition check */
            emit_label_def(l_cont);
            gen_expression(node->data.while_stmt.condition);
            emit_inst2("test", op_reg("eax"), op_reg("eax"));
            emit_inst1("jne", op_label(l_start));
        } else {
            /* Original while pattern: condition at top, jmp back at bottom */
            emit_label_def(l_start);
            gen_expression(node->data.while_stmt.condition);
            emit_inst2("test", op_reg("eax"), op_reg("eax"));
            emit_inst1("je", op_label(l_end));

            int saved_stack_offset = stack_offset;
            int saved_locals_count = locals_count;

            int lsp_idx = loop_saved_stack_ptr;
            loop_saved_stack_offset[lsp_idx] = saved_stack_offset;
            loop_saved_locals_count[lsp_idx] = saved_locals_count;
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

            if (saved_stack_offset != stack_offset) {
                emit_inst2("lea", op_mem("ebp", saved_stack_offset), op_reg("esp"));
            }
            stack_offset = saved_stack_offset;
            locals_count = saved_locals_count;

            emit_inst1("jmp", op_label(l_start));
        }
        
        emit_label_def(l_end);
        last_value_clear();
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
        
        { int i0 = loop_saved_stack_ptr; loop_saved_stack_offset[i0] = saved_stack_offset_dw; loop_saved_locals_count[i0] = saved_locals_count_dw; loop_saved_stack_ptr = i0 + 1; }
        { int i1 = break_label_ptr; break_label_stack[i1] = label_end; break_label_ptr = i1 + 1; }
        { int i2 = continue_label_ptr; continue_label_stack[i2] = label_continue; continue_label_ptr = i2 + 1; }
        gen_statement(node->data.while_stmt.body);
        continue_label_ptr--;
        break_label_ptr--;
        loop_saved_stack_ptr--;

        // Restore RSP to loop entry value
        if (saved_stack_offset_dw != stack_offset) {
            emit_inst2("lea", op_mem("ebp", saved_stack_offset_dw), op_reg("esp"));
        }
        stack_offset = saved_stack_offset_dw;
        locals_count = saved_locals_count_dw;

        emit_label_def(l_cont);
        gen_expression(node->data.while_stmt.condition);
        emit_inst2("test", op_reg("eax"), op_reg("eax"));
        emit_inst1("jne", op_label(l_start));
        
        emit_label_def(l_end);
        last_value_clear();
    } else if (node->type == AST_FOR) {
        /* Vectorized loop: emit SSE packed instructions */
        if (node->vec_info) {
            gen_vectorized_loop(node);
            last_value_clear();
            return;
        }
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

        if (OPT_AT_LEAST(OPT_O2) && node->data.for_stmt.condition) {
            /* Loop rotation: for(init;cond;incr){body}
               → init; if(cond) do { body; incr; } while(cond);
               Eliminates one unconditional jmp per iteration. */

            /* Guard: skip loop if condition is initially false */
            gen_expression(node->data.for_stmt.condition);
            emit_inst2("test", op_reg("eax"), op_reg("eax"));
            emit_inst1("je", op_label(l_end));

            /* Loop body */
            emit_label_def(l_start);

            int saved_stack_offset_for = stack_offset;
            int saved_locals_count_for = locals_count;

            { int i0 = loop_saved_stack_ptr; loop_saved_stack_offset[i0] = saved_stack_offset_for; loop_saved_locals_count[i0] = saved_locals_count_for; loop_saved_stack_ptr = i0 + 1; }
            { int i1 = break_label_ptr; break_label_stack[i1] = label_end; break_label_ptr = i1 + 1; }
            { int i2 = continue_label_ptr; continue_label_stack[i2] = label_continue; continue_label_ptr = i2 + 1; }
            gen_statement(node->data.for_stmt.body);
            continue_label_ptr--;
            break_label_ptr--;
            loop_saved_stack_ptr--;

            if (saved_stack_offset_for != stack_offset) {
                emit_inst2("lea", op_mem("ebp", saved_stack_offset_for), op_reg("esp"));
            }
            stack_offset = saved_stack_offset_for;
            locals_count = saved_locals_count_for;

            /* Continue label = increment + backward condition check */
            emit_label_def(l_cont);
            if (node->data.for_stmt.increment) {
                gen_expression(node->data.for_stmt.increment);
            }
            gen_expression(node->data.for_stmt.condition);
            emit_inst2("test", op_reg("eax"), op_reg("eax"));
            emit_inst1("jne", op_label(l_start));
        } else {
            /* Original for pattern: condition at top, jmp back */
            emit_label_def(l_start);
            if (node->data.for_stmt.condition) {
                gen_expression(node->data.for_stmt.condition);
                emit_inst2("test", op_reg("eax"), op_reg("eax"));
                emit_inst1("je", op_label(l_end));
            }

            int saved_stack_offset_for = stack_offset;
            int saved_locals_count_for = locals_count;

            { int i0 = loop_saved_stack_ptr; loop_saved_stack_offset[i0] = saved_stack_offset_for; loop_saved_locals_count[i0] = saved_locals_count_for; loop_saved_stack_ptr = i0 + 1; }
            { int i1 = break_label_ptr; break_label_stack[i1] = label_end; break_label_ptr = i1 + 1; }
            { int i2 = continue_label_ptr; continue_label_stack[i2] = label_continue; continue_label_ptr = i2 + 1; }
            gen_statement(node->data.for_stmt.body);
            continue_label_ptr--;
            break_label_ptr--;
            loop_saved_stack_ptr--;

            if (saved_stack_offset_for != stack_offset) {
                emit_inst2("lea", op_mem("ebp", saved_stack_offset_for), op_reg("esp"));
            }
            stack_offset = saved_stack_offset_for;
            locals_count = saved_locals_count_for;

            emit_label_def(l_cont);
            if (node->data.for_stmt.increment) {
                gen_expression(node->data.for_stmt.increment);
            }
            emit_inst1("jmp", op_label(l_start));
        }
        
        emit_label_def(l_end);
        last_value_clear();
    } else if (node->type == AST_BREAK) {
        if (break_label_ptr > 0) {
            // Restore RSP to loop entry value before breaking
            if (loop_saved_stack_ptr > 0) {
                int saved = loop_saved_stack_offset[loop_saved_stack_ptr - 1];
                if (saved != stack_offset) {
                    emit_inst2("lea", op_mem("ebp", saved), op_reg("esp"));
                }
                // Reset compiler state so the next case starts with correct offsets
                stack_offset = saved;
                locals_count = loop_saved_locals_count[loop_saved_stack_ptr - 1];
            }
            char l_break[32];
            sprintf(l_break, ".L%d", break_label_stack[break_label_ptr - 1]);
            emit_inst1("jmp", op_label(l_break));
            last_value_clear();
        } else {
            fprintf(stderr, "Error: 'break' outside of loop or switch\n");
        }
    } else if (node->type == AST_CONTINUE) {
        if (continue_label_ptr > 0) {
            // Restore RSP to loop entry value before continuing
            if (loop_saved_stack_ptr > 0) {
                int saved = loop_saved_stack_offset[loop_saved_stack_ptr - 1];
                if (saved != stack_offset) {
                    emit_inst2("lea", op_mem("ebp", saved), op_reg("esp"));
                }
                // Reset compiler state so subsequent code starts with correct offsets
                stack_offset = saved;
                locals_count = loop_saved_locals_count[loop_saved_stack_ptr - 1];
            }
            char l_cont[32];
            sprintf(l_cont, ".L%d", continue_label_stack[continue_label_ptr - 1]);
            emit_inst1("jmp", op_label(l_cont));
            last_value_clear();
        } else {
            fprintf(stderr, "Error: 'continue' outside of loop\n");
        }
    } else if (node->type == AST_GOTO) {
        emit_inst1("jmp", op_label(node->data.goto_stmt.label));
        last_value_clear();
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

        // Use flat array + manual indexing to avoid 2D array (self-compilation limitation)
        char *case_labels[1024];
        for (int i = 0; i < case_count; i++) {
            char *lbl = malloc(32);
            sprintf(lbl, ".L%d", label_count++);
            case_labels[i] = lbl;
            emit_inst2("cmp", op_imm(cases[i]->data.case_stmt.value), op_reg("eax"));
            emit_inst1("je", op_label(case_labels[i]));
            cases[i]->resolved_type = (Type *)strdup(case_labels[i]);
        }

        if (default_node) {
            char default_label[32];
            sprintf(default_label, ".L%d", label_count++);
            default_node->resolved_type = (Type *)strdup(default_label);
            emit_inst1("jmp", op_label(default_label));
        } else {
            emit_inst1("jmp", op_label(l_end));
        }

        { int i0 = break_label_ptr; break_label_stack[i0] = label_end; break_label_ptr = i0 + 1; }
        { int i1 = loop_saved_stack_ptr; loop_saved_stack_offset[i1] = stack_offset; loop_saved_locals_count[i1] = locals_count; loop_saved_stack_ptr = i1 + 1; }
        gen_statement(node->data.switch_stmt.body);
        /* Restore stack_offset and locals_count to pre-switch state BEFORE popping */
        {
            int si2 = loop_saved_stack_ptr - 1;
            if (stack_offset != loop_saved_stack_offset[si2]) {
                emit_inst2("lea", op_mem("ebp", loop_saved_stack_offset[si2]), op_reg("esp"));
            }
            stack_offset = loop_saved_stack_offset[si2];
            locals_count = loop_saved_locals_count[si2];
        }
        break_label_ptr--;
        loop_saved_stack_ptr--;

        emit_label_def(l_end);
        last_value_clear();
    } else if (node->type == AST_CASE) {
        if (node->resolved_type) {
            /* Restore stack_offset to the switch entry value.
             * At the case label, RSP is at the switch-entry RSP because
             * we jumped here from the dispatch. Sync stack_offset. */
            if (loop_saved_stack_ptr > 0) {
                int si = loop_saved_stack_ptr - 1;
                stack_offset = loop_saved_stack_offset[si];
                locals_count = loop_saved_locals_count[si];
            }
            emit_label_def((char *)node->resolved_type);
        }
    } else if (node->type == AST_DEFAULT) {
        if (node->resolved_type) {
            /* Same as AST_CASE: restore stack_offset at label entry */
            if (loop_saved_stack_ptr > 0) {
                int si = loop_saved_stack_ptr - 1;
                stack_offset = loop_saved_stack_offset[si];
                locals_count = loop_saved_locals_count[si];
            }
            emit_label_def((char *)node->resolved_type);
        }
    } else if (node->type == AST_BLOCK) {
        for (size_t i = 0; i < node->children_count; i++) {
            gen_statement(node->children[i]);
        }
    } else if (node->type == AST_ASSERT) {
        /* Runtime assert: evaluate condition, ud2 if false */
        if (node->data.assert_stmt.condition) {
            gen_expression(node->data.assert_stmt.condition);
            /* Result is in eax — test it */
            emit_inst2("test", op_reg("eax"), op_reg("eax"));
            int lbl_ok = label_count++;
            char l_ok[32];
            sprintf(l_ok, ".L%d", lbl_ok);
            emit_inst1("jne", op_label(l_ok));
            /* Assertion failed: emit ud2 (undefined instruction → crash) */
            emit_inst0("ud2");
            emit_label_def(l_ok);
        }
        last_value_clear();
    } else {
        gen_expression(node);
    }
}

static void gen_function(ASTNode *node) {
    if (node->data.function.body == NULL) {
        if (obj_writer) {
            coff_writer_add_symbol(obj_writer, node->data.function.name, 0, 0, 0x20, IMAGE_SYM_CLASS_EXTERNAL);
        } else if (current_syntax == SYNTAX_INTEL) {
            fprintf(out, ".extern %s\n", node->data.function.name);
        }
        return;
    }
    debug_last_line = 0;  /* reset for each function */
    debug_record_line(node);

    /* Record function start for debug info */
    if (obj_writer && g_compiler_options.debug_info) {
        coff_writer_begin_debug_func(obj_writer, node->data.function.name,
                                      (uint32_t)obj_writer->text_section.size,
                                      debug_type_kind(node->resolved_type),
                                      node->resolved_type ? (int32_t)node->resolved_type->size : 0);
    }

    current_function_end_label = label_count++;

    // Reset peephole state for new function
    peep_unreachable = 0;
    peep_pending_jmp = 0;
    peep_pending_push = 0;
    peep_pending_jcc = 0;
    peep_jcc_jmp_pair = 0;
    peep_setcc_state = 0;
    if (current_syntax == SYNTAX_ATT) {
        if (out && !node->data.function.is_static) fprintf(out, ".globl %s\n", node->data.function.name);
        emit_label_def_ex(node->data.function.name, node->data.function.is_static);
    } else {
        if (out && !node->data.function.is_static) {
            fprintf(out, "PUBLIC %s\n", node->data.function.name);
            fprintf(out, "%s PROC\n", node->data.function.name);
        } else if (out) {
            fprintf(out, "%s PROC\n", node->data.function.name);
        }
        emit_label_def_ex(node->data.function.name, node->data.function.is_static);
    }
    
    // Prologue
    emit_inst1("push", op_reg("ebp"));
    emit_inst2("mov", op_reg("esp"), op_reg("ebp"));

    /* PGO instrumentation: increment function entry counter */
    if (g_compiler_options.pgo_generate && node->data.function.name) {
        int probe_id = pgo_alloc_probe(node->data.function.name);
        if (probe_id >= 0) {
            char counter_label[80];
            sprintf(counter_label, "__pgo_cnt_%d", probe_id);
            emit_inst1("incq", op_label(counter_label));
        }
        pgo_func_branch_id = 0; /* reset per-function branch counter */
    }
    
    locals_count = 0;
    current_func_return_type = node->resolved_type;
    current_func_name = node->data.function.name;
    stack_offset = 0;
    sret_offset = 0;
    last_value_clear();
    
    /* If this function returns a struct, %edi holds the hidden return pointer.
     * Save it to a local slot and shift all parameter registers by 1. */
    int sret_reg_shift = 0;
    if (is_struct_return(current_func_return_type)) {
        stack_offset -= 8;
        sret_offset = stack_offset;
        emit_inst2("sub", op_imm(8), op_reg("esp"));
        emit_inst2("mov", op_reg("edi"), op_mem("esp", 0));
        sret_reg_shift = 1;  /* param 0 comes from arg_regs[1], etc. */
    }
    
    /* Register allocator Phase 1: analyze AST and determine register assignments.
     * Must run before parameter handling so params can be placed in registers. */
    regalloc_analyze(node);
    
    /* Register allocator Phase 2: save callee-saved registers we'll use.
     * These push instructions go right after the prologue. */
    regalloc_emit_saves();
    
    // Handle parameters (platform ABI)
    const char **arg_regs = g_arg_regs;
    const char **xmm_arg_regs = g_xmm_arg_regs;
    int max_reg = g_max_reg_args;
    for (size_t i = 0; i < node->children_count; i++) {
        ASTNode *param = node->children[i];
        if (param->type == AST_VAR_DECL) {
            int size = param->resolved_type ? param->resolved_type->size : 8;
            int alloc_size = size;
            int slot_size = (g_target == TARGET_DOS) ? 4 : 8;
            if (alloc_size < slot_size && param->resolved_type && param->resolved_type->kind != TYPE_STRUCT && param->resolved_type->kind != TYPE_ARRAY) {
                alloc_size = slot_size;
            }
            
            if (locals_count >= 8192) { fprintf(stderr, "Error: Too many locals\n"); exit(1); }
            locals[locals_count].name = param->data.var_decl.name;
            locals[locals_count].label = NULL;
            locals[locals_count].type = param->resolved_type;
            
            int reg_idx = (int)i + sret_reg_shift;
            if (reg_idx < max_reg) {
                /* Check if this parameter is register-allocated */
                int ra_idx = regalloc_find_assignment(param->data.var_decl.name);
                if (ra_idx >= 0 && !is_float_type(param->resolved_type)) {
                    /* Parameter goes directly to callee-saved register */
                    locals[locals_count].offset = 0;
                    locals[locals_count].reg = regalloc_assignments[ra_idx].reg64;
                    locals_count++;
                    emit_inst2("mov", op_reg(arg_regs[reg_idx]),
                               op_reg(regalloc_assignments[ra_idx].reg64));
                } else {
                    /* Original path: spill to stack */
                    locals[locals_count].reg = NULL;
                    stack_offset -= alloc_size;
                    locals[locals_count].offset = stack_offset;
                    locals_count++;
                    if (is_float_type(param->resolved_type)) {
                        emit_push_xmm(xmm_arg_regs[i]);
                    } else {
                        emit_inst2("sub", op_imm(alloc_size), op_reg("esp"));
                        if (size == 1) emit_inst2("movb", op_reg(get_reg_8(arg_regs[reg_idx])), op_mem("esp", 0));
                        else if (size == 2) emit_inst2("movw", op_reg(get_reg_16(arg_regs[reg_idx])), op_mem("esp", 0));
                        else if (size == 4) emit_inst2("movl", op_reg(get_reg_32(arg_regs[reg_idx])), op_mem("esp", 0));
                        else emit_inst2("mov", op_reg(arg_regs[reg_idx]), op_mem("esp", 0));
                    }
                }
            } else {
                // Stack params: already on caller's stack at positive ebp offset
                // Win64: [ebp+16] = shadow[0], [ebp+48] = param5, ...
                // SysV:  [ebp+16] = param7, [ebp+24] = param8, ...
                locals[locals_count].reg = NULL;
                int param_offset;
                if (g_use_shadow_space) {
                    param_offset = 48 + ((int)i - max_reg) * 8; // Win64
                } else if (g_target == TARGET_DOS) {
                    param_offset = 8 + ((int)i - max_reg) * 4; // 32-bit cdecl
                } else {
                    param_offset = 16 + ((int)i - max_reg) * 8; // SysV
                }
                locals[locals_count].offset = param_offset;
                locals_count++;
            }
        }
    }

    /* Record parameters as debug variables */
    if (obj_writer && g_compiler_options.debug_info) {
        for (int pi = 0; pi < locals_count; pi++) {
            debug_record_var(locals[pi].name, locals[pi].offset, 1, locals[pi].type);
        }
    }
    
    gen_statement(node->data.function.body);
    // Epilogue label
    char label_buffer[32];
    sprintf(label_buffer, ".Lend_%d", current_function_end_label);
    emit_label_def(label_buffer);

    /* PGO: in main's epilogue, call __pgo_dump to write profiling data */
    if (g_compiler_options.pgo_generate &&
        node->data.function.name && strcmp(node->data.function.name, "main") == 0) {
        /* Save return value (%eax) across the dump call */
        emit_inst1("push", op_reg("eax"));
        emit_inst1("call", op_label("__pgo_dump"));
        emit_inst1("pop", op_reg("eax"));
    }
    
    /* Register allocator: restore callee-saved registers before epilogue */
    regalloc_restore_registers();
    
    emit_inst0("leave");
    emit_inst0("ret");

    /* Record function end for debug info */
    if (obj_writer && g_compiler_options.debug_info) {
        coff_writer_end_debug_func(obj_writer, (uint32_t)obj_writer->text_section.size);
    }
    
    if (out && current_syntax == SYNTAX_INTEL) {
        fprintf(out, "%s ENDP\n", node->data.function.name);
    }
}
