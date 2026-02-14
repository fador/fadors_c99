/*
 * ir.c — Intermediate Representation (IR) and Control Flow Graph (CFG)
 *
 * Lowers AST to three-address code IR, splits into basic blocks,
 * and constructs the control flow graph with predecessor/successor edges.
 *
 * The IR is built per-function:
 *   1. AST expressions → flat 3-address instructions using virtual registers
 *   2. Control flow (if/while/for/switch) → labels + branch instructions
 *   3. Instructions partitioned into basic blocks at label/branch boundaries
 *   4. CFG edges derived from branch targets
 */

#include "ir.h"
#include "lexer.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ================================================================== */
/* Memory helpers                                                     */
/* ================================================================== */

static void *ir_alloc(size_t size) {
    void *p = calloc(1, size);
    if (!p) {
        fprintf(stderr, "ir: out of memory\n");
        exit(1);
    }
    return p;
}

static char *ir_strdup(const char *s) {
    if (!s) return NULL;
    char *d = strdup(s);
    if (!d) {
        fprintf(stderr, "ir: out of memory\n");
        exit(1);
    }
    return d;
}

/* ================================================================== */
/* IR Instruction creation                                            */
/* ================================================================== */

IRInstr *ir_instr_new(IROpcode opcode, int line) {
    IRInstr *instr = (IRInstr *)ir_alloc(sizeof(IRInstr));
    instr->opcode = opcode;
    instr->line = line;
    instr->dst = ir_op_none();
    instr->src1 = ir_op_none();
    instr->src2 = ir_op_none();
    instr->false_target = -1;
    instr->default_target = -1;
    instr->ssa_var = -1;
    instr->next = NULL;
    return instr;
}

/* ================================================================== */
/* Basic Block operations                                             */
/* ================================================================== */

static IRBlock *ir_block_new(int id, const char *label) {
    IRBlock *b = (IRBlock *)ir_alloc(sizeof(IRBlock));
    b->id = id;
    b->label = label ? ir_strdup(label) : NULL;
    b->first = NULL;
    b->last = NULL;
    b->instr_count = 0;
    b->pred_count = 0;
    b->succ_count = 0;
    b->idom = -1;
    b->loop_header = -1;
    b->loop_depth = 0;
    b->dom_frontier = NULL;
    b->dom_frontier_count = 0;
    b->visited = 0;
    b->live_in = NULL;
    b->live_out = NULL;
    b->def = NULL;
    b->use = NULL;
    return b;
}

void ir_block_append(IRBlock *block, IRInstr *instr) {
    instr->next = NULL;
    if (!block->first) {
        block->first = instr;
        block->last = instr;
    } else {
        block->last->next = instr;
        block->last = instr;
    }
    block->instr_count++;
}

/* ================================================================== */
/* IR Function operations                                             */
/* ================================================================== */

static IRFunction *ir_function_new(const char *name, int line) {
    IRFunction *func = (IRFunction *)ir_alloc(sizeof(IRFunction));
    func->name = ir_strdup(name);
    func->line = line;
    func->block_count = 0;
    func->block_capacity = 16;
    func->blocks = (IRBlock **)ir_alloc(func->block_capacity * sizeof(IRBlock *));
    func->entry_block = 0;
    func->next_vreg = 0;
    func->var_count = 0;
    func->var_capacity = 32;
    func->vars = ir_alloc(func->var_capacity * sizeof(*func->vars));
    func->param_count = 0;
    func->param_names = NULL;
    func->param_types = NULL;
    func->return_type = NULL;
    func->is_ssa = 0;
    func->ssa_param_vregs = NULL;
    func->regalloc = NULL;
    func->regalloc_spill = NULL;
    func->spill_count = 0;
    func->has_regalloc = 0;
    return func;
}

int ir_new_block(IRFunction *func, const char *label) {
    if (func->block_count >= func->block_capacity) {
        func->block_capacity *= 2;
        func->blocks = (IRBlock **)realloc(func->blocks,
                                           func->block_capacity * sizeof(IRBlock *));
    }
    int id = func->block_count;
    char auto_label[64];
    if (!label) {
        snprintf(auto_label, sizeof(auto_label), "bb%d", id);
        label = auto_label;
    }
    func->blocks[func->block_count++] = ir_block_new(id, label);
    return id;
}

/* Allocate a new virtual register. */
static int ir_new_vreg(IRFunction *func) {
    return func->next_vreg++;
}

/* Look up or create a variable → vreg mapping. */
static int ir_var_lookup(IRFunction *func, const char *name, Type *type) {
    for (int i = 0; i < func->var_count; i++) {
        if (strcmp(func->vars[i].name, name) == 0)
            return func->vars[i].vreg;
    }
    /* New variable: assign a vreg */
    if (func->var_count >= func->var_capacity) {
        func->var_capacity *= 2;
        func->vars = realloc(func->vars, func->var_capacity * sizeof(*func->vars));
    }
    int vreg = ir_new_vreg(func);
    func->vars[func->var_count].name = ir_strdup(name);
    func->vars[func->var_count].vreg = vreg;
    func->vars[func->var_count].type = type;
    func->vars[func->var_count].is_param = 0;
    func->var_count++;
    return vreg;
}

/* ================================================================== */
/* CFG Edge Management                                                */
/* ================================================================== */

void ir_cfg_add_edge(IRFunction *func, int from, int to) {
    if (from < 0 || from >= func->block_count) return;
    if (to < 0 || to >= func->block_count) return;

    IRBlock *f = func->blocks[from];
    IRBlock *t = func->blocks[to];

    /* Check for duplicate edge */
    for (int i = 0; i < f->succ_count; i++) {
        if (f->succs[i] == to) return;
    }

    if (f->succ_count < IR_MAX_SUCCS) {
        f->succs[f->succ_count++] = to;
    }
    if (t->pred_count < IR_MAX_PREDS) {
        t->preds[t->pred_count++] = from;
    }
}

void ir_build_cfg(IRFunction *func) {
    /* Clear existing edges */
    for (int i = 0; i < func->block_count; i++) {
        func->blocks[i]->pred_count = 0;
        func->blocks[i]->succ_count = 0;
    }

    /* Scan each block's terminator to determine edges */
    for (int i = 0; i < func->block_count; i++) {
        IRBlock *block = func->blocks[i];
        IRInstr *term = block->last;
        if (!term) {
            /* Empty block: fall through to next block */
            if (i + 1 < func->block_count) {
                ir_cfg_add_edge(func, i, i + 1);
            }
            continue;
        }

        switch (term->opcode) {
        case IR_JUMP:
            if (term->src1.kind == IR_OPERAND_LABEL) {
                ir_cfg_add_edge(func, i, term->src1.val.label);
            }
            break;

        case IR_BRANCH:
            /* true target in src2, false target in false_target */
            if (term->src2.kind == IR_OPERAND_LABEL) {
                ir_cfg_add_edge(func, i, term->src2.val.label);
            }
            if (term->false_target >= 0) {
                ir_cfg_add_edge(func, i, term->false_target);
            }
            break;

        case IR_RET:
            /* No successors — function exit */
            break;

        case IR_SWITCH:
            /* Edges to all case targets and default */
            for (int c = 0; c < term->case_count; c++) {
                ir_cfg_add_edge(func, i, term->cases[c].target);
            }
            if (term->default_target >= 0) {
                ir_cfg_add_edge(func, i, term->default_target);
            }
            break;

        default:
            /* Non-terminator last instruction: fall through to next block */
            if (i + 1 < func->block_count) {
                ir_cfg_add_edge(func, i, i + 1);
            }
            break;
        }
    }
}

/* ================================================================== */
/* IR Builder: AST → IR lowering                                      */
/* ================================================================== */

/* Builder context for lowering one function at a time. */
typedef struct {
    IRFunction *func;
    int current_block;         /* block we're emitting into */

    /* Label stacks for break/continue targets */
    int break_targets[64];
    int break_depth;
    int continue_targets[64];
    int continue_depth;
} IRBuilder;

static void builder_init(IRBuilder *b, IRFunction *func) {
    b->func = func;
    b->current_block = 0;
    b->break_depth = 0;
    b->continue_depth = 0;
}

/* Get the current block we're emitting into. */
static IRBlock *builder_current(IRBuilder *b) {
    return b->func->blocks[b->current_block];
}

/* Switch emission to a different block. */
static void builder_set_block(IRBuilder *b, int block_id) {
    b->current_block = block_id;
}

/* Emit an instruction into the current block. */
static void builder_emit(IRBuilder *b, IRInstr *instr) {
    ir_block_append(builder_current(b), instr);
}

/* Create a new block and return its ID. */
static int builder_new_block(IRBuilder *b, const char *label) {
    return ir_new_block(b->func, label);
}

/* Ensure the current block ends with a terminator.
   If it doesn't, emit a jump to the given fallthrough block. */
static void builder_ensure_terminator(IRBuilder *b, int fallthrough) {
    IRBlock *cur = builder_current(b);
    if (cur->last && ir_is_terminator(cur->last->opcode))
        return;
    IRInstr *jmp = ir_instr_new(IR_JUMP, 0);
    jmp->src1 = ir_op_label(fallthrough);
    builder_emit(b, jmp);
}

/* ------------------------------------------------------------------ */
/* Expression lowering: returns a vreg containing the result          */
/* ------------------------------------------------------------------ */

/* Map AST binary operator (TokenType) to IR opcode. */
static IROpcode token_to_ir_binop(TokenType op) {
    switch (op) {
    case TOKEN_PLUS:            return IR_ADD;
    case TOKEN_MINUS:           return IR_SUB;
    case TOKEN_STAR:            return IR_MUL;
    case TOKEN_SLASH:           return IR_DIV;
    case TOKEN_PERCENT:         return IR_MOD;
    case TOKEN_AMPERSAND:       return IR_AND;
    case TOKEN_PIPE:            return IR_OR;
    case TOKEN_CARET:           return IR_XOR;
    case TOKEN_LESS_LESS:       return IR_SHL;
    case TOKEN_GREATER_GREATER: return IR_SHR;
    case TOKEN_EQUAL_EQUAL:     return IR_CMP_EQ;
    case TOKEN_BANG_EQUAL:      return IR_CMP_NE;
    case TOKEN_LESS:            return IR_CMP_LT;
    case TOKEN_LESS_EQUAL:      return IR_CMP_LE;
    case TOKEN_GREATER:         return IR_CMP_GT;
    case TOKEN_GREATER_EQUAL:   return IR_CMP_GE;
    case TOKEN_AMPERSAND_AMPERSAND: return IR_LOGICAL_AND;
    case TOKEN_PIPE_PIPE:       return IR_LOGICAL_OR;
    default:                    return IR_NOP;
    }
}

/* Forward declarations */
static IROperand ir_lower_expr(IRBuilder *b, ASTNode *expr);
static void ir_lower_stmt(IRBuilder *b, ASTNode *stmt);
static void ir_lower_block(IRBuilder *b, ASTNode *block);

