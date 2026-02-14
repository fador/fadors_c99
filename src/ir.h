#ifndef IR_H
#define IR_H

/*
 * ir.h — Intermediate Representation (IR) and Control Flow Graph (CFG)
 *
 * This module provides a three-address code IR lowered from the AST,
 * organized into basic blocks with explicit control flow edges.
 *
 * Design:
 *   - Operands: virtual registers (temps), named variables, constants, labels
 *   - Instructions: 3-address code (dst = src1 OP src2)
 *   - Basic blocks: sequences of non-branching instructions terminated by
 *     exactly one branch/jump/return
 *   - CFG: directed graph of basic blocks with predecessor/successor edges
 *   - IR functions: contain a list of basic blocks forming the CFG
 *
 * Usage:
 *   IRProgram *ir = ir_build_program(ast_program, opt_level);
 *   ir_dump_program(ir, stdout);   // debug print
 *   // ... run analysis/optimization passes on IR ...
 *   ir_free_program(ir);
 */

#include "ast.h"
#include "codegen.h"
#include <stdio.h>

/* ================================================================== */
/* IR Operand                                                         */
/* ================================================================== */

typedef enum {
    IR_OPERAND_NONE,       /* unused operand slot */
    IR_OPERAND_VREG,       /* virtual register: t0, t1, t2, ... */
    IR_OPERAND_VAR,        /* named variable (before register allocation) */
    IR_OPERAND_IMM_INT,    /* immediate integer constant */
    IR_OPERAND_IMM_FLOAT,  /* immediate float constant */
    IR_OPERAND_LABEL,      /* basic block label reference */
    IR_OPERAND_FUNC,       /* function name (for calls) */
    IR_OPERAND_STRING      /* string literal reference */
} IROperandKind;

typedef struct {
    IROperandKind kind;
    union {
        int vreg;              /* IR_OPERAND_VREG: virtual register number */
        char *name;            /* IR_OPERAND_VAR / IR_OPERAND_FUNC / IR_OPERAND_STRING */
        long long imm_int;     /* IR_OPERAND_IMM_INT */
        double imm_float;      /* IR_OPERAND_IMM_FLOAT */
        int label;             /* IR_OPERAND_LABEL: basic block ID */
    } val;
    Type *type;                /* type of this operand (may be NULL) */
} IROperand;

/* Convenience constructors */
static inline IROperand ir_op_none(void) {
    IROperand op = {0};
    op.kind = IR_OPERAND_NONE;
    return op;
}

static inline IROperand ir_op_vreg(int vreg, Type *type) {
    IROperand op = {0};
    op.kind = IR_OPERAND_VREG;
    op.val.vreg = vreg;
    op.type = type;
    return op;
}

static inline IROperand ir_op_imm_int(long long value) {
    IROperand op = {0};
    op.kind = IR_OPERAND_IMM_INT;
    op.val.imm_int = value;
    return op;
}

static inline IROperand ir_op_imm_float(double value) {
    IROperand op = {0};
    op.kind = IR_OPERAND_IMM_FLOAT;
    op.val.imm_float = value;
    return op;
}

static inline IROperand ir_op_label(int block_id) {
    IROperand op = {0};
    op.kind = IR_OPERAND_LABEL;
    op.val.label = block_id;
    return op;
}

/* ================================================================== */
/* IR Instruction Opcodes                                             */
/* ================================================================== */

