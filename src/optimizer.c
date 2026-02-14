/*
 * optimizer.c — AST-level optimization passes for -O1 and above.
 *
 * Passes (applied bottom-up per expression, then per statement):
 *   1. Constant folding:  3 + 4 → 7, -(-x) → x, etc.
 *   2. Strength reduction: x * 2 → x << 1, x / 4 → x >> 2, x % 2 → x & 1
 *   3. Dead code elimination: remove statements after return/break/continue/goto in a block
 *   4. Algebraic simplification: x + 0 → x, x * 1 → x, x * 0 → 0, etc.
 */

#include "optimizer.h"
#include "lexer.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
/* Helper: is a node a compile-time integer constant?                  */
/* ------------------------------------------------------------------ */
static int is_const_int(ASTNode *node) {
    return node && node->type == AST_INTEGER;
}

/* Helper: create an integer literal node */
static ASTNode *make_int(long long value, int line) {
    ASTNode *n = (ASTNode *)calloc(1, sizeof(ASTNode));
    n->type = AST_INTEGER;
    n->data.integer.value = value;
    n->line = line;
    return n;
}

/* ------------------------------------------------------------------ */
/* Helper: is value a power of two?  Returns the exponent, or -1.     */
/* ------------------------------------------------------------------ */
static int log2_if_power(long long v) {
    if (v <= 0) return -1;
    if ((v & (v - 1)) != 0) return -1;
    int n = 0;
    while (v > 1) { v >>= 1; n++; }
    return n;
}

/* ------------------------------------------------------------------ */
/* Forward declarations                                               */
/* ------------------------------------------------------------------ */
static ASTNode *opt_expr(ASTNode *node);
static void opt_block(ASTNode *block);
static void opt_stmt(ASTNode *node);

/* ------------------------------------------------------------------ */
/* Constant folding for binary expressions                            */
/* ------------------------------------------------------------------ */
static ASTNode *fold_binary(ASTNode *node) {
    ASTNode *left  = node->data.binary_expr.left;
    ASTNode *right = node->data.binary_expr.right;

    if (!is_const_int(left) || !is_const_int(right))
        return node;

    long long l = left->data.integer.value;
    long long r = right->data.integer.value;
    long long result;

    switch (node->data.binary_expr.op) {
        case TOKEN_PLUS:            result = l + r; break;
        case TOKEN_MINUS:           result = l - r; break;
        case TOKEN_STAR:            result = l * r; break;
        case TOKEN_SLASH:           if (r == 0) return node; result = l / r; break;
        case TOKEN_PERCENT:         if (r == 0) return node; result = l % r; break;
        case TOKEN_LESS_LESS:       result = l << r; break;
        case TOKEN_GREATER_GREATER: result = l >> r; break;
        case TOKEN_AMPERSAND:       result = l & r; break;
        case TOKEN_PIPE:            result = l | r; break;
        case TOKEN_CARET:           result = l ^ r; break;
        case TOKEN_EQUAL_EQUAL:     result = (l == r); break;
        case TOKEN_BANG_EQUAL:      result = (l != r); break;
        case TOKEN_LESS:            result = (l < r); break;
        case TOKEN_GREATER:         result = (l > r); break;
        case TOKEN_LESS_EQUAL:      result = (l <= r); break;
        case TOKEN_GREATER_EQUAL:   result = (l >= r); break;
        case TOKEN_AMPERSAND_AMPERSAND: result = (l && r); break;
        case TOKEN_PIPE_PIPE:           result = (l || r); break;
        default: return node;
    }

    return make_int(result, node->line);
}

/* ------------------------------------------------------------------ */
/* Strength reduction: multiply/divide/mod by power-of-two → shifts   */
/* ------------------------------------------------------------------ */
static ASTNode *strength_reduce(ASTNode *node) {
    ASTNode *left  = node->data.binary_expr.left;
    ASTNode *right = node->data.binary_expr.right;
    TokenType op   = node->data.binary_expr.op;

    /* x * 2^n → x << n */
    if (op == TOKEN_STAR) {
        if (is_const_int(right)) {
            int shift = log2_if_power(right->data.integer.value);
            if (shift > 0) {
                node->data.binary_expr.op = TOKEN_LESS_LESS;
                right->data.integer.value = shift;
                return node;
            }
        }
        if (is_const_int(left)) {
            int shift = log2_if_power(left->data.integer.value);
            if (shift > 0) {
                /* Swap: const << x → x << const */
                node->data.binary_expr.left  = right;
                node->data.binary_expr.right = left;
                node->data.binary_expr.op = TOKEN_LESS_LESS;
                left->data.integer.value = shift;
                return node;
            }
        }
    }

    /* x / 2^n → x >> n  (signed: only safe for positive constants, but matches gcc -O1 behavior for simple cases) */
    if (op == TOKEN_SLASH && is_const_int(right)) {
        int shift = log2_if_power(right->data.integer.value);
        if (shift > 0) {
            node->data.binary_expr.op = TOKEN_GREATER_GREATER;
            right->data.integer.value = shift;
            return node;
        }
    }

    /* x % 2^n → x & (2^n - 1) */
    if (op == TOKEN_PERCENT && is_const_int(right)) {
        long long val = right->data.integer.value;
        int shift = log2_if_power(val);
        if (shift > 0) {
            node->data.binary_expr.op = TOKEN_AMPERSAND;
            right->data.integer.value = val - 1;
            return node;
        }
    }

    return node;
}