/* Lower an expression to IR, returning an operand with the result. */
static IROperand ir_lower_expr(IRBuilder *b, ASTNode *expr) {
    if (!expr) return ir_op_none();

    switch (expr->type) {
    case AST_INTEGER: {
        int dst = ir_new_vreg(b->func);
        IRInstr *instr = ir_instr_new(IR_CONST, expr->line);
        instr->dst = ir_op_vreg(dst, expr->resolved_type);
        instr->src1 = ir_op_imm_int(expr->data.integer.value);
        builder_emit(b, instr);
        return ir_op_vreg(dst, expr->resolved_type);
    }

    case AST_FLOAT: {
        int dst = ir_new_vreg(b->func);
        IRInstr *instr = ir_instr_new(IR_CONST, expr->line);
        instr->dst = ir_op_vreg(dst, expr->resolved_type);
        instr->src1 = ir_op_imm_float(expr->data.float_val.value);
        builder_emit(b, instr);
        return ir_op_vreg(dst, expr->resolved_type);
    }

    case AST_STRING: {
        int dst = ir_new_vreg(b->func);
        IRInstr *instr = ir_instr_new(IR_CONST, expr->line);
        instr->dst = ir_op_vreg(dst, expr->resolved_type);
        IROperand str_op = {0};
        str_op.kind = IR_OPERAND_STRING;
        str_op.val.name = ir_strdup(expr->data.string.value);
        instr->src1 = str_op;
        builder_emit(b, instr);
        return ir_op_vreg(dst, expr->resolved_type);
    }

    case AST_IDENTIFIER: {
        int vreg = ir_var_lookup(b->func, expr->data.identifier.name,
                                 expr->resolved_type);
        /* Emit a COPY from the named variable's vreg to a fresh temp
           (simplifies SSA construction later — each use gets its own temp) */
        int dst = ir_new_vreg(b->func);
        IRInstr *instr = ir_instr_new(IR_COPY, expr->line);
        instr->dst = ir_op_vreg(dst, expr->resolved_type);
        instr->src1 = ir_op_vreg(vreg, expr->resolved_type);
        builder_emit(b, instr);
        return ir_op_vreg(dst, expr->resolved_type);
    }

    case AST_BINARY_EXPR: {
        TokenType op = expr->data.binary_expr.op;

        /* Short-circuit logical operators: && and || */
        if (op == TOKEN_AMPERSAND_AMPERSAND || op == TOKEN_PIPE_PIPE) {
            int result_vreg = ir_new_vreg(b->func);
            int rhs_block = builder_new_block(b, op == TOKEN_AMPERSAND_AMPERSAND ?
                                              "and_rhs" : "or_rhs");
            int merge_block = builder_new_block(b, "logic_merge");

            /* Evaluate LHS */
            IROperand lhs = ir_lower_expr(b, expr->data.binary_expr.left);

            /* Branch based on LHS value */
            IRInstr *br = ir_instr_new(IR_BRANCH, expr->line);
            br->src1 = lhs;
            if (op == TOKEN_AMPERSAND_AMPERSAND) {
                /* &&: if LHS is true, eval RHS; if false, result is 0 */
                br->src2 = ir_op_label(rhs_block);
                br->false_target = merge_block;
            } else {
                /* ||: if LHS is true, result is 1; if false, eval RHS */
                br->src2 = ir_op_label(merge_block);
                br->false_target = rhs_block;
            }
            builder_emit(b, br);

            int lhs_block_id = b->current_block;

            /* Evaluate RHS in its own block */
            builder_set_block(b, rhs_block);
            IROperand rhs = ir_lower_expr(b, expr->data.binary_expr.right);

            /* Convert RHS to bool: cmp_ne rhs, 0 */
            int rhs_bool = ir_new_vreg(b->func);
            IRInstr *cmp = ir_instr_new(IR_CMP_NE, expr->line);
            cmp->dst = ir_op_vreg(rhs_bool, NULL);
            cmp->src1 = rhs;
            cmp->src2 = ir_op_imm_int(0);
            builder_emit(b, cmp);

            /* Jump to merge */
            builder_ensure_terminator(b, merge_block);
            int rhs_block_end = b->current_block;

            /* Merge block: phi(lhs_result, rhs_result) */
            builder_set_block(b, merge_block);

            /* For now, use a simplified approach: store result in a dedicated vreg.
               Full PHI nodes will be added during SSA construction. */
            IRInstr *phi = ir_instr_new(IR_PHI, expr->line);
            phi->dst = ir_op_vreg(result_vreg, expr->resolved_type);
            phi->phi_count = 2;
            phi->phi_args = (IROperand *)ir_alloc(2 * sizeof(IROperand));
            phi->phi_preds = (int *)ir_alloc(2 * sizeof(int));

            if (op == TOKEN_AMPERSAND_AMPERSAND) {
                phi->phi_args[0] = ir_op_imm_int(0);  /* LHS false → result 0 */
                phi->phi_preds[0] = lhs_block_id;
                phi->phi_args[1] = ir_op_vreg(rhs_bool, NULL);
                phi->phi_preds[1] = rhs_block_end;
            } else {
                phi->phi_args[0] = ir_op_imm_int(1);  /* LHS true → result 1 */
                phi->phi_preds[0] = lhs_block_id;
                phi->phi_args[1] = ir_op_vreg(rhs_bool, NULL);
                phi->phi_preds[1] = rhs_block_end;
            }
            builder_emit(b, phi);

            return ir_op_vreg(result_vreg, expr->resolved_type);
        }

        /* Regular binary expression */
        IROperand lhs = ir_lower_expr(b, expr->data.binary_expr.left);
        IROperand rhs = ir_lower_expr(b, expr->data.binary_expr.right);

        IROpcode ir_op = token_to_ir_binop(op);
        if (ir_op == IR_NOP) {
            /* Unknown operator — return LHS as fallback */
            return lhs;
        }

        int dst = ir_new_vreg(b->func);
        IRInstr *instr = ir_instr_new(ir_op, expr->line);
        instr->dst = ir_op_vreg(dst, expr->resolved_type);
        instr->src1 = lhs;
        instr->src2 = rhs;
        builder_emit(b, instr);
        return ir_op_vreg(dst, expr->resolved_type);
    }

    case AST_NEG: {
        IROperand src = ir_lower_expr(b, expr->data.unary.expression);
        int dst = ir_new_vreg(b->func);
        IRInstr *instr = ir_instr_new(IR_NEG, expr->line);
        instr->dst = ir_op_vreg(dst, expr->resolved_type);
        instr->src1 = src;
        builder_emit(b, instr);
        return ir_op_vreg(dst, expr->resolved_type);
    }

    case AST_NOT: {
        IROperand src = ir_lower_expr(b, expr->data.unary.expression);
        int dst = ir_new_vreg(b->func);
        IRInstr *instr = ir_instr_new(IR_NOT, expr->line);
        instr->dst = ir_op_vreg(dst, expr->resolved_type);
        instr->src1 = src;
        builder_emit(b, instr);
        return ir_op_vreg(dst, expr->resolved_type);
    }

    case AST_BITWISE_NOT: {
        IROperand src = ir_lower_expr(b, expr->data.unary.expression);
        int dst = ir_new_vreg(b->func);
        IRInstr *instr = ir_instr_new(IR_BITNOT, expr->line);
        instr->dst = ir_op_vreg(dst, expr->resolved_type);
        instr->src1 = src;
        builder_emit(b, instr);
        return ir_op_vreg(dst, expr->resolved_type);
    }

    case AST_DEREF: {
        IROperand src = ir_lower_expr(b, expr->data.unary.expression);
        int dst = ir_new_vreg(b->func);
        IRInstr *instr = ir_instr_new(IR_LOAD, expr->line);
        instr->dst = ir_op_vreg(dst, expr->resolved_type);
        instr->src1 = src;
        builder_emit(b, instr);
        return ir_op_vreg(dst, expr->resolved_type);
    }

    case AST_ADDR_OF: {
        IROperand src = ir_lower_expr(b, expr->data.unary.expression);
        int dst = ir_new_vreg(b->func);
        IRInstr *instr = ir_instr_new(IR_ADDR_OF, expr->line);
        instr->dst = ir_op_vreg(dst, expr->resolved_type);
        instr->src1 = src;
        builder_emit(b, instr);
        return ir_op_vreg(dst, expr->resolved_type);
    }

    case AST_PRE_INC:
    case AST_PRE_DEC: {
        /* ++x: load x, add 1, store back, result is new value */
        IROperand src = ir_lower_expr(b, expr->data.unary.expression);
        int result = ir_new_vreg(b->func);
        IRInstr *instr = ir_instr_new(
            expr->type == AST_PRE_INC ? IR_ADD : IR_SUB, expr->line);
        instr->dst = ir_op_vreg(result, expr->resolved_type);
        instr->src1 = src;
        instr->src2 = ir_op_imm_int(1);
        builder_emit(b, instr);

        /* Store back to the variable */
        if (expr->data.unary.expression->type == AST_IDENTIFIER) {
            int var_vreg = ir_var_lookup(b->func,
                expr->data.unary.expression->data.identifier.name,
                expr->resolved_type);
            IRInstr *store = ir_instr_new(IR_COPY, expr->line);
            store->dst = ir_op_vreg(var_vreg, expr->resolved_type);
            store->src1 = ir_op_vreg(result, expr->resolved_type);
            builder_emit(b, store);
        }
        return ir_op_vreg(result, expr->resolved_type);
    }

    case AST_POST_INC:
    case AST_POST_DEC: {
        /* x++: load x, result is old value, then add 1 and store back */
        IROperand src = ir_lower_expr(b, expr->data.unary.expression);

        /* Save old value */
        int old_val = ir_new_vreg(b->func);
        IRInstr *copy = ir_instr_new(IR_COPY, expr->line);
        copy->dst = ir_op_vreg(old_val, expr->resolved_type);
        copy->src1 = src;
        builder_emit(b, copy);

        /* Compute new value */
        int new_val = ir_new_vreg(b->func);
        IRInstr *instr = ir_instr_new(
            expr->type == AST_POST_INC ? IR_ADD : IR_SUB, expr->line);
        instr->dst = ir_op_vreg(new_val, expr->resolved_type);
        instr->src1 = src;
        instr->src2 = ir_op_imm_int(1);
        builder_emit(b, instr);

        /* Store back */
        if (expr->data.unary.expression->type == AST_IDENTIFIER) {
            int var_vreg = ir_var_lookup(b->func,
                expr->data.unary.expression->data.identifier.name,
                expr->resolved_type);
            IRInstr *store = ir_instr_new(IR_COPY, expr->line);
            store->dst = ir_op_vreg(var_vreg, expr->resolved_type);
            store->src1 = ir_op_vreg(new_val, expr->resolved_type);
            builder_emit(b, store);
        }
        return ir_op_vreg(old_val, expr->resolved_type);
    }

    case AST_CAST: {
        IROperand src = ir_lower_expr(b, expr->data.cast.expression);
        int dst = ir_new_vreg(b->func);
        IRInstr *instr = ir_instr_new(IR_CAST, expr->line);
        instr->dst = ir_op_vreg(dst, expr->data.cast.target_type);
        instr->src1 = src;
        builder_emit(b, instr);
        return ir_op_vreg(dst, expr->data.cast.target_type);
    }

    case AST_ARRAY_ACCESS: {
        IROperand arr = ir_lower_expr(b, expr->data.array_access.array);
        IROperand idx = ir_lower_expr(b, expr->data.array_access.index);
        int dst = ir_new_vreg(b->func);
        IRInstr *instr = ir_instr_new(IR_INDEX, expr->line);
        instr->dst = ir_op_vreg(dst, expr->resolved_type);
        instr->src1 = arr;
        instr->src2 = idx;
        builder_emit(b, instr);
        return ir_op_vreg(dst, expr->resolved_type);
    }

    case AST_MEMBER_ACCESS: {
        IROperand base = ir_lower_expr(b, expr->data.member_access.struct_expr);
        int dst = ir_new_vreg(b->func);
        IRInstr *instr = ir_instr_new(IR_MEMBER, expr->line);
        instr->dst = ir_op_vreg(dst, expr->resolved_type);
        instr->src1 = base;
        /* Store the member name in src2 as a VAR operand for now */
        IROperand member_op = {0};
        member_op.kind = IR_OPERAND_VAR;
        member_op.val.name = ir_strdup(expr->data.member_access.member_name);
        instr->src2 = member_op;
        builder_emit(b, instr);
        return ir_op_vreg(dst, expr->resolved_type);
    }

    case AST_CALL: {
        /* Lower arguments and emit IR_PARAM for each */
        int arg_count = (int)expr->children_count;
        for (int i = 0; i < arg_count; i++) {
            IROperand arg = ir_lower_expr(b, expr->children[i]);
            IRInstr *param = ir_instr_new(IR_PARAM, expr->line);
            param->src1 = arg;
            builder_emit(b, param);
        }

        /* Emit the call */
        int dst = ir_new_vreg(b->func);
        IRInstr *call = ir_instr_new(IR_CALL, expr->line);
        call->dst = ir_op_vreg(dst, expr->resolved_type);
        IROperand func_op = {0};
        func_op.kind = IR_OPERAND_FUNC;
        func_op.val.name = ir_strdup(expr->data.call.name);
        call->src1 = func_op;
        call->src2 = ir_op_imm_int(arg_count);
        builder_emit(b, call);
        return ir_op_vreg(dst, expr->resolved_type);
    }

    case AST_ASSIGN: {
        /* Lower RHS */
        IROperand rhs = ir_lower_expr(b, expr->data.assign.value);

        /* Determine assignment target */
        ASTNode *target = expr->data.assign.left;
        if (target && target->type == AST_IDENTIFIER) {
            /* Simple variable assignment */
            int var_vreg = ir_var_lookup(b->func,
                target->data.identifier.name, target->resolved_type);
            IRInstr *store = ir_instr_new(IR_COPY, expr->line);
            store->dst = ir_op_vreg(var_vreg, target->resolved_type);
            store->src1 = rhs;
            builder_emit(b, store);
            return rhs;
        } else if (target && target->type == AST_DEREF) {
            /* *ptr = value: memory store */
            IROperand addr = ir_lower_expr(b, target->data.unary.expression);
            IRInstr *store = ir_instr_new(IR_STORE, expr->line);
            store->dst = addr;
            store->src1 = rhs;
            builder_emit(b, store);
            return rhs;
        } else if (target && target->type == AST_ARRAY_ACCESS) {
            /* arr[idx] = value */
            IROperand arr = ir_lower_expr(b, target->data.array_access.array);
            IROperand idx = ir_lower_expr(b, target->data.array_access.index);
            int addr = ir_new_vreg(b->func);
            IRInstr *idx_addr = ir_instr_new(IR_INDEX_ADDR, expr->line);
            idx_addr->dst = ir_op_vreg(addr, NULL);
            idx_addr->src1 = arr;
            idx_addr->src2 = idx;
            builder_emit(b, idx_addr);

            IRInstr *store = ir_instr_new(IR_STORE, expr->line);
            store->dst = ir_op_vreg(addr, NULL);
            store->src1 = rhs;
            builder_emit(b, store);
            return rhs;
        } else if (target && target->type == AST_MEMBER_ACCESS) {
            /* struct.member = value or ptr->member = value */
            IROperand base = ir_lower_expr(b, target->data.member_access.struct_expr);
            int addr = ir_new_vreg(b->func);
            IRInstr *member = ir_instr_new(IR_MEMBER, expr->line);
            member->dst = ir_op_vreg(addr, NULL);
            member->src1 = base;
            IROperand mop = {0};
            mop.kind = IR_OPERAND_VAR;
            mop.val.name = ir_strdup(target->data.member_access.member_name);
            member->src2 = mop;
            builder_emit(b, member);

            IRInstr *store = ir_instr_new(IR_STORE, expr->line);
            store->dst = ir_op_vreg(addr, NULL);
            store->src1 = rhs;
            builder_emit(b, store);
            return rhs;
        }
        /* Generic fallback: lower target, emit store */
        IROperand target_op = ir_lower_expr(b, target);
        IRInstr *store = ir_instr_new(IR_STORE, expr->line);
        store->dst = target_op;
        store->src1 = rhs;
        builder_emit(b, store);
        return rhs;
    }

    case AST_INIT_LIST: {
        /* Initializer list — lower each element. Return the first element
           for simple cases; full aggregate init handled at statement level. */
        if (expr->children_count > 0) {
            return ir_lower_expr(b, expr->children[0]);
        }
        return ir_op_none();
    }

    default:
        /* Unsupported expression type — emit a NOP and return no result */
        return ir_op_none();
    }
}

/* ------------------------------------------------------------------ */
/* Statement lowering                                                 */
/* ------------------------------------------------------------------ */

static void ir_lower_stmt(IRBuilder *b, ASTNode *stmt) {
    if (!stmt) return;

    switch (stmt->type) {
    case AST_BLOCK:
        ir_lower_block(b, stmt);
        break;

    case AST_VAR_DECL: {
        int var_vreg = ir_var_lookup(b->func, stmt->data.var_decl.name,
                                     stmt->resolved_type);
        if (stmt->data.var_decl.initializer) {
            IROperand init = ir_lower_expr(b, stmt->data.var_decl.initializer);
            IRInstr *store = ir_instr_new(IR_COPY, stmt->line);
            store->dst = ir_op_vreg(var_vreg, stmt->resolved_type);
            store->src1 = init;
            builder_emit(b, store);
        }
        break;
    }

    case AST_ASSIGN: {
        /* Treat assignment as an expression (result discarded) */
        ir_lower_expr(b, stmt);
        break;
    }

    case AST_RETURN: {
        if (stmt->data.return_stmt.expression) {
            IROperand val = ir_lower_expr(b, stmt->data.return_stmt.expression);
            IRInstr *ret = ir_instr_new(IR_RET, stmt->line);
            ret->src1 = val;
            builder_emit(b, ret);
        } else {
            IRInstr *ret = ir_instr_new(IR_RET, stmt->line);
            builder_emit(b, ret);
        }
        break;
    }

    case AST_IF: {
        int then_block = builder_new_block(b, "if_then");
        int else_block = stmt->data.if_stmt.else_branch
                             ? builder_new_block(b, "if_else") : -1;
        int merge_block = builder_new_block(b, "if_merge");

        /* Evaluate condition */
        IROperand cond = ir_lower_expr(b, stmt->data.if_stmt.condition);

        /* Branch */
        IRInstr *br = ir_instr_new(IR_BRANCH, stmt->line);
        br->src1 = cond;
        br->src2 = ir_op_label(then_block);
        br->false_target = (else_block >= 0) ? else_block : merge_block;
        builder_emit(b, br);

        /* Then branch */
        builder_set_block(b, then_block);
        ir_lower_stmt(b, stmt->data.if_stmt.then_branch);
        builder_ensure_terminator(b, merge_block);

        /* Else branch */
        if (else_block >= 0) {
            builder_set_block(b, else_block);
            ir_lower_stmt(b, stmt->data.if_stmt.else_branch);
            builder_ensure_terminator(b, merge_block);
        }

        /* Continue in merge block */
        builder_set_block(b, merge_block);
        break;
    }

    case AST_WHILE: {
        int cond_block = builder_new_block(b, "while_cond");
        int body_block = builder_new_block(b, "while_body");
        int exit_block = builder_new_block(b, "while_exit");

        /* Push break/continue targets */
        b->break_targets[b->break_depth++] = exit_block;
        b->continue_targets[b->continue_depth++] = cond_block;

        /* Jump to condition check */
        builder_ensure_terminator(b, cond_block);

        /* Condition block */
        builder_set_block(b, cond_block);
        IROperand cond = ir_lower_expr(b, stmt->data.while_stmt.condition);
        IRInstr *br = ir_instr_new(IR_BRANCH, stmt->line);
        br->src1 = cond;
        br->src2 = ir_op_label(body_block);
        br->false_target = exit_block;
        builder_emit(b, br);

        /* Body block */
        builder_set_block(b, body_block);
        ir_lower_stmt(b, stmt->data.while_stmt.body);
        builder_ensure_terminator(b, cond_block);

        /* Pop break/continue */
        b->break_depth--;
        b->continue_depth--;

        /* Continue in exit block */
        builder_set_block(b, exit_block);
        break;
    }

    case AST_DO_WHILE: {
        int body_block = builder_new_block(b, "do_body");
        int cond_block = builder_new_block(b, "do_cond");
        int exit_block = builder_new_block(b, "do_exit");

        b->break_targets[b->break_depth++] = exit_block;
        b->continue_targets[b->continue_depth++] = cond_block;

        /* Jump to body */
        builder_ensure_terminator(b, body_block);

        /* Body */
        builder_set_block(b, body_block);
        ir_lower_stmt(b, stmt->data.while_stmt.body);
        builder_ensure_terminator(b, cond_block);

        /* Condition */
        builder_set_block(b, cond_block);
        IROperand cond = ir_lower_expr(b, stmt->data.while_stmt.condition);
        IRInstr *br = ir_instr_new(IR_BRANCH, stmt->line);
        br->src1 = cond;
        br->src2 = ir_op_label(body_block);
        br->false_target = exit_block;
        builder_emit(b, br);

        b->break_depth--;
        b->continue_depth--;

        builder_set_block(b, exit_block);
        break;
    }

    case AST_FOR: {
        int cond_block = builder_new_block(b, "for_cond");
        int body_block = builder_new_block(b, "for_body");
        int incr_block = builder_new_block(b, "for_incr");
        int exit_block = builder_new_block(b, "for_exit");

        b->break_targets[b->break_depth++] = exit_block;
        b->continue_targets[b->continue_depth++] = incr_block;

        /* Init */
        if (stmt->data.for_stmt.init) {
            ir_lower_stmt(b, stmt->data.for_stmt.init);
        }
        builder_ensure_terminator(b, cond_block);

        /* Condition */
        builder_set_block(b, cond_block);
        if (stmt->data.for_stmt.condition) {
            IROperand cond = ir_lower_expr(b, stmt->data.for_stmt.condition);
            IRInstr *br = ir_instr_new(IR_BRANCH, stmt->line);
            br->src1 = cond;
            br->src2 = ir_op_label(body_block);
            br->false_target = exit_block;
            builder_emit(b, br);
        } else {
            /* No condition: always true (infinite loop) */
            IRInstr *jmp = ir_instr_new(IR_JUMP, stmt->line);
            jmp->src1 = ir_op_label(body_block);
            builder_emit(b, jmp);
        }

        /* Body */
        builder_set_block(b, body_block);
        if (stmt->data.for_stmt.body) {
            ir_lower_stmt(b, stmt->data.for_stmt.body);
        }
        builder_ensure_terminator(b, incr_block);

        /* Increment */
        builder_set_block(b, incr_block);
        if (stmt->data.for_stmt.increment) {
            ir_lower_expr(b, stmt->data.for_stmt.increment);
        }
        builder_ensure_terminator(b, cond_block);

        b->break_depth--;
        b->continue_depth--;

        builder_set_block(b, exit_block);
        break;
    }

    case AST_SWITCH: {
        int exit_block = builder_new_block(b, "switch_exit");
        b->break_targets[b->break_depth++] = exit_block;

        /* Evaluate switch expression */
        IROperand switch_val = ir_lower_expr(b, stmt->data.switch_stmt.condition);

        /* Scan the switch body for case/default labels to create blocks */
        ASTNode *body = stmt->data.switch_stmt.body;
        if (body && body->type == AST_BLOCK) {
            /* First pass: count cases and create blocks */
            int num_cases = 0;
            int has_default = 0;
            for (size_t i = 0; i < body->children_count; i++) {
                if (body->children[i]->type == AST_CASE) num_cases++;
                if (body->children[i]->type == AST_DEFAULT) has_default = 1;
            }

            /* Create case blocks */
            int *case_blocks = (int *)ir_alloc((num_cases + 1) * sizeof(int));
            long long *case_vals = (long long *)ir_alloc(num_cases * sizeof(long long));
            int default_block = -1;
            int case_idx = 0;

            /* Second pass: create a block for each case/default */
            for (size_t i = 0; i < body->children_count; i++) {
                if (body->children[i]->type == AST_CASE) {
                    char lbl[32];
                    snprintf(lbl, sizeof(lbl), "case_%lld",
                             (long long)body->children[i]->data.case_stmt.value);
                    case_blocks[case_idx] = builder_new_block(b, lbl);
                    case_vals[case_idx] = body->children[i]->data.case_stmt.value;
                    case_idx++;
                } else if (body->children[i]->type == AST_DEFAULT) {
                    default_block = builder_new_block(b, "default");
                }
            }

            /* Emit IR_SWITCH instruction */
            IRInstr *sw = ir_instr_new(IR_SWITCH, stmt->line);
            sw->src1 = switch_val;
            sw->case_count = num_cases;
            sw->cases = (IRSwitchCase *)ir_alloc(num_cases * sizeof(IRSwitchCase));
            for (int c = 0; c < num_cases; c++) {
                sw->cases[c].value = case_vals[c];
                sw->cases[c].target = case_blocks[c];
            }
            sw->default_target = has_default ? default_block : exit_block;
            builder_emit(b, sw);

            /* Third pass: emit code for each case body */
            case_idx = 0;
            for (size_t i = 0; i < body->children_count; i++) {
                if (body->children[i]->type == AST_CASE) {
                    builder_set_block(b, case_blocks[case_idx]);
                    case_idx++;
                } else if (body->children[i]->type == AST_DEFAULT) {
                    builder_set_block(b, default_block);
                } else {
                    ir_lower_stmt(b, body->children[i]);
                }
            }

            /* Ensure last case falls through to exit */
            builder_ensure_terminator(b, exit_block);

            free(case_blocks);
            free(case_vals);
        }

        b->break_depth--;
        builder_set_block(b, exit_block);
        break;
    }

    case AST_BREAK: {
        if (b->break_depth > 0) {
            IRInstr *jmp = ir_instr_new(IR_JUMP, stmt->line);
            jmp->src1 = ir_op_label(b->break_targets[b->break_depth - 1]);
            builder_emit(b, jmp);
        }
        break;
    }

    case AST_CONTINUE: {
        if (b->continue_depth > 0) {
            IRInstr *jmp = ir_instr_new(IR_JUMP, stmt->line);
            jmp->src1 = ir_op_label(b->continue_targets[b->continue_depth - 1]);
            builder_emit(b, jmp);
        }
        break;
    }

    case AST_GOTO: {
        /* For goto, we need a label → block mapping.
           For now, emit a NOP placeholder — full goto support requires
           a pre-pass to collect all labels. */
        IRInstr *nop = ir_instr_new(IR_NOP, stmt->line);
        builder_emit(b, nop);
        break;
    }

    case AST_LABEL: {
        /* Create a new block for the label target */
        int label_block = builder_new_block(b, stmt->data.label_stmt.name);
        builder_ensure_terminator(b, label_block);
        builder_set_block(b, label_block);
        break;
    }

    case AST_CALL: {
        /* Statement-level function call (result discarded) */
        ir_lower_expr(b, stmt);
        break;
    }

    case AST_PRE_INC:
    case AST_PRE_DEC:
    case AST_POST_INC:
    case AST_POST_DEC: {
        ir_lower_expr(b, stmt);
        break;
    }

    case AST_ASSERT: {
        /* Lower assert condition: if condition is false, emit trap/abort.
           For IR purposes, lower as: if (!cond) { __builtin_trap(); } */
        if (stmt->data.assert_stmt.condition) {
            int trap_block = builder_new_block(b, "assert_fail");
            int ok_block = builder_new_block(b, "assert_ok");

            IROperand cond = ir_lower_expr(b, stmt->data.assert_stmt.condition);
            IRInstr *br = ir_instr_new(IR_BRANCH, stmt->line);
            br->src1 = cond;
            br->src2 = ir_op_label(ok_block);
            br->false_target = trap_block;
            builder_emit(b, br);

            builder_set_block(b, trap_block);
            /* Emit a trap/unreachable */
            IRInstr *trap = ir_instr_new(IR_NOP, stmt->line);
            builder_emit(b, trap);
            /* Assert failure is unreachable in correct code, but we need a terminator */
            IRInstr *ret = ir_instr_new(IR_RET, stmt->line);
            builder_emit(b, ret);

            builder_set_block(b, ok_block);
        }
        break;
    }

    case AST_CASE:
    case AST_DEFAULT:
        /* Handled by switch lowering above */
        break;

    default:
        /* Expression statement or unsupported — try to lower as expression */
        ir_lower_expr(b, stmt);
        break;
    }
}