typedef enum {
    /* Data movement */
    IR_CONST,          /* dst = imm                    (load constant)     */
    IR_COPY,           /* dst = src1                   (register copy)     */
    IR_ALLOCA,         /* dst = alloca size            (stack allocation)  */

    /* Arithmetic / logic (binary) */
    IR_ADD,            /* dst = src1 + src2                                */
    IR_SUB,            /* dst = src1 - src2                                */
    IR_MUL,            /* dst = src1 * src2                                */
    IR_DIV,            /* dst = src1 / src2                                */
    IR_MOD,            /* dst = src1 % src2                                */
    IR_AND,            /* dst = src1 & src2                                */
    IR_OR,             /* dst = src1 | src2                                */
    IR_XOR,            /* dst = src1 ^ src2                                */
    IR_SHL,            /* dst = src1 << src2                               */
    IR_SHR,            /* dst = src1 >> src2                               */

    /* Comparison (result is 0 or 1) */
    IR_CMP_EQ,         /* dst = (src1 == src2)                             */
    IR_CMP_NE,         /* dst = (src1 != src2)                             */
    IR_CMP_LT,         /* dst = (src1 <  src2)                             */
    IR_CMP_LE,         /* dst = (src1 <= src2)                             */
    IR_CMP_GT,         /* dst = (src1 >  src2)                             */
    IR_CMP_GE,         /* dst = (src1 >= src2)                             */

    /* Logical */
    IR_LOGICAL_AND,    /* dst = src1 && src2 (short-circuit)               */
    IR_LOGICAL_OR,     /* dst = src1 || src2 (short-circuit)               */

    /* Unary */
    IR_NEG,            /* dst = -src1                                      */
    IR_NOT,            /* dst = !src1         (logical not)                */
    IR_BITNOT,         /* dst = ~src1         (bitwise not)                */

    /* Memory */
    IR_LOAD,           /* dst = *src1         (memory load)                */
    IR_STORE,          /* *dst = src1         (memory store)               */
    IR_ADDR_OF,        /* dst = &src1         (address of variable)        */
    IR_MEMBER,         /* dst = src1 + offset (struct member, offset in src2) */

    /* Type conversion */
    IR_CAST,           /* dst = (type)src1                                 */

    /* Array */
    IR_INDEX,          /* dst = src1[src2]    (array index, scaled)        */
    IR_INDEX_ADDR,     /* dst = &src1[src2]   (address of array element)   */

    /* Function call */
    IR_PARAM,          /* param src1          (push call argument)         */
    IR_CALL,           /* dst = call src1, N  (call function, N = arg count in src2) */

    /* Control flow — these are always the last instruction in a basic block */
    IR_JUMP,           /* goto label          (unconditional jump)         */
    IR_BRANCH,         /* if src1 goto label_true else label_false         */
    IR_RET,            /* return src1         (function return)            */
    IR_SWITCH,         /* switch src1         (multi-way branch)           */

    /* Misc */
    IR_NOP,            /* no operation (placeholder)                       */
    IR_PHI             /* dst = phi(src1:label1, src2:label2, ...)  (SSA)  */
} IROpcode;

/* ================================================================== */
/* IR Instruction                                                     */
/* ================================================================== */

/* Switch case entry for IR_SWITCH */
typedef struct {
    long long value;   /* case value */
    int target;        /* target basic block ID */
} IRSwitchCase;

typedef struct IRInstr {
    IROpcode opcode;
    IROperand dst;     /* destination operand */
    IROperand src1;    /* first source operand */
    IROperand src2;    /* second source operand */
    int line;          /* source line number (for debug info) */
    int ssa_var;       /* SSA: variable index this PHI belongs to (-1 if N/A) */

    /* For IR_BRANCH: false target (true target in src2 label) */
    int false_target;

    /* For IR_SWITCH: case table */
    IRSwitchCase *cases;
    int case_count;
    int default_target;   /* default case basic block ID (-1 if none) */

    /* For IR_PHI: variable-length list of (value, predecessor block) pairs */
    IROperand *phi_args;     /* array of values */
    int *phi_preds;          /* array of predecessor block IDs */
    int phi_count;

    struct IRInstr *next;    /* intrusive linked list within basic block */
} IRInstr;

/* ================================================================== */
/* Basic Block                                                        */
/* ================================================================== */

#define IR_MAX_PREDS 32
#define IR_MAX_SUCCS 4

typedef struct IRBlock {
    int id;                    /* unique block ID within function */
    char *label;               /* optional human-readable label */

    /* Instructions (doubly-linked for easy insertion/removal) */
    IRInstr *first;            /* first instruction */
    IRInstr *last;             /* last instruction (must be a terminator) */
    int instr_count;

    /* CFG edges */
    int preds[IR_MAX_PREDS];   /* predecessor block IDs */
    int pred_count;
    int succs[IR_MAX_SUCCS];   /* successor block IDs */
    int succ_count;

    /* Analysis data (populated by analysis passes) */
    int *live_in;              /* bitset: variables live at block entry */
    int *live_out;             /* bitset: variables live at block exit */
    int *def;                  /* bitset: variables defined in this block */
    int *use;                  /* bitset: variables used before def in this block */

    /* Dominator tree (populated by dominator analysis) */
    int idom;                  /* immediate dominator block ID (-1 for entry) */
    int *dom_frontier;         /* dominance frontier block IDs */
    int dom_frontier_count;

    /* Loop info */
    int loop_depth;            /* nesting depth (0 = not in a loop) */
    int loop_header;           /* block ID of loop header (-1 if not in loop) */

    /* Visited flag for graph traversals */
    int visited;
} IRBlock;

/* ================================================================== */
/* IR Function                                                        */
/* ================================================================== */

