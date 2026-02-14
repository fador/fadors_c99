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

    /* Set up parameters */
    func->param_count = (int)func_node->children_count;
    if (func->param_count > 0) {
        func->param_names = (char **)ir_alloc(func->param_count * sizeof(char *));
        func->param_types = (Type **)ir_alloc(func->param_count * sizeof(Type *));
        for (int i = 0; i < func->param_count; i++) {
            ASTNode *param = func_node->children[i];
            func->param_names[i] = ir_strdup(param->data.var_decl.name);
            func->param_types[i] = param->resolved_type;

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
    fprintf(out, "\n");

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

    fprintf(out, "  ; %d blocks, %d vregs%s\n\n",
            func->block_count, func->next_vreg,
            func->is_ssa ? " (SSA)" : "");

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