/* Lower a block of statements. */
static void ir_lower_block(IRBuilder *b, ASTNode *block) {
    if (!block) return;
    if (block->type == AST_BLOCK) {
        for (size_t i = 0; i < block->children_count; i++) {
            ir_lower_stmt(b, block->children[i]);
            /* If we just emitted a terminator, don't emit more instructions
               into a terminated block (create a new one for dead code) */
            IRBlock *cur = builder_current(b);
            if (cur->last && ir_is_terminator(cur->last->opcode) &&
                i + 1 < block->children_count) {
                /* Create a dead block for unreachable code */
                int dead = builder_new_block(b, "dead");
                builder_set_block(b, dead);
            }
        }
    } else {
        ir_lower_stmt(b, block);
    }
}

/* ================================================================== */
/* Build an IR function from an AST function node                     */
/* ================================================================== */

static IRFunction *ir_build_function(ASTNode *func_node) {
    if (!func_node || func_node->type != AST_FUNCTION) return NULL;
    if (!func_node->data.function.body) return NULL;  /* declaration only */

    IRFunction *func = ir_function_new(func_node->data.function.name,
                                        func_node->line);
    func->return_type = func_node->resolved_type;

    /* Set up parameters (skip void-only params from (void) prototypes) */
    int raw_param_count = (int)func_node->children_count;
    func->param_count = 0;
    if (raw_param_count > 0) {
        func->param_names = (char **)ir_alloc(raw_param_count * sizeof(char *));
        func->param_types = (Type **)ir_alloc(raw_param_count * sizeof(Type *));
        for (int i = 0; i < raw_param_count; i++) {
            ASTNode *param = func_node->children[i];
            /* Skip void-only params (no name) or params with void type */
            if (!param->data.var_decl.name) continue;
            if (param->resolved_type && param->resolved_type->kind == TYPE_VOID)
                continue;
            int idx = func->param_count++;
            func->param_names[idx] = ir_strdup(param->data.var_decl.name);
            func->param_types[idx] = param->resolved_type;

            /* Create vreg for each parameter */
            int vreg = ir_var_lookup(func, param->data.var_decl.name,
                                     param->resolved_type);
            /* Mark as parameter */
            for (int v = 0; v < func->var_count; v++) {
                if (func->vars[v].vreg == vreg) {
                    func->vars[v].is_param = 1;
                    break;
                }
            }
        }
    }

    /* Create the entry block */
    int entry = ir_new_block(func, "entry");
    func->entry_block = entry;

    /* Initialize the builder and lower the function body */
    IRBuilder builder;
    builder_init(&builder, func);

    ir_lower_block(&builder, func_node->data.function.body);

    /* Ensure the last block has a terminator (implicit return void) */
    IRBlock *last = builder_current(&builder);
    if (!last->last || !ir_is_terminator(last->last->opcode)) {
        IRInstr *ret = ir_instr_new(IR_RET, func_node->line);
        builder_emit(&builder, ret);
    }

    /* Build CFG edges from terminators */
    ir_build_cfg(func);

    return func;
}

/* ================================================================== */
/* Build IR program from AST program                                  */
/* ================================================================== */

IRProgram *ir_build_program(ASTNode *program, OptLevel level) {
    if (!program) return NULL;

    IRProgram *ir = (IRProgram *)ir_alloc(sizeof(IRProgram));
    ir->func_count = 0;
    ir->func_capacity = 16;
    ir->functions = (IRFunction **)ir_alloc(ir->func_capacity * sizeof(IRFunction *));
    ir->global_count = 0;
    ir->global_capacity = 16;
    ir->globals = ir_alloc(ir->global_capacity * sizeof(*ir->globals));
    ir->string_count = 0;
    ir->string_capacity = 16;
    ir->strings = ir_alloc(ir->string_capacity * sizeof(*ir->strings));

    for (size_t i = 0; i < program->children_count; i++) {
        ASTNode *child = program->children[i];

        if (child->type == AST_FUNCTION) {
            if (!child->data.function.body) continue;  /* skip declarations */

            IRFunction *func = ir_build_function(child);
            if (func) {
                if (ir->func_count >= ir->func_capacity) {
                    ir->func_capacity *= 2;
                    ir->functions = (IRFunction **)realloc(
                        ir->functions,
                        ir->func_capacity * sizeof(IRFunction *));
                }
                ir->functions[ir->func_count++] = func;
            }
        } else if (child->type == AST_VAR_DECL) {
            /* Global variable */
            if (ir->global_count >= ir->global_capacity) {
                ir->global_capacity *= 2;
                ir->globals = realloc(ir->globals,
                    ir->global_capacity * sizeof(*ir->globals));
            }
            ir->globals[ir->global_count].name = ir_strdup(child->data.var_decl.name);
            ir->globals[ir->global_count].type = child->resolved_type;
            if (child->data.var_decl.initializer &&
                child->data.var_decl.initializer->type == AST_INTEGER) {
                ir->globals[ir->global_count].init_value =
                    child->data.var_decl.initializer->data.integer.value;
                ir->globals[ir->global_count].has_init = 1;
            } else {
                ir->globals[ir->global_count].init_value = 0;
                ir->globals[ir->global_count].has_init = 0;
            }
            ir->global_count++;
        }
    }

    (void)level;  /* optimization level used by future IR-level passes */
    return ir;
}

/* ================================================================== */
/* SSA Construction                                                   */
/*                                                                    */
/* Implements the standard SSA construction algorithm:                 */
/*   1. Compute dominator tree (Cooper-Harvey-Kennedy iterative)      */
/*   2. Compute dominance frontiers                                   */
/*   3. Insert phi-functions at iterated dominance frontiers          */
/*   4. Rename variables (DFS on dominator tree)                      */
/* ================================================================== */

/* ------------------------------------------------------------------ */
/* Reverse postorder (RPO) computation via DFS                        */
/* ------------------------------------------------------------------ */

static void rpo_dfs(IRFunction *func, int block_id, int *visited,
                    int *rpo, int *rpo_idx) {
    if (block_id < 0 || block_id >= func->block_count) return;
    if (visited[block_id]) return;
    visited[block_id] = 1;

    IRBlock *b = func->blocks[block_id];
    for (int i = 0; i < b->succ_count; i++) {
        rpo_dfs(func, b->succs[i], visited, rpo, rpo_idx);
    }
    rpo[--(*rpo_idx)] = block_id;
}

/* Returns an array of block IDs in reverse postorder.
 * *out_count is set to the number of reachable blocks.
 * Caller must free the returned array. */
static int *compute_rpo(IRFunction *func, int *out_count) {
    int n = func->block_count;
    int *visited = (int *)calloc(n, sizeof(int));
    int *rpo = (int *)calloc(n, sizeof(int));
    int rpo_idx = n;

    rpo_dfs(func, func->entry_block, visited, rpo, &rpo_idx);
    free(visited);

    int count = n - rpo_idx;
    if (rpo_idx > 0) {
        memmove(rpo, rpo + rpo_idx, count * sizeof(int));
    }
    *out_count = count;
    return rpo;
}

/* ------------------------------------------------------------------ */
/* Dominator tree computation (Cooper, Harvey, Kennedy 2001)          */
/* ------------------------------------------------------------------ */

static int dom_intersect(int *doms, int *rpo_num, int b1, int b2) {
    int finger1 = b1;
    int finger2 = b2;
    while (finger1 != finger2) {
        while (rpo_num[finger1] > rpo_num[finger2])
            finger1 = doms[finger1];
        while (rpo_num[finger2] > rpo_num[finger1])
            finger2 = doms[finger2];
    }
    return finger1;
}

void ir_compute_dominators(IRFunction *func) {
    int n = func->block_count;
    if (n == 0) return;

    /* Compute reverse postorder */
    int rpo_count;
    int *rpo = compute_rpo(func, &rpo_count);

    /* Assign RPO numbers (for intersect comparisons) */
    int *rpo_num = (int *)malloc(n * sizeof(int));
    for (int i = 0; i < n; i++) rpo_num[i] = -1;  /* unreachable */
    for (int i = 0; i < rpo_count; i++) rpo_num[rpo[i]] = i;

    /* Initialize idom array: undefined = -1, entry = self */
    int *doms = (int *)malloc(n * sizeof(int));
    for (int i = 0; i < n; i++) doms[i] = -1;
    doms[func->entry_block] = func->entry_block;

    /* Iterative dominator computation */
    int changed = 1;
    while (changed) {
        changed = 0;
        for (int i = 0; i < rpo_count; i++) {
            int b = rpo[i];
            if (b == func->entry_block) continue;

            IRBlock *block = func->blocks[b];

            /* Find first processed predecessor */
            int new_idom = -1;
            for (int p = 0; p < block->pred_count; p++) {
                if (doms[block->preds[p]] != -1) {
                    new_idom = block->preds[p];
                    break;
                }
            }
            if (new_idom == -1) continue;  /* unreachable */

            /* Intersect with other processed predecessors */
            for (int p = 0; p < block->pred_count; p++) {
                int pred = block->preds[p];
                if (pred == new_idom) continue;
                if (doms[pred] != -1) {
                    new_idom = dom_intersect(doms, rpo_num, pred, new_idom);
                }
            }

            if (doms[b] != new_idom) {
                doms[b] = new_idom;
                changed = 1;
            }
        }
    }

    /* Store results in blocks */
    for (int i = 0; i < n; i++) {
        func->blocks[i]->idom = doms[i];
    }
    func->blocks[func->entry_block]->idom = -1;  /* entry has no idom */

    free(doms);
    free(rpo);
    free(rpo_num);
}

/* ------------------------------------------------------------------ */
/* Dominance frontier computation                                     */
/* ------------------------------------------------------------------ */

void ir_compute_dom_frontiers(IRFunction *func) {
    int n = func->block_count;

    /* Reset existing frontiers */
    for (int i = 0; i < n; i++) {
        free(func->blocks[i]->dom_frontier);
        func->blocks[i]->dom_frontier = NULL;
        func->blocks[i]->dom_frontier_count = 0;
    }

    /* Allocate dynamic arrays for frontiers */
    int *df_cap = (int *)calloc(n, sizeof(int));
    for (int i = 0; i < n; i++) {
        df_cap[i] = 4;
        func->blocks[i]->dom_frontier = (int *)malloc(4 * sizeof(int));
    }

    /* Standard DF computation:
     * For each join point b (pred_count >= 2), walk up the dominator tree
     * from each predecessor until we reach b's immediate dominator,
     * adding b to the dominance frontier of each block along the way. */
    for (int b = 0; b < n; b++) {
        IRBlock *block = func->blocks[b];
        if (block->pred_count < 2) continue;

        for (int p = 0; p < block->pred_count; p++) {
            int runner = block->preds[p];
            while (runner >= 0 && runner != block->idom) {
                IRBlock *rb = func->blocks[runner];

                /* Check for duplicate */
                int found = 0;
                for (int d = 0; d < rb->dom_frontier_count; d++) {
                    if (rb->dom_frontier[d] == b) { found = 1; break; }
                }
                if (!found) {
                    if (rb->dom_frontier_count >= df_cap[runner]) {
                        df_cap[runner] *= 2;
                        rb->dom_frontier = (int *)realloc(
                            rb->dom_frontier, df_cap[runner] * sizeof(int));
                    }
                    rb->dom_frontier[rb->dom_frontier_count++] = b;
                }
                runner = func->blocks[runner]->idom;
            }
        }
    }

    free(df_cap);
}

/* ------------------------------------------------------------------ */
/* Phi-function insertion at iterated dominance frontiers             */
/* ------------------------------------------------------------------ */

static void ir_ssa_insert_phis(IRFunction *func) {
    int n = func->block_count;
    int nv = func->var_count;
    if (nv == 0 || n == 0) return;

    /* Build reverse map: canonical vreg → variable index */
    int max_vreg = func->next_vreg;
    int *var_of_vreg = (int *)malloc(max_vreg * sizeof(int));
    for (int i = 0; i < max_vreg; i++) var_of_vreg[i] = -1;
    for (int i = 0; i < nv; i++) var_of_vreg[func->vars[i].vreg] = i;

    /* For each variable, find def blocks */
    int **def_blocks = (int **)calloc(nv, sizeof(int *));
    int *def_count = (int *)calloc(nv, sizeof(int));
    int *def_cap = (int *)calloc(nv, sizeof(int));
    for (int i = 0; i < nv; i++) {
        def_cap[i] = 4;
        def_blocks[i] = (int *)malloc(4 * sizeof(int));
    }

    for (int b = 0; b < n; b++) {
        for (IRInstr *instr = func->blocks[b]->first; instr; instr = instr->next) {
            if (instr->dst.kind == IR_OPERAND_VREG &&
                instr->dst.val.vreg < max_vreg) {
                int vi = var_of_vreg[instr->dst.val.vreg];
                if (vi >= 0) {
                    /* Add block b to def_blocks[vi] if not already there */
                    int found = 0;
                    for (int d = 0; d < def_count[vi]; d++) {
                        if (def_blocks[vi][d] == b) { found = 1; break; }
                    }
                    if (!found) {
                        if (def_count[vi] >= def_cap[vi]) {
                            def_cap[vi] *= 2;
                            def_blocks[vi] = (int *)realloc(
                                def_blocks[vi], def_cap[vi] * sizeof(int));
                        }
                        def_blocks[vi][def_count[vi]++] = b;
                    }
                }
            }
        }
    }

    /* Insert phi-functions using iterated dominance frontier */
    int *has_phi = (int *)calloc(nv * n, sizeof(int));
    int *worklist = (int *)malloc(n * sizeof(int));
    int *in_worklist = (int *)calloc(n, sizeof(int));

    for (int v = 0; v < nv; v++) {
        /* Initialize worklist with def blocks for variable v */
        int wl_count = 0;
        memset(in_worklist, 0, n * sizeof(int));
        for (int d = 0; d < def_count[v]; d++) {
            worklist[wl_count++] = def_blocks[v][d];
            in_worklist[def_blocks[v][d]] = 1;
        }

        while (wl_count > 0) {
            int d = worklist[--wl_count];
            in_worklist[d] = 0;

            IRBlock *db = func->blocks[d];
            for (int f = 0; f < db->dom_frontier_count; f++) {
                int y = db->dom_frontier[f];
                if (has_phi[v * n + y]) continue;
                has_phi[v * n + y] = 1;

                /* Insert phi at top of block y */
                IRBlock *yb = func->blocks[y];
                IRInstr *phi = ir_instr_new(IR_PHI, 0);
                phi->ssa_var = v;
                phi->dst = ir_op_vreg(func->vars[v].vreg, func->vars[v].type);
                phi->phi_count = yb->pred_count;
                phi->phi_args = (IROperand *)ir_alloc(
                    yb->pred_count * sizeof(IROperand));
                phi->phi_preds = (int *)ir_alloc(
                    yb->pred_count * sizeof(int));
                for (int p = 0; p < yb->pred_count; p++) {
                    phi->phi_args[p] = ir_op_none();  /* filled during rename */
                    phi->phi_preds[p] = yb->preds[p];
                }

                /* Prepend to block */
                phi->next = yb->first;
                yb->first = phi;
                if (!yb->last) yb->last = phi;
                yb->instr_count++;

                /* If y is not already a def site, add to worklist */
                if (!in_worklist[y]) {
                    worklist[wl_count++] = y;
                    in_worklist[y] = 1;
                }
            }
        }
    }

    free(has_phi);
    free(worklist);
    free(in_worklist);
    for (int i = 0; i < nv; i++) free(def_blocks[i]);
    free(def_blocks);
    free(def_count);
    free(def_cap);
    free(var_of_vreg);
}