/* ------------------------------------------------------------------ */
/* Algebraic simplification (identities and annihilators)             */
/* ------------------------------------------------------------------ */
static ASTNode *algebraic_simplify(ASTNode *node) {
    ASTNode *left  = node->data.binary_expr.left;
    ASTNode *right = node->data.binary_expr.right;
    TokenType op   = node->data.binary_expr.op;

    /* x + 0 → x,  0 + x → x */
    if (op == TOKEN_PLUS) {
        if (is_const_int(right) && right->data.integer.value == 0) return left;
        if (is_const_int(left)  && left->data.integer.value  == 0) return right;
    }

    /* x - 0 → x */
    if (op == TOKEN_MINUS) {
        if (is_const_int(right) && right->data.integer.value == 0) return left;
    }

    /* x * 1 → x,  1 * x → x */
    if (op == TOKEN_STAR) {
        if (is_const_int(right) && right->data.integer.value == 1) return left;
        if (is_const_int(left)  && left->data.integer.value  == 1) return right;
    }

    /* x * 0 → 0,  0 * x → 0 */
    if (op == TOKEN_STAR) {
        if (is_const_int(right) && right->data.integer.value == 0) return make_int(0, node->line);
        if (is_const_int(left)  && left->data.integer.value  == 0) return make_int(0, node->line);
    }

    /* x / 1 → x */
    if (op == TOKEN_SLASH) {
        if (is_const_int(right) && right->data.integer.value == 1) return left;
    }

    /* x | 0 → x,  0 | x → x */
    if (op == TOKEN_PIPE) {
        if (is_const_int(right) && right->data.integer.value == 0) return left;
        if (is_const_int(left)  && left->data.integer.value  == 0) return right;
    }

    /* x & 0 → 0 */
    if (op == TOKEN_AMPERSAND) {
        if (is_const_int(right) && right->data.integer.value == 0) return make_int(0, node->line);
        if (is_const_int(left)  && left->data.integer.value  == 0) return make_int(0, node->line);
    }

    /* x ^ 0 → x,  0 ^ x → x */
    if (op == TOKEN_CARET) {
        if (is_const_int(right) && right->data.integer.value == 0) return left;
        if (is_const_int(left)  && left->data.integer.value  == 0) return right;
    }

    /* x << 0 → x,  x >> 0 → x */
    if (op == TOKEN_LESS_LESS || op == TOKEN_GREATER_GREATER) {
        if (is_const_int(right) && right->data.integer.value == 0) return left;
    }

    return node;
}