typedef struct {
    char *name;                /* function name */
    Type *return_type;         /* return type */

    /* Parameters */
    char **param_names;        /* parameter names */
    Type **param_types;        /* parameter types */
    int param_count;

    /* Basic blocks */
    IRBlock **blocks;          /* array of basic block pointers */
    int block_count;
    int block_capacity;
    int entry_block;           /* ID of entry block (usually 0) */

    /* Virtual register counter */
    int next_vreg;

    /* Named variable table (for mapping AST variable names to vregs) */
    struct {
        char *name;
        int vreg;
        Type *type;
        int is_param;          /* 1 if this is a function parameter */
    } *vars;
    int var_count;
    int var_capacity;

    /* Source info */
    int line;                  /* function definition line */

    /* SSA state */
    int is_ssa;                /* 1 if in SSA form, 0 otherwise */
    int *ssa_param_vregs;      /* SSA entry vregs for parameters (NULL if not SSA) */
} IRFunction;

/* ================================================================== */
/* IR Program (collection of functions + globals)                     */
/* ================================================================== */

typedef struct {
    IRFunction **functions;
    int func_count;
    int func_capacity;

    /* Global variables */
    struct {
        char *name;
        Type *type;
        long long init_value;  /* integer initial value (0 if none) */
        int has_init;
    } *globals;
    int global_count;
    int global_capacity;

    /* String literals */
    struct {
        char *value;
        int length;
        int id;                /* string table index */
    } *strings;
    int string_count;
    int string_capacity;
} IRProgram;

/* ================================================================== */
/* API: IR Construction                                               */
/* ================================================================== */

/* Build IR from AST program node.
 * Walks all functions, lowers to 3-address code, splits into basic blocks,
 * and constructs CFG edges. */
IRProgram *ir_build_program(ASTNode *program, OptLevel level);

/* ================================================================== */
/* API: Basic Block / CFG Operations                                  */
/* ================================================================== */

/* Create a new basic block in a function, returns its ID. */
int ir_new_block(IRFunction *func, const char *label);

/* Append an instruction to a basic block. */
void ir_block_append(IRBlock *block, IRInstr *instr);

/* Add a CFG edge from block `from` to block `to`. */
void ir_cfg_add_edge(IRFunction *func, int from, int to);

/* Build CFG edges by scanning terminator instructions in all blocks. */
void ir_build_cfg(IRFunction *func);

/* ================================================================== */
/* API: IR Instruction Creation                                       */
/* ================================================================== */

/* Allocate a new instruction with the given opcode. */
IRInstr *ir_instr_new(IROpcode opcode, int line);

/* ================================================================== */
/* API: SSA Construction                                              */
/* ================================================================== */

/* Compute immediate dominators for all blocks using Cooper-Harvey-Kennedy. */
void ir_compute_dominators(IRFunction *func);

/* Compute dominance frontiers from dominator tree. */
void ir_compute_dom_frontiers(IRFunction *func);

/* Full SSA construction: dominators + frontiers + phi insertion + rename. */
void ir_ssa_construct(IRFunction *func);

/* Construct SSA for all functions in the program. */
void ir_ssa_construct_program(IRProgram *prog);

/* Validate SSA properties (every vreg defined exactly once).
 * Returns 1 if valid, 0 otherwise. Prints violations to stderr. */
int ir_ssa_validate(IRFunction *func);

/* ================================================================== */
/* API: Debug Output                                                  */
/* ================================================================== */

/* Dump the entire IR program to a file in human-readable form. */
void ir_dump_program(IRProgram *prog, FILE *out);

/* Dump a single function's IR (with CFG info). */
void ir_dump_function(IRFunction *func, FILE *out);

/* Dump a single basic block. */
void ir_dump_block(IRBlock *block, FILE *out);

/* Print an IR operand. */
void ir_dump_operand(IROperand *op, FILE *out);

/* Get a human-readable name for an IR opcode. */
const char *ir_opcode_name(IROpcode op);

/* ================================================================== */
/* API: Memory Management                                             */
/* ================================================================== */

/* Free an IR program and all its contents. */
void ir_free_program(IRProgram *prog);

/* Free an IR function. */
void ir_free_function(IRFunction *func);

/* ================================================================== */
/* API: Utility                                                       */
/* ================================================================== */

/* Check if an instruction is a terminator (ends a basic block). */
static inline int ir_is_terminator(IROpcode op) {
    return op == IR_JUMP || op == IR_BRANCH || op == IR_RET || op == IR_SWITCH;
}

/* Check if an instruction has side effects (calls, stores). */
static inline int ir_has_side_effects(IROpcode op) {
    return op == IR_CALL || op == IR_STORE || op == IR_PARAM || op == IR_RET;
}

#endif /* IR_H */