/* ------------------------------------------------------------------ */
/* Variable renaming (DFS on dominator tree)                          */
/* ------------------------------------------------------------------ */

typedef struct {
    int *stack;
    int top;
    int cap;
} SSAVarStack;

static void ssa_stack_init(SSAVarStack *s) {
    s->cap = 8;
    s->top = 0;
    s->stack = (int *)malloc(s->cap * sizeof(int));
}

static void ssa_stack_free(SSAVarStack *s) {
    free(s->stack);
}

static void ssa_push(SSAVarStack *s, int vreg) {
    if (s->top >= s->cap) {
        s->cap *= 2;
        s->stack = (int *)realloc(s->stack, s->cap * sizeof(int));
    }
    s->stack[s->top++] = vreg;
}

static int ssa_top(SSAVarStack *s) {
    if (s->top <= 0) return -1;
    return s->stack[s->top - 1];
}

static int ssa_new_name(SSAVarStack *s, IRFunction *func) {
    int vreg = func->next_vreg++;
    ssa_push(s, vreg);
    return vreg;
}

/* Build dominator tree children lists.
 * Caller must free dom_children[i] for each i, plus dom_children and counts. */
static void build_dom_children(IRFunction *func, int ***out_children,
                               int **out_counts) {
    int n = func->block_count;
    int **children = (int **)calloc(n, sizeof(int *));
    int *counts = (int *)calloc(n, sizeof(int));
    int *caps = (int *)calloc(n, sizeof(int));
    for (int i = 0; i < n; i++) {
        caps[i] = 4;
        children[i] = (int *)malloc(4 * sizeof(int));
    }

    for (int i = 0; i < n; i++) {
        int idom = func->blocks[i]->idom;
        if (idom >= 0 && idom != i) {
            if (counts[idom] >= caps[idom]) {
                caps[idom] *= 2;
                children[idom] = (int *)realloc(
                    children[idom], caps[idom] * sizeof(int));
            }
            children[idom][counts[idom]++] = i;
        }
    }

    free(caps);
    *out_children = children;
    *out_counts = counts;
}

static void ssa_rename_block(IRFunction *func, int block_id,
                             SSAVarStack *stacks, int *var_of_vreg,
                             int max_orig_vreg, int **dom_children,
                             int *dom_child_counts) {
    IRBlock *block = func->blocks[block_id];
    int nv = func->var_count;

    /* Track how many versions we push per variable for later cleanup */
    int *local_pushes = (int *)calloc(nv, sizeof(int));

    /* 1. Process PHI definitions: rename dst, push new version */
    for (IRInstr *instr = block->first;
         instr && instr->opcode == IR_PHI;
         instr = instr->next) {
        int vi = instr->ssa_var;
        if (vi >= 0 && vi < nv) {
            int new_vreg = ssa_new_name(&stacks[vi], func);
            instr->dst.val.vreg = new_vreg;
            local_pushes[vi]++;
        }
    }

    /* 2. Process non-PHI instructions: rename uses then defs */
    for (IRInstr *instr = block->first; instr; instr = instr->next) {
        if (instr->opcode == IR_PHI) continue;

        /* Rename uses in src1 */
        if (instr->src1.kind == IR_OPERAND_VREG &&
            instr->src1.val.vreg < max_orig_vreg) {
            int vi = var_of_vreg[instr->src1.val.vreg];
            if (vi >= 0) {
                int cur = ssa_top(&stacks[vi]);
                if (cur >= 0) instr->src1.val.vreg = cur;
            }
        }

        /* Rename uses in src2 */
        if (instr->src2.kind == IR_OPERAND_VREG &&
            instr->src2.val.vreg < max_orig_vreg) {
            int vi = var_of_vreg[instr->src2.val.vreg];
            if (vi >= 0) {
                int cur = ssa_top(&stacks[vi]);
                if (cur >= 0) instr->src2.val.vreg = cur;
            }
        }

        /* Rename definition in dst */
        if (instr->dst.kind == IR_OPERAND_VREG &&
            instr->dst.val.vreg < max_orig_vreg) {
            int vi = var_of_vreg[instr->dst.val.vreg];
            if (vi >= 0) {
                int new_vreg = ssa_new_name(&stacks[vi], func);
                instr->dst.val.vreg = new_vreg;
                local_pushes[vi]++;
            }
        }
    }

    /* 3. Fill phi arguments in successor blocks */
    for (int s = 0; s < block->succ_count; s++) {
        int succ_id = block->succs[s];
        IRBlock *succ = func->blocks[succ_id];

        /* Determine our predecessor index in the successor */
        int pred_idx = -1;
        for (int p = 0; p < succ->pred_count; p++) {
            if (succ->preds[p] == block_id) { pred_idx = p; break; }
        }
        if (pred_idx < 0) continue;

        /* Fill matching phi arguments */
        for (IRInstr *phi = succ->first;
             phi && phi->opcode == IR_PHI;
             phi = phi->next) {
            int vi = phi->ssa_var;
            if (vi >= 0 && vi < nv && pred_idx < phi->phi_count) {
                int cur = ssa_top(&stacks[vi]);
                if (cur >= 0) {
                    phi->phi_args[pred_idx] =
                        ir_op_vreg(cur, func->vars[vi].type);
                } else {
                    /* Variable undefined along this path — use 0 (undef) */
                    phi->phi_args[pred_idx] = ir_op_imm_int(0);
                }
            }
        }
    }

    /* 4. Recurse into dominator tree children */
    for (int c = 0; c < dom_child_counts[block_id]; c++) {
        ssa_rename_block(func, dom_children[block_id][c],
                         stacks, var_of_vreg, max_orig_vreg,
                         dom_children, dom_child_counts);
    }

    /* 5. Pop all versions pushed in this block */
    for (int v = 0; v < nv; v++) {
        stacks[v].top -= local_pushes[v];
    }

    free(local_pushes);
}

static void ir_ssa_rename(IRFunction *func) {
    int nv = func->var_count;
    if (nv == 0) return;

    /* Save the pre-SSA vreg count to know which vregs are "original" */
    int max_orig_vreg = func->next_vreg;

    /* Build reverse map: canonical vreg → variable index */
    int *var_of_vreg = (int *)malloc(max_orig_vreg * sizeof(int));
    for (int i = 0; i < max_orig_vreg; i++) var_of_vreg[i] = -1;
    for (int i = 0; i < nv; i++) var_of_vreg[func->vars[i].vreg] = i;

    /* Build dominator tree children lists */
    int **dom_children;
    int *dom_child_counts;
    build_dom_children(func, &dom_children, &dom_child_counts);

    /* Initialize per-variable stacks */
    SSAVarStack *stacks = (SSAVarStack *)malloc(nv * sizeof(SSAVarStack));
    for (int v = 0; v < nv; v++) {
        ssa_stack_init(&stacks[v]);
    }

    /* Push initial versions for parameters (implicitly defined at entry) */
    func->ssa_param_vregs = (int *)malloc(nv * sizeof(int));
    for (int v = 0; v < nv; v++) {
        func->ssa_param_vregs[v] = -1;
        if (func->vars[v].is_param) {
            int entry_vreg = func->next_vreg++;
            ssa_push(&stacks[v], entry_vreg);
            func->ssa_param_vregs[v] = entry_vreg;
        }
    }

    /* Run DFS rename starting from entry block */
    ssa_rename_block(func, func->entry_block, stacks, var_of_vreg,
                     max_orig_vreg, dom_children, dom_child_counts);

    /* Cleanup */
    for (int v = 0; v < nv; v++) ssa_stack_free(&stacks[v]);
    free(stacks);
    for (int i = 0; i < func->block_count; i++) free(dom_children[i]);
    free(dom_children);
    free(dom_child_counts);
    free(var_of_vreg);
}

/* ------------------------------------------------------------------ */
/* SSA construction entry point                                       */
/* ------------------------------------------------------------------ */

void ir_ssa_construct(IRFunction *func) {
    if (!func || func->block_count == 0) return;
    if (func->var_count == 0) { func->is_ssa = 1; return; }

    /* Step 1: Compute dominator tree */
    ir_compute_dominators(func);

    /* Step 2: Compute dominance frontiers */
    ir_compute_dom_frontiers(func);

    /* Step 3: Insert phi-functions */
    ir_ssa_insert_phis(func);

    /* Step 4: Rename variables */
    ir_ssa_rename(func);

    func->is_ssa = 1;
}

void ir_ssa_construct_program(IRProgram *prog) {
    if (!prog) return;
    for (int i = 0; i < prog->func_count; i++) {
        ir_ssa_construct(prog->functions[i]);
    }
}

/* ------------------------------------------------------------------ */
/* SSA validation                                                     */
/* ------------------------------------------------------------------ */

int ir_ssa_validate(IRFunction *func) {
    if (!func || !func->is_ssa) return 0;

    int max_vreg = func->next_vreg;
    int *def_count = (int *)calloc(max_vreg, sizeof(int));
    int valid = 1;

    /* Count definitions of each vreg */
    for (int b = 0; b < func->block_count; b++) {
        for (IRInstr *instr = func->blocks[b]->first; instr; instr = instr->next) {
            if (instr->dst.kind == IR_OPERAND_VREG) {
                int v = instr->dst.val.vreg;
                if (v >= 0 && v < max_vreg) def_count[v]++;
            }
        }
    }

    /* Check single-definition property */
    for (int v = 0; v < max_vreg; v++) {
        if (def_count[v] > 1) {
            fprintf(stderr, "SSA violation: t%d defined %d times\n",
                    v, def_count[v]);
            valid = 0;
        }
    }

    /* Check that every used vreg is defined (or is a parameter entry vreg) */
    int *defined = (int *)calloc(max_vreg, sizeof(int));
    for (int b = 0; b < func->block_count; b++) {
        for (IRInstr *instr = func->blocks[b]->first; instr; instr = instr->next) {
            if (instr->dst.kind == IR_OPERAND_VREG) {
                int v = instr->dst.val.vreg;
                if (v >= 0 && v < max_vreg) defined[v] = 1;
            }
        }
    }
    /* Mark parameter entry vregs as defined (implicit defs at function entry) */
    if (func->ssa_param_vregs) {
        for (int v = 0; v < func->var_count; v++) {
            int pv = func->ssa_param_vregs[v];
            if (pv >= 0 && pv < max_vreg) defined[pv] = 1;
        }
    }

    for (int b = 0; b < func->block_count; b++) {
        for (IRInstr *instr = func->blocks[b]->first; instr; instr = instr->next) {
            if (instr->src1.kind == IR_OPERAND_VREG) {
                int v = instr->src1.val.vreg;
                if (v >= 0 && v < max_vreg && !defined[v]) {
                    fprintf(stderr,
                        "SSA violation: t%d used but not defined (bb%d)\n",
                        v, b);
                    valid = 0;
                }
            }
            if (instr->src2.kind == IR_OPERAND_VREG) {
                int v = instr->src2.val.vreg;
                if (v >= 0 && v < max_vreg && !defined[v]) {
                    fprintf(stderr,
                        "SSA violation: t%d used but not defined (bb%d)\n",
                        v, b);
                    valid = 0;
                }
            }
            if (instr->opcode == IR_PHI) {
                for (int p = 0; p < instr->phi_count; p++) {
                    if (instr->phi_args[p].kind == IR_OPERAND_VREG) {
                        int v = instr->phi_args[p].val.vreg;
                        if (v >= 0 && v < max_vreg && !defined[v]) {
                            fprintf(stderr,
                                "SSA violation: PHI arg t%d not defined "
                                "(bb%d pred bb%d)\n",
                                v, b, instr->phi_preds[p]);
                            valid = 0;
                        }
                    }
                }
            }
        }
    }

    /* Check PHI node consistency: pred count matches block pred count */
    for (int b = 0; b < func->block_count; b++) {
        IRBlock *block = func->blocks[b];
        for (IRInstr *instr = block->first;
             instr && instr->opcode == IR_PHI;
             instr = instr->next) {
            if (instr->phi_count != block->pred_count) {
                fprintf(stderr,
                    "SSA violation: PHI in bb%d has %d args but block has "
                    "%d preds\n", b, instr->phi_count, block->pred_count);
                valid = 0;
            }
        }
    }

    free(def_count);
    free(defined);
    return valid;
}

/* ================================================================== */
/* Analysis Passes                                                    */
/*                                                                    */
/* 1. Liveness analysis (backward dataflow)                           */
/* 2. Reaching definitions (forward dataflow)                         */
/* 3. Loop detection (back edges + natural loop bodies)               */
/* ================================================================== */

/* ------------------------------------------------------------------ */
/* Bitset helpers                                                     */
/* ------------------------------------------------------------------ */

static int bitset_words(int nbits) {
    return (nbits + 31) / 32;
}

static int *bitset_alloc(int words) {
    return (int *)calloc(words, sizeof(int));
}

static void bitset_set(int *bs, int bit) {
    bs[bit >> 5] |= (1 << (bit & 31));
}

static int bitset_test(int *bs, int bit) {
    return (bs[bit >> 5] >> (bit & 31)) & 1;
}

/* dst = dst ∪ src. Returns 1 if dst changed. */
static int bitset_union(int *dst, const int *src, int words) {
    int changed = 0;
    for (int i = 0; i < words; i++) {
        int old = dst[i];
        dst[i] |= src[i];
        if (dst[i] != old) changed = 1;
    }
    return changed;
}

/* dst = src1 - src2 (set difference: src1 & ~src2) */
static void bitset_diff(int *dst, const int *src1, const int *src2, int words) {
    for (int i = 0; i < words; i++) {
        dst[i] = src1[i] & ~src2[i];
    }
}

/* Count number of set bits. */
static int bitset_popcount(const int *bs, int words) {
    int count = 0;
    for (int i = 0; i < words; i++) {
        unsigned int v = (unsigned int)bs[i];
        /* Brian Kernighan's algorithm */
        while (v) { v &= v - 1; count++; }
    }
    return count;
}

/* ------------------------------------------------------------------ */
/* Collect vreg uses from an operand                                 */
/* ------------------------------------------------------------------ */

static void collect_vreg_use(IROperand *op, int *use_set, int *def_set,
                             int words) {
    if (op->kind == IR_OPERAND_VREG) {
        int v = op->val.vreg;
        if (v >= 0 && v < words * 32 && !bitset_test(def_set, v)) {
            bitset_set(use_set, v);
        }
    }
}

/* ------------------------------------------------------------------ */
/* 1. Liveness Analysis                                               */
/* ------------------------------------------------------------------ */

void ir_compute_def_use(IRFunction *func) {
    int n = func->block_count;
    int nv = func->next_vreg;
    int words = bitset_words(nv);

    for (int b = 0; b < n; b++) {
        IRBlock *block = func->blocks[b];

        /* Allocate or clear bitsets */
        free(block->def);
        free(block->use);
        block->def = bitset_alloc(words);
        block->use = bitset_alloc(words);

        /* Scan instructions forward: use = used-before-defined */
        for (IRInstr *instr = block->first; instr; instr = instr->next) {
            /* PHI nodes: arguments are uses from predecessor blocks,
               not from this block. Skip src1/src2 for PHI. */
            if (instr->opcode == IR_PHI) {
                /* PHI args are considered uses in the corresponding
                   predecessor blocks, not in this block.
                   Only the destination is a def here. */
            } else {
                /* Collect uses in src1, src2 */
                collect_vreg_use(&instr->src1, block->use, block->def, words);
                collect_vreg_use(&instr->src2, block->use, block->def, words);

                /* For BRANCH: src1 is a use */
                /* For CALL: src2 is arg count (imm), already handled */

                /* PHI args in successor blocks reference vregs —
                   handled when we process successor blocks' phis below */
            }

            /* Collect def in dst */
            if (instr->dst.kind == IR_OPERAND_VREG) {
                int v = instr->dst.val.vreg;
                if (v >= 0 && v < nv) {
                    bitset_set(block->def, v);
                }
            }
        }
    }

    /* For PHI nodes in successor blocks: the phi argument corresponding to
       predecessor block B is a use in block B. */
    for (int b = 0; b < n; b++) {
        IRBlock *block = func->blocks[b];
        for (int s = 0; s < block->succ_count; s++) {
            IRBlock *succ = func->blocks[block->succs[s]];

            /* Find our predecessor index in the successor */
            int pred_idx = -1;
            for (int p = 0; p < succ->pred_count; p++) {
                if (succ->preds[p] == b) { pred_idx = p; break; }
            }
            if (pred_idx < 0) continue;

            /* Add phi argument vregs as uses in this block */
            for (IRInstr *phi = succ->first;
                 phi && phi->opcode == IR_PHI;
                 phi = phi->next) {
                if (pred_idx < phi->phi_count) {
                    collect_vreg_use(&phi->phi_args[pred_idx],
                                     block->use, block->def, words);
                }
            }
        }
    }
}