/* ------------------------------------------------------------------ */
/* Optimize a single expression (bottom-up)                           */
/* ------------------------------------------------------------------ */
static ASTNode *opt_expr(ASTNode *node) {
    if (!node) return NULL;

    switch (node->type) {
        case AST_BINARY_EXPR:
            /* Recurse into children first (bottom-up) */
            node->data.binary_expr.left  = opt_expr(node->data.binary_expr.left);
            node->data.binary_expr.right = opt_expr(node->data.binary_expr.right);

            /* Try constant folding first */
            node = fold_binary(node);
            if (node->type == AST_INTEGER) return node; /* Fully folded */

            /* Algebraic identities */
            node = algebraic_simplify(node);
            if (node->type != AST_BINARY_EXPR) return node;

            /* Strength reduction */
            node = strength_reduce(node);
            return node;

        case AST_NEG:
            node->data.unary.expression = opt_expr(node->data.unary.expression);
            /* -CONST → fold */
            if (is_const_int(node->data.unary.expression)) {
                return make_int(-node->data.unary.expression->data.integer.value, node->line);
            }
            /* -(-x) → x */
            if (node->data.unary.expression->type == AST_NEG) {
                return node->data.unary.expression->data.unary.expression;
            }
            return node;

        case AST_NOT:
            node->data.unary.expression = opt_expr(node->data.unary.expression);
            /* !CONST → fold */
            if (is_const_int(node->data.unary.expression)) {
                return make_int(!node->data.unary.expression->data.integer.value, node->line);
            }
            return node;

        case AST_BITWISE_NOT:
            node->data.unary.expression = opt_expr(node->data.unary.expression);
            /* ~CONST → fold */
            if (is_const_int(node->data.unary.expression)) {
                return make_int(~node->data.unary.expression->data.integer.value, node->line);
            }
            /* ~~x → x */
            if (node->data.unary.expression->type == AST_BITWISE_NOT) {
                return node->data.unary.expression->data.unary.expression;
            }
            return node;

        case AST_CAST:
            node->data.cast.expression = opt_expr(node->data.cast.expression);
            return node;

        case AST_CALL:
            /* Optimize each argument */
            for (size_t i = 0; i < node->children_count; i++) {
                node->children[i] = opt_expr(node->children[i]);
            }
            return node;

        case AST_ARRAY_ACCESS:
            node->data.array_access.array = opt_expr(node->data.array_access.array);
            node->data.array_access.index = opt_expr(node->data.array_access.index);
            return node;

        case AST_MEMBER_ACCESS:
            node->data.member_access.struct_expr = opt_expr(node->data.member_access.struct_expr);
            return node;

        case AST_DEREF:
            node->data.unary.expression = opt_expr(node->data.unary.expression);
            return node;

        case AST_ADDR_OF:
            node->data.unary.expression = opt_expr(node->data.unary.expression);
            return node;

        case AST_PRE_INC:
        case AST_PRE_DEC:
        case AST_POST_INC:
        case AST_POST_DEC:
            node->data.unary.expression = opt_expr(node->data.unary.expression);
            return node;

        default:
            /* AST_INTEGER, AST_FLOAT, AST_IDENTIFIER, AST_STRING — nothing to optimize */
            return node;
    }
}

/* ------------------------------------------------------------------ */
/* Optimize a statement (recursing into sub-expressions and blocks)   */
/* ------------------------------------------------------------------ */
static void opt_stmt(ASTNode *node) {
    if (!node) return;

    switch (node->type) {
        case AST_RETURN:
            if (node->data.return_stmt.expression)
                node->data.return_stmt.expression = opt_expr(node->data.return_stmt.expression);
            break;

        case AST_VAR_DECL:
            if (node->data.var_decl.initializer)
                node->data.var_decl.initializer = opt_expr(node->data.var_decl.initializer);
            break;

        case AST_ASSIGN:
            node->data.assign.value = opt_expr(node->data.assign.value);
            break;

        case AST_IF:
            node->data.if_stmt.condition = opt_expr(node->data.if_stmt.condition);
            /* Constant condition: eliminate dead branch */
            if (is_const_int(node->data.if_stmt.condition)) {
                long long cond = node->data.if_stmt.condition->data.integer.value;
                if (cond) {
                    /* Always true: keep then-branch, drop else */
                    node->data.if_stmt.else_branch = NULL;
                } else {
                    /* Always false: replace then with else (or empty) */
                    if (node->data.if_stmt.else_branch) {
                        node->data.if_stmt.then_branch = node->data.if_stmt.else_branch;
                        node->data.if_stmt.else_branch = NULL;
                        node->data.if_stmt.condition = make_int(1, node->line);
                    } else {
                        /* No else: make the whole if a no-op by setting condition to 0 and empty body
                           Actually simpler: convert to an empty block */
                        node->type = AST_BLOCK;
                        node->children = NULL;
                        node->children_count = 0;
                        break;
                    }
                }
            }
            if (node->data.if_stmt.then_branch)
                opt_stmt(node->data.if_stmt.then_branch);
            if (node->data.if_stmt.else_branch)
                opt_stmt(node->data.if_stmt.else_branch);
            break;

        case AST_WHILE:
            node->data.while_stmt.condition = opt_expr(node->data.while_stmt.condition);
            /* while(0) → dead code (convert to empty block) */
            if (is_const_int(node->data.while_stmt.condition) &&
                node->data.while_stmt.condition->data.integer.value == 0) {
                node->type = AST_BLOCK;
                node->children = NULL;
                node->children_count = 0;
                break;
            }
            if (node->data.while_stmt.body)
                opt_stmt(node->data.while_stmt.body);
            break;

        case AST_DO_WHILE:
            if (node->data.while_stmt.body)
                opt_stmt(node->data.while_stmt.body);
            node->data.while_stmt.condition = opt_expr(node->data.while_stmt.condition);
            break;

        case AST_FOR:
            if (node->data.for_stmt.init)
                opt_stmt(node->data.for_stmt.init);
            if (node->data.for_stmt.condition)
                node->data.for_stmt.condition = opt_expr(node->data.for_stmt.condition);
            if (node->data.for_stmt.increment)
                node->data.for_stmt.increment = opt_expr(node->data.for_stmt.increment);
            /* for(init; 0; ...) → just init, body never executes */
            if (node->data.for_stmt.condition &&
                is_const_int(node->data.for_stmt.condition) &&
                node->data.for_stmt.condition->data.integer.value == 0) {
                /* Keep init statement (may have side effects like declarations) but skip body */
                if (node->data.for_stmt.init) {
                    ASTNode *init = node->data.for_stmt.init;
                    *node = *init;  /* Replace the for node with just init */
                } else {
                    node->type = AST_BLOCK;
                    node->children = NULL;
                    node->children_count = 0;
                }
                break;
            }
            if (node->data.for_stmt.body)
                opt_stmt(node->data.for_stmt.body);
            break;

        case AST_SWITCH:
            node->data.switch_stmt.condition = opt_expr(node->data.switch_stmt.condition);
            if (node->data.switch_stmt.body)
                opt_stmt(node->data.switch_stmt.body);
            break;

        case AST_BLOCK:
            opt_block(node);
            break;

        default:
            /* Expression-statement: optimize the expression */
            if (node->type == AST_CALL || node->type == AST_BINARY_EXPR ||
                node->type == AST_ASSIGN || node->type == AST_PRE_INC ||
                node->type == AST_PRE_DEC || node->type == AST_POST_INC ||
                node->type == AST_POST_DEC) {
                /* These may appear as statements; optimize their sub-expressions */
                ASTNode *optimized = opt_expr(node);
                if (optimized != node) {
                    *node = *optimized;
                }
            }
            break;
    }
}