void ir_compute_liveness(IRFunction *func) {
    int n = func->block_count;
    int nv = func->next_vreg;
    int words = bitset_words(nv);
    if (nv == 0 || n == 0) return;

    /* Ensure def/use sets exist */
    if (!func->blocks[0]->def) {
        ir_compute_def_use(func);
    }

    /* Allocate live_in / live_out bitsets */
    for (int b = 0; b < n; b++) {
        IRBlock *block = func->blocks[b];
        free(block->live_in);
        free(block->live_out);
        block->live_in = bitset_alloc(words);
        block->live_out = bitset_alloc(words);
    }

    /* Iterative backward dataflow to fixed point */
    int *temp = bitset_alloc(words);
    int changed = 1;
    while (changed) {
        changed = 0;

        /* Process blocks in reverse order (approximate reverse postorder) */
        for (int b = n - 1; b >= 0; b--) {
            IRBlock *block = func->blocks[b];

            /* live_out[B] = ∪ live_in[S] for all successors S */
            for (int s = 0; s < block->succ_count; s++) {
                IRBlock *succ = func->blocks[block->succs[s]];
                if (bitset_union(block->live_out, succ->live_in, words))
                    changed = 1;
            }

            /* live_in[B] = use[B] ∪ (live_out[B] - def[B]) */
            bitset_diff(temp, block->live_out, block->def, words);
            bitset_union(temp, block->use, words);

            /* Check if live_in changed */
            for (int w = 0; w < words; w++) {
                if (temp[w] != block->live_in[w]) {
                    changed = 1;
                    break;
                }
            }
            /* Copy temp → live_in */
            for (int w = 0; w < words; w++) {
                block->live_in[w] = temp[w];
            }

            /* Reset temp */
            memset(temp, 0, words * sizeof(int));
        }
    }
    free(temp);

    /* Mark parameter entry vregs as implicitly live at entry. */
    if (func->ssa_param_vregs) {
        IRBlock *entry = func->blocks[func->entry_block];
        for (int v = 0; v < func->var_count; v++) {
            int pv = func->ssa_param_vregs[v];
            if (pv >= 0 && pv < nv) {
                bitset_set(entry->def, pv);
            }
        }
    }
}

void ir_compute_liveness_program(IRProgram *prog) {
    if (!prog) return;
    for (int i = 0; i < prog->func_count; i++) {
        ir_compute_liveness(prog->functions[i]);
    }
}

int ir_is_live_in(IRBlock *block, int vreg, int bsw) {
    if (!block->live_in || vreg < 0 || vreg >= bsw * 32) return 0;
    return bitset_test(block->live_in, vreg);
}

int ir_is_live_out(IRBlock *block, int vreg, int bsw) {
    if (!block->live_out || vreg < 0 || vreg >= bsw * 32) return 0;
    return bitset_test(block->live_out, vreg);
}

/* ------------------------------------------------------------------ */
/* 2. Reaching Definitions                                            */
/* ------------------------------------------------------------------ */

IRReachDefs *ir_compute_reaching_defs(IRFunction *func) {
    int n = func->block_count;
    if (n == 0) return NULL;

    IRReachDefs *rd = (IRReachDefs *)ir_alloc(sizeof(IRReachDefs));
    rd->block_count = n;

    /* Phase 1: Collect all definition points */
    rd->def_count = 0;
    rd->def_capacity = 64;
    rd->defs = (IRDefPoint *)ir_alloc(rd->def_capacity * sizeof(IRDefPoint));

    /* Also build a mapping: vreg → list of def IDs (for kill set computation) */
    int max_vreg = func->next_vreg;
    int *vreg_first_def = (int *)malloc(max_vreg * sizeof(int));
    for (int i = 0; i < max_vreg; i++) vreg_first_def[i] = -1;

    /* Linked list of defs per vreg (implicit via next_def_of_vreg array) */
    int *next_def_of_vreg = NULL;  /* allocated after collecting all defs */

    for (int b = 0; b < n; b++) {
        int idx = 0;
        for (IRInstr *instr = func->blocks[b]->first; instr;
             instr = instr->next, idx++) {
            if (instr->dst.kind == IR_OPERAND_VREG) {
                if (rd->def_count >= rd->def_capacity) {
                    rd->def_capacity *= 2;
                    rd->defs = (IRDefPoint *)realloc(
                        rd->defs, rd->def_capacity * sizeof(IRDefPoint));
                }
                int def_id = rd->def_count;
                rd->defs[def_id].block_id = b;
                rd->defs[def_id].vreg = instr->dst.val.vreg;
                rd->defs[def_id].instr_idx = idx;
                rd->def_count++;
            }
        }
    }

    /* Build per-vreg def lists */
    next_def_of_vreg = (int *)malloc(rd->def_count * sizeof(int));
    for (int i = 0; i < rd->def_count; i++) next_def_of_vreg[i] = -1;
    for (int i = 0; i < max_vreg; i++) vreg_first_def[i] = -1;
    for (int d = rd->def_count - 1; d >= 0; d--) {
        int v = rd->defs[d].vreg;
        if (v >= 0 && v < max_vreg) {
            next_def_of_vreg[d] = vreg_first_def[v];
            vreg_first_def[v] = d;
        }
    }

    /* Phase 2: Compute gen/kill bitsets per block */
    int words = bitset_words(rd->def_count);
    rd->bitset_words = words;

    rd->gen = (int **)ir_alloc(n * sizeof(int *));
    rd->kill = (int **)ir_alloc(n * sizeof(int *));
    rd->reach_in = (int **)ir_alloc(n * sizeof(int *));
    rd->reach_out = (int **)ir_alloc(n * sizeof(int *));

    for (int b = 0; b < n; b++) {
        rd->gen[b] = bitset_alloc(words);
        rd->kill[b] = bitset_alloc(words);
        rd->reach_in[b] = bitset_alloc(words);
        rd->reach_out[b] = bitset_alloc(words);
    }

    /* For each def in block b:
       gen[b] += {d}  (last def of vreg in block wins)
       kill[b] += all other defs of the same vreg */
    for (int d = 0; d < rd->def_count; d++) {
        int b = rd->defs[d].block_id;
        int v = rd->defs[d].vreg;

        /* Add all defs of vreg v to kill[b] */
        for (int od = vreg_first_def[v]; od >= 0; od = next_def_of_vreg[od]) {
            if (od != d) {
                bitset_set(rd->kill[b], od);
            }
        }
    }

    /* gen: only the LAST def of each vreg in the block */
    for (int b = 0; b < n; b++) {
        /* Find last def of each vreg in this block */
        int *last_def = (int *)malloc(max_vreg * sizeof(int));
        for (int i = 0; i < max_vreg; i++) last_def[i] = -1;

        for (int d = 0; d < rd->def_count; d++) {
            if (rd->defs[d].block_id == b) {
                last_def[rd->defs[d].vreg] = d;
            }
        }

        for (int v = 0; v < max_vreg; v++) {
            if (last_def[v] >= 0) {
                bitset_set(rd->gen[b], last_def[v]);
            }
        }
        free(last_def);
    }

    /* Phase 3: Iterative forward dataflow to fixed point
       reach_out[B] = gen[B] ∪ (reach_in[B] - kill[B])
       reach_in[B]  = ∪ reach_out[P] for all predecessors P */
    int *temp = bitset_alloc(words);
    int changed = 1;
    while (changed) {
        changed = 0;
        for (int b = 0; b < n; b++) {
            IRBlock *block = func->blocks[b];

            /* reach_in[B] = ∪ reach_out[P] */
            int in_changed = 0;
            for (int p = 0; p < block->pred_count; p++) {
                if (bitset_union(rd->reach_in[b],
                                 rd->reach_out[block->preds[p]], words))
                    in_changed = 1;
            }

            /* reach_out[B] = gen[B] ∪ (reach_in[B] - kill[B]) */
            bitset_diff(temp, rd->reach_in[b], rd->kill[b], words);
            bitset_union(temp, rd->gen[b], words);

            /* Check if reach_out changed */
            for (int w = 0; w < words; w++) {
                if (temp[w] != rd->reach_out[b][w]) {
                    changed = 1;
                    break;
                }
            }

            for (int w = 0; w < words; w++) {
                rd->reach_out[b][w] = temp[w];
            }
            memset(temp, 0, words * sizeof(int));

            if (in_changed) changed = 1;
        }
    }
    free(temp);
    free(vreg_first_def);
    free(next_def_of_vreg);

    return rd;
}

void ir_free_reach_defs(IRReachDefs *rd) {
    if (!rd) return;
    free(rd->defs);
    for (int b = 0; b < rd->block_count; b++) {
        free(rd->gen[b]);
        free(rd->kill[b]);
        free(rd->reach_in[b]);
        free(rd->reach_out[b]);
    }
    free(rd->gen);
    free(rd->kill);
    free(rd->reach_in);
    free(rd->reach_out);
    free(rd);
}

/* ------------------------------------------------------------------ */
/* 3. Loop Detection                                                  */
/* ------------------------------------------------------------------ */

/* Check if block a dominates block b (walk the dominator tree from b). */
static int ir_dominates(IRFunction *func, int a, int b) {
    if (a == b) return 1;
    int cur = b;
    while (cur >= 0) {
        if (cur == a) return 1;
        int idom = func->blocks[cur]->idom;
        if (idom < 0 || idom == cur) break;
        cur = idom;
    }
    return 0;
}

/* Collect natural loop body: all blocks that can reach back_edge_src
 * without going through header, plus header itself. */
static void collect_loop_body(IRFunction *func, int header, int back_edge_src,
                              int **body, int *body_count, int *body_cap) {
    int n = func->block_count;
    int *in_loop = (int *)calloc(n, sizeof(int));
    int *stack = (int *)malloc(n * sizeof(int));
    int sp = 0;

    in_loop[header] = 1;
    (*body)[(*body_count)++] = header;

    if (back_edge_src != header) {
        in_loop[back_edge_src] = 1;
        if (*body_count >= *body_cap) {
            *body_cap *= 2;
            *body = (int *)realloc(*body, *body_cap * sizeof(int));
        }
        (*body)[(*body_count)++] = back_edge_src;
        stack[sp++] = back_edge_src;
    }

    /* DFS backwards from back_edge_src through predecessors */
    while (sp > 0) {
        int cur = stack[--sp];
        IRBlock *block = func->blocks[cur];
        for (int p = 0; p < block->pred_count; p++) {
            int pred = block->preds[p];
            if (!in_loop[pred]) {
                in_loop[pred] = 1;
                if (*body_count >= *body_cap) {
                    *body_cap *= 2;
                    *body = (int *)realloc(*body, *body_cap * sizeof(int));
                }
                (*body)[(*body_count)++] = pred;
                stack[sp++] = pred;
            }
        }
    }

    free(in_loop);
    free(stack);
}

IRLoopInfo *ir_detect_loops(IRFunction *func) {
    int n = func->block_count;
    if (n == 0) return NULL;

    /* Ensure dominator tree is computed */
    if (func->blocks[0]->idom == -1 && func->entry_block == 0 && n > 1) {
        ir_compute_dominators(func);
    }

    IRLoopInfo *li = (IRLoopInfo *)ir_alloc(sizeof(IRLoopInfo));
    li->loop_count = 0;
    li->loop_capacity = 8;
    li->loops = (IRLoop *)ir_alloc(li->loop_capacity * sizeof(IRLoop));

    /* Reset loop info on blocks */
    for (int b = 0; b < n; b++) {
        func->blocks[b]->loop_depth = 0;
        func->blocks[b]->loop_header = -1;
    }

    /* Find back edges: edge B → H where H dominates B */
    for (int b = 0; b < n; b++) {
        IRBlock *block = func->blocks[b];
        for (int s = 0; s < block->succ_count; s++) {
            int h = block->succs[s];
            if (ir_dominates(func, h, b)) {
                /* Back edge found: b → h */
                if (li->loop_count >= li->loop_capacity) {
                    li->loop_capacity *= 2;
                    li->loops = (IRLoop *)realloc(
                        li->loops, li->loop_capacity * sizeof(IRLoop));
                }

                IRLoop *loop = &li->loops[li->loop_count];
                loop->header = h;
                loop->back_edge_src = b;
                loop->depth = 0;  /* computed below */
                loop->body_count = 0;
                int body_cap = 8;
                loop->body = (int *)malloc(body_cap * sizeof(int));

                collect_loop_body(func, h, b,
                                  &loop->body, &loop->body_count, &body_cap);
                li->loop_count++;
            }
        }
    }

    /* Compute loop depths: a block's depth = number of loops it's in.
       Also set loop_header to the innermost loop's header. */
    for (int l = 0; l < li->loop_count; l++) {
        IRLoop *loop = &li->loops[l];
        for (int i = 0; i < loop->body_count; i++) {
            int b = loop->body[i];
            func->blocks[b]->loop_depth++;
        }
    }

    /* Set loop_header to the header of the innermost loop.
       The innermost loop is the one with the smallest body. */
    /* First, sort loops by body size descending so smaller (inner) loops
       overwrite larger (outer) loops' header assignment. */
    for (int i = 0; i < li->loop_count; i++) {
        for (int j = i + 1; j < li->loop_count; j++) {
            if (li->loops[j].body_count > li->loops[i].body_count) {
                IRLoop tmp = li->loops[i];
                li->loops[i] = li->loops[j];
                li->loops[j] = tmp;
            }
        }
    }

    for (int l = 0; l < li->loop_count; l++) {
        IRLoop *loop = &li->loops[l];
        for (int i = 0; i < loop->body_count; i++) {
            int b = loop->body[i];
            func->blocks[b]->loop_header = loop->header;
        }
    }

    /* Assign depth to each loop: depth = loop_depth of header block
       before this loop was counted (= nesting level).
       Simpler: depth = number of outer loops containing this header + 1. */
    for (int l = 0; l < li->loop_count; l++) {
        IRLoop *loop = &li->loops[l];
        int depth = 0;
        for (int l2 = 0; l2 < li->loop_count; l2++) {
            if (l2 == l) continue;
            IRLoop *outer = &li->loops[l2];
            /* Check if outer contains our header */
            for (int i = 0; i < outer->body_count; i++) {
                if (outer->body[i] == loop->header) {
                    depth++;
                    break;
                }
            }
        }
        loop->depth = depth + 1;
    }

    return li;
}

void ir_free_loop_info(IRLoopInfo *li) {
    if (!li) return;
    for (int i = 0; i < li->loop_count; i++) {
        free(li->loops[i].body);
    }
    free(li->loops);
    free(li);
}

/* ------------------------------------------------------------------ */
/* Combined analysis driver                                           */
/* ------------------------------------------------------------------ */

void ir_analyze_function(IRFunction *func) {
    if (!func || func->block_count == 0) return;

    /* Liveness analysis (depends on def/use) */
    ir_compute_liveness(func);

    /* Loop detection (depends on dominator tree, already computed in SSA) */
    IRLoopInfo *li = ir_detect_loops(func);
    ir_free_loop_info(li);  /* results stored on blocks */
}

void ir_analyze_program(IRProgram *prog) {
    if (!prog) return;
    for (int i = 0; i < prog->func_count; i++) {
        ir_analyze_function(prog->functions[i]);
    }
}

/* ================================================================== */
/* Optimization Pass: Sparse Conditional Constant Propagation (SCCP)  */
/* ================================================================== */

/*
 * SSA-based global constant propagation.  For each vreg, compute a
 * lattice value:
 *   TOP     = undefined (may become constant)
 *   CONST(c)= known constant integer c
 *   BOTTOM  = variable (not constant)
 *
 * Walk instructions in RPO.  When a vreg becomes CONST, fold all uses.
 * PHI nodes are resolved: if all arguments are the same constant, the
 * PHI produces that constant; if arguments disagree, it's BOTTOM.
 */

typedef enum { SCCP_TOP, SCCP_CONST, SCCP_BOTTOM } SCCPState;

typedef struct {
    SCCPState state;
    long long value;        /* valid only when state == SCCP_CONST */
} SCCPCell;

/* Try to get the constant value of an operand using lattice cells.
 * Returns 1 if op is a constant, filling *out_val. */
static int sccp_get_const(IROperand *op, SCCPCell *cells, int nv,
                          long long *out_val) {
    if (op->kind == IR_OPERAND_IMM_INT) {
        *out_val = op->val.imm_int;
        return 1;
    }
    if (op->kind == IR_OPERAND_VREG) {
        int v = op->val.vreg;
        if (v >= 0 && v < nv && cells[v].state == SCCP_CONST) {
            *out_val = cells[v].value;
            return 1;
        }
    }
    return 0;
}

/* Evaluate a binary operation on two constants. Returns 1 on success. */
static int sccp_eval_binop(IROpcode op, long long a, long long b,
                           long long *result) {
    switch (op) {
    case IR_ADD:    *result = a + b; return 1;
    case IR_SUB:    *result = a - b; return 1;
    case IR_MUL:    *result = a * b; return 1;
    case IR_DIV:    if (b == 0) return 0; *result = a / b; return 1;
    case IR_MOD:    if (b == 0) return 0; *result = a % b; return 1;
    case IR_AND:    *result = a & b; return 1;
    case IR_OR:     *result = a | b; return 1;
    case IR_XOR:    *result = a ^ b; return 1;
    case IR_SHL:    *result = a << b; return 1;
    case IR_SHR:    *result = a >> b; return 1;
    case IR_CMP_EQ: *result = (a == b); return 1;
    case IR_CMP_NE: *result = (a != b); return 1;
    case IR_CMP_LT: *result = (a < b); return 1;
    case IR_CMP_LE: *result = (a <= b); return 1;
    case IR_CMP_GT: *result = (a > b); return 1;
    case IR_CMP_GE: *result = (a >= b); return 1;
    default: return 0;
    }
}

/* Mark a cell as CONST(v) or BOTTOM.  Returns 1 if changed. */
static int sccp_set(SCCPCell *cells, int vreg, SCCPState state,
                    long long value) {
    SCCPCell *c = &cells[vreg];
    if (c->state == SCCP_BOTTOM) return 0;  /* already bottom, can't go up */
    if (state == SCCP_CONST) {
        if (c->state == SCCP_CONST && c->value == value) return 0;
        if (c->state == SCCP_CONST && c->value != value) {
            /* conflict → bottom */
            c->state = SCCP_BOTTOM;
            return 1;
        }
        /* TOP → CONST */
        c->state = SCCP_CONST;
        c->value = value;
        return 1;
    }
    /* BOTTOM */
    if (c->state != SCCP_BOTTOM) {
        c->state = SCCP_BOTTOM;
        return 1;
    }
    return 0;
}

void ir_sccp(IRFunction *func) {
    if (!func || func->block_count == 0) return;
    int nv = func->next_vreg;
    if (nv == 0) return;

    SCCPCell *cells = (SCCPCell *)ir_alloc(nv * sizeof(SCCPCell));
    /* All cells start as TOP */

    /* Mark parameter vregs as BOTTOM (unknown) */
    if (func->ssa_param_vregs) {
        for (int v = 0; v < func->var_count; v++) {
            int pv = func->ssa_param_vregs[v];
            if (pv >= 0 && pv < nv && func->vars[v].is_param) {
                cells[pv].state = SCCP_BOTTOM;
            }
        }
    }

    /* Iterative propagation to fixed point */
    int changed = 1;
    int iters = 0;
    while (changed && iters < 100) {
        changed = 0;
        iters++;

        for (int b = 0; b < func->block_count; b++) {
            IRBlock *block = func->blocks[b];
            for (IRInstr *instr = block->first; instr; instr = instr->next) {
                if (instr->dst.kind != IR_OPERAND_VREG) continue;
                int dst = instr->dst.val.vreg;
                if (dst < 0 || dst >= nv) continue;

                switch (instr->opcode) {
                case IR_CONST:
                    if (instr->src1.kind == IR_OPERAND_IMM_INT) {
                        changed |= sccp_set(cells, dst, SCCP_CONST,
                                            instr->src1.val.imm_int);
                    } else {
                        changed |= sccp_set(cells, dst, SCCP_BOTTOM, 0);
                    }
                    break;

                case IR_COPY: {
                    long long val;
                    if (sccp_get_const(&instr->src1, cells, nv, &val)) {
                        changed |= sccp_set(cells, dst, SCCP_CONST, val);
                    } else if (instr->src1.kind == IR_OPERAND_VREG) {
                        int sv = instr->src1.val.vreg;
                        if (sv >= 0 && sv < nv &&
                            cells[sv].state == SCCP_BOTTOM) {
                            changed |= sccp_set(cells, dst, SCCP_BOTTOM, 0);
                        }
                    } else {
                        changed |= sccp_set(cells, dst, SCCP_BOTTOM, 0);
                    }
                    break;
                }

                case IR_ADD: case IR_SUB: case IR_MUL: case IR_DIV:
                case IR_MOD: case IR_AND: case IR_OR:  case IR_XOR:
                case IR_SHL: case IR_SHR:
                case IR_CMP_EQ: case IR_CMP_NE: case IR_CMP_LT:
                case IR_CMP_LE: case IR_CMP_GT: case IR_CMP_GE: {
                    long long a, bv, result;
                    if (sccp_get_const(&instr->src1, cells, nv, &a) &&
                        sccp_get_const(&instr->src2, cells, nv, &bv) &&
                        sccp_eval_binop(instr->opcode, a, bv, &result)) {
                        changed |= sccp_set(cells, dst, SCCP_CONST, result);
                    } else {
                        /* If either operand is BOTTOM → BOTTOM */
                        int bot = 0;
                        if (instr->src1.kind == IR_OPERAND_VREG) {
                            int sv = instr->src1.val.vreg;
                            if (sv >= 0 && sv < nv &&
                                cells[sv].state == SCCP_BOTTOM)
                                bot = 1;
                        }
                        if (instr->src2.kind == IR_OPERAND_VREG) {
                            int sv = instr->src2.val.vreg;
                            if (sv >= 0 && sv < nv &&
                                cells[sv].state == SCCP_BOTTOM)
                                bot = 1;
                        }
                        /* Also if either operand is a non-vreg non-imm */
                        if (!bot && instr->src1.kind != IR_OPERAND_VREG &&
                            instr->src1.kind != IR_OPERAND_IMM_INT)
                            bot = 1;
                        if (!bot && instr->src2.kind != IR_OPERAND_VREG &&
                            instr->src2.kind != IR_OPERAND_IMM_INT)
                            bot = 1;
                        if (bot)
                            changed |= sccp_set(cells, dst, SCCP_BOTTOM, 0);
                    }
                    break;
                }

                case IR_NEG: {
                    long long val;
                    if (sccp_get_const(&instr->src1, cells, nv, &val)) {
                        changed |= sccp_set(cells, dst, SCCP_CONST, -val);
                    } else if (instr->src1.kind == IR_OPERAND_VREG) {
                        int sv = instr->src1.val.vreg;
                        if (sv >= 0 && sv < nv &&
                            cells[sv].state == SCCP_BOTTOM)
                            changed |= sccp_set(cells, dst, SCCP_BOTTOM, 0);
                    } else {
                        changed |= sccp_set(cells, dst, SCCP_BOTTOM, 0);
                    }
                    break;
                }

                case IR_NOT: {
                    long long val;
                    if (sccp_get_const(&instr->src1, cells, nv, &val)) {
                        changed |= sccp_set(cells, dst, SCCP_CONST, !val);
                    } else if (instr->src1.kind == IR_OPERAND_VREG) {
                        int sv = instr->src1.val.vreg;
                        if (sv >= 0 && sv < nv &&
                            cells[sv].state == SCCP_BOTTOM)
                            changed |= sccp_set(cells, dst, SCCP_BOTTOM, 0);
                    } else {
                        changed |= sccp_set(cells, dst, SCCP_BOTTOM, 0);
                    }
                    break;
                }

                case IR_BITNOT: {
                    long long val;
                    if (sccp_get_const(&instr->src1, cells, nv, &val)) {
                        changed |= sccp_set(cells, dst, SCCP_CONST, ~val);
                    } else if (instr->src1.kind == IR_OPERAND_VREG) {
                        int sv = instr->src1.val.vreg;
                        if (sv >= 0 && sv < nv &&
                            cells[sv].state == SCCP_BOTTOM)
                            changed |= sccp_set(cells, dst, SCCP_BOTTOM, 0);
                    } else {
                        changed |= sccp_set(cells, dst, SCCP_BOTTOM, 0);
                    }
                    break;
                }

                case IR_PHI: {
                    /* Meet of all phi args: if all CONST with same val → CONST
                       if any BOTTOM or different constants → BOTTOM
                       if all TOP or CONST(same) → CONST or remain TOP */
                    int has_bottom = 0;
                    int has_const = 0;
                    long long phi_val = 0;
                    for (int p = 0; p < instr->phi_count; p++) {
                        IROperand *arg = &instr->phi_args[p];
                        if (arg->kind == IR_OPERAND_IMM_INT) {
                            if (!has_const) {
                                phi_val = arg->val.imm_int;
                                has_const = 1;
                            } else if (arg->val.imm_int != phi_val) {
                                has_bottom = 1;
                            }
                        } else if (arg->kind == IR_OPERAND_VREG) {
                            int av = arg->val.vreg;
                            if (av >= 0 && av < nv) {
                                if (cells[av].state == SCCP_BOTTOM) {
                                    has_bottom = 1;
                                } else if (cells[av].state == SCCP_CONST) {
                                    if (!has_const) {
                                        phi_val = cells[av].value;
                                        has_const = 1;
                                    } else if (cells[av].value != phi_val) {
                                        has_bottom = 1;
                                    }
                                }
                                /* TOP args don't contribute */
                            }
                        } else {
                            has_bottom = 1;
                        }
                    }
                    if (has_bottom) {
                        changed |= sccp_set(cells, dst, SCCP_BOTTOM, 0);
                    } else if (has_const) {
                        changed |= sccp_set(cells, dst, SCCP_CONST, phi_val);
                    }
                    /* else all TOP → remain TOP */
                    break;
                }

                default:
                    /* Calls, loads, stores, etc. → BOTTOM */
                    changed |= sccp_set(cells, dst, SCCP_BOTTOM, 0);
                    break;
                }
            }
        }
    }

    /* Phase 2: Rewrite — replace uses of constant vregs with immediates.
     * Also replace constant-producing instructions with IR_CONST. */
    int opt_count = 0;
    for (int b = 0; b < func->block_count; b++) {
        IRBlock *block = func->blocks[b];
        for (IRInstr *instr = block->first; instr; instr = instr->next) {
            /* Replace source operands that are constant vregs */
            if (instr->src1.kind == IR_OPERAND_VREG) {
                int v = instr->src1.val.vreg;
                if (v >= 0 && v < nv && cells[v].state == SCCP_CONST &&
                    instr->opcode != IR_PHI) {
                    instr->src1 = ir_op_imm_int(cells[v].value);
                    opt_count++;
                }
            }
            if (instr->src2.kind == IR_OPERAND_VREG) {
                int v = instr->src2.val.vreg;
                if (v >= 0 && v < nv && cells[v].state == SCCP_CONST &&
                    instr->opcode != IR_PHI) {
                    instr->src2 = ir_op_imm_int(cells[v].value);
                    opt_count++;
                }
            }

            /* Replace PHI args that are constant */
            if (instr->opcode == IR_PHI) {
                for (int p = 0; p < instr->phi_count; p++) {
                    if (instr->phi_args[p].kind == IR_OPERAND_VREG) {
                        int v = instr->phi_args[p].val.vreg;
                        if (v >= 0 && v < nv &&
                            cells[v].state == SCCP_CONST) {
                            instr->phi_args[p] =
                                ir_op_imm_int(cells[v].value);
                            opt_count++;
                        }
                    }
                }
            }

            /* If destination is constant, replace the instruction with
             * IR_CONST (unless it's already a CONST or has side effects) */
            if (instr->dst.kind == IR_OPERAND_VREG) {
                int v = instr->dst.val.vreg;
                if (v >= 0 && v < nv && cells[v].state == SCCP_CONST &&
                    instr->opcode != IR_CONST &&
                    !ir_has_side_effects(instr->opcode) &&
                    instr->opcode != IR_PHI) {
                    instr->opcode = IR_CONST;
                    instr->src1 = ir_op_imm_int(cells[v].value);
                    instr->src2 = ir_op_none();
                    opt_count++;
                }
            }
        }
    }

    /* Phase 3: Fold constant branches.
     * If a branch condition is a constant, convert to unconditional jump
     * and remove the dead edge. */
    for (int b = 0; b < func->block_count; b++) {
        IRBlock *block = func->blocks[b];
        IRInstr *term = block->last;
        if (!term || term->opcode != IR_BRANCH) continue;

        long long cond_val;
        if (sccp_get_const(&term->src1, cells, nv, &cond_val)) {
            int true_target = term->src2.val.label;
            int false_target = term->false_target;
            int keep_target = cond_val ? true_target : false_target;

            /* Convert branch to unconditional jump */
            term->opcode = IR_JUMP;
            term->src1 = ir_op_label(keep_target);
            term->src2 = ir_op_none();
            term->false_target = -1;
            opt_count++;

            /* Rebuild CFG edges for this block */
            block->succ_count = 0;
            ir_cfg_add_edge(func, b, keep_target);

            /* Remove this block from the predecessor lists of the dead target */
            int dead_target = cond_val ? false_target : true_target;
            IRBlock *dead_block = func->blocks[dead_target];
            int new_pred_count = 0;
            for (int p = 0; p < dead_block->pred_count; p++) {
                if (dead_block->preds[p] != b)
                    dead_block->preds[new_pred_count++] = dead_block->preds[p];
            }
            dead_block->pred_count = new_pred_count;
        }
    }

    free(cells);
    (void)opt_count;  /* suppress unused warning */
}

void ir_sccp_program(IRProgram *prog) {
    if (!prog) return;
    for (int i = 0; i < prog->func_count; i++) {
        ir_sccp(prog->functions[i]);
    }
}

/* ================================================================== */
/* Optimization Pass: Global Value Numbering / CSE                    */
/* ================================================================== */

/*
 * Dominator-based value numbering.  Walk blocks in dominator-tree
 * preorder.  For each instruction (opcode, vn(src1), vn(src2)),
 * compute a hash.  If a prior instruction has the same hash and
 * operands, replace the current instruction with a copy from the
 * earlier result.
 *
 * This is a simplified GVN that works on SSA form:
 * - Only pure (side-effect-free, non-terminator) instructions are
 *   candidates for elimination.
 * - VN is assigned per SSA vreg at definition (identity).
 * - Instructions are hashed as (opcode << 16 | vn(src1) << 8 | vn(src2)).
 */

#define GVN_TABLE_SIZE 256

typedef struct GVNEntry {
    unsigned hash;
    IROpcode opcode;
    int src1_vn;      /* value number of first source */
    long long src1_imm;/* immediate if src1 is const */
    int src2_vn;       /* value number of second source */
    long long src2_imm;/* immediate if src2 is const */
    int result_vreg;   /* vreg holding the result */
    struct GVNEntry *next;
} GVNEntry;

typedef struct {
    GVNEntry *buckets[GVN_TABLE_SIZE];
    int *vn;            /* value number for each vreg */
    int next_vn;
    int nv;             /* total vregs */
} GVNTable;

static void gvn_init(GVNTable *t, int nv) {
    memset(t->buckets, 0, sizeof(t->buckets));
    t->nv = nv;
    t->next_vn = nv;  /* start value numbers after vreg range */
    t->vn = (int *)ir_alloc(nv * sizeof(int));
    /* Initially each vreg is its own value number */
    for (int i = 0; i < nv; i++) t->vn[i] = i;
}

static void gvn_free(GVNTable *t) {
    for (int i = 0; i < GVN_TABLE_SIZE; i++) {
        GVNEntry *e = t->buckets[i];
        while (e) {
            GVNEntry *next = e->next;
            free(e);
            e = next;
        }
    }
    free(t->vn);
}

static int gvn_get_vn_op(GVNTable *t, IROperand *op) {
    if (op->kind == IR_OPERAND_VREG) {
        int v = op->val.vreg;
        if (v >= 0 && v < t->nv) return t->vn[v];
    }
    /* For immediates, use a sentinel that includes the value.
       We use negative values: -(imm + 1) to avoid collision with vn range.
       This is imperfect for large values but works for typical code. */
    if (op->kind == IR_OPERAND_IMM_INT) {
        return -(int)(op->val.imm_int + 1);
    }
    return -999999;  /* sentinel: never matches */
}

static unsigned gvn_hash(IROpcode op, int vn1, long long imm1,
                         int vn2, long long imm2) {
    unsigned h = (unsigned)op * 2654435761u;
    h ^= (unsigned)vn1 * 2246822519u;
    h ^= (unsigned)(imm1 & 0xFFFFFFFF) * 3266489917u;
    h ^= (unsigned)vn2 * 668265263u;
    h ^= (unsigned)(imm2 & 0xFFFFFFFF) * 374761393u;
    return h;
}

/* Check if opcode is a pure computation that can be CSE'd */
static int gvn_is_pure(IROpcode op) {
    switch (op) {
    case IR_ADD: case IR_SUB: case IR_MUL: case IR_DIV: case IR_MOD:
    case IR_AND: case IR_OR:  case IR_XOR: case IR_SHL: case IR_SHR:
    case IR_CMP_EQ: case IR_CMP_NE: case IR_CMP_LT:
    case IR_CMP_LE: case IR_CMP_GT: case IR_CMP_GE:
    case IR_NEG: case IR_NOT: case IR_BITNOT:
    case IR_CAST:
        return 1;
    default:
        return 0;
    }
}

/* Try to find a matching entry.  Returns result_vreg or -1. */
static int gvn_lookup(GVNTable *t, IROpcode op,
                      int vn1, long long imm1,
                      int vn2, long long imm2) {
    unsigned h = gvn_hash(op, vn1, imm1, vn2, imm2);
    unsigned idx = h % GVN_TABLE_SIZE;
    for (GVNEntry *e = t->buckets[idx]; e; e = e->next) {
        if (e->hash == h && e->opcode == op &&
            e->src1_vn == vn1 && e->src1_imm == imm1 &&
            e->src2_vn == vn2 && e->src2_imm == imm2)
            return e->result_vreg;
    }
    return -1;
}

/* Insert a new entry. */
static void gvn_insert(GVNTable *t, IROpcode op,
                       int vn1, long long imm1,
                       int vn2, long long imm2,
                       int result_vreg) {
    unsigned h = gvn_hash(op, vn1, imm1, vn2, imm2);
    unsigned idx = h % GVN_TABLE_SIZE;
    GVNEntry *e = (GVNEntry *)ir_alloc(sizeof(GVNEntry));
    e->hash = h;
    e->opcode = op;
    e->src1_vn = vn1;
    e->src1_imm = imm1;
    e->src2_vn = vn2;
    e->src2_imm = imm2;
    e->result_vreg = result_vreg;
    e->next = t->buckets[idx];
    t->buckets[idx] = e;
}