/* ------------------------------------------------------------------ */
/* Dead code elimination in blocks:                                   */
/* Remove statements after unconditional return/break/continue/goto   */
/* Also recursively optimize each statement in the block.             */
/* ------------------------------------------------------------------ */
static void opt_block(ASTNode *block) {
    if (!block || block->type != AST_BLOCK) return;

    /* First: optimize each child statement */
    for (size_t i = 0; i < block->children_count; i++) {
        opt_stmt(block->children[i]);
    }

    /* Second: find the first return/break/continue/goto and truncate.
     * Be careful not to truncate across case/default labels in switch bodies,
     * because those labels are reachable via the switch jump table. */
    for (size_t i = 0; i < block->children_count; i++) {
        ASTNode *child = block->children[i];
        if (child->type == AST_RETURN || child->type == AST_BREAK ||
            child->type == AST_CONTINUE || child->type == AST_GOTO) {
            /* Check if any remaining sibling is a case/default label */
            int has_case_label = 0;
            for (size_t j = i + 1; j < block->children_count; j++) {
                if (block->children[j]->type == AST_CASE ||
                    block->children[j]->type == AST_DEFAULT) {
                    has_case_label = 1;
                    break;
                }
            }
            if (has_case_label) continue;  /* don't truncate — more cases follow */
            /* Everything after this statement is dead code — truncate */
            if (i + 1 < block->children_count) {
                block->children_count = i + 1;
            }
            break;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Optimize a function body                                           */
/* ------------------------------------------------------------------ */
static void opt_function(ASTNode *func) {
    if (!func || func->type != AST_FUNCTION) return;
    if (!func->data.function.body) return;  /* declaration only */

    opt_stmt(func->data.function.body);
}

/* ------------------------------------------------------------------ */
/* Top-level entry point                                              */
/* ------------------------------------------------------------------ */
ASTNode *optimize(ASTNode *program, OptLevel level) {
    if (level < OPT_O1) return program;  /* -O0: no optimization */
    if (!program) return program;

    for (size_t i = 0; i < program->children_count; i++) {
        ASTNode *child = program->children[i];
        if (child->type == AST_FUNCTION) {
            opt_function(child);
        }
        /* Global variable initializers */
        if (child->type == AST_VAR_DECL && child->data.var_decl.initializer) {
            child->data.var_decl.initializer = opt_expr(child->data.var_decl.initializer);
        }
    }

    return program;
}