/* Walk blocks in dominator-tree preorder */
static void gvn_process_block(GVNTable *t, IRFunction *func, int block_id,
                               int *opt_count) {
    IRBlock *block = func->blocks[block_id];

    for (IRInstr *instr = block->first; instr; instr = instr->next) {
        if (instr->dst.kind != IR_OPERAND_VREG) continue;
        int dst = instr->dst.val.vreg;
        if (dst < 0 || dst >= t->nv) continue;

        /* Propagate value numbers through copies and consts */
        if (instr->opcode == IR_COPY && instr->src1.kind == IR_OPERAND_VREG) {
            int sv = instr->src1.val.vreg;
            if (sv >= 0 && sv < t->nv) {
                t->vn[dst] = t->vn[sv];
            }
            continue;
        }
        if (instr->opcode == IR_CONST || instr->opcode == IR_COPY ||
            instr->opcode == IR_PHI) {
            /* These define new values; keep their identity VN */
            continue;
        }

        if (!gvn_is_pure(instr->opcode)) continue;

        int vn1 = gvn_get_vn_op(t, &instr->src1);
        long long imm1 = (instr->src1.kind == IR_OPERAND_IMM_INT)
                         ? instr->src1.val.imm_int : 0;
        int vn2 = gvn_get_vn_op(t, &instr->src2);
        long long imm2 = (instr->src2.kind == IR_OPERAND_IMM_INT)
                         ? instr->src2.val.imm_int : 0;

        int existing = gvn_lookup(t, instr->opcode, vn1, imm1, vn2, imm2);
        if (existing >= 0) {
            /* Replace with copy from existing result */
            instr->opcode = IR_COPY;
            instr->src1 = ir_op_vreg(existing, instr->dst.type);
            instr->src2 = ir_op_none();
            /* Set value number to same as existing */
            t->vn[dst] = t->vn[existing];
            (*opt_count)++;
        } else {
            /* Record this computation */
            gvn_insert(t, instr->opcode, vn1, imm1, vn2, imm2, dst);
        }
    }

    /* Recurse into dominated blocks */
    for (int c = 0; c < func->block_count; c++) {
        if (c != block_id && func->blocks[c]->idom == block_id) {
            gvn_process_block(t, func, c, opt_count);
        }
    }
}

void ir_gvn_cse(IRFunction *func) {
    if (!func || func->block_count == 0 || !func->is_ssa) return;
    int nv = func->next_vreg;
    if (nv == 0) return;

    GVNTable table;
    gvn_init(&table, nv);

    int opt_count = 0;
    gvn_process_block(&table, func, func->entry_block, &opt_count);

    gvn_free(&table);
    (void)opt_count;
}

void ir_gvn_cse_program(IRProgram *prog) {
    if (!prog) return;
    for (int i = 0; i < prog->func_count; i++) {
        ir_gvn_cse(prog->functions[i]);
    }
}

/* ================================================================== */
/* Optimization Pass: Loop-Invariant Code Motion (LICM)               */
/* ================================================================== */

/*
 * For each natural loop, identify instructions whose operands are all
 * defined outside the loop (or are constants/loop-invariants themselves).
 * Move such instructions to a preheader block inserted before the loop
 * header.
 *
 * Requirements:
 *   - Dominator tree computed
 *   - Loop detection done (loop_depth, loop_header set on blocks)
 *   - SSA form (each vreg defined once → easy to check def location)
 *
 * Algorithm:
 *   1. Build a vreg→(block, instruction) def map
 *   2. For each loop, find the preheader (unique pred outside the loop,
 *      or create one)
 *   3. Mark instructions as invariant if all sources are defined outside
 *      the loop or are themselves invariant.  Iterate to fixed point.
 *   4. Move invariant instructions to the preheader.
 */

/* Find which block defines a given vreg (in SSA, there's exactly one). */
static int licm_find_def_block(IRFunction *func, int vreg) {
    for (int b = 0; b < func->block_count; b++) {
        for (IRInstr *instr = func->blocks[b]->first; instr;
             instr = instr->next) {
            if (instr->dst.kind == IR_OPERAND_VREG &&
                instr->dst.val.vreg == vreg)
                return b;
        }
    }
    return -1;
}

/* Check if a block is in a given loop body. */
static int licm_in_loop(int *loop_body, int body_count, int block_id) {
    for (int i = 0; i < body_count; i++) {
        if (loop_body[i] == block_id) return 1;
    }
    return 0;
}

/* Check if an operand is defined outside the loop or is a constant. */
static int licm_operand_invariant(IRFunction *func, IROperand *op,
                                  int *loop_body, int body_count,
                                  int *is_invariant_vreg, int nv) {
    if (op->kind == IR_OPERAND_NONE ||
        op->kind == IR_OPERAND_IMM_INT ||
        op->kind == IR_OPERAND_IMM_FLOAT ||
        op->kind == IR_OPERAND_LABEL ||
        op->kind == IR_OPERAND_FUNC ||
        op->kind == IR_OPERAND_STRING)
        return 1;

    if (op->kind == IR_OPERAND_VREG) {
        int v = op->val.vreg;
        if (v >= 0 && v < nv && is_invariant_vreg[v])
            return 1;
        /* Defined outside the loop? */
        int def_b = licm_find_def_block(func, v);
        if (def_b < 0)
            return 1;  /* No definition found — function parameter, always invariant */
        if (!licm_in_loop(loop_body, body_count, def_b))
            return 1;
    }
    return 0;
}

/* Find or create a preheader for a loop.
 * The preheader is the unique predecessor of the header that is NOT
 * in the loop body.  If there are multiple such predecessors, or
 * none, we create a new block. */
static int licm_ensure_preheader(IRFunction *func, int header,
                                 int *loop_body, int body_count) {
    IRBlock *hdr = func->blocks[header];
    int preheader = -1;
    int outside_pred_count = 0;

    for (int p = 0; p < hdr->pred_count; p++) {
        if (!licm_in_loop(loop_body, body_count, hdr->preds[p])) {
            preheader = hdr->preds[p];
            outside_pred_count++;
        }
    }

    /* If exactly one outside predecessor that's not the header itself,
       use it as the preheader (if its only successor is the header). */
    if (outside_pred_count == 1 && preheader >= 0) {
        IRBlock *ph = func->blocks[preheader];
        if (ph->succ_count == 1 && ph->succs[0] == header) {
            return preheader;
        }
    }

    /* Create a new preheader block */
    int ph_id = ir_new_block(func, "preheader");
    IRBlock *ph = func->blocks[ph_id];

    /* Insert a jump from preheader to header */
    IRInstr *jmp = ir_instr_new(IR_JUMP, 0);
    jmp->src1 = ir_op_label(header);
    ir_block_append(ph, jmp);

    /* Redirect all outside predecessors of header to go to preheader */
    for (int p = 0; p < hdr->pred_count; p++) {
        if (!licm_in_loop(loop_body, body_count, hdr->preds[p])) {
            int pred_id = hdr->preds[p];
            IRBlock *pred = func->blocks[pred_id];

            /* Update the terminator to jump to preheader instead of header */
            IRInstr *term = pred->last;
            if (term) {
                if (term->opcode == IR_JUMP &&
                    term->src1.kind == IR_OPERAND_LABEL &&
                    term->src1.val.label == header) {
                    term->src1.val.label = ph_id;
                }
                if (term->opcode == IR_BRANCH) {
                    if (term->src2.kind == IR_OPERAND_LABEL &&
                        term->src2.val.label == header)
                        term->src2.val.label = ph_id;
                    if (term->false_target == header)
                        term->false_target = ph_id;
                }
            }

            /* Update successor list of pred */
            for (int s = 0; s < pred->succ_count; s++) {
                if (pred->succs[s] == header)
                    pred->succs[s] = ph_id;
            }

            /* Add pred → preheader edge */
            if (ph->pred_count < IR_MAX_PREDS)
                ph->preds[ph->pred_count++] = pred_id;
        }
    }

    /* Update header's pred list: remove outside preds, add preheader */
    int new_pred_count = 0;
    for (int p = 0; p < hdr->pred_count; p++) {
        if (licm_in_loop(loop_body, body_count, hdr->preds[p])) {
            hdr->preds[new_pred_count++] = hdr->preds[p];
        }
    }
    hdr->preds[new_pred_count++] = ph_id;
    hdr->pred_count = new_pred_count;

    /* Preheader succeeds to header */
    ph->succ_count = 1;
    ph->succs[0] = header;

    /* Update PHI nodes in the header: change predecessor references
       from outside preds to preheader */
    for (IRInstr *phi = hdr->first; phi && phi->opcode == IR_PHI;
         phi = phi->next) {
        for (int p = 0; p < phi->phi_count; p++) {
            if (!licm_in_loop(loop_body, body_count, phi->phi_preds[p])) {
                phi->phi_preds[p] = ph_id;
            }
        }
    }

    return ph_id;
}

void ir_licm(IRFunction *func) {
    if (!func || func->block_count == 0) return;
    int nv = func->next_vreg;
    if (nv == 0) return;

    /* Detect loops */
    IRLoopInfo *li = ir_detect_loops(func);
    if (!li || li->loop_count == 0) {
        ir_free_loop_info(li);
        return;
    }

    int *is_invariant = (int *)ir_alloc(nv * sizeof(int));
    int opt_count = 0;

    /* Process each loop (innermost first — already sorted by body size) */
    for (int l = li->loop_count - 1; l >= 0; l--) {
        IRLoop *loop = &li->loops[l];
        if (loop->body_count == 0) continue;

        memset(is_invariant, 0, nv * sizeof(int));

        /* Iteratively find loop-invariant instructions */
        int changed = 1;
        while (changed) {
            changed = 0;
            for (int i = 0; i < loop->body_count; i++) {
                int b = loop->body[i];
                IRBlock *block = func->blocks[b];
                for (IRInstr *instr = block->first; instr;
                     instr = instr->next) {
                    if (instr->dst.kind != IR_OPERAND_VREG) continue;
                    int dst = instr->dst.val.vreg;
                    if (dst < 0 || dst >= nv) continue;
                    if (is_invariant[dst]) continue;  /* already found */

                    /* Skip non-pure instructions */
                    if (ir_has_side_effects(instr->opcode)) continue;
                    if (ir_is_terminator(instr->opcode)) continue;
                    if (instr->opcode == IR_PHI) continue;
                    if (instr->opcode == IR_LOAD) continue;  /* may alias */
                    if (instr->opcode == IR_ALLOCA) continue;

                    /* Check if all sources are invariant */
                    int inv = 1;
                    if (instr->src1.kind != IR_OPERAND_NONE) {
                        if (!licm_operand_invariant(func, &instr->src1,
                                loop->body, loop->body_count,
                                is_invariant, nv))
                            inv = 0;
                    }
                    if (inv && instr->src2.kind != IR_OPERAND_NONE) {
                        if (!licm_operand_invariant(func, &instr->src2,
                                loop->body, loop->body_count,
                                is_invariant, nv))
                            inv = 0;
                    }

                    if (inv) {
                        is_invariant[dst] = 1;
                        changed = 1;
                    }
                }
            }
        }

        /* Count how many invariant instructions we found */
        int inv_count = 0;
        for (int v = 0; v < nv; v++) {
            if (is_invariant[v]) inv_count++;
        }
        if (inv_count == 0) continue;

        /* Get or create preheader */
        int ph_id = licm_ensure_preheader(func, loop->header,
                                          loop->body, loop->body_count);
        IRBlock *ph = func->blocks[ph_id];

        /* Move invariant instructions to preheader (before the jump) */
        IRInstr *ph_insert_point = ph->last;  /* the jump instruction */

        for (int i = 0; i < loop->body_count; i++) {
            int b = loop->body[i];
            IRBlock *block = func->blocks[b];
            IRInstr *prev = NULL;
            IRInstr *instr = block->first;
            while (instr) {
                IRInstr *next = instr->next;
                if (instr->dst.kind == IR_OPERAND_VREG &&
                    instr->dst.val.vreg >= 0 &&
                    instr->dst.val.vreg < nv &&
                    is_invariant[instr->dst.val.vreg]) {
                    /* Remove from current block */
                    if (prev) {
                        prev->next = next;
                    } else {
                        block->first = next;
                    }
                    if (instr == block->last) {
                        block->last = prev;
                    }
                    block->instr_count--;

                    /* Insert before the jump in preheader */
                    instr->next = ph_insert_point;
                    if (ph->first == ph_insert_point) {
                        ph->first = instr;
                    } else {
                        /* Find the instruction before ph_insert_point */
                        IRInstr *scan = ph->first;
                        while (scan && scan->next != ph_insert_point)
                            scan = scan->next;
                        if (scan) scan->next = instr;
                    }
                    ph->instr_count++;
                    opt_count++;

                    /* Don't update prev — it stays the same */
                } else {
                    prev = instr;
                }
                instr = next;
            }
        }
    }

    free(is_invariant);
    ir_free_loop_info(li);
    (void)opt_count;
}

void ir_licm_program(IRProgram *prog) {
    if (!prog) return;
    for (int i = 0; i < prog->func_count; i++) {
        ir_licm(prog->functions[i]);
    }
}

/* ================================================================== */
/* Optimization Pass: Linear Scan Register Allocation                 */
/* ================================================================== */

/*
 * Compute liveness intervals for each virtual register and assign
 * physical registers using a linear scan algorithm.
 *
 * Physical registers (System V AMD64 ABI callee-saved & caller-saved):
 *   Allocatable GPRs: rax, rcx, rdx, rsi, rdi, r8-r11 (caller-saved)
 *                     rbx, r12-r15 (callee-saved)
 *   Excluded: rsp (stack pointer), rbp (frame pointer)
 *
 * Each vreg gets either a physical register or a spill slot.
 * Results stored in IRFunction's regalloc arrays.
 */

#define RA_NUM_REGS 14    /* Number of allocatable GPRs */

/* Physical register IDs (matching x86_64 encoding) */
typedef enum {
    RA_RAX = 0, RA_RCX = 1, RA_RDX = 2, RA_RBX = 3,
    RA_RSI = 6, RA_RDI = 7,
    RA_R8 = 8,  RA_R9 = 9,  RA_R10 = 10, RA_R11 = 11,
    RA_R12 = 12, RA_R13 = 13, RA_R14 = 14, RA_R15 = 15,
    RA_NONE = -1, RA_SPILL = -2
} RAPhysReg;

/* Map allocatable register index → physical register */
static const int ra_alloc_regs[RA_NUM_REGS] = {
    RA_RAX, RA_RCX, RA_RDX, RA_RBX, RA_RSI, RA_RDI,
    RA_R8, RA_R9, RA_R10, RA_R11, RA_R12, RA_R13, RA_R14, RA_R15
};

static const char *ra_reg_name(int phys_reg) {
    switch (phys_reg) {
    case RA_RAX: return "rax"; case RA_RCX: return "rcx";
    case RA_RDX: return "rdx"; case RA_RBX: return "rbx";
    case RA_RSI: return "rsi"; case RA_RDI: return "rdi";
    case RA_R8:  return "r8";  case RA_R9:  return "r9";
    case RA_R10: return "r10"; case RA_R11: return "r11";
    case RA_R12: return "r12"; case RA_R13: return "r13";
    case RA_R14: return "r14"; case RA_R15: return "r15";
    default: return "spill";
    }
}

/* Liveness interval: [start, end) in linear instruction order */
typedef struct {
    int vreg;
    int start;     /* first instruction index where vreg is defined */
    int end;       /* last instruction index where vreg is used */
    int phys_reg;  /* assigned physical register, or RA_SPILL */
    int spill_slot;/* spill slot index (-1 if not spilled) */
} RAInterval;

/* Compute linear instruction positions and liveness intervals. */
static RAInterval *ra_compute_intervals(IRFunction *func, int *out_count) {
    int nv = func->next_vreg;
    *out_count = 0;
    if (nv == 0) return NULL;

    /* Assign linear positions to instructions */
    int pos = 0;

    /* Start/end for each vreg. -1 means undefined. */
    int *start = (int *)malloc(nv * sizeof(int));
    int *end   = (int *)malloc(nv * sizeof(int));
    for (int i = 0; i < nv; i++) { start[i] = -1; end[i] = -1; }

    pos = 0;
    for (int b = 0; b < func->block_count; b++) {
        for (IRInstr *instr = func->blocks[b]->first; instr;
             instr = instr->next) {
            /* Record def */
            if (instr->dst.kind == IR_OPERAND_VREG) {
                int v = instr->dst.val.vreg;
                if (v >= 0 && v < nv) {
                    if (start[v] < 0) start[v] = pos;
                    if (pos > end[v]) end[v] = pos;
                }
            }
            /* Record uses */
            if (instr->src1.kind == IR_OPERAND_VREG) {
                int v = instr->src1.val.vreg;
                if (v >= 0 && v < nv) {
                    if (start[v] < 0) start[v] = pos;
                    if (pos > end[v]) end[v] = pos;
                }
            }
            if (instr->src2.kind == IR_OPERAND_VREG) {
                int v = instr->src2.val.vreg;
                if (v >= 0 && v < nv) {
                    if (start[v] < 0) start[v] = pos;
                    if (pos > end[v]) end[v] = pos;
                }
            }
            /* PHI args */
            if (instr->opcode == IR_PHI) {
                for (int p = 0; p < instr->phi_count; p++) {
                    if (instr->phi_args[p].kind == IR_OPERAND_VREG) {
                        int v = instr->phi_args[p].val.vreg;
                        if (v >= 0 && v < nv) {
                            if (start[v] < 0) start[v] = pos;
                            if (pos > end[v]) end[v] = pos;
                        }
                    }
                }
            }
            pos++;
        }
    }

    /* Build interval array (only for vregs that are actually used) */
    RAInterval *intervals = (RAInterval *)ir_alloc(nv * sizeof(RAInterval));
    int count = 0;
    for (int v = 0; v < nv; v++) {
        if (start[v] >= 0) {
            intervals[count].vreg = v;
            intervals[count].start = start[v];
            intervals[count].end = end[v];
            intervals[count].phys_reg = RA_NONE;
            intervals[count].spill_slot = -1;
            count++;
        }
    }

    free(start);
    free(end);
    *out_count = count;
    return intervals;
}

/* Sort intervals by start position (insertion sort for small sizes) */
static void ra_sort_intervals(RAInterval *intervals, int count) {
    for (int i = 1; i < count; i++) {
        RAInterval key = intervals[i];
        int j = i - 1;
        while (j >= 0 && intervals[j].start > key.start) {
            intervals[j + 1] = intervals[j];
            j--;
        }
        intervals[j + 1] = key;
    }
}

void ir_regalloc(IRFunction *func) {
    if (!func || func->block_count == 0) return;

    int interval_count;
    RAInterval *intervals = ra_compute_intervals(func, &interval_count);
    if (!intervals || interval_count == 0) {
        free(intervals);
        return;
    }

    ra_sort_intervals(intervals, interval_count);

    /* Active list: intervals currently assigned to a register */
    int *active = (int *)ir_alloc(interval_count * sizeof(int));
    int active_count = 0;

    /* Track which physical registers are free */
    int reg_free[RA_NUM_REGS];
    for (int i = 0; i < RA_NUM_REGS; i++) reg_free[i] = 1;

    int next_spill_slot = 0;

    for (int i = 0; i < interval_count; i++) {
        RAInterval *cur = &intervals[i];

        /* Expire old intervals: remove active intervals that end before
         * the current interval starts */
        int new_active_count = 0;
        for (int a = 0; a < active_count; a++) {
            RAInterval *act = &intervals[active[a]];
            if (act->end < cur->start) {
                /* Free the register */
                if (act->phys_reg >= 0) {
                    for (int r = 0; r < RA_NUM_REGS; r++) {
                        if (ra_alloc_regs[r] == act->phys_reg) {
                            reg_free[r] = 1;
                            break;
                        }
                    }
                }
            } else {
                active[new_active_count++] = active[a];
            }
        }
        active_count = new_active_count;

        /* Try to allocate a free register */
        int allocated = 0;
        for (int r = 0; r < RA_NUM_REGS; r++) {
            if (reg_free[r]) {
                cur->phys_reg = ra_alloc_regs[r];
                reg_free[r] = 0;
                active[active_count++] = i;
                allocated = 1;
                break;
            }
        }

        if (!allocated) {
            /* Spill: find the active interval with the farthest end point */
            int spill_idx = -1;
            int max_end = -1;
            for (int a = 0; a < active_count; a++) {
                if (intervals[active[a]].end > max_end) {
                    max_end = intervals[active[a]].end;
                    spill_idx = a;
                }
            }

            if (spill_idx >= 0 && max_end > cur->end) {
                /* Spill the longest-lived active interval */
                RAInterval *victim = &intervals[active[spill_idx]];
                cur->phys_reg = victim->phys_reg;
                victim->phys_reg = RA_SPILL;
                victim->spill_slot = next_spill_slot++;
                active[spill_idx] = i;  /* replace victim with cur */
            } else {
                /* Spill current interval */
                cur->phys_reg = RA_SPILL;
                cur->spill_slot = next_spill_slot++;
            }
        }
    }

    /* Store allocation results on the function */
    func->regalloc = (int *)ir_alloc(func->next_vreg * sizeof(int));
    func->regalloc_spill = (int *)ir_alloc(func->next_vreg * sizeof(int));
    func->spill_count = next_spill_slot;
    func->has_regalloc = 1;

    for (int v = 0; v < func->next_vreg; v++) {
        func->regalloc[v] = RA_SPILL;
        func->regalloc_spill[v] = -1;
    }

    for (int i = 0; i < interval_count; i++) {
        int v = intervals[i].vreg;
        func->regalloc[v] = intervals[i].phys_reg;
        func->regalloc_spill[v] = intervals[i].spill_slot;
    }

    free(active);
    free(intervals);
}

void ir_regalloc_program(IRProgram *prog) {
    if (!prog) return;
    for (int i = 0; i < prog->func_count; i++) {
        ir_regalloc(prog->functions[i]);
    }
}

/* ================================================================== */
/* Combined optimization driver                                       */
/* ================================================================== */

void ir_optimize_function(IRFunction *func) {
    if (!func || func->block_count == 0) return;

    /* 1. Sparse Conditional Constant Propagation */
    ir_sccp(func);

    /* 2. Global Value Numbering / CSE */
    ir_gvn_cse(func);

    /* 3. Loop-Invariant Code Motion */
    ir_licm(func);

    /* 4. Register allocation */
    ir_regalloc(func);
}

void ir_optimize_program(IRProgram *prog) {
    if (!prog) return;
    for (int i = 0; i < prog->func_count; i++) {
        ir_optimize_function(prog->functions[i]);
    }
}

/* ================================================================== */
/* Debug Output                                                       */
/* ================================================================== */

const char *ir_opcode_name(IROpcode op) {
    switch (op) {
    case IR_CONST:       return "const";
    case IR_COPY:        return "copy";
    case IR_ALLOCA:      return "alloca";
    case IR_ADD:         return "add";
    case IR_SUB:         return "sub";
    case IR_MUL:         return "mul";
    case IR_DIV:         return "div";
    case IR_MOD:         return "mod";
    case IR_AND:         return "and";
    case IR_OR:          return "or";
    case IR_XOR:         return "xor";
    case IR_SHL:         return "shl";
    case IR_SHR:         return "shr";
    case IR_CMP_EQ:      return "cmp_eq";
    case IR_CMP_NE:      return "cmp_ne";
    case IR_CMP_LT:      return "cmp_lt";
    case IR_CMP_LE:      return "cmp_le";
    case IR_CMP_GT:      return "cmp_gt";
    case IR_CMP_GE:      return "cmp_ge";
    case IR_LOGICAL_AND: return "logical_and";
    case IR_LOGICAL_OR:  return "logical_or";
    case IR_NEG:         return "neg";
    case IR_NOT:         return "not";
    case IR_BITNOT:      return "bitnot";
    case IR_LOAD:        return "load";
    case IR_STORE:       return "store";
    case IR_ADDR_OF:     return "addr_of";
    case IR_MEMBER:      return "member";
    case IR_CAST:        return "cast";
    case IR_INDEX:       return "index";
    case IR_INDEX_ADDR:  return "index_addr";
    case IR_PARAM:       return "param";
    case IR_CALL:        return "call";
    case IR_JUMP:        return "jump";
    case IR_BRANCH:      return "branch";
    case IR_RET:         return "ret";
    case IR_SWITCH:      return "switch";
    case IR_NOP:         return "nop";
    case IR_PHI:         return "phi";
    default:             return "???";
    }
}

void ir_dump_operand(IROperand *op, FILE *out) {
    switch (op->kind) {
    case IR_OPERAND_NONE:
        fprintf(out, "_");
        break;
    case IR_OPERAND_VREG:
        fprintf(out, "t%d", op->val.vreg);
        break;
    case IR_OPERAND_VAR:
        fprintf(out, "%%%s", op->val.name ? op->val.name : "?");
        break;
    case IR_OPERAND_IMM_INT:
        fprintf(out, "$%lld", op->val.imm_int);
        break;
    case IR_OPERAND_IMM_FLOAT:
        fprintf(out, "$%.6g", op->val.imm_float);
        break;
    case IR_OPERAND_LABEL:
        fprintf(out, "bb%d", op->val.label);
        break;
    case IR_OPERAND_FUNC:
        fprintf(out, "@%s", op->val.name ? op->val.name : "?");
        break;
    case IR_OPERAND_STRING:
        fprintf(out, "\"%s\"", op->val.name ? op->val.name : "");
        break;
    }
}

void ir_dump_block(IRBlock *block, FILE *out) {
    fprintf(out, "  %s (bb%d):", block->label ? block->label : "???", block->id);

    /* Print predecessors */
    if (block->pred_count > 0) {
        fprintf(out, "  ; preds:");
        for (int i = 0; i < block->pred_count; i++)
            fprintf(out, " bb%d", block->preds[i]);
    }
    /* Print dominator info if available */
    if (block->idom >= 0) {
        fprintf(out, "  ; idom: bb%d", block->idom);
    }
    if (block->dom_frontier_count > 0) {
        fprintf(out, "  ; DF:");
        for (int i = 0; i < block->dom_frontier_count; i++)
            fprintf(out, " bb%d", block->dom_frontier[i]);
    }
    /* Loop info */
    if (block->loop_depth > 0) {
        fprintf(out, "  ; loop: depth=%d hdr=bb%d",
                block->loop_depth, block->loop_header);
    }
    fprintf(out, "\n");

    /* Print liveness summary if computed */
    if (block->live_in) {
        int words = 0;
        /* Find max vreg in live sets to determine bitset width */
        /* Just print non-empty liveness lines */
        int max_word = 0;
        for (int w = 0; w < 64; w++) {  /* reasonable upper bound */
            if (block->live_in[w] || (block->live_out && block->live_out[w]))
                max_word = w + 1;
            else if (w > 0 && !block->live_in[w] &&
                     (!block->live_out || !block->live_out[w]))
                break;
        }
        words = max_word;
        if (words > 0) {
            int live_in_count = bitset_popcount(block->live_in, words);
            int live_out_count = block->live_out
                ? bitset_popcount(block->live_out, words) : 0;
            if (live_in_count > 0 || live_out_count > 0) {
                fprintf(out, "    ; live_in(%d):", live_in_count);
                for (int v = 0; v < words * 32; v++) {
                    if (bitset_test(block->live_in, v))
                        fprintf(out, " t%d", v);
                }
                fprintf(out, "\n");
                fprintf(out, "    ; live_out(%d):", live_out_count);
                for (int v = 0; v < words * 32; v++) {
                    if (block->live_out && bitset_test(block->live_out, v))
                        fprintf(out, " t%d", v);
                }
                fprintf(out, "\n");
            }
        }
    }

    /* Print instructions */
    for (IRInstr *instr = block->first; instr; instr = instr->next) {
        fprintf(out, "    ");

        if (instr->opcode == IR_PHI) {
            /* Special format for PHI */
            ir_dump_operand(&instr->dst, out);
            fprintf(out, " = phi");
            for (int i = 0; i < instr->phi_count; i++) {
                fprintf(out, " [");
                ir_dump_operand(&instr->phi_args[i], out);
                fprintf(out, ", bb%d]", instr->phi_preds[i]);
            }
            fprintf(out, "\n");
            continue;
        }

        if (instr->opcode == IR_SWITCH) {
            fprintf(out, "switch ");
            ir_dump_operand(&instr->src1, out);
            fprintf(out, " {");
            for (int c = 0; c < instr->case_count; c++) {
                fprintf(out, " case %lld: bb%d", instr->cases[c].value,
                        instr->cases[c].target);
            }
            if (instr->default_target >= 0)
                fprintf(out, " default: bb%d", instr->default_target);
            fprintf(out, " }\n");
            continue;
        }

        if (instr->opcode == IR_BRANCH) {
            fprintf(out, "branch ");
            ir_dump_operand(&instr->src1, out);
            fprintf(out, " ? ");
            ir_dump_operand(&instr->src2, out);
            fprintf(out, " : bb%d\n", instr->false_target);
            continue;
        }

        /* Standard instruction format: dst = op src1, src2 */
        if (instr->dst.kind != IR_OPERAND_NONE) {
            ir_dump_operand(&instr->dst, out);
            fprintf(out, " = ");
        }
        fprintf(out, "%s", ir_opcode_name(instr->opcode));
        if (instr->src1.kind != IR_OPERAND_NONE) {
            fprintf(out, " ");
            ir_dump_operand(&instr->src1, out);
        }
        if (instr->src2.kind != IR_OPERAND_NONE) {
            fprintf(out, ", ");
            ir_dump_operand(&instr->src2, out);
        }
        fprintf(out, "\n");
    }

    /* Print successors */
    if (block->succ_count > 0) {
        fprintf(out, "    ; succs:");
        for (int i = 0; i < block->succ_count; i++)
            fprintf(out, " bb%d", block->succs[i]);
        fprintf(out, "\n");
    }
}

void ir_dump_function(IRFunction *func, FILE *out) {
    fprintf(out, "function @%s(", func->name);
    for (int i = 0; i < func->param_count; i++) {
        if (i > 0) fprintf(out, ", ");
        fprintf(out, "%s", func->param_names[i]);
    }
    fprintf(out, ") {\n");

    /* Print variable table */
    if (func->var_count > 0) {
        fprintf(out, "  ; vars:");
        for (int i = 0; i < func->var_count; i++) {
            fprintf(out, " %s=t%d", func->vars[i].name, func->vars[i].vreg);
            if (func->vars[i].is_param) fprintf(out, "[param]");
        }
        fprintf(out, "\n");
    }

    fprintf(out, "  ; %d blocks, %d vregs%s\n",
            func->block_count, func->next_vreg,
            func->is_ssa ? " (SSA)" : "");

    /* Print register allocation summary */
    if (func->has_regalloc) {
        int allocated = 0, spilled = 0;
        for (int v = 0; v < func->next_vreg; v++) {
            if (func->regalloc[v] >= 0) allocated++;
            else if (func->regalloc[v] == -2) spilled++;
        }
        fprintf(out, "  ; regalloc: %d in regs, %d spilled (%d slots)\n",
                allocated, spilled, func->spill_count);
        /* Show a few assignments */
        int shown = 0;
        fprintf(out, "  ; assign:");
        for (int v = 0; v < func->next_vreg && shown < 16; v++) {
            if (func->regalloc[v] >= 0) {
                fprintf(out, " t%d=%s", v, ra_reg_name(func->regalloc[v]));
                shown++;
            }
        }
        if (spilled > 0) {
            for (int v = 0; v < func->next_vreg && shown < 20; v++) {
                if (func->regalloc[v] == -2) {
                    fprintf(out, " t%d=spill[%d]", v,
                            func->regalloc_spill[v]);
                    shown++;
                }
            }
        }
        fprintf(out, "\n");
    }

    fprintf(out, "\n");

    /* Print all blocks */
    for (int i = 0; i < func->block_count; i++) {
        ir_dump_block(func->blocks[i], out);
        fprintf(out, "\n");
    }

    fprintf(out, "}\n\n");
}

void ir_dump_program(IRProgram *prog, FILE *out) {
    fprintf(out, "; === IR Program ===\n");
    fprintf(out, "; %d functions, %d globals\n\n",
            prog->func_count, prog->global_count);

    /* Globals */
    for (int i = 0; i < prog->global_count; i++) {
        fprintf(out, "@%s", prog->globals[i].name);
        if (prog->globals[i].has_init)
            fprintf(out, " = %lld", prog->globals[i].init_value);
        fprintf(out, "\n");
    }
    if (prog->global_count > 0) fprintf(out, "\n");

    /* Functions */
    for (int i = 0; i < prog->func_count; i++) {
        ir_dump_function(prog->functions[i], out);
    }
}

/* ================================================================== */
/* Memory Management                                                  */
/* ================================================================== */

static void ir_free_instr(IRInstr *instr) {
    if (!instr) return;
    /* Free string data in operands */
    if (instr->src1.kind == IR_OPERAND_VAR ||
        instr->src1.kind == IR_OPERAND_FUNC ||
        instr->src1.kind == IR_OPERAND_STRING) {
        free(instr->src1.val.name);
    }
    if (instr->src2.kind == IR_OPERAND_VAR ||
        instr->src2.kind == IR_OPERAND_FUNC ||
        instr->src2.kind == IR_OPERAND_STRING) {
        free(instr->src2.val.name);
    }
    if (instr->dst.kind == IR_OPERAND_VAR ||
        instr->dst.kind == IR_OPERAND_FUNC ||
        instr->dst.kind == IR_OPERAND_STRING) {
        free(instr->dst.val.name);
    }
    if (instr->cases) free(instr->cases);
    if (instr->phi_args) free(instr->phi_args);
    if (instr->phi_preds) free(instr->phi_preds);
    free(instr);
}

static void ir_free_block(IRBlock *block) {
    if (!block) return;
    IRInstr *instr = block->first;
    while (instr) {
        IRInstr *next = instr->next;
        ir_free_instr(instr);
        instr = next;
    }
    free(block->label);
    free(block->live_in);
    free(block->live_out);
    free(block->def);
    free(block->use);
    free(block->dom_frontier);
    free(block);
}

void ir_free_function(IRFunction *func) {
    if (!func) return;
    for (int i = 0; i < func->block_count; i++) {
        ir_free_block(func->blocks[i]);
    }
    free(func->blocks);
    free(func->name);
    for (int i = 0; i < func->param_count; i++) {
        free(func->param_names[i]);
    }
    free(func->param_names);
    free(func->param_types);
    for (int i = 0; i < func->var_count; i++) {
        free(func->vars[i].name);
    }
    free(func->vars);
    free(func->ssa_param_vregs);
    free(func->regalloc);
    free(func->regalloc_spill);
    free(func);
}

void ir_free_program(IRProgram *prog) {
    if (!prog) return;
    for (int i = 0; i < prog->func_count; i++) {
        ir_free_function(prog->functions[i]);
    }
    free(prog->functions);
    for (int i = 0; i < prog->global_count; i++) {
        free(prog->globals[i].name);
    }
    free(prog->globals);
    for (int i = 0; i < prog->string_count; i++) {
        free(prog->strings[i].value);
    }
    free(prog->strings);
    free(prog);
}
