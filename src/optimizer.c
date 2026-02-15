/*
 * optimizer.c — AST-level optimization passes for -O1 and above.
 *
 * Passes (applied bottom-up per expression, then per statement):
 *   1. Constant folding:  3 + 4 → 7, -(-x) → x, etc.
 *   2. Strength reduction: x * 2 → x << 1, x / 4 → x >> 2, x % 2 → x & 1
 *   3. Dead code elimination: remove statements after return/break/continue/goto in a block
 *   4. Algebraic simplification: x + 0 → x, x * 1 → x, x * 0 → 0, etc.
 *
 * -O2 additional passes (within basic blocks):
 *   5. Constant propagation: x = 5; y = x + 3 → y = 8
 *   6. Copy propagation: x = a; ... use x → ... use a (when a unchanged)
 *   7. Dead store elimination: x = 5; x = 10; → x = 10 (remove first store)
 *   8. Function inlining: inline small functions (single return expr) at call sites
 *
 * -O3 additional passes (aggressive):
 *   9. Aggressive inlining: inline multi-statement functions (up to ~20 stmts)
 *  10. Loop unrolling: full unroll for N ≤ 8, partial unroll factor 2-4
 *  11. Loop strength reduction: array[i] in loops → pointer increment
 *
 * -O3 vectorization pass:
 *  16. Vectorization hints: detect simple a[i] = b[i] OP c[i] loops and
 *      annotate them for SSE packed instruction codegen.
 *
 * -O3 interprocedural passes:
 *  12. IPA constant propagation: specialize parameters always passed as same constant
 *  13. Dead argument elimination: remove parameters never read in function body
 *  14. Dead function elimination: remove functions with zero callers after inlining
 *  15. Return value propagation: replace calls to functions that always return same constant
 */

#include "optimizer.h"
#include "codegen.h"
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

        case AST_ASSERT:
            if (node->data.assert_stmt.condition)
                node->data.assert_stmt.condition = opt_expr(node->data.assert_stmt.condition);
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

/* ================================================================== */
/* Assert-based value range analysis                                  */
/* ================================================================== */

/* Value range entry: variable name → known range / properties */
#define MAX_RANGES 64
typedef struct {
    const char *name;       /* variable name */
    long long   min_val;    /* minimum known value (LLONG_MIN = unbounded) */
    long long   max_val;    /* maximum known value (LLONG_MAX = unbounded) */
    int         is_pow2;    /* 1 if assert guarantees power-of-2 */
    int         exact;      /* 1 if min_val == max_val (exact constant) */
} RangeEntry;

typedef struct {
    RangeEntry entries[MAX_RANGES];
    int count;
} RangeEnv;

static void range_env_init(RangeEnv *env) {
    env->count = 0;
}

static RangeEntry *range_env_find(RangeEnv *env, const char *name) {
    for (int i = 0; i < env->count; i++) {
        if (strcmp(env->entries[i].name, name) == 0) return &env->entries[i];
    }
    return NULL;
}

static void range_env_set(RangeEnv *env, const char *name,
                           long long lo, long long hi, int pow2) {
    RangeEntry *e = range_env_find(env, name);
    if (!e) {
        if (env->count >= MAX_RANGES) return;
        e = &env->entries[env->count++];
        e->name = name;
    }
    e->min_val = lo;
    e->max_val = hi;
    e->is_pow2 = pow2;
    e->exact = (lo == hi) ? 1 : 0;
}

/* Invalidate a range entry (e.g., after assignment to the variable) */
static void range_env_invalidate(RangeEnv *env, const char *name) {
    for (int i = 0; i < env->count; i++) {
        if (strcmp(env->entries[i].name, name) == 0) {
            /* Remove by shifting down */
            for (int j = i; j < env->count - 1; j++)
                env->entries[j] = env->entries[j + 1];
            env->count--;
            return;
        }
    }
}

/* Extract a variable name from an AST_IDENTIFIER node */
static const char *range_get_ident(ASTNode *node) {
    if (!node) return NULL;
    if (node->type == AST_IDENTIFIER) return node->data.identifier.name;
    return NULL;
}

/* Check if expr matches the pattern: (x & (x - 1)) == 0
 * This is the canonical power-of-2 test.
 * Returns the variable name if matched, NULL otherwise. */
static const char *range_match_pow2(ASTNode *cond) {
    if (!cond || cond->type != AST_BINARY_EXPR) return NULL;
    if (cond->data.binary_expr.op != TOKEN_EQUAL_EQUAL) return NULL;

    ASTNode *lhs = cond->data.binary_expr.left;
    ASTNode *rhs = cond->data.binary_expr.right;

    /* RHS must be 0 */
    if (!is_const_int(rhs) || rhs->data.integer.value != 0) return NULL;

    /* LHS must be (x & (x - 1)) */
    if (lhs->type != AST_BINARY_EXPR) return NULL;
    if (lhs->data.binary_expr.op != TOKEN_AMPERSAND) return NULL;

    ASTNode *and_l = lhs->data.binary_expr.left;
    ASTNode *and_r = lhs->data.binary_expr.right;

    /* Pattern A: x & (x - 1) */
    const char *name_l = range_get_ident(and_l);
    if (name_l && and_r->type == AST_BINARY_EXPR &&
        and_r->data.binary_expr.op == TOKEN_MINUS) {
        const char *name_r = range_get_ident(and_r->data.binary_expr.left);
        if (name_r && strcmp(name_l, name_r) == 0 &&
            is_const_int(and_r->data.binary_expr.right) &&
            and_r->data.binary_expr.right->data.integer.value == 1) {
            return name_l;
        }
    }

    /* Pattern B: (x - 1) & x */
    const char *name_r = range_get_ident(and_r);
    if (name_r && and_l->type == AST_BINARY_EXPR &&
        and_l->data.binary_expr.op == TOKEN_MINUS) {
        const char *name_l2 = range_get_ident(and_l->data.binary_expr.left);
        if (name_l2 && strcmp(name_r, name_l2) == 0 &&
            is_const_int(and_l->data.binary_expr.right) &&
            and_l->data.binary_expr.right->data.integer.value == 1) {
            return name_r;
        }
    }

    return NULL;
}

/* Extract value range info from a single comparison expression.
 * Populates env with the range information found. */
static void range_extract_cmp(ASTNode *cond, RangeEnv *env) {
    if (!cond || cond->type != AST_BINARY_EXPR) return;

    TokenType op = cond->data.binary_expr.op;
    ASTNode *left = cond->data.binary_expr.left;
    ASTNode *right = cond->data.binary_expr.right;

    /* var OP const */
    const char *name = range_get_ident(left);
    if (name && is_const_int(right)) {
        long long val = right->data.integer.value;
        RangeEntry *existing = range_env_find(env, name);
        long long lo = existing ? existing->min_val : (-2147483647LL - 1);
        long long hi = existing ? existing->max_val : 2147483647LL;
        int pow2 = existing ? existing->is_pow2 : 0;

        switch (op) {
        case TOKEN_LESS:           /* x < val → x <= val-1 */
            if (val - 1 < hi) hi = val - 1;
            break;
        case TOKEN_LESS_EQUAL:     /* x <= val */
            if (val < hi) hi = val;
            break;
        case TOKEN_GREATER:        /* x > val → x >= val+1 */
            if (val + 1 > lo) lo = val + 1;
            break;
        case TOKEN_GREATER_EQUAL:  /* x >= val */
            if (val > lo) lo = val;
            break;
        case TOKEN_EQUAL_EQUAL:    /* x == val */
            lo = val; hi = val;
            break;
        default:
            return;
        }
        range_env_set(env, name, lo, hi, pow2);
        return;
    }

    /* const OP var (reverse) */
    name = range_get_ident(right);
    if (name && is_const_int(left)) {
        long long val = left->data.integer.value;
        RangeEntry *existing = range_env_find(env, name);
        long long lo = existing ? existing->min_val : (-2147483647LL - 1);
        long long hi = existing ? existing->max_val : 2147483647LL;
        int pow2 = existing ? existing->is_pow2 : 0;

        switch (op) {
        case TOKEN_LESS:           /* const < x → x > const → x >= const+1 */
            if (val + 1 > lo) lo = val + 1;
            break;
        case TOKEN_LESS_EQUAL:     /* const <= x → x >= const */
            if (val > lo) lo = val;
            break;
        case TOKEN_GREATER:        /* const > x → x < const → x <= const-1 */
            if (val - 1 < hi) hi = val - 1;
            break;
        case TOKEN_GREATER_EQUAL:  /* const >= x → x <= const */
            if (val < hi) hi = val;
            break;
        case TOKEN_EQUAL_EQUAL:    /* const == x → exact */
            lo = val; hi = val;
            break;
        default:
            return;
        }
        range_env_set(env, name, lo, hi, pow2);
        return;
    }
}

/* Analyze an assert condition and extract value ranges.
 * Handles: simple comparisons, && chains, power-of-2 patterns, x > 0. */
static void range_extract_assert(ASTNode *cond, RangeEnv *env) {
    if (!cond) return;

    /* Handle && chains: assert(a && b) → extract from both a and b */
    if (cond->type == AST_BINARY_EXPR &&
        cond->data.binary_expr.op == TOKEN_AMPERSAND_AMPERSAND) {
        range_extract_assert(cond->data.binary_expr.left, env);
        range_extract_assert(cond->data.binary_expr.right, env);
        return;
    }

    /* Check for power-of-2 pattern: (x & (x-1)) == 0 */
    const char *pow2_var = range_match_pow2(cond);
    if (pow2_var) {
        RangeEntry *existing = range_env_find(env, pow2_var);
        long long lo = existing ? existing->min_val : (-2147483647LL - 1);
        long long hi = existing ? existing->max_val : 2147483647LL;
        range_env_set(env, pow2_var, lo, hi, 1);
        return;
    }

    /* Simple comparison: x REL const */
    range_extract_cmp(cond, env);
}

/* Apply range-based optimizations to an expression.
 * - x * var where var is known power-of-2: cannot convert at AST level
 *   (var is not a compile-time constant). Instead, we can use the range
 *   info to inform codegen. But for assert(x == const), we can substitute.
 * - var * const / var / const: already handled by strength_reduce for constants.
 *
 * Key optimization: when a variable is known to be a power-of-2 via assert,
 * we can replace expressions like:
 *   y * x  → y << log2(x)   [but x is variable — need runtime log2]
 * However, if assert also gives us an exact value (range is exact, and power-of-2),
 * we can substitute the constant directly.
 *
 * More practical: if assert(x >= 0 && x <= 300), and we see x / 4, the existing
 * strength reduction handles this. The range info contributes by:
 *   - Confirming x is non-negative, allowing unsigned optimizations for / and %
 *   - When x is exact power-of-2 constant, we already handle it
 *   - When variable is flagged is_pow2 by assert, and used as divisor/multiplier:
 *     y / x → y >> __builtin_ctz(x)  — requires runtime intrinsic, skip for now.
 *     y * x → y << __builtin_ctz(x)  — same.
 *
 * Practical optimization we CAN do:
 *   1. assert(x == CONST) → substitute x with CONST in subsequent expressions
 *      (enables constant folding + existing strength reduction)
 *   2. assert(x >= 0) → enables signed div/mod → unsigned shift optimization
 *      (existing strength_reduce already handles x / 2^n → x >> n, but only
 *       when the divisor is constant — the range info gives us confidence it's safe)
 *
 * For is_pow2 variables specifically, we replace:
 *   y * pow2_var → y << ctz(pow2_var)  but since pow2_var is a variable,
 *   we need codegen support. Instead, at the AST level, if the pow2 var
 *   also has a known exact value, we substitute.
 */

/* Substitute assert-derived exact constants into expressions */
static ASTNode *range_subst_expr(ASTNode *node, RangeEnv *env) {
    if (!node) return NULL;

    switch (node->type) {
    case AST_IDENTIFIER: {
        const char *name = node->data.identifier.name;
        RangeEntry *r = range_env_find(env, name);
        if (r && r->exact) {
            /* Known exact value from assert — substitute constant */
            return make_int(r->min_val, node->line);
        }
        return node;
    }
    case AST_BINARY_EXPR:
        node->data.binary_expr.left = range_subst_expr(node->data.binary_expr.left, env);
        node->data.binary_expr.right = range_subst_expr(node->data.binary_expr.right, env);
        /* After substitution, check if we can fold */
        node = fold_binary(node);
        if (node->type == AST_INTEGER) return node;
        node = algebraic_simplify(node);
        if (node->type != AST_BINARY_EXPR) return node;
        node = strength_reduce(node);
        return node;
    case AST_NEG:
    case AST_NOT:
    case AST_BITWISE_NOT:
        node->data.unary.expression = range_subst_expr(node->data.unary.expression, env);
        return node;
    case AST_CAST:
        node->data.cast.expression = range_subst_expr(node->data.cast.expression, env);
        return node;
    case AST_CALL:
        for (size_t i = 0; i < node->children_count; i++)
            node->children[i] = range_subst_expr(node->children[i], env);
        return node;
    case AST_ARRAY_ACCESS:
        node->data.array_access.array = range_subst_expr(node->data.array_access.array, env);
        node->data.array_access.index = range_subst_expr(node->data.array_access.index, env);
        return node;
    default:
        return node;
    }
}

/* Apply range-based substitution to a statement */
static void range_subst_stmt(ASTNode *node, RangeEnv *env) {
    if (!node) return;
    switch (node->type) {
    case AST_RETURN:
        if (node->data.return_stmt.expression)
            node->data.return_stmt.expression = range_subst_expr(node->data.return_stmt.expression, env);
        break;
    case AST_VAR_DECL:
        if (node->data.var_decl.initializer)
            node->data.var_decl.initializer = range_subst_expr(node->data.var_decl.initializer, env);
        break;
    case AST_ASSIGN:
        node->data.assign.value = range_subst_expr(node->data.assign.value, env);
        /* Assignment to a ranged variable invalidates its range */
        if (node->data.assign.left && node->data.assign.left->type == AST_IDENTIFIER) {
            range_env_invalidate(env, node->data.assign.left->data.identifier.name);
        }
        break;
    default:
        break;
    }
}

/* Walk a block-level AST, find AST_ASSERT nodes, extract value ranges,
 * and apply range-based optimizations to subsequent statements.
 * Should be called after O1 passes so constants are already folded. */
static void range_analyze_block(ASTNode *block) {
    if (!block || block->type != AST_BLOCK) return;

    RangeEnv env;
    range_env_init(&env);

    for (size_t i = 0; i < block->children_count; i++) {
        ASTNode *stmt = block->children[i];
        if (!stmt) continue;

        if (stmt->type == AST_ASSERT) {
            /* Extract value ranges from the assert condition */
            range_extract_assert(stmt->data.assert_stmt.condition, &env);
            continue;
        }

        /* Apply range-based substitutions to this statement */
        if (env.count > 0) {
            range_subst_stmt(stmt, &env);
        }

        /* Assignment invalidates ranges for the target variable */
        if (stmt->type == AST_ASSIGN && stmt->data.assign.left &&
            stmt->data.assign.left->type == AST_IDENTIFIER) {
            range_env_invalidate(&env, stmt->data.assign.left->data.identifier.name);
        }
        if (stmt->type == AST_VAR_DECL) {
            range_env_invalidate(&env, stmt->data.var_decl.name);
        }

        /* Control flow: recurse into sub-blocks but reset ranges at flow boundaries */
        if (stmt->type == AST_IF) {
            if (stmt->data.if_stmt.then_branch)
                range_analyze_block(stmt->data.if_stmt.then_branch);
            if (stmt->data.if_stmt.else_branch)
                range_analyze_block(stmt->data.if_stmt.else_branch);
        } else if (stmt->type == AST_WHILE || stmt->type == AST_DO_WHILE) {
            if (stmt->data.while_stmt.body)
                range_analyze_block(stmt->data.while_stmt.body);
        } else if (stmt->type == AST_FOR) {
            if (stmt->data.for_stmt.body)
                range_analyze_block(stmt->data.for_stmt.body);
        } else if (stmt->type == AST_BLOCK) {
            range_analyze_block(stmt);
        }
    }
}

/* ================================================================== */
/* -O2: Within-block constant/copy propagation and dead store elim.   */
/* ================================================================== */

/* A tracked variable binding: variable name → known value */
#define MAX_BINDINGS 256
typedef struct {
    const char *name;         /* variable name */
    ASTNode    *value;        /* AST_INTEGER, AST_IDENTIFIER, or NULL (unknown) */
    int         store_idx;    /* index in block where last written (-1 if none) */
    int         was_read;     /* whether the variable was read since last write */
} VarBinding;

typedef struct {
    VarBinding entries[MAX_BINDINGS];
    int count;
} PropEnv;

static void prop_env_init(PropEnv *env) {
    env->count = 0;
}

static VarBinding *prop_env_find(PropEnv *env, const char *name) {
    for (int i = 0; i < env->count; i++) {
        if (strcmp(env->entries[i].name, name) == 0) return &env->entries[i];
    }
    return NULL;
}

static void prop_env_set(PropEnv *env, const char *name, ASTNode *value, int store_idx) {
    VarBinding *b = prop_env_find(env, name);
    if (b) {
        b->value = value;
        b->store_idx = store_idx;
        b->was_read = 0;
        return;
    }
    if (env->count < MAX_BINDINGS) {
        env->entries[env->count].name = name;
        env->entries[env->count].value = value;
        env->entries[env->count].store_idx = store_idx;
        env->entries[env->count].was_read = 0;
        env->count++;
    }
}

static void prop_env_mark_read(PropEnv *env, const char *name) {
    VarBinding *b = prop_env_find(env, name);
    if (b) b->was_read = 1;
}

/* Invalidate a binding (variable modified in unknown way) */
static void prop_env_invalidate(PropEnv *env, const char *name) {
    VarBinding *b = prop_env_find(env, name);
    if (b) {
        b->value = NULL;
        b->was_read = 1; /* conservative: assume it was needed */
    }
}

/* Invalidate all bindings (call, pointer write, etc.) */
static void prop_env_invalidate_all(PropEnv *env) {
    for (int i = 0; i < env->count; i++) {
        env->entries[i].value = NULL;
        env->entries[i].was_read = 1;
    }
}

/* Check if an expression has side effects (calls, increments, etc.) */
static int has_side_effects(ASTNode *node) {
    if (!node) return 0;
    switch (node->type) {
        case AST_CALL:
        case AST_PRE_INC:
        case AST_PRE_DEC:
        case AST_POST_INC:
        case AST_POST_DEC:
        case AST_ASSIGN:
            return 1;
        case AST_BINARY_EXPR:
            return has_side_effects(node->data.binary_expr.left) ||
                   has_side_effects(node->data.binary_expr.right);
        case AST_NEG:
        case AST_NOT:
        case AST_BITWISE_NOT:
        case AST_DEREF:
        case AST_ADDR_OF:
            return has_side_effects(node->data.unary.expression);
        case AST_CAST:
            return has_side_effects(node->data.cast.expression);
        case AST_ARRAY_ACCESS:
            return has_side_effects(node->data.array_access.array) ||
                   has_side_effects(node->data.array_access.index);
        case AST_MEMBER_ACCESS:
            return has_side_effects(node->data.member_access.struct_expr);
        default:
            return 0;
    }
}

/* Collect all variable names read by an expression */
static void collect_reads(ASTNode *node, PropEnv *env) {
    if (!node) return;
    switch (node->type) {
        case AST_IDENTIFIER:
            prop_env_mark_read(env, node->data.identifier.name);
            return;
        case AST_BINARY_EXPR:
            collect_reads(node->data.binary_expr.left, env);
            collect_reads(node->data.binary_expr.right, env);
            return;
        case AST_NEG:
        case AST_NOT:
        case AST_BITWISE_NOT:
        case AST_DEREF:
        case AST_ADDR_OF:
        case AST_PRE_INC:
        case AST_PRE_DEC:
        case AST_POST_INC:
        case AST_POST_DEC:
            collect_reads(node->data.unary.expression, env);
            return;
        case AST_CAST:
            collect_reads(node->data.cast.expression, env);
            return;
        case AST_CALL:
            for (size_t i = 0; i < node->children_count; i++)
                collect_reads(node->children[i], env);
            return;
        case AST_ARRAY_ACCESS:
            collect_reads(node->data.array_access.array, env);
            collect_reads(node->data.array_access.index, env);
            return;
        case AST_MEMBER_ACCESS:
            collect_reads(node->data.member_access.struct_expr, env);
            return;
        case AST_ASSIGN:
            collect_reads(node->data.assign.value, env);
            /* also read the target if it's complex (array, deref, member) */
            if (node->data.assign.left && node->data.assign.left->type != AST_IDENTIFIER)
                collect_reads(node->data.assign.left, env);
            return;
        default:
            return;
    }
}

/* Substitute known bindings in an expression (returns modified expression).
 * Only substitutes AST_IDENTIFIER references to propagated constants/copies. */
static ASTNode *prop_substitute(ASTNode *node, PropEnv *env) {
    if (!node) return NULL;
    switch (node->type) {
        case AST_IDENTIFIER: {
            VarBinding *b = prop_env_find(env, node->data.identifier.name);
            if (b && b->value) {
                prop_env_mark_read(env, node->data.identifier.name);
                if (b->value->type == AST_INTEGER) {
                    /* constant propagation: replace x with known const */
                    return make_int(b->value->data.integer.value, node->line);
                }
                if (b->value->type == AST_IDENTIFIER) {
                    /* copy propagation: check that the source var hasn't been invalidated */
                    VarBinding *src = prop_env_find(env, b->value->data.identifier.name);
                    if (src && src->value == NULL) {
                        /* Source was invalidated — the copy binding is stale. Can still use
                         * the source variable name, just can't transitively propagate. */
                    }
                    /* Replace with source identifier */
                    ASTNode *copy = (ASTNode *)calloc(1, sizeof(ASTNode));
                    copy->type = AST_IDENTIFIER;
                    copy->data.identifier.name = b->value->data.identifier.name;
                    copy->resolved_type = node->resolved_type;
                    copy->line = node->line;
                    prop_env_mark_read(env, b->value->data.identifier.name);
                    return copy;
                }
            }
            return node;
        }
        case AST_BINARY_EXPR:
            node->data.binary_expr.left = prop_substitute(node->data.binary_expr.left, env);
            node->data.binary_expr.right = prop_substitute(node->data.binary_expr.right, env);
            return node;
        case AST_NEG:
        case AST_NOT:
        case AST_BITWISE_NOT:
        case AST_DEREF:
            node->data.unary.expression = prop_substitute(node->data.unary.expression, env);
            return node;
        case AST_ADDR_OF:
            /* Do NOT substitute into the operand of address-of.
             * &x must remain &x; replacing x with its constant value
             * would produce &<literal>, which is nonsensical. */
            return node;
        case AST_CAST:
            node->data.cast.expression = prop_substitute(node->data.cast.expression, env);
            return node;
        case AST_CALL:
            for (size_t i = 0; i < node->children_count; i++)
                node->children[i] = prop_substitute(node->children[i], env);
            return node;
        case AST_ARRAY_ACCESS:
            node->data.array_access.array = prop_substitute(node->data.array_access.array, env);
            node->data.array_access.index = prop_substitute(node->data.array_access.index, env);
            return node;
        case AST_MEMBER_ACCESS:
            node->data.member_access.struct_expr = prop_substitute(node->data.member_access.struct_expr, env);
            return node;
        default:
            return node;
    }
}

/* ------------------------------------------------------------------ */
/* O2: Propagation + dead store elimination on a block                */
/* ------------------------------------------------------------------ */
static void o2_propagate_block(ASTNode *block) {
    if (!block || block->type != AST_BLOCK) return;

    PropEnv env;
    prop_env_init(&env);

    for (size_t i = 0; i < block->children_count; i++) {
        ASTNode *stmt = block->children[i];
        if (!stmt) continue;

        /* ---- Variable declaration with initializer ---- */
        if (stmt->type == AST_VAR_DECL && stmt->data.var_decl.initializer) {
            /* Don't propagate address-of (pointer aliasing) */
            if (stmt->data.var_decl.initializer->type == AST_ADDR_OF) {
                prop_env_invalidate(&env, stmt->data.var_decl.name);
                collect_reads(stmt->data.var_decl.initializer, &env);
                continue;
            }
            /* Substitute in the initializer expression */
            stmt->data.var_decl.initializer = prop_substitute(stmt->data.var_decl.initializer, &env);
            /* Then run O1 opts on the substituted result */
            stmt->data.var_decl.initializer = opt_expr(stmt->data.var_decl.initializer);
            collect_reads(stmt->data.var_decl.initializer, &env);

            /* Record the binding: name → value */
            ASTNode *val = stmt->data.var_decl.initializer;
            if (val->type == AST_INTEGER || val->type == AST_IDENTIFIER) {
                prop_env_set(&env, stmt->data.var_decl.name, val, (int)i);
            } else {
                prop_env_set(&env, stmt->data.var_decl.name, NULL, (int)i);
            }
            continue;
        }

        /* ---- Simple assignment: identifier = expr ---- */
        if (stmt->type == AST_ASSIGN && stmt->data.assign.left &&
            stmt->data.assign.left->type == AST_IDENTIFIER) {
            const char *varname = stmt->data.assign.left->data.identifier.name;

            /* Don't propagate if RHS takes address */
            if (stmt->data.assign.value->type == AST_ADDR_OF) {
                prop_env_invalidate(&env, varname);
                collect_reads(stmt->data.assign.value, &env);
                continue;
            }

            /* Substitute in RHS */
            stmt->data.assign.value = prop_substitute(stmt->data.assign.value, &env);
            stmt->data.assign.value = opt_expr(stmt->data.assign.value);
            collect_reads(stmt->data.assign.value, &env);

            /* Dead store: if previous write to this var was not read, mark it dead */
            VarBinding *prev = prop_env_find(&env, varname);
            if (prev && prev->store_idx >= 0 && !prev->was_read) {
                /* Previous store is dead — convert to empty block (no-op) */
                ASTNode *dead = block->children[prev->store_idx];
                if (dead->type == AST_ASSIGN && !has_side_effects(dead->data.assign.value)) {
                    dead->type = AST_BLOCK;
                    dead->children = NULL;
                    dead->children_count = 0;
                }
                /* Don't eliminate var_decl dead stores — the declaration is still needed */
            }

            /* Record new binding */
            ASTNode *val = stmt->data.assign.value;
            if (val->type == AST_INTEGER || val->type == AST_IDENTIFIER) {
                prop_env_set(&env, varname, val, (int)i);
            } else {
                prop_env_set(&env, varname, NULL, (int)i);
            }
            continue;
        }

        /* ---- Return: substitute and mark reads ---- */
        if (stmt->type == AST_RETURN && stmt->data.return_stmt.expression) {
            stmt->data.return_stmt.expression = prop_substitute(stmt->data.return_stmt.expression, &env);
            stmt->data.return_stmt.expression = opt_expr(stmt->data.return_stmt.expression);
            collect_reads(stmt->data.return_stmt.expression, &env);
            prop_env_invalidate_all(&env); /* can't propagate past return */
            continue;
        }

        /* ---- Control flow: invalidate for safety ---- */
        if (stmt->type == AST_IF || stmt->type == AST_WHILE ||
            stmt->type == AST_DO_WHILE || stmt->type == AST_FOR ||
            stmt->type == AST_SWITCH) {
            /* Only substitute in conditions of non-looping constructs (if/switch).
             * Loop conditions (while/for/do-while) must NOT be substituted because
             * the loop body may modify variables used in the condition — propagating
             * a pre-loop value would make the condition constant, causing infinite loops. */
            if (stmt->type == AST_IF && stmt->data.if_stmt.condition) {
                stmt->data.if_stmt.condition = prop_substitute(stmt->data.if_stmt.condition, &env);
                stmt->data.if_stmt.condition = opt_expr(stmt->data.if_stmt.condition);
                collect_reads(stmt->data.if_stmt.condition, &env);
            }
            if (stmt->type == AST_SWITCH && stmt->data.switch_stmt.condition) {
                stmt->data.switch_stmt.condition = prop_substitute(stmt->data.switch_stmt.condition, &env);
                stmt->data.switch_stmt.condition = opt_expr(stmt->data.switch_stmt.condition);
                collect_reads(stmt->data.switch_stmt.condition, &env);
            }
            /* Invalidate all — branches/loops may modify any variable */
            prop_env_invalidate_all(&env);
            continue;
        }

        /* ---- Function calls as statements: invalidate all ---- */
        if (stmt->type == AST_CALL) {
            for (size_t j = 0; j < stmt->children_count; j++) {
                stmt->children[j] = prop_substitute(stmt->children[j], &env);
                stmt->children[j] = opt_expr(stmt->children[j]);
                collect_reads(stmt->children[j], &env);
            }
            prop_env_invalidate_all(&env);
            continue;
        }

        /* ---- Break/continue/goto: stop propagation ---- */
        if (stmt->type == AST_BREAK || stmt->type == AST_CONTINUE ||
            stmt->type == AST_GOTO) {
            prop_env_invalidate_all(&env);
            continue;
        }

        /* ---- Labels/case: jump target, invalidate ---- */
        if (stmt->type == AST_LABEL || stmt->type == AST_CASE ||
            stmt->type == AST_DEFAULT) {
            prop_env_invalidate_all(&env);
            continue;
        }

        /* ---- Increment/decrement expressions ---- */
        if (stmt->type == AST_PRE_INC || stmt->type == AST_PRE_DEC ||
            stmt->type == AST_POST_INC || stmt->type == AST_POST_DEC) {
            if (stmt->data.unary.expression &&
                stmt->data.unary.expression->type == AST_IDENTIFIER) {
                prop_env_invalidate(&env, stmt->data.unary.expression->data.identifier.name);
            }
            continue;
        }

        /* ---- Complex assignments (deref, struct, array) ---- */
        if (stmt->type == AST_ASSIGN) {
            /* Non-simple LHS — can't track, but substitute in both sides */
            collect_reads(stmt->data.assign.left, &env);
            stmt->data.assign.value = prop_substitute(stmt->data.assign.value, &env);
            stmt->data.assign.value = opt_expr(stmt->data.assign.value);
            collect_reads(stmt->data.assign.value, &env);
            /* Pointer/deref write might alias anything */
            prop_env_invalidate_all(&env);
            continue;
        }

        /* ---- Blocks: recurse ---- */
        if (stmt->type == AST_BLOCK) {
            o2_propagate_block(stmt);
            prop_env_invalidate_all(&env);
            continue;
        }

        /* ---- Anything else: conservative invalidation ---- */
        prop_env_invalidate_all(&env);
    }
}

/* ================================================================== */
/* -O2 pass: Function Inlining                                        */
/*   Inline small functions (single return expr) at call sites.       */
/* ================================================================== */

#define MAX_INLINE_CANDIDATES 256
#define MAX_INLINE_PARAMS 16

typedef struct {
    const char *name;
    ASTNode   *return_expr;   /* the expression in "return expr;" */
    int        param_count;
    const char *param_names[MAX_INLINE_PARAMS];
    int        inline_hint;   /* from AST: 0=none, 1=inline, 2=always_inline, -1=noinline */
} InlineCandidate;

static InlineCandidate g_inline_cands[MAX_INLINE_CANDIDATES];
static int g_inline_cand_count;

/* Deep-clone an AST expression tree.  Only handles expression nodes
   (the kinds that can appear in a function's return expression). */
static ASTNode *ast_clone_expr(ASTNode *n) {
    if (!n) return NULL;
    ASTNode *c = (ASTNode *)calloc(1, sizeof(ASTNode));
    c->type = n->type;
    c->line = n->line;
    c->resolved_type = n->resolved_type;

    switch (n->type) {
    case AST_INTEGER:
        c->data.integer.value = n->data.integer.value;
        break;
    case AST_FLOAT:
        c->data.float_val.value = n->data.float_val.value;
        break;
    case AST_IDENTIFIER:
        c->data.identifier.name = strdup(n->data.identifier.name);
        break;
    case AST_STRING:
        c->data.string.value = (char *)malloc(n->data.string.length + 1);
        memcpy(c->data.string.value, n->data.string.value, n->data.string.length + 1);
        c->data.string.length = n->data.string.length;
        break;
    case AST_BINARY_EXPR:
        c->data.binary_expr.op    = n->data.binary_expr.op;
        c->data.binary_expr.left  = ast_clone_expr(n->data.binary_expr.left);
        c->data.binary_expr.right = ast_clone_expr(n->data.binary_expr.right);
        break;
    case AST_NEG:
    case AST_NOT:
    case AST_BITWISE_NOT:
    case AST_PRE_INC:
    case AST_PRE_DEC:
    case AST_POST_INC:
    case AST_POST_DEC:
    case AST_DEREF:
    case AST_ADDR_OF:
        c->data.unary.expression = ast_clone_expr(n->data.unary.expression);
        break;
    case AST_CAST:
        c->data.cast.expression  = ast_clone_expr(n->data.cast.expression);
        c->data.cast.target_type = n->data.cast.target_type;
        break;
    case AST_CALL:
        c->data.call.name = strdup(n->data.call.name);
        for (size_t i = 0; i < n->children_count; i++)
            ast_add_child(c, ast_clone_expr(n->children[i]));
        break;
    case AST_MEMBER_ACCESS:
        c->data.member_access.struct_expr = ast_clone_expr(n->data.member_access.struct_expr);
        c->data.member_access.member_name = strdup(n->data.member_access.member_name);
        c->data.member_access.is_arrow    = n->data.member_access.is_arrow;
        break;
    case AST_ARRAY_ACCESS:
        c->data.array_access.array = ast_clone_expr(n->data.array_access.array);
        c->data.array_access.index = ast_clone_expr(n->data.array_access.index);
        break;
    case AST_IF: /* ternary */
        c->data.if_stmt.condition   = ast_clone_expr(n->data.if_stmt.condition);
        c->data.if_stmt.then_branch = ast_clone_expr(n->data.if_stmt.then_branch);
        c->data.if_stmt.else_branch = ast_clone_expr(n->data.if_stmt.else_branch);
        break;
    case AST_ASSIGN:
        c->data.assign.left  = ast_clone_expr(n->data.assign.left);
        c->data.assign.value = ast_clone_expr(n->data.assign.value);
        break;
    default:
        /* unsupported expression kind — copy verbatim (no deep children) */
        c->data = n->data;
        break;
    }
    return c;
}

/* Substitute parameter identifiers with argument expressions (cloned). */
static ASTNode *inline_substitute(ASTNode *expr, int pcnt,
                                  const char **pnames, ASTNode **args) {
    if (!expr) return NULL;

    /* Leaf: identifier matching a parameter? */
    if (expr->type == AST_IDENTIFIER) {
        for (int i = 0; i < pcnt; i++) {
            if (strcmp(expr->data.identifier.name, pnames[i]) == 0) {
                ASTNode *rep = ast_clone_expr(args[i]);
                rep->resolved_type = expr->resolved_type ? expr->resolved_type
                                                         : args[i]->resolved_type;
                return rep;
            }
        }
        return expr;   /* not a parameter — leave as-is */
    }

    /* Recurse into sub-expressions */
    switch (expr->type) {
    case AST_BINARY_EXPR:
        expr->data.binary_expr.left  = inline_substitute(expr->data.binary_expr.left, pcnt, pnames, args);
        expr->data.binary_expr.right = inline_substitute(expr->data.binary_expr.right, pcnt, pnames, args);
        break;
    case AST_NEG: case AST_NOT: case AST_BITWISE_NOT:
    case AST_PRE_INC: case AST_PRE_DEC:
    case AST_POST_INC: case AST_POST_DEC:
    case AST_DEREF: case AST_ADDR_OF:
        expr->data.unary.expression = inline_substitute(expr->data.unary.expression, pcnt, pnames, args);
        break;
    case AST_CAST:
        expr->data.cast.expression = inline_substitute(expr->data.cast.expression, pcnt, pnames, args);
        break;
    case AST_CALL:
        for (size_t i = 0; i < expr->children_count; i++)
            expr->children[i] = inline_substitute(expr->children[i], pcnt, pnames, args);
        break;
    case AST_MEMBER_ACCESS:
        expr->data.member_access.struct_expr =
            inline_substitute(expr->data.member_access.struct_expr, pcnt, pnames, args);
        break;
    case AST_ARRAY_ACCESS:
        expr->data.array_access.array = inline_substitute(expr->data.array_access.array, pcnt, pnames, args);
        expr->data.array_access.index = inline_substitute(expr->data.array_access.index, pcnt, pnames, args);
        break;
    case AST_IF:
        expr->data.if_stmt.condition   = inline_substitute(expr->data.if_stmt.condition, pcnt, pnames, args);
        expr->data.if_stmt.then_branch = inline_substitute(expr->data.if_stmt.then_branch, pcnt, pnames, args);
        expr->data.if_stmt.else_branch = inline_substitute(expr->data.if_stmt.else_branch, pcnt, pnames, args);
        break;
    case AST_ASSIGN:
        expr->data.assign.left  = inline_substitute(expr->data.assign.left, pcnt, pnames, args);
        expr->data.assign.value = inline_substitute(expr->data.assign.value, pcnt, pnames, args);
        break;
    default:
        break;
    }
    return expr;
}

/* Scan program for small inlineable functions (single return expr). */
static void find_inline_candidates(ASTNode *program) {
    g_inline_cand_count = 0;
    for (size_t i = 0; i < program->children_count; i++) {
        ASTNode *fn = program->children[i];
        if (fn->type != AST_FUNCTION || !fn->data.function.body) continue;

        /* __attribute__((noinline)) / __declspec(noinline) — never inline */
        if (fn->data.function.inline_hint == -1) continue;

        ASTNode *body = fn->data.function.body;
        if (body->type != AST_BLOCK || body->children_count != 1) continue;

        ASTNode *stmt = body->children[0];
        if (stmt->type != AST_RETURN || !stmt->data.return_stmt.expression) continue;

        if ((int)fn->children_count > MAX_INLINE_PARAMS) continue;
        if (g_inline_cand_count >= MAX_INLINE_CANDIDATES) break;

        InlineCandidate *c = &g_inline_cands[g_inline_cand_count++];
        c->name        = fn->data.function.name;
        c->return_expr = stmt->data.return_stmt.expression;
        c->param_count = (int)fn->children_count;
        c->inline_hint = fn->data.function.inline_hint;
        for (int j = 0; j < c->param_count; j++) {
            c->param_names[j] = fn->children[j]->data.var_decl.name;
        }
    }
}

/* Look up an inline candidate by name. */
static InlineCandidate *find_inline_cand(const char *name) {
    for (int i = 0; i < g_inline_cand_count; i++) {
        if (strcmp(g_inline_cands[i].name, name) == 0) return &g_inline_cands[i];
    }
    return NULL;
}

/* Try to inline a call expression.  Returns the inlined expression or NULL. */
static ASTNode *try_inline_call(ASTNode *call) {
    if (call->type != AST_CALL) return NULL;
    InlineCandidate *cand = find_inline_cand(call->data.call.name);
    if (!cand) return NULL;
    if ((int)call->children_count != cand->param_count) return NULL;

    /* Safety: do not inline if any argument has side effects (to avoid
       duplication of effects when a param is used more than once). */
    for (size_t i = 0; i < call->children_count; i++) {
        if (has_side_effects(call->children[i])) return NULL;
    }

    /* Clone the return expression and substitute parameters. */
    ASTNode *inlined = ast_clone_expr(cand->return_expr);
    inlined = inline_substitute(inlined, cand->param_count,
                                cand->param_names, call->children);
    /* Preserve the call's resolved type */
    if (call->resolved_type && !inlined->resolved_type)
        inlined->resolved_type = call->resolved_type;

    /* Run O1 optimizations on the inlined result (catch constant folding etc.) */
    inlined = opt_expr(inlined);
    return inlined;
}

/* Walk an expression tree and inline eligible calls in-place. */
static ASTNode *inline_expr(ASTNode *expr) {
    if (!expr) return NULL;

    /* Try to inline this node first */
    if (expr->type == AST_CALL) {
        /* First inline within the call's arguments */
        for (size_t i = 0; i < expr->children_count; i++)
            expr->children[i] = inline_expr(expr->children[i]);
        ASTNode *r = try_inline_call(expr);
        if (r) return inline_expr(r);  /* re-check the result for nested inlines */
        return expr;
    }

    /* Recurse into sub-expressions */
    switch (expr->type) {
    case AST_BINARY_EXPR:
        expr->data.binary_expr.left  = inline_expr(expr->data.binary_expr.left);
        expr->data.binary_expr.right = inline_expr(expr->data.binary_expr.right);
        break;
    case AST_NEG: case AST_NOT: case AST_BITWISE_NOT:
    case AST_PRE_INC: case AST_PRE_DEC:
    case AST_POST_INC: case AST_POST_DEC:
    case AST_DEREF: case AST_ADDR_OF:
        expr->data.unary.expression = inline_expr(expr->data.unary.expression);
        break;
    case AST_CAST:
        expr->data.cast.expression = inline_expr(expr->data.cast.expression);
        break;
    case AST_MEMBER_ACCESS:
        expr->data.member_access.struct_expr = inline_expr(expr->data.member_access.struct_expr);
        break;
    case AST_ARRAY_ACCESS:
        expr->data.array_access.array = inline_expr(expr->data.array_access.array);
        expr->data.array_access.index = inline_expr(expr->data.array_access.index);
        break;
    case AST_IF:
        expr->data.if_stmt.condition   = inline_expr(expr->data.if_stmt.condition);
        expr->data.if_stmt.then_branch = inline_expr(expr->data.if_stmt.then_branch);
        expr->data.if_stmt.else_branch = inline_expr(expr->data.if_stmt.else_branch);
        break;
    case AST_ASSIGN:
        expr->data.assign.left  = inline_expr(expr->data.assign.left);
        expr->data.assign.value = inline_expr(expr->data.assign.value);
        break;
    default:
        break;
    }
    return expr;
}

/* Walk a statement tree and inline calls in all expressions. */
static void inline_stmt(ASTNode *stmt) {
    if (!stmt) return;
    switch (stmt->type) {
    case AST_BLOCK:
        for (size_t i = 0; i < stmt->children_count; i++)
            inline_stmt(stmt->children[i]);
        break;
    case AST_RETURN:
        if (stmt->data.return_stmt.expression)
            stmt->data.return_stmt.expression = inline_expr(stmt->data.return_stmt.expression);
        break;
    case AST_VAR_DECL:
        if (stmt->data.var_decl.initializer)
            stmt->data.var_decl.initializer = inline_expr(stmt->data.var_decl.initializer);
        break;
    case AST_ASSIGN:
        stmt->data.assign.left  = inline_expr(stmt->data.assign.left);
        stmt->data.assign.value = inline_expr(stmt->data.assign.value);
        break;
    case AST_IF:
        stmt->data.if_stmt.condition = inline_expr(stmt->data.if_stmt.condition);
        inline_stmt(stmt->data.if_stmt.then_branch);
        if (stmt->data.if_stmt.else_branch)
            inline_stmt(stmt->data.if_stmt.else_branch);
        break;
    case AST_WHILE:
        stmt->data.while_stmt.condition = inline_expr(stmt->data.while_stmt.condition);
        inline_stmt(stmt->data.while_stmt.body);
        break;
    case AST_DO_WHILE:
        stmt->data.while_stmt.condition = inline_expr(stmt->data.while_stmt.condition);
        inline_stmt(stmt->data.while_stmt.body);
        break;
    case AST_FOR:
        if (stmt->data.for_stmt.init) inline_stmt(stmt->data.for_stmt.init);
        if (stmt->data.for_stmt.condition)
            stmt->data.for_stmt.condition = inline_expr(stmt->data.for_stmt.condition);
        if (stmt->data.for_stmt.increment)
            stmt->data.for_stmt.increment = inline_expr(stmt->data.for_stmt.increment);
        inline_stmt(stmt->data.for_stmt.body);
        break;
    case AST_SWITCH:
        stmt->data.switch_stmt.condition = inline_expr(stmt->data.switch_stmt.condition);
        inline_stmt(stmt->data.switch_stmt.body);
        break;
    default:
        /* Expression statements (bare calls, increments, etc.) */
        if (stmt->type == AST_CALL) {
            for (size_t i = 0; i < stmt->children_count; i++)
                stmt->children[i] = inline_expr(stmt->children[i]);
            /* Don't inline statement-level calls (value is discarded) */
        }
        break;
    }
}

/* ================================================================== */
/* -O3: Aggressive Optimizations                                      */
/* ================================================================== */

/* ------------------------------------------------------------------ */
/* Deep-clone a statement tree (for loop unrolling & aggressive inline)*/
/* ------------------------------------------------------------------ */
static ASTNode *ast_clone_stmt(ASTNode *n);

static ASTNode *ast_clone_stmt(ASTNode *n) {
    if (!n) return NULL;
    ASTNode *c = (ASTNode *)calloc(1, sizeof(ASTNode));
    c->type = n->type;
    c->line = n->line;
    c->resolved_type = n->resolved_type;

    switch (n->type) {
    case AST_BLOCK:
        for (size_t i = 0; i < n->children_count; i++)
            ast_add_child(c, ast_clone_stmt(n->children[i]));
        break;
    case AST_RETURN:
        c->data.return_stmt.expression = n->data.return_stmt.expression
            ? ast_clone_expr(n->data.return_stmt.expression) : NULL;
        break;
    case AST_VAR_DECL:
        c->data.var_decl.name = strdup(n->data.var_decl.name);
        c->data.var_decl.initializer = n->data.var_decl.initializer
            ? ast_clone_expr(n->data.var_decl.initializer) : NULL;
        c->data.var_decl.is_static = n->data.var_decl.is_static;
        c->data.var_decl.is_extern = n->data.var_decl.is_extern;
        break;
    case AST_ASSIGN:
        c->data.assign.left  = ast_clone_expr(n->data.assign.left);
        c->data.assign.value = ast_clone_expr(n->data.assign.value);
        break;
    case AST_IF:
        c->data.if_stmt.condition   = ast_clone_expr(n->data.if_stmt.condition);
        c->data.if_stmt.then_branch = ast_clone_stmt(n->data.if_stmt.then_branch);
        c->data.if_stmt.else_branch = n->data.if_stmt.else_branch
            ? ast_clone_stmt(n->data.if_stmt.else_branch) : NULL;
        break;
    case AST_WHILE:
    case AST_DO_WHILE:
        c->data.while_stmt.condition = ast_clone_expr(n->data.while_stmt.condition);
        c->data.while_stmt.body = ast_clone_stmt(n->data.while_stmt.body);
        break;
    case AST_FOR:
        c->data.for_stmt.init = n->data.for_stmt.init
            ? ast_clone_stmt(n->data.for_stmt.init) : NULL;
        c->data.for_stmt.condition = n->data.for_stmt.condition
            ? ast_clone_expr(n->data.for_stmt.condition) : NULL;
        c->data.for_stmt.increment = n->data.for_stmt.increment
            ? ast_clone_expr(n->data.for_stmt.increment) : NULL;
        c->data.for_stmt.body = ast_clone_stmt(n->data.for_stmt.body);
        break;
    case AST_SWITCH:
        c->data.switch_stmt.condition = ast_clone_expr(n->data.switch_stmt.condition);
        c->data.switch_stmt.body = ast_clone_stmt(n->data.switch_stmt.body);
        break;
    case AST_CASE:
        c->data.case_stmt.value = n->data.case_stmt.value;
        break;
    case AST_DEFAULT:
    case AST_BREAK:
    case AST_CONTINUE:
        break;
    case AST_GOTO:
        c->data.goto_stmt.label = strdup(n->data.goto_stmt.label);
        break;
    case AST_LABEL:
        c->data.label_stmt.name = strdup(n->data.label_stmt.name);
        break;
    default:
        /* Expression-as-statement (calls, increments, etc.) — clone as expression */
        {
            ASTNode *tmp = ast_clone_expr(n);
            *c = *tmp;
            free(tmp);
        }
        break;
    }
    return c;
}

/* Substitute parameter names with argument expressions in a statement tree.
   Used by aggressive inlining. */
static void inline_substitute_stmt(ASTNode *stmt, int pcnt,
                                    const char **pnames, ASTNode **args) {
    if (!stmt) return;
    switch (stmt->type) {
    case AST_BLOCK:
        for (size_t i = 0; i < stmt->children_count; i++)
            inline_substitute_stmt(stmt->children[i], pcnt, pnames, args);
        break;
    case AST_RETURN:
        if (stmt->data.return_stmt.expression)
            stmt->data.return_stmt.expression =
                inline_substitute(stmt->data.return_stmt.expression, pcnt, pnames, args);
        break;
    case AST_VAR_DECL:
        if (stmt->data.var_decl.initializer)
            stmt->data.var_decl.initializer =
                inline_substitute(stmt->data.var_decl.initializer, pcnt, pnames, args);
        break;
    case AST_ASSIGN:
        stmt->data.assign.left =
            inline_substitute(stmt->data.assign.left, pcnt, pnames, args);
        stmt->data.assign.value =
            inline_substitute(stmt->data.assign.value, pcnt, pnames, args);
        break;
    case AST_IF:
        stmt->data.if_stmt.condition =
            inline_substitute(stmt->data.if_stmt.condition, pcnt, pnames, args);
        inline_substitute_stmt(stmt->data.if_stmt.then_branch, pcnt, pnames, args);
        if (stmt->data.if_stmt.else_branch)
            inline_substitute_stmt(stmt->data.if_stmt.else_branch, pcnt, pnames, args);
        break;
    case AST_WHILE:
    case AST_DO_WHILE:
        stmt->data.while_stmt.condition =
            inline_substitute(stmt->data.while_stmt.condition, pcnt, pnames, args);
        inline_substitute_stmt(stmt->data.while_stmt.body, pcnt, pnames, args);
        break;
    case AST_FOR:
        if (stmt->data.for_stmt.init)
            inline_substitute_stmt(stmt->data.for_stmt.init, pcnt, pnames, args);
        if (stmt->data.for_stmt.condition)
            stmt->data.for_stmt.condition =
                inline_substitute(stmt->data.for_stmt.condition, pcnt, pnames, args);
        if (stmt->data.for_stmt.increment)
            stmt->data.for_stmt.increment =
                inline_substitute(stmt->data.for_stmt.increment, pcnt, pnames, args);
        inline_substitute_stmt(stmt->data.for_stmt.body, pcnt, pnames, args);
        break;
    case AST_SWITCH:
        stmt->data.switch_stmt.condition =
            inline_substitute(stmt->data.switch_stmt.condition, pcnt, pnames, args);
        inline_substitute_stmt(stmt->data.switch_stmt.body, pcnt, pnames, args);
        break;
    default:
        /* Expression-statement: substitute in-place */
        {
            ASTNode *r = inline_substitute(stmt, pcnt, pnames, args);
            if (r != stmt) *stmt = *r;
        }
        break;
    }
}

/* ------------------------------------------------------------------ */
/* O3 Pass 1: Aggressive inlining of multi-statement functions        */
/*                                                                    */
/* For functions with up to MAX_AGGRESSIVE_INLINE_STMTS statements    */
/* (where the last is a return), inline the body at call sites by     */
/* injecting variable declarations and assignments before the call,   */
/* then replacing the call with the return expression.                */
/* ------------------------------------------------------------------ */

#define MAX_AGGRESSIVE_INLINE_STMTS 20
#define MAX_AGGRESSIVE_INLINE_CANDIDATES 256

typedef struct {
    const char *name;
    ASTNode    *body;           /* the function body (AST_BLOCK) */
    int         param_count;
    const char *param_names[MAX_INLINE_PARAMS];
    int         inline_hint;
    int         stmt_count;     /* number of statements in body */
} AggressiveInlineCandidate;

static AggressiveInlineCandidate g_agg_inline_cands[MAX_AGGRESSIVE_INLINE_CANDIDATES];
static int g_agg_inline_cand_count;
static int g_agg_inline_counter;   /* unique suffix counter for inlined variables */

/* Check if a function body is safe to aggressively inline:
   - No goto/label (would break with statement injection)
   - No nested function definitions
   - Must end with a return statement
   - Body must be a block with ≤ MAX_AGGRESSIVE_INLINE_STMTS statements */
/* Recursively check if a statement subtree contains any AST_RETURN. */
static int stmt_contains_return(ASTNode *s) {
    if (!s) return 0;
    if (s->type == AST_RETURN) return 1;
    if (s->type == AST_BLOCK) {
        for (size_t i = 0; i < s->children_count; i++)
            if (stmt_contains_return(s->children[i])) return 1;
        return 0;
    }
    if (s->type == AST_IF)
        return stmt_contains_return(s->data.if_stmt.then_branch) ||
               stmt_contains_return(s->data.if_stmt.else_branch);
    if (s->type == AST_WHILE || s->type == AST_DO_WHILE)
        return stmt_contains_return(s->data.while_stmt.body);
    if (s->type == AST_FOR)
        return stmt_contains_return(s->data.for_stmt.init) ||
               stmt_contains_return(s->data.for_stmt.body);
    if (s->type == AST_SWITCH)
        return stmt_contains_return(s->data.switch_stmt.body);
    return 0;
}

static int is_safe_for_aggressive_inline(ASTNode *body) {
    if (!body || body->type != AST_BLOCK) return 0;
    if (body->children_count == 0) return 0;
    if (body->children_count > MAX_AGGRESSIVE_INLINE_STMTS) return 0;

    /* Last statement must be a return with an expression */
    ASTNode *last = body->children[body->children_count - 1];
    if (last->type != AST_RETURN || !last->data.return_stmt.expression) return 0;

    /* Check all statements for illegal constructs */
    for (size_t i = 0; i < body->children_count; i++) {
        ASTNode *s = body->children[i];
        /* No goto/label (would create cross-function jumps) */
        if (s->type == AST_GOTO || s->type == AST_LABEL) return 0;
        /* No break/continue at top level (would escape inline block) */
        if (s->type == AST_BREAK || s->type == AST_CONTINUE) return 0;
        /* No nested returns except the last statement — including returns
           buried inside if/while/for/switch branches, which would become
           returns from the caller function after inlining. */
        if (s->type == AST_RETURN && i != body->children_count - 1) return 0;
        if (s->type != AST_RETURN && stmt_contains_return(s)) return 0;
    }
    return 1;
}

/* Find candidates for aggressive inlining (multi-statement functions). */
static void find_aggressive_inline_candidates(ASTNode *program) {
    g_agg_inline_cand_count = 0;
    g_agg_inline_counter = 0;

    for (size_t i = 0; i < program->children_count; i++) {
        ASTNode *fn = program->children[i];
        if (fn->type != AST_FUNCTION || !fn->data.function.body) continue;
        if (fn->data.function.inline_hint == -1) continue; /* noinline */

        ASTNode *body = fn->data.function.body;

        /* Skip single-statement functions — already handled by O2 inliner */
        if (body->type == AST_BLOCK && body->children_count == 1) continue;

        if (!is_safe_for_aggressive_inline(body)) continue;
        if ((int)fn->children_count > MAX_INLINE_PARAMS) continue;
        if (g_agg_inline_cand_count >= MAX_AGGRESSIVE_INLINE_CANDIDATES) break;

        AggressiveInlineCandidate *c = &g_agg_inline_cands[g_agg_inline_cand_count++];
        c->name = fn->data.function.name;
        c->body = body;
        c->param_count = (int)fn->children_count;
        c->inline_hint = fn->data.function.inline_hint;
        c->stmt_count = (int)body->children_count;
        for (int j = 0; j < c->param_count; j++) {
            c->param_names[j] = fn->children[j]->data.var_decl.name;
        }
    }
}

/* Find aggressive inline candidate by name. */
static AggressiveInlineCandidate *find_agg_inline_cand(const char *name) {
    for (int i = 0; i < g_agg_inline_cand_count; i++) {
        if (strcmp(g_agg_inline_cands[i].name, name) == 0)
            return &g_agg_inline_cands[i];
    }
    return NULL;
}

/* Rename local variables in cloned body to avoid name collisions.
   Appends _inlN suffix to all var_decl names and their references. */
static void rename_inline_locals(ASTNode *stmt, const char **old_names,
                                  const char **new_names, int name_count) {
    if (!stmt) return;
    switch (stmt->type) {
    case AST_IDENTIFIER:
        for (int i = 0; i < name_count; i++) {
            if (strcmp(stmt->data.identifier.name, old_names[i]) == 0) {
                stmt->data.identifier.name = strdup(new_names[i]);
                return;
            }
        }
        return;
    case AST_VAR_DECL:
        for (int i = 0; i < name_count; i++) {
            if (strcmp(stmt->data.var_decl.name, old_names[i]) == 0) {
                stmt->data.var_decl.name = strdup(new_names[i]);
                break;
            }
        }
        if (stmt->data.var_decl.initializer)
            rename_inline_locals(stmt->data.var_decl.initializer, old_names, new_names, name_count);
        return;
    case AST_BLOCK:
        for (size_t i = 0; i < stmt->children_count; i++)
            rename_inline_locals(stmt->children[i], old_names, new_names, name_count);
        return;
    case AST_RETURN:
        if (stmt->data.return_stmt.expression)
            rename_inline_locals(stmt->data.return_stmt.expression, old_names, new_names, name_count);
        return;
    case AST_ASSIGN:
        rename_inline_locals(stmt->data.assign.left, old_names, new_names, name_count);
        rename_inline_locals(stmt->data.assign.value, old_names, new_names, name_count);
        return;
    case AST_BINARY_EXPR:
        rename_inline_locals(stmt->data.binary_expr.left, old_names, new_names, name_count);
        rename_inline_locals(stmt->data.binary_expr.right, old_names, new_names, name_count);
        return;
    case AST_NEG: case AST_NOT: case AST_BITWISE_NOT:
    case AST_PRE_INC: case AST_PRE_DEC:
    case AST_POST_INC: case AST_POST_DEC:
    case AST_DEREF: case AST_ADDR_OF:
        rename_inline_locals(stmt->data.unary.expression, old_names, new_names, name_count);
        return;
    case AST_CAST:
        rename_inline_locals(stmt->data.cast.expression, old_names, new_names, name_count);
        return;
    case AST_CALL:
        for (size_t i = 0; i < stmt->children_count; i++)
            rename_inline_locals(stmt->children[i], old_names, new_names, name_count);
        return;
    case AST_MEMBER_ACCESS:
        rename_inline_locals(stmt->data.member_access.struct_expr, old_names, new_names, name_count);
        return;
    case AST_ARRAY_ACCESS:
        rename_inline_locals(stmt->data.array_access.array, old_names, new_names, name_count);
        rename_inline_locals(stmt->data.array_access.index, old_names, new_names, name_count);
        return;
    case AST_IF:
        rename_inline_locals(stmt->data.if_stmt.condition, old_names, new_names, name_count);
        rename_inline_locals(stmt->data.if_stmt.then_branch, old_names, new_names, name_count);
        if (stmt->data.if_stmt.else_branch)
            rename_inline_locals(stmt->data.if_stmt.else_branch, old_names, new_names, name_count);
        return;
    case AST_WHILE: case AST_DO_WHILE:
        rename_inline_locals(stmt->data.while_stmt.condition, old_names, new_names, name_count);
        rename_inline_locals(stmt->data.while_stmt.body, old_names, new_names, name_count);
        return;
    case AST_FOR:
        if (stmt->data.for_stmt.init)
            rename_inline_locals(stmt->data.for_stmt.init, old_names, new_names, name_count);
        if (stmt->data.for_stmt.condition)
            rename_inline_locals(stmt->data.for_stmt.condition, old_names, new_names, name_count);
        if (stmt->data.for_stmt.increment)
            rename_inline_locals(stmt->data.for_stmt.increment, old_names, new_names, name_count);
        rename_inline_locals(stmt->data.for_stmt.body, old_names, new_names, name_count);
        return;
    case AST_SWITCH:
        rename_inline_locals(stmt->data.switch_stmt.condition, old_names, new_names, name_count);
        rename_inline_locals(stmt->data.switch_stmt.body, old_names, new_names, name_count);
        return;
    default:
        return;
    }
}

/* Collect var_decl names in a function body (for renaming). */
static int collect_local_names(ASTNode *stmt, const char **names, int max_names) {
    int count = 0;
    if (!stmt) return 0;
    if (stmt->type == AST_VAR_DECL) {
        if (count < max_names)
            names[count++] = stmt->data.var_decl.name;
        return count;
    }
    if (stmt->type == AST_BLOCK) {
        for (size_t i = 0; i < stmt->children_count; i++) {
            int n = collect_local_names(stmt->children[i], names + count, max_names - count);
            count += n;
            if (count >= max_names) return count;
        }
    }
    if (stmt->type == AST_FOR && stmt->data.for_stmt.init) {
        int n = collect_local_names(stmt->data.for_stmt.init, names + count, max_names - count);
        count += n;
    }
    return count;
}

/* Try aggressive inline of a call within a block.
   Returns the number of statements injected (0 if not inlined).
   The call expression is replaced with a reference to the result variable. */
static int try_aggressive_inline_in_block(ASTNode *block, size_t stmt_idx,
                                           ASTNode **call_ref) {
    ASTNode *call = *call_ref;
    if (!call || call->type != AST_CALL) return 0;

    AggressiveInlineCandidate *cand = find_agg_inline_cand(call->data.call.name);
    if (!cand) return 0;
    if ((int)call->children_count != cand->param_count) return 0;

    /* Safety: don't inline if args have side effects */
    for (size_t i = 0; i < call->children_count; i++) {
        if (has_side_effects(call->children[i])) return 0;
    }

    int suffix = g_agg_inline_counter++;

    /* Clone the function body */
    ASTNode *cloned_body = ast_clone_stmt(cand->body);

    /* Substitute parameters with argument expressions */
    inline_substitute_stmt(cloned_body, cand->param_count,
                           cand->param_names, call->children);

    /* Collect and rename local variables to avoid collisions */
    const char *local_names[128];
    int local_count = collect_local_names(cloned_body, local_names, 128);
    if (local_count > 0) {
        const char *new_names[128];
        for (int i = 0; i < local_count; i++) {
            char buf[256];
            sprintf(buf, "%s_inl%d", local_names[i], suffix);
            new_names[i] = strdup(buf);
        }
        rename_inline_locals(cloned_body, local_names, new_names, local_count);
    }

    /* The last statement is "return expr;" — extract the return expression */
    ASTNode *last_stmt = cloned_body->children[cloned_body->children_count - 1];
    ASTNode *return_expr_node = last_stmt->data.return_stmt.expression;

    /* Number of statements to inject (everything except the return) */
    int inject_count = (int)cloned_body->children_count - 1;

    /* Expand the block's children array to make room */
    if (inject_count > 0) {
        size_t new_count = block->children_count + inject_count;
        ASTNode **new_children = (ASTNode **)malloc(new_count * sizeof(ASTNode *));

        /* Copy children before stmt_idx */
        for (size_t i = 0; i < stmt_idx; i++)
            new_children[i] = block->children[i];
        /* Insert the cloned body statements (minus return) */
        for (int i = 0; i < inject_count; i++)
            new_children[stmt_idx + i] = cloned_body->children[i];
        /* Copy stmt_idx onward (shifted) */
        for (size_t i = stmt_idx; i < block->children_count; i++)
            new_children[i + inject_count] = block->children[i];

        free(block->children);
        block->children = new_children;
        block->children_count = new_count;
    }

    /* Replace the call expr with the return expression */
    *call_ref = return_expr_node;

    return inject_count;
}

/* Walk a block and aggressively inline calls in var_decl initializers,
   assignments, and return statements. */
static void o3_aggressive_inline_block(ASTNode *block) {
    if (!block || block->type != AST_BLOCK) return;

    for (size_t i = 0; i < block->children_count; i++) {
        ASTNode *stmt = block->children[i];
        if (!stmt) continue;

        /* Recurse into sub-blocks first */
        if (stmt->type == AST_BLOCK) {
            o3_aggressive_inline_block(stmt);
            continue;
        }
        if (stmt->type == AST_IF) {
            if (stmt->data.if_stmt.then_branch)
                o3_aggressive_inline_block(stmt->data.if_stmt.then_branch);
            if (stmt->data.if_stmt.else_branch)
                o3_aggressive_inline_block(stmt->data.if_stmt.else_branch);
            continue;
        }
        if (stmt->type == AST_WHILE || stmt->type == AST_DO_WHILE) {
            if (stmt->data.while_stmt.body)
                o3_aggressive_inline_block(stmt->data.while_stmt.body);
            continue;
        }
        if (stmt->type == AST_FOR) {
            if (stmt->data.for_stmt.body)
                o3_aggressive_inline_block(stmt->data.for_stmt.body);
            continue;
        }
        if (stmt->type == AST_SWITCH) {
            if (stmt->data.switch_stmt.body)
                o3_aggressive_inline_block(stmt->data.switch_stmt.body);
            continue;
        }

        /* Try to inline in var_decl initializer */
        if (stmt->type == AST_VAR_DECL && stmt->data.var_decl.initializer &&
            stmt->data.var_decl.initializer->type == AST_CALL) {
            int injected = try_aggressive_inline_in_block(
                block, i, &stmt->data.var_decl.initializer);
            if (injected > 0) {
                i += injected; /* skip past injected statements */
                continue;
            }
        }

        /* Try to inline in assignment RHS */
        if (stmt->type == AST_ASSIGN && stmt->data.assign.value &&
            stmt->data.assign.value->type == AST_CALL) {
            int injected = try_aggressive_inline_in_block(
                block, i, &stmt->data.assign.value);
            if (injected > 0) {
                i += injected;
                continue;
            }
        }

        /* Try to inline in return expression */
        if (stmt->type == AST_RETURN && stmt->data.return_stmt.expression &&
            stmt->data.return_stmt.expression->type == AST_CALL) {
            int injected = try_aggressive_inline_in_block(
                block, i, &stmt->data.return_stmt.expression);
            if (injected > 0) {
                i += injected;
                continue;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* O3 Pass 2: Loop Unrolling                                          */
/*                                                                    */
/* Detects for-loops of the form:                                     */
/*   for (int i = A; i < B; i++ / i = i + 1)  body                   */
/* where A, B are compile-time constants.                             */
/*                                                                    */
/* Full unroll for N = B - A ≤ 8, partial unroll (factor 2-4) for     */
/* larger known counts.                                               */
/* ------------------------------------------------------------------ */

/* Check if a for-loop has the pattern:
   init: var_decl (int i = A) or assign (i = A)
   condition: i < B  or  i <= B  or  i != B
   increment: i++ or i = i + 1 or i += 1
   Returns 1 and fills out_var, out_start, out_end, out_iterations. */
static int analyze_for_loop(ASTNode *for_node,
                             const char **out_var,
                             long long *out_start,
                             long long *out_end,
                             long long *out_iterations) {
    if (!for_node || for_node->type != AST_FOR) return 0;
    ASTNode *init = for_node->data.for_stmt.init;
    ASTNode *cond = for_node->data.for_stmt.condition;
    ASTNode *incr = for_node->data.for_stmt.increment;

    if (!init || !cond || !incr) return 0;

    /* Extract loop variable and start value from init */
    const char *var_name = NULL;
    long long start_val = 0;

    if (init->type == AST_VAR_DECL && init->data.var_decl.initializer &&
        is_const_int(init->data.var_decl.initializer)) {
        var_name = init->data.var_decl.name;
        start_val = init->data.var_decl.initializer->data.integer.value;
    } else if (init->type == AST_ASSIGN &&
               init->data.assign.left &&
               init->data.assign.left->type == AST_IDENTIFIER &&
               is_const_int(init->data.assign.value)) {
        var_name = init->data.assign.left->data.identifier.name;
        start_val = init->data.assign.value->data.integer.value;
    }
    if (!var_name) return 0;

    /* Extract end value from condition: var < B, var <= B, var != B */
    if (cond->type != AST_BINARY_EXPR) return 0;
    ASTNode *cond_left = cond->data.binary_expr.left;
    ASTNode *cond_right = cond->data.binary_expr.right;
    TokenType cond_op = cond->data.binary_expr.op;

    if (!cond_left || cond_left->type != AST_IDENTIFIER) return 0;
    if (strcmp(cond_left->data.identifier.name, var_name) != 0) return 0;
    if (!is_const_int(cond_right)) return 0;

    long long end_val = cond_right->data.integer.value;
    long long iterations;

    if (cond_op == TOKEN_LESS) {
        iterations = end_val - start_val;
    } else if (cond_op == TOKEN_LESS_EQUAL) {
        iterations = end_val - start_val + 1;
    } else if (cond_op == TOKEN_BANG_EQUAL) {
        iterations = end_val - start_val;
    } else {
        return 0;
    }

    if (iterations <= 0) return 0;

    /* Check increment is i++ or i = i + 1 */
    if (incr->type == AST_POST_INC || incr->type == AST_PRE_INC) {
        if (!incr->data.unary.expression ||
            incr->data.unary.expression->type != AST_IDENTIFIER ||
            strcmp(incr->data.unary.expression->data.identifier.name, var_name) != 0)
            return 0;
    } else if (incr->type == AST_ASSIGN) {
        /* i = i + 1 */
        if (!incr->data.assign.left ||
            incr->data.assign.left->type != AST_IDENTIFIER ||
            strcmp(incr->data.assign.left->data.identifier.name, var_name) != 0)
            return 0;
        ASTNode *rhs = incr->data.assign.value;
        if (!rhs || rhs->type != AST_BINARY_EXPR ||
            rhs->data.binary_expr.op != TOKEN_PLUS)
            return 0;
        /* Check: i + 1  or  1 + i */
        ASTNode *rl = rhs->data.binary_expr.left;
        ASTNode *rr = rhs->data.binary_expr.right;
        int ok = 0;
        if (rl->type == AST_IDENTIFIER &&
            strcmp(rl->data.identifier.name, var_name) == 0 &&
            is_const_int(rr) && rr->data.integer.value == 1)
            ok = 1;
        if (rr->type == AST_IDENTIFIER &&
            strcmp(rr->data.identifier.name, var_name) == 0 &&
            is_const_int(rl) && rl->data.integer.value == 1)
            ok = 1;
        if (!ok) return 0;
    } else {
        return 0;
    }

    *out_var = var_name;
    *out_start = start_val;
    *out_end = end_val;
    *out_iterations = iterations;
    return 1;
}

/* Replace all occurrences of var_name identifier with a constant value in expr */
static void subst_loop_var(ASTNode *node, const char *var_name, long long value) {
    if (!node) return;
    switch (node->type) {
    case AST_IDENTIFIER:
        if (strcmp(node->data.identifier.name, var_name) == 0) {
            node->type = AST_INTEGER;
            node->data.integer.value = value;
        }
        return;
    case AST_BINARY_EXPR:
        subst_loop_var(node->data.binary_expr.left, var_name, value);
        subst_loop_var(node->data.binary_expr.right, var_name, value);
        return;
    case AST_NEG: case AST_NOT: case AST_BITWISE_NOT:
    case AST_PRE_INC: case AST_PRE_DEC:
    case AST_POST_INC: case AST_POST_DEC:
    case AST_DEREF: case AST_ADDR_OF:
        subst_loop_var(node->data.unary.expression, var_name, value);
        return;
    case AST_CAST:
        subst_loop_var(node->data.cast.expression, var_name, value);
        return;
    case AST_CALL:
        for (size_t i = 0; i < node->children_count; i++)
            subst_loop_var(node->children[i], var_name, value);
        return;
    case AST_MEMBER_ACCESS:
        subst_loop_var(node->data.member_access.struct_expr, var_name, value);
        return;
    case AST_ARRAY_ACCESS:
        subst_loop_var(node->data.array_access.array, var_name, value);
        subst_loop_var(node->data.array_access.index, var_name, value);
        return;
    case AST_ASSIGN:
        subst_loop_var(node->data.assign.left, var_name, value);
        subst_loop_var(node->data.assign.value, var_name, value);
        return;
    case AST_BLOCK:
        for (size_t i = 0; i < node->children_count; i++)
            subst_loop_var(node->children[i], var_name, value);
        return;
    case AST_RETURN:
        if (node->data.return_stmt.expression)
            subst_loop_var(node->data.return_stmt.expression, var_name, value);
        return;
    case AST_VAR_DECL:
        if (node->data.var_decl.initializer)
            subst_loop_var(node->data.var_decl.initializer, var_name, value);
        return;
    case AST_IF:
        subst_loop_var(node->data.if_stmt.condition, var_name, value);
        subst_loop_var(node->data.if_stmt.then_branch, var_name, value);
        if (node->data.if_stmt.else_branch)
            subst_loop_var(node->data.if_stmt.else_branch, var_name, value);
        return;
    case AST_WHILE: case AST_DO_WHILE:
        subst_loop_var(node->data.while_stmt.condition, var_name, value);
        subst_loop_var(node->data.while_stmt.body, var_name, value);
        return;
    case AST_FOR:
        if (node->data.for_stmt.init)
            subst_loop_var(node->data.for_stmt.init, var_name, value);
        if (node->data.for_stmt.condition)
            subst_loop_var(node->data.for_stmt.condition, var_name, value);
        if (node->data.for_stmt.increment)
            subst_loop_var(node->data.for_stmt.increment, var_name, value);
        subst_loop_var(node->data.for_stmt.body, var_name, value);
        return;
    default:
        return;
    }
}

/* Check if loop body contains break/continue/goto/return that would
   complicate unrolling. */
static int body_has_flow_control(ASTNode *node) {
    if (!node) return 0;
    if (node->type == AST_BREAK || node->type == AST_CONTINUE ||
        node->type == AST_GOTO || node->type == AST_RETURN)
        return 1;
    if (node->type == AST_BLOCK) {
        for (size_t i = 0; i < node->children_count; i++)
            if (body_has_flow_control(node->children[i])) return 1;
    }
    if (node->type == AST_IF) {
        if (body_has_flow_control(node->data.if_stmt.then_branch)) return 1;
        if (body_has_flow_control(node->data.if_stmt.else_branch)) return 1;
    }
    /* Don't recurse into nested loops — break/continue in inner loops is OK */
    return 0;
}

/* Count AST nodes in a subtree (rough cost estimate for unrolling). */
static int count_ast_nodes(ASTNode *node) {
    if (!node) return 0;
    int count = 1;
    if (node->type == AST_BLOCK) {
        for (size_t i = 0; i < node->children_count; i++)
            count += count_ast_nodes(node->children[i]);
    }
    switch (node->type) {
    case AST_BINARY_EXPR:
        count += count_ast_nodes(node->data.binary_expr.left);
        count += count_ast_nodes(node->data.binary_expr.right);
        break;
    case AST_NEG: case AST_NOT: case AST_BITWISE_NOT:
    case AST_PRE_INC: case AST_PRE_DEC:
    case AST_POST_INC: case AST_POST_DEC:
    case AST_DEREF: case AST_ADDR_OF:
        count += count_ast_nodes(node->data.unary.expression);
        break;
    case AST_CAST:
        count += count_ast_nodes(node->data.cast.expression);
        break;
    case AST_CALL:
        for (size_t i = 0; i < node->children_count; i++)
            count += count_ast_nodes(node->children[i]);
        break;
    case AST_ASSIGN:
        count += count_ast_nodes(node->data.assign.left);
        count += count_ast_nodes(node->data.assign.value);
        break;
    case AST_RETURN:
        count += count_ast_nodes(node->data.return_stmt.expression);
        break;
    case AST_VAR_DECL:
        count += count_ast_nodes(node->data.var_decl.initializer);
        break;
    case AST_ARRAY_ACCESS:
        count += count_ast_nodes(node->data.array_access.array);
        count += count_ast_nodes(node->data.array_access.index);
        break;
    case AST_MEMBER_ACCESS:
        count += count_ast_nodes(node->data.member_access.struct_expr);
        break;
    case AST_IF:
        count += count_ast_nodes(node->data.if_stmt.condition);
        count += count_ast_nodes(node->data.if_stmt.then_branch);
        count += count_ast_nodes(node->data.if_stmt.else_branch);
        break;
    case AST_WHILE: case AST_DO_WHILE:
        count += count_ast_nodes(node->data.while_stmt.condition);
        count += count_ast_nodes(node->data.while_stmt.body);
        break;
    case AST_FOR:
        count += count_ast_nodes(node->data.for_stmt.init);
        count += count_ast_nodes(node->data.for_stmt.condition);
        count += count_ast_nodes(node->data.for_stmt.increment);
        count += count_ast_nodes(node->data.for_stmt.body);
        break;
    default:
        break;
    }
    return count;
}

/* Fully unroll a for-loop: replace with a block of cloned bodies.
   Returns the replacement block, or NULL if not unrolled. */
static ASTNode *try_full_unroll(ASTNode *for_node) {
    const char *var_name;
    long long start_val, end_val, iterations;

    if (!analyze_for_loop(for_node, &var_name, &start_val, &end_val, &iterations))
        return NULL;

    /* Full unroll threshold: N ≤ 8 */
    if (iterations > 8 || iterations <= 0) return NULL;

    ASTNode *body = for_node->data.for_stmt.body;
    if (!body) return NULL;

    /* Don't unroll if body has break/continue/goto/return */
    if (body_has_flow_control(body)) return NULL;

    /* Don't unroll if body is too large (> 50 nodes per iteration) */
    if (count_ast_nodes(body) > 50) return NULL;

    /* Create a block with N copies of the body, each with i substituted */
    ASTNode *result = ast_create_node(AST_BLOCK);

    /* Keep the init statement (for variable declaration) */
    if (for_node->data.for_stmt.init)
        ast_add_child(result, ast_clone_stmt(for_node->data.for_stmt.init));

    for (long long iter = start_val; iter < start_val + iterations; iter++) {
        ASTNode *copy = ast_clone_stmt(body);
        subst_loop_var(copy, var_name, iter);
        /* Run constant folding on the substituted copy */
        opt_stmt(copy);
        /* If body is a block, flatten its children into result */
        if (copy->type == AST_BLOCK) {
            for (size_t j = 0; j < copy->children_count; j++)
                ast_add_child(result, copy->children[j]);
        } else {
            ast_add_child(result, copy);
        }
    }

    return result;
}

/* Partial unroll: unroll loop body by factor F (2 or 4).
   Creates:  for (i = start; i < end - (end-start)%F; i += F) { body; body; ... }
             + remainder loop
   Returns replacement node or NULL. */
static ASTNode *try_partial_unroll(ASTNode *for_node) {
    const char *var_name;
    long long start_val, end_val, iterations;

    if (!analyze_for_loop(for_node, &var_name, &start_val, &end_val, &iterations))
        return NULL;

    /* Only partial-unroll for medium loops (9..256 iterations) */
    if (iterations <= 8 || iterations > 256) return NULL;

    ASTNode *body = for_node->data.for_stmt.body;
    if (!body) return NULL;

    /* Don't unroll if body has complex flow control */
    if (body_has_flow_control(body)) return NULL;

    /* Don't unroll very large bodies */
    if (count_ast_nodes(body) > 30) return NULL;

    /* Choose unroll factor: 4 if iterations % 4 == 0, else 2 */
    int factor = (iterations % 4 == 0) ? 4 : 2;
    long long main_end = start_val + (iterations / factor) * factor;
    long long remainder = iterations % factor;

    ASTNode *result = ast_create_node(AST_BLOCK);

    /* Keep init */
    if (for_node->data.for_stmt.init)
        ast_add_child(result, ast_clone_stmt(for_node->data.for_stmt.init));

    /* Main unrolled loop: for (i = start; i < main_end; i += factor) */
    ASTNode *main_loop = ast_create_node(AST_FOR);

    /* init: i = start (already handled, use assignment) */
    ASTNode *main_init = ast_create_node(AST_ASSIGN);
    main_init->data.assign.left = ast_create_node(AST_IDENTIFIER);
    main_init->data.assign.left->data.identifier.name = strdup(var_name);
    main_init->data.assign.value = make_int(start_val, for_node->line);
    main_loop->data.for_stmt.init = main_init;

    /* condition: i < main_end */
    ASTNode *main_cond = ast_create_node(AST_BINARY_EXPR);
    main_cond->data.binary_expr.op = TOKEN_LESS;
    main_cond->data.binary_expr.left = ast_create_node(AST_IDENTIFIER);
    main_cond->data.binary_expr.left->data.identifier.name = strdup(var_name);
    main_cond->data.binary_expr.right = make_int(main_end, for_node->line);
    main_loop->data.for_stmt.condition = main_cond;

    /* increment: i = i + 1  (the factor-1 internal i++ bumps give the rest) */
    ASTNode *main_incr = ast_create_node(AST_ASSIGN);
    main_incr->data.assign.left = ast_create_node(AST_IDENTIFIER);
    main_incr->data.assign.left->data.identifier.name = strdup(var_name);
    ASTNode *incr_expr = ast_create_node(AST_BINARY_EXPR);
    incr_expr->data.binary_expr.op = TOKEN_PLUS;
    incr_expr->data.binary_expr.left = ast_create_node(AST_IDENTIFIER);
    incr_expr->data.binary_expr.left->data.identifier.name = strdup(var_name);
    incr_expr->data.binary_expr.right = make_int(1, for_node->line);
    main_incr->data.assign.value = incr_expr;
    main_loop->data.for_stmt.increment = main_incr;

    /* body: concatenate factor copies, each offset by j (using i+j) */
    ASTNode *main_body = ast_create_node(AST_BLOCK);
    for (int j = 0; j < factor; j++) {
        ASTNode *copy = ast_clone_stmt(body);
        if (j > 0) {
            /* Replace var_name with (var_name + j) in the body copy.
               We do this by finding identifiers matching var_name and
               wrapping them in var_name + j. */
            /* For simplicity, we just leave the loop body using var_name
               and add i = i + 1 between copies */
        }
        if (copy->type == AST_BLOCK) {
            for (size_t k = 0; k < copy->children_count; k++)
                ast_add_child(main_body, copy->children[k]);
        } else {
            ast_add_child(main_body, copy);
        }
        if (j < factor - 1) {
            /* Insert i = i + 1 between copies */
            ASTNode *bump = ast_create_node(AST_ASSIGN);
            bump->line = for_node->line;
            bump->data.assign.left = ast_create_node(AST_IDENTIFIER);
            bump->data.assign.left->data.identifier.name = strdup(var_name);
            ASTNode *bump_rhs = ast_create_node(AST_BINARY_EXPR);
            bump_rhs->data.binary_expr.op = TOKEN_PLUS;
            bump_rhs->data.binary_expr.left = ast_create_node(AST_IDENTIFIER);
            bump_rhs->data.binary_expr.left->data.identifier.name = strdup(var_name);
            bump_rhs->data.binary_expr.right = make_int(1, for_node->line);
            bump->data.assign.value = bump_rhs;
            ast_add_child(main_body, bump);
        }
    }
    main_loop->data.for_stmt.body = main_body;
    ast_add_child(result, main_loop);

    /* Remainder: unrolled copies for the remaining iterations */
    if (remainder > 0) {
        for (long long r = 0; r < remainder; r++) {
            ASTNode *copy = ast_clone_stmt(body);
            subst_loop_var(copy, var_name, main_end + r);
            opt_stmt(copy);
            if (copy->type == AST_BLOCK) {
                for (size_t k = 0; k < copy->children_count; k++)
                    ast_add_child(result, copy->children[k]);
            } else {
                ast_add_child(result, copy);
            }
        }
    }

    return result;
}

/* Apply loop unrolling to all for-loops in a statement tree. */
static void o3_unroll_loops(ASTNode *node) {
    if (!node) return;

    if (node->type == AST_BLOCK) {
        for (size_t i = 0; i < node->children_count; i++) {
            ASTNode *child = node->children[i];
            if (child->type == AST_FOR) {
                /* Try full unroll first */
                ASTNode *unrolled = try_full_unroll(child);
                if (!unrolled)
                    unrolled = try_partial_unroll(child);
                if (unrolled) {
                    node->children[i] = unrolled;
                    /* Don't recurse into unrolled result — prevents cascade */
                    continue;
                }
            }
            /* Recurse into non-unrolled children (for nested loops) */
            o3_unroll_loops(child);
        }
        return;
    }

    /* Recurse into compound statements */
    switch (node->type) {
    case AST_IF:
        o3_unroll_loops(node->data.if_stmt.then_branch);
        o3_unroll_loops(node->data.if_stmt.else_branch);
        break;
    case AST_WHILE:
    case AST_DO_WHILE:
        o3_unroll_loops(node->data.while_stmt.body);
        break;
    case AST_FOR:
        o3_unroll_loops(node->data.for_stmt.body);
        break;
    case AST_SWITCH:
        o3_unroll_loops(node->data.switch_stmt.body);
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* O3 Pass 3: Loop strength reduction                                 */
/*                                                                    */
/* Transforms array indexing in for-loops from:                       */
/*   for (i = 0; i < N; i++) { ... a[i] ... }                        */
/* to equivalent code using accumulated index values.                 */
/*                                                                    */
/* This is simpler than full pointer-based strength reduction:        */
/* We look for a[i] where 'i' is the loop variable and 'a' is        */
/* invariant, and fold i's known value progression into the generated */
/* code (already handled well by constant folding after unrolling).   */
/*                                                                    */
/* For non-unrolled loops, we transform:                              */
/*   a[i] → a[i]  (keep as-is; the codegen already uses efficient    */
/*   lea-based indexing for array accesses)                           */
/*                                                                    */
/* The main benefit comes from the combination with loop unrolling:   */
/*   After unrolling, a[i] becomes a[0], a[1], a[2], ... which are   */
/*   then constant-folded into direct indexed addressing.             */
/* ------------------------------------------------------------------ */

/* (Loop strength reduction is primarily achieved through the         */
/* combination of loop unrolling + constant folding + the existing    */
/* strength reduction pass. No additional code needed here.)          */

/* ================================================================== */
/* -O3: Vectorization Hints (SSE Packed Operations)                   */
/* ================================================================== */

/* ------------------------------------------------------------------ */
/* Vectorization Pass: Detect and annotate vectorizable loops         */
/*                                                                    */
/* Detects simple patterns of the form:                               */
/*   for (i = 0; i < N; i++) { a[i] = b[i] OP c[i]; }               */
/* where OP is +, -, *, / and all arrays are the same element type    */
/* (int or float, both 4 bytes). Annotates the AST_FOR node with     */
/* VecInfo so the codegen can emit packed SSE/SSE2 instructions       */
/* instead of scalar code, processing 4 elements at a time.           */
/* ------------------------------------------------------------------ */

/* Check if an expression is a simple array access arr[var] where
   var matches the given loop variable name. Returns the array
   identifier name, or NULL if the pattern doesn't match. */
static const char *vec_match_array_access(ASTNode *expr, const char *loop_var) {
    if (!expr || expr->type != AST_ARRAY_ACCESS) return NULL;
    ASTNode *arr = expr->data.array_access.array;
    ASTNode *idx = expr->data.array_access.index;
    if (!arr || !idx) return NULL;
    if (arr->type != AST_IDENTIFIER) return NULL;
    if (idx->type != AST_IDENTIFIER) return NULL;
    if (strcmp(idx->data.identifier.name, loop_var) != 0) return NULL;
    return arr->data.identifier.name;
}

/* Get the element type kind and size for an array access expression.
   Returns 1 on success (and fills out_kind, out_size), 0 on failure. */
static int vec_get_elem_type(ASTNode *array_ident, int *out_kind, int *out_size) {
    if (!array_ident || !array_ident->resolved_type) return 0;
    Type *t = array_ident->resolved_type;
    /* Array or pointer type — element type is in ptr_to */
    if (t->kind == TYPE_ARRAY || t->kind == TYPE_PTR) {
        if (!t->data.ptr_to) return 0;
        *out_kind = t->data.ptr_to->kind;
        *out_size = t->data.ptr_to->size;
        return 1;
    }
    return 0;
}

/* Try to vectorize a for-loop. Returns 1 if successfully annotated. */
static int try_vectorize_loop(ASTNode *for_node) {
    const char *var_name;
    long long start_val, end_val, iterations;

    if (!analyze_for_loop(for_node, &var_name, &start_val, &end_val, &iterations))
        return 0;

    /* Must start at 0 for simple pointer arithmetic */
    if (start_val != 0) return 0;

    /* Determine vector width: AVX uses 256-bit (8 elements), SSE uses 128-bit (4) */
    int vec_width = 4;
    int avx = g_compiler_options.avx_level;
    /* AVX (level 1) enables 256-bit float; AVX2 (level 2) adds 256-bit int */

    /* Need at least vec_width iterations to benefit from vectorization */
    if (iterations < 4) return 0;

    /* Body must be a single statement (possibly wrapped in a block) */
    ASTNode *body = for_node->data.for_stmt.body;
    if (!body) return 0;
    if (body->type == AST_BLOCK) {
        if (body->children_count != 1) return 0;
        body = body->children[0];
    }

    /* Must be an assignment: a[i] = expr */
    if (body->type != AST_ASSIGN) return 0;
    ASTNode *lhs = body->data.assign.left;
    ASTNode *rhs = body->data.assign.value;
    if (!lhs || !rhs) return 0;

    /* LHS must be arr[loop_var] */
    const char *dst = vec_match_array_access(lhs, var_name);
    if (!dst) return 0;

    /* Check element type: must be int (4 bytes) or float (4 bytes) */
    int elem_kind = 0, elem_size = 0;
    if (!vec_get_elem_type(lhs->data.array_access.array, &elem_kind, &elem_size))
        return 0;
    int is_float;
    if (elem_kind == TYPE_FLOAT && elem_size == 4) is_float = 1;
    else if (elem_kind == TYPE_INT && elem_size == 4) is_float = 0;
    else return 0;

    /* RHS must be binary expr: b[i] OP c[i] */
    if (rhs->type != AST_BINARY_EXPR) return 0;
    TokenType op = rhs->data.binary_expr.op;

    /* Check for supported operations */
    if (is_float) {
        if (op != TOKEN_PLUS && op != TOKEN_MINUS &&
            op != TOKEN_STAR && op != TOKEN_SLASH)
            return 0;
    } else {
        /* Integer: only + and - (SSE2 has no packed int32 multiply) */
        if (op != TOKEN_PLUS && op != TOKEN_MINUS)
            return 0;
    }

    /* Both operands must be arr[loop_var] */
    const char *src1 = vec_match_array_access(rhs->data.binary_expr.left, var_name);
    const char *src2 = vec_match_array_access(rhs->data.binary_expr.right, var_name);
    if (!src1 || !src2) return 0;

    /* Check that source arrays have matching element type */
    int s1_kind, s1_size, s2_kind, s2_size;
    if (!vec_get_elem_type(rhs->data.binary_expr.left->data.array_access.array,
                           &s1_kind, &s1_size))
        return 0;
    if (!vec_get_elem_type(rhs->data.binary_expr.right->data.array_access.array,
                           &s2_kind, &s2_size))
        return 0;
    if (s1_kind != elem_kind || s2_kind != elem_kind) return 0;
    if (s1_size != 4 || s2_size != 4) return 0;

    /* Determine final vector width based on AVX level and element type */
    if (is_float && avx >= 1) vec_width = 8;   /* AVX: 256-bit float */
    if (!is_float && avx >= 2) vec_width = 8;  /* AVX2: 256-bit integer */
    if (iterations < vec_width) vec_width = 4; /* Fall back to SSE if not enough iterations */
    if (iterations < 4) return 0;              /* Still need at least 4 */

    /* All checks passed — annotate the loop for vectorization */
    VecInfo *vi = (VecInfo *)calloc(1, sizeof(VecInfo));
    vi->width = vec_width;
    vi->elem_size = 4;
    vi->is_float = is_float;
    vi->op = op;
    vi->iterations = (int)iterations;
    vi->loop_var = var_name;
    vi->dst = dst;
    vi->src1 = src1;
    vi->src2 = src2;
    for_node->vec_info = vi;
    return 1;
}

/* Walk a statement tree and try to vectorize eligible for-loops */
static void o3_vectorize_loops(ASTNode *node) {
    if (!node) return;
    switch (node->type) {
    case AST_BLOCK:
        for (size_t i = 0; i < node->children_count; i++)
            o3_vectorize_loops(node->children[i]);
        break;
    case AST_FOR:
        try_vectorize_loop(node);
        /* Don't recurse into vectorized loop body — it will be handled
           entirely by the codegen's vector path */
        if (!node->vec_info)
            o3_vectorize_loops(node->data.for_stmt.body);
        break;
    case AST_IF:
        o3_vectorize_loops(node->data.if_stmt.then_branch);
        if (node->data.if_stmt.else_branch)
            o3_vectorize_loops(node->data.if_stmt.else_branch);
        break;
    case AST_WHILE:
    case AST_DO_WHILE:
        o3_vectorize_loops(node->data.while_stmt.body);
        break;
    case AST_SWITCH:
        o3_vectorize_loops(node->data.switch_stmt.body);
        break;
    default:
        break;
    }
}

/* ================================================================== */
/* -O3: Interprocedural Optimization (IPA) Passes                     */
/* ================================================================== */

/* ------------------------------------------------------------------ */
/* IPA Pass 1: Return Value Propagation                               */
/*                                                                    */
/* If a function always returns the same compile-time constant        */
/* (possibly after O1/O2 constant folding), replace all call sites    */
/* with that constant value. Only applies to non-void, non-extern     */
/* functions with a single return statement returning a constant.     */
/* ------------------------------------------------------------------ */

#define MAX_RVP_CANDIDATES 256

typedef struct {
    const char *name;
    long long   return_value;
} RVPCandidate;

static RVPCandidate g_rvp_cands[MAX_RVP_CANDIDATES];
static int g_rvp_cand_count;

/* Find functions that always return the same constant. */
static void find_rvp_candidates(ASTNode *program) {
    g_rvp_cand_count = 0;

    for (size_t i = 0; i < program->children_count; i++) {
        ASTNode *fn = program->children[i];
        if (fn->type != AST_FUNCTION || !fn->data.function.body) continue;
        if (fn->data.function.inline_hint == -1) continue; /* noinline */

        /* Skip 'main' — its return value is the program exit code, not a constant */
        if (strcmp(fn->data.function.name, "main") == 0) continue;

        ASTNode *body = fn->data.function.body;
        if (body->type != AST_BLOCK) continue;

        /* Check all return paths — for simplicity, only handle single-return
           functions (body is a block with exactly one return at the end) */
        int return_count = 0;
        ASTNode *return_expr = NULL;

        for (size_t j = 0; j < body->children_count; j++) {
            ASTNode *stmt = body->children[j];
            if (stmt->type == AST_RETURN) {
                return_count++;
                return_expr = stmt->data.return_stmt.expression;
            }
            /* If there are if/while/for/switch statements, they might contain
               additional returns, making analysis complex. Skip if body has
               any control flow statements. */
            if (stmt->type == AST_IF || stmt->type == AST_WHILE ||
                stmt->type == AST_DO_WHILE || stmt->type == AST_FOR ||
                stmt->type == AST_SWITCH) {
                return_count = -1; /* mark as complex */
                break;
            }
        }

        if (return_count != 1 || !return_expr) continue;
        if (!is_const_int(return_expr)) continue;

        if (g_rvp_cand_count >= MAX_RVP_CANDIDATES) break;

        g_rvp_cands[g_rvp_cand_count].name = fn->data.function.name;
        g_rvp_cands[g_rvp_cand_count].return_value = return_expr->data.integer.value;
        g_rvp_cand_count++;
    }
}

/* Look up RVP candidate by name. */
static RVPCandidate *find_rvp_cand(const char *name) {
    for (int i = 0; i < g_rvp_cand_count; i++) {
        if (strcmp(g_rvp_cands[i].name, name) == 0) return &g_rvp_cands[i];
    }
    return NULL;
}

/* Replace calls to constant-returning functions with their return value.
   Walk an expression tree and substitute matching calls. */
static ASTNode *rvp_substitute_expr(ASTNode *expr) {
    if (!expr) return NULL;

    if (expr->type == AST_CALL) {
        /* First recurse into arguments */
        for (size_t i = 0; i < expr->children_count; i++)
            expr->children[i] = rvp_substitute_expr(expr->children[i]);

        /* Check if all arguments have no side effects (safe to eliminate) */
        int safe = 1;
        for (size_t i = 0; i < expr->children_count; i++) {
            if (has_side_effects(expr->children[i])) { safe = 0; break; }
        }

        if (safe) {
            RVPCandidate *cand = find_rvp_cand(expr->data.call.name);
            if (cand) {
                return make_int(cand->return_value, expr->line);
            }
        }
        return expr;
    }

    /* Recurse into sub-expressions */
    switch (expr->type) {
    case AST_BINARY_EXPR:
        expr->data.binary_expr.left  = rvp_substitute_expr(expr->data.binary_expr.left);
        expr->data.binary_expr.right = rvp_substitute_expr(expr->data.binary_expr.right);
        break;
    case AST_NEG: case AST_NOT: case AST_BITWISE_NOT:
    case AST_PRE_INC: case AST_PRE_DEC:
    case AST_POST_INC: case AST_POST_DEC:
    case AST_DEREF: case AST_ADDR_OF:
        expr->data.unary.expression = rvp_substitute_expr(expr->data.unary.expression);
        break;
    case AST_CAST:
        expr->data.cast.expression = rvp_substitute_expr(expr->data.cast.expression);
        break;
    case AST_MEMBER_ACCESS:
        expr->data.member_access.struct_expr = rvp_substitute_expr(expr->data.member_access.struct_expr);
        break;
    case AST_ARRAY_ACCESS:
        expr->data.array_access.array = rvp_substitute_expr(expr->data.array_access.array);
        expr->data.array_access.index = rvp_substitute_expr(expr->data.array_access.index);
        break;
    case AST_IF:
        expr->data.if_stmt.condition   = rvp_substitute_expr(expr->data.if_stmt.condition);
        expr->data.if_stmt.then_branch = rvp_substitute_expr(expr->data.if_stmt.then_branch);
        expr->data.if_stmt.else_branch = rvp_substitute_expr(expr->data.if_stmt.else_branch);
        break;
    case AST_ASSIGN:
        expr->data.assign.left  = rvp_substitute_expr(expr->data.assign.left);
        expr->data.assign.value = rvp_substitute_expr(expr->data.assign.value);
        break;
    default:
        break;
    }
    return expr;
}

/* Walk a statement tree and apply RVP substitution in all expressions. */
static void rvp_substitute_stmt(ASTNode *stmt) {
    if (!stmt) return;
    switch (stmt->type) {
    case AST_BLOCK:
        for (size_t i = 0; i < stmt->children_count; i++)
            rvp_substitute_stmt(stmt->children[i]);
        break;
    case AST_RETURN:
        if (stmt->data.return_stmt.expression)
            stmt->data.return_stmt.expression = rvp_substitute_expr(stmt->data.return_stmt.expression);
        break;
    case AST_VAR_DECL:
        if (stmt->data.var_decl.initializer)
            stmt->data.var_decl.initializer = rvp_substitute_expr(stmt->data.var_decl.initializer);
        break;
    case AST_ASSIGN:
        stmt->data.assign.value = rvp_substitute_expr(stmt->data.assign.value);
        break;
    case AST_IF:
        stmt->data.if_stmt.condition = rvp_substitute_expr(stmt->data.if_stmt.condition);
        rvp_substitute_stmt(stmt->data.if_stmt.then_branch);
        if (stmt->data.if_stmt.else_branch)
            rvp_substitute_stmt(stmt->data.if_stmt.else_branch);
        break;
    case AST_WHILE:
    case AST_DO_WHILE:
        stmt->data.while_stmt.condition = rvp_substitute_expr(stmt->data.while_stmt.condition);
        rvp_substitute_stmt(stmt->data.while_stmt.body);
        break;
    case AST_FOR:
        if (stmt->data.for_stmt.init) rvp_substitute_stmt(stmt->data.for_stmt.init);
        if (stmt->data.for_stmt.condition)
            stmt->data.for_stmt.condition = rvp_substitute_expr(stmt->data.for_stmt.condition);
        if (stmt->data.for_stmt.increment)
            stmt->data.for_stmt.increment = rvp_substitute_expr(stmt->data.for_stmt.increment);
        rvp_substitute_stmt(stmt->data.for_stmt.body);
        break;
    case AST_SWITCH:
        stmt->data.switch_stmt.condition = rvp_substitute_expr(stmt->data.switch_stmt.condition);
        rvp_substitute_stmt(stmt->data.switch_stmt.body);
        break;
    default:
        if (stmt->type == AST_CALL) {
            for (size_t i = 0; i < stmt->children_count; i++)
                stmt->children[i] = rvp_substitute_expr(stmt->children[i]);
        }
        break;
    }
}

/* ------------------------------------------------------------------ */
/* IPA Pass 2: IPA Constant Propagation                               */
/*                                                                    */
/* For each function, check all call sites. If a particular parameter */
/* is always passed the same constant value across every call site,   */
/* substitute that constant for the parameter throughout the function */
/* body, enabling further constant folding.                           */
/* ------------------------------------------------------------------ */

#define MAX_IPA_FUNC 256

typedef struct {
    const char *func_name;
    int         param_count;
    long long   const_values[MAX_INLINE_PARAMS];  /* constant value for each param */
    int         is_constant[MAX_INLINE_PARAMS];    /* 1 if always same constant */
    int         call_count;                        /* number of call sites found */
} IPAConstInfo;

static IPAConstInfo g_ipa_funcs[MAX_IPA_FUNC];
static int g_ipa_func_count;

/* Forward declaration */
static void ipa_scan_calls_in_expr(ASTNode *expr);
static void ipa_scan_calls_in_stmt(ASTNode *stmt);

/* Register or update IPA info for a function call. */
static void ipa_register_call(const char *func_name, ASTNode **args, int arg_count) {
    /* Find or create entry */
    IPAConstInfo *info = NULL;
    for (int i = 0; i < g_ipa_func_count; i++) {
        if (strcmp(g_ipa_funcs[i].func_name, func_name) == 0) {
            info = &g_ipa_funcs[i];
            break;
        }
    }

    if (!info) {
        if (g_ipa_func_count >= MAX_IPA_FUNC) return;
        info = &g_ipa_funcs[g_ipa_func_count++];
        info->func_name = func_name;
        info->param_count = arg_count;
        info->call_count = 0;
        for (int i = 0; i < arg_count && i < MAX_INLINE_PARAMS; i++) {
            if (is_const_int(args[i])) {
                info->const_values[i] = args[i]->data.integer.value;
                info->is_constant[i] = 1;
            } else {
                info->is_constant[i] = 0;
            }
        }
        info->call_count = 1;
        return;
    }

    /* Update existing entry */
    info->call_count++;
    if (info->param_count != arg_count) {
        /* Mismatched call — invalidate all */
        for (int i = 0; i < info->param_count && i < MAX_INLINE_PARAMS; i++)
            info->is_constant[i] = 0;
        return;
    }

    for (int i = 0; i < arg_count && i < MAX_INLINE_PARAMS; i++) {
        if (!info->is_constant[i]) continue;
        if (!is_const_int(args[i]) ||
            args[i]->data.integer.value != info->const_values[i]) {
            info->is_constant[i] = 0;
        }
    }
}

/* Scan expressions for calls to collect IPA info. */
static void ipa_scan_calls_in_expr(ASTNode *expr) {
    if (!expr) return;
    if (expr->type == AST_CALL) {
        ipa_register_call(expr->data.call.name, expr->children, (int)expr->children_count);
        for (size_t i = 0; i < expr->children_count; i++)
            ipa_scan_calls_in_expr(expr->children[i]);
        return;
    }
    switch (expr->type) {
    case AST_BINARY_EXPR:
        ipa_scan_calls_in_expr(expr->data.binary_expr.left);
        ipa_scan_calls_in_expr(expr->data.binary_expr.right);
        break;
    case AST_NEG: case AST_NOT: case AST_BITWISE_NOT:
    case AST_PRE_INC: case AST_PRE_DEC:
    case AST_POST_INC: case AST_POST_DEC:
    case AST_DEREF: case AST_ADDR_OF:
        ipa_scan_calls_in_expr(expr->data.unary.expression);
        break;
    case AST_CAST:
        ipa_scan_calls_in_expr(expr->data.cast.expression);
        break;
    case AST_MEMBER_ACCESS:
        ipa_scan_calls_in_expr(expr->data.member_access.struct_expr);
        break;
    case AST_ARRAY_ACCESS:
        ipa_scan_calls_in_expr(expr->data.array_access.array);
        ipa_scan_calls_in_expr(expr->data.array_access.index);
        break;
    case AST_IF:
        ipa_scan_calls_in_expr(expr->data.if_stmt.condition);
        ipa_scan_calls_in_expr(expr->data.if_stmt.then_branch);
        ipa_scan_calls_in_expr(expr->data.if_stmt.else_branch);
        break;
    case AST_ASSIGN:
        ipa_scan_calls_in_expr(expr->data.assign.left);
        ipa_scan_calls_in_expr(expr->data.assign.value);
        break;
    default:
        break;
    }
}

/* Scan statements for calls. */
static void ipa_scan_calls_in_stmt(ASTNode *stmt) {
    if (!stmt) return;
    switch (stmt->type) {
    case AST_BLOCK:
        for (size_t i = 0; i < stmt->children_count; i++)
            ipa_scan_calls_in_stmt(stmt->children[i]);
        break;
    case AST_RETURN:
        if (stmt->data.return_stmt.expression)
            ipa_scan_calls_in_expr(stmt->data.return_stmt.expression);
        break;
    case AST_VAR_DECL:
        if (stmt->data.var_decl.initializer)
            ipa_scan_calls_in_expr(stmt->data.var_decl.initializer);
        break;
    case AST_ASSIGN:
        ipa_scan_calls_in_expr(stmt->data.assign.value);
        break;
    case AST_IF:
        ipa_scan_calls_in_expr(stmt->data.if_stmt.condition);
        ipa_scan_calls_in_stmt(stmt->data.if_stmt.then_branch);
        if (stmt->data.if_stmt.else_branch)
            ipa_scan_calls_in_stmt(stmt->data.if_stmt.else_branch);
        break;
    case AST_WHILE: case AST_DO_WHILE:
        ipa_scan_calls_in_expr(stmt->data.while_stmt.condition);
        ipa_scan_calls_in_stmt(stmt->data.while_stmt.body);
        break;
    case AST_FOR:
        if (stmt->data.for_stmt.init) ipa_scan_calls_in_stmt(stmt->data.for_stmt.init);
        if (stmt->data.for_stmt.condition)
            ipa_scan_calls_in_expr(stmt->data.for_stmt.condition);
        if (stmt->data.for_stmt.increment)
            ipa_scan_calls_in_expr(stmt->data.for_stmt.increment);
        ipa_scan_calls_in_stmt(stmt->data.for_stmt.body);
        break;
    case AST_SWITCH:
        ipa_scan_calls_in_expr(stmt->data.switch_stmt.condition);
        ipa_scan_calls_in_stmt(stmt->data.switch_stmt.body);
        break;
    default:
        if (stmt->type == AST_CALL) {
            ipa_register_call(stmt->data.call.name, stmt->children, (int)stmt->children_count);
            for (size_t i = 0; i < stmt->children_count; i++)
                ipa_scan_calls_in_expr(stmt->children[i]);
        }
        break;
    }
}

/* Apply IPA constant propagation: for each function where a parameter
   is always the same constant, substitute it in the function body. */
static void ipa_propagate_constants(ASTNode *program) {
    g_ipa_func_count = 0;

    /* Step 1: Scan all call sites in all functions */
    for (size_t i = 0; i < program->children_count; i++) {
        ASTNode *fn = program->children[i];
        if (fn->type == AST_FUNCTION && fn->data.function.body) {
            ipa_scan_calls_in_stmt(fn->data.function.body);
        }
    }

    /* Step 2: For each function with constant parameters, substitute */
    for (int ci = 0; ci < g_ipa_func_count; ci++) {
        IPAConstInfo *info = &g_ipa_funcs[ci];
        if (info->call_count < 1) continue;

        /* Check if any parameter is always constant */
        int any_const = 0;
        for (int p = 0; p < info->param_count && p < MAX_INLINE_PARAMS; p++) {
            if (info->is_constant[p]) { any_const = 1; break; }
        }
        if (!any_const) continue;

        /* Find the function definition */
        ASTNode *fn = NULL;
        for (size_t i = 0; i < program->children_count; i++) {
            ASTNode *child = program->children[i];
            if (child->type == AST_FUNCTION && child->data.function.body &&
                strcmp(child->data.function.name, info->func_name) == 0) {
                fn = child;
                break;
            }
        }
        if (!fn) continue;

        /* Skip main — its parameters are argc/argv */
        if (strcmp(fn->data.function.name, "main") == 0) continue;

        /* Only specialize static functions — non-static ones may be called
           from other translation units with different argument values. */
        if (!fn->data.function.is_static) continue;

        /* Substitute constant parameters in the function body */
        if ((int)fn->children_count != info->param_count) continue;

        PropEnv subst_env;
        prop_env_init(&subst_env);

        for (int p = 0; p < info->param_count && p < MAX_INLINE_PARAMS; p++) {
            if (!info->is_constant[p]) continue;
            const char *pname = fn->children[p]->data.var_decl.name;
            ASTNode *cval = make_int(info->const_values[p], fn->line);
            prop_env_set(&subst_env, pname, cval, -1);
        }

        /* Apply substitution to the function body using prop_substitute */
        if (fn->data.function.body->type == AST_BLOCK) {
            for (size_t j = 0; j < fn->data.function.body->children_count; j++) {
                ASTNode *stmt = fn->data.function.body->children[j];
                if (stmt->type == AST_RETURN && stmt->data.return_stmt.expression) {
                    stmt->data.return_stmt.expression =
                        prop_substitute(stmt->data.return_stmt.expression, &subst_env);
                    stmt->data.return_stmt.expression =
                        opt_expr(stmt->data.return_stmt.expression);
                }
                if (stmt->type == AST_VAR_DECL && stmt->data.var_decl.initializer) {
                    stmt->data.var_decl.initializer =
                        prop_substitute(stmt->data.var_decl.initializer, &subst_env);
                    stmt->data.var_decl.initializer =
                        opt_expr(stmt->data.var_decl.initializer);
                }
                if (stmt->type == AST_ASSIGN) {
                    stmt->data.assign.value =
                        prop_substitute(stmt->data.assign.value, &subst_env);
                    stmt->data.assign.value = opt_expr(stmt->data.assign.value);
                }
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* IPA Pass 3: Dead Argument Elimination                              */
/*                                                                    */
/* For each function, check if any parameter is never referenced in   */
/* the function body. If so, remove it from the parameter list and    */
/* update all call sites to drop the corresponding argument.          */
/* Skips main, extern, and variadic functions.                        */
/* ------------------------------------------------------------------ */

/* Check if a parameter name is referenced anywhere in an expression. */
static int param_is_used_in_expr(ASTNode *expr, const char *param_name) {
    if (!expr) return 0;
    if (expr->type == AST_IDENTIFIER &&
        strcmp(expr->data.identifier.name, param_name) == 0)
        return 1;
    switch (expr->type) {
    case AST_BINARY_EXPR:
        return param_is_used_in_expr(expr->data.binary_expr.left, param_name) ||
               param_is_used_in_expr(expr->data.binary_expr.right, param_name);
    case AST_NEG: case AST_NOT: case AST_BITWISE_NOT:
    case AST_PRE_INC: case AST_PRE_DEC:
    case AST_POST_INC: case AST_POST_DEC:
    case AST_DEREF: case AST_ADDR_OF:
        return param_is_used_in_expr(expr->data.unary.expression, param_name);
    case AST_CAST:
        return param_is_used_in_expr(expr->data.cast.expression, param_name);
    case AST_CALL:
        for (size_t i = 0; i < expr->children_count; i++)
            if (param_is_used_in_expr(expr->children[i], param_name)) return 1;
        return 0;
    case AST_MEMBER_ACCESS:
        return param_is_used_in_expr(expr->data.member_access.struct_expr, param_name);
    case AST_ARRAY_ACCESS:
        return param_is_used_in_expr(expr->data.array_access.array, param_name) ||
               param_is_used_in_expr(expr->data.array_access.index, param_name);
    case AST_IF:
        return param_is_used_in_expr(expr->data.if_stmt.condition, param_name) ||
               param_is_used_in_expr(expr->data.if_stmt.then_branch, param_name) ||
               param_is_used_in_expr(expr->data.if_stmt.else_branch, param_name);
    case AST_ASSIGN:
        return param_is_used_in_expr(expr->data.assign.left, param_name) ||
               param_is_used_in_expr(expr->data.assign.value, param_name);
    default:
        return 0;
    }
}

/* Check if a parameter name is referenced anywhere in a statement tree. */
static int param_is_used_in_stmt(ASTNode *stmt, const char *param_name) {
    if (!stmt) return 0;
    switch (stmt->type) {
    case AST_BLOCK:
        for (size_t i = 0; i < stmt->children_count; i++)
            if (param_is_used_in_stmt(stmt->children[i], param_name)) return 1;
        return 0;
    case AST_RETURN:
        return param_is_used_in_expr(stmt->data.return_stmt.expression, param_name);
    case AST_VAR_DECL:
        return param_is_used_in_expr(stmt->data.var_decl.initializer, param_name);
    case AST_ASSIGN:
        return param_is_used_in_expr(stmt->data.assign.left, param_name) ||
               param_is_used_in_expr(stmt->data.assign.value, param_name);
    case AST_IF:
        return param_is_used_in_expr(stmt->data.if_stmt.condition, param_name) ||
               param_is_used_in_stmt(stmt->data.if_stmt.then_branch, param_name) ||
               param_is_used_in_stmt(stmt->data.if_stmt.else_branch, param_name);
    case AST_WHILE: case AST_DO_WHILE:
        return param_is_used_in_expr(stmt->data.while_stmt.condition, param_name) ||
               param_is_used_in_stmt(stmt->data.while_stmt.body, param_name);
    case AST_FOR:
        return param_is_used_in_stmt(stmt->data.for_stmt.init, param_name) ||
               param_is_used_in_expr(stmt->data.for_stmt.condition, param_name) ||
               param_is_used_in_expr(stmt->data.for_stmt.increment, param_name) ||
               param_is_used_in_stmt(stmt->data.for_stmt.body, param_name);
    case AST_SWITCH:
        return param_is_used_in_expr(stmt->data.switch_stmt.condition, param_name) ||
               param_is_used_in_stmt(stmt->data.switch_stmt.body, param_name);
    default:
        if (stmt->type == AST_CALL) {
            for (size_t i = 0; i < stmt->children_count; i++)
                if (param_is_used_in_expr(stmt->children[i], param_name)) return 1;
        }
        return param_is_used_in_expr(stmt, param_name);
    }
}

/* Remove argument at position `arg_idx` from all calls to `func_name` in an expression. */
static void dae_remove_arg_in_expr(ASTNode *expr, const char *func_name, int arg_idx) {
    if (!expr) return;
    if (expr->type == AST_CALL) {
        if (strcmp(expr->data.call.name, func_name) == 0 &&
            (int)expr->children_count > arg_idx) {
            /* Remove the argument by shifting subsequent args down */
            for (int j = arg_idx; j < (int)expr->children_count - 1; j++)
                expr->children[j] = expr->children[j + 1];
            expr->children_count--;
        }
        for (size_t i = 0; i < expr->children_count; i++)
            dae_remove_arg_in_expr(expr->children[i], func_name, arg_idx);
        return;
    }
    switch (expr->type) {
    case AST_BINARY_EXPR:
        dae_remove_arg_in_expr(expr->data.binary_expr.left, func_name, arg_idx);
        dae_remove_arg_in_expr(expr->data.binary_expr.right, func_name, arg_idx);
        break;
    case AST_NEG: case AST_NOT: case AST_BITWISE_NOT:
    case AST_PRE_INC: case AST_PRE_DEC:
    case AST_POST_INC: case AST_POST_DEC:
    case AST_DEREF: case AST_ADDR_OF:
        dae_remove_arg_in_expr(expr->data.unary.expression, func_name, arg_idx);
        break;
    case AST_CAST:
        dae_remove_arg_in_expr(expr->data.cast.expression, func_name, arg_idx);
        break;
    case AST_MEMBER_ACCESS:
        dae_remove_arg_in_expr(expr->data.member_access.struct_expr, func_name, arg_idx);
        break;
    case AST_ARRAY_ACCESS:
        dae_remove_arg_in_expr(expr->data.array_access.array, func_name, arg_idx);
        dae_remove_arg_in_expr(expr->data.array_access.index, func_name, arg_idx);
        break;
    case AST_IF:
        dae_remove_arg_in_expr(expr->data.if_stmt.condition, func_name, arg_idx);
        dae_remove_arg_in_expr(expr->data.if_stmt.then_branch, func_name, arg_idx);
        dae_remove_arg_in_expr(expr->data.if_stmt.else_branch, func_name, arg_idx);
        break;
    case AST_ASSIGN:
        dae_remove_arg_in_expr(expr->data.assign.left, func_name, arg_idx);
        dae_remove_arg_in_expr(expr->data.assign.value, func_name, arg_idx);
        break;
    default:
        break;
    }
}

/* Remove argument at position `arg_idx` from all calls to `func_name` in a statement. */
static void dae_remove_arg_in_stmt(ASTNode *stmt, const char *func_name, int arg_idx) {
    if (!stmt) return;
    switch (stmt->type) {
    case AST_BLOCK:
        for (size_t i = 0; i < stmt->children_count; i++)
            dae_remove_arg_in_stmt(stmt->children[i], func_name, arg_idx);
        break;
    case AST_RETURN:
        dae_remove_arg_in_expr(stmt->data.return_stmt.expression, func_name, arg_idx);
        break;
    case AST_VAR_DECL:
        dae_remove_arg_in_expr(stmt->data.var_decl.initializer, func_name, arg_idx);
        break;
    case AST_ASSIGN:
        dae_remove_arg_in_expr(stmt->data.assign.value, func_name, arg_idx);
        break;
    case AST_IF:
        dae_remove_arg_in_expr(stmt->data.if_stmt.condition, func_name, arg_idx);
        dae_remove_arg_in_stmt(stmt->data.if_stmt.then_branch, func_name, arg_idx);
        if (stmt->data.if_stmt.else_branch)
            dae_remove_arg_in_stmt(stmt->data.if_stmt.else_branch, func_name, arg_idx);
        break;
    case AST_WHILE: case AST_DO_WHILE:
        dae_remove_arg_in_expr(stmt->data.while_stmt.condition, func_name, arg_idx);
        dae_remove_arg_in_stmt(stmt->data.while_stmt.body, func_name, arg_idx);
        break;
    case AST_FOR:
        if (stmt->data.for_stmt.init) dae_remove_arg_in_stmt(stmt->data.for_stmt.init, func_name, arg_idx);
        dae_remove_arg_in_expr(stmt->data.for_stmt.condition, func_name, arg_idx);
        dae_remove_arg_in_expr(stmt->data.for_stmt.increment, func_name, arg_idx);
        dae_remove_arg_in_stmt(stmt->data.for_stmt.body, func_name, arg_idx);
        break;
    case AST_SWITCH:
        dae_remove_arg_in_expr(stmt->data.switch_stmt.condition, func_name, arg_idx);
        dae_remove_arg_in_stmt(stmt->data.switch_stmt.body, func_name, arg_idx);
        break;
    default:
        if (stmt->type == AST_CALL) {
            if (strcmp(stmt->data.call.name, func_name) == 0 &&
                (int)stmt->children_count > arg_idx) {
                for (int j = arg_idx; j < (int)stmt->children_count - 1; j++)
                    stmt->children[j] = stmt->children[j + 1];
                stmt->children_count--;
            }
            for (size_t i = 0; i < stmt->children_count; i++)
                dae_remove_arg_in_expr(stmt->children[i], func_name, arg_idx);
        }
        break;
    }
}

/* Dead argument elimination pass. */
static void ipa_dead_arg_elimination(ASTNode *program) {
    for (size_t fi = 0; fi < program->children_count; fi++) {
        ASTNode *fn = program->children[fi];
        if (fn->type != AST_FUNCTION || !fn->data.function.body) continue;
        if (strcmp(fn->data.function.name, "main") == 0) continue;
        /* Only modify static functions — non-static ones may be called from
           other translation units expecting the original parameter list. */
        if (!fn->data.function.is_static) continue;
        if (fn->children_count == 0) continue;

        /* Check each parameter from right to left (to avoid index shifting issues) */
        for (int p = (int)fn->children_count - 1; p >= 0; p--) {
            ASTNode *param = fn->children[p];
            if (param->type != AST_VAR_DECL) continue;

            const char *param_name = param->data.var_decl.name;

            /* Check if the parameter is used anywhere in the function body */
            if (param_is_used_in_stmt(fn->data.function.body, param_name))
                continue;

            /* Parameter is dead — remove it from the function definition */
            for (int j = p; j < (int)fn->children_count - 1; j++)
                fn->children[j] = fn->children[j + 1];
            fn->children_count--;

            /* Remove corresponding argument from all call sites */
            for (size_t ci = 0; ci < program->children_count; ci++) {
                ASTNode *caller = program->children[ci];
                if (caller->type == AST_FUNCTION && caller->data.function.body) {
                    dae_remove_arg_in_stmt(caller->data.function.body,
                                           fn->data.function.name, p);
                }
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* IPA Pass 4: Dead Function Elimination                              */
/*                                                                    */
/* After inlining and other IPA passes, some functions may have zero  */
/* remaining call sites. Remove their definitions from the program    */
/* to reduce code size. Skips 'main' and functions with external      */
/* linkage that could be called from other translation units.         */
/* ------------------------------------------------------------------ */

/* Check if `func_name` is called anywhere in an expression tree. */
static int func_is_called_in_expr(ASTNode *expr, const char *func_name) {
    if (!expr) return 0;
    if (expr->type == AST_CALL &&
        strcmp(expr->data.call.name, func_name) == 0)
        return 1;
    switch (expr->type) {
    case AST_CALL:
        for (size_t i = 0; i < expr->children_count; i++)
            if (func_is_called_in_expr(expr->children[i], func_name)) return 1;
        return 0;
    case AST_BINARY_EXPR:
        return func_is_called_in_expr(expr->data.binary_expr.left, func_name) ||
               func_is_called_in_expr(expr->data.binary_expr.right, func_name);
    case AST_NEG: case AST_NOT: case AST_BITWISE_NOT:
    case AST_PRE_INC: case AST_PRE_DEC:
    case AST_POST_INC: case AST_POST_DEC:
    case AST_DEREF: case AST_ADDR_OF:
        return func_is_called_in_expr(expr->data.unary.expression, func_name);
    case AST_CAST:
        return func_is_called_in_expr(expr->data.cast.expression, func_name);
    case AST_MEMBER_ACCESS:
        return func_is_called_in_expr(expr->data.member_access.struct_expr, func_name);
    case AST_ARRAY_ACCESS:
        return func_is_called_in_expr(expr->data.array_access.array, func_name) ||
               func_is_called_in_expr(expr->data.array_access.index, func_name);
    case AST_IF:
        return func_is_called_in_expr(expr->data.if_stmt.condition, func_name) ||
               func_is_called_in_expr(expr->data.if_stmt.then_branch, func_name) ||
               func_is_called_in_expr(expr->data.if_stmt.else_branch, func_name);
    case AST_ASSIGN:
        return func_is_called_in_expr(expr->data.assign.left, func_name) ||
               func_is_called_in_expr(expr->data.assign.value, func_name);
    default:
        return 0;
    }
}

/* Check if `func_name` is called anywhere in a statement tree. */
static int func_is_called_in_stmt(ASTNode *stmt, const char *func_name) {
    if (!stmt) return 0;
    switch (stmt->type) {
    case AST_BLOCK:
        for (size_t i = 0; i < stmt->children_count; i++)
            if (func_is_called_in_stmt(stmt->children[i], func_name)) return 1;
        return 0;
    case AST_RETURN:
        return func_is_called_in_expr(stmt->data.return_stmt.expression, func_name);
    case AST_VAR_DECL:
        return func_is_called_in_expr(stmt->data.var_decl.initializer, func_name);
    case AST_ASSIGN:
        return func_is_called_in_expr(stmt->data.assign.left, func_name) ||
               func_is_called_in_expr(stmt->data.assign.value, func_name);
    case AST_IF:
        return func_is_called_in_expr(stmt->data.if_stmt.condition, func_name) ||
               func_is_called_in_stmt(stmt->data.if_stmt.then_branch, func_name) ||
               func_is_called_in_stmt(stmt->data.if_stmt.else_branch, func_name);
    case AST_WHILE: case AST_DO_WHILE:
        return func_is_called_in_expr(stmt->data.while_stmt.condition, func_name) ||
               func_is_called_in_stmt(stmt->data.while_stmt.body, func_name);
    case AST_FOR:
        return func_is_called_in_stmt(stmt->data.for_stmt.init, func_name) ||
               func_is_called_in_expr(stmt->data.for_stmt.condition, func_name) ||
               func_is_called_in_expr(stmt->data.for_stmt.increment, func_name) ||
               func_is_called_in_stmt(stmt->data.for_stmt.body, func_name);
    case AST_SWITCH:
        return func_is_called_in_expr(stmt->data.switch_stmt.condition, func_name) ||
               func_is_called_in_stmt(stmt->data.switch_stmt.body, func_name);
    default:
        if (stmt->type == AST_CALL &&
            strcmp(stmt->data.call.name, func_name) == 0) return 1;
        if (stmt->type == AST_CALL) {
            for (size_t i = 0; i < stmt->children_count; i++)
                if (func_is_called_in_expr(stmt->children[i], func_name)) return 1;
        }
        return 0;
    }
}

/* Dead function elimination pass. */
static void ipa_dead_function_elimination(ASTNode *program) {
    for (size_t fi = 0; fi < program->children_count; fi++) {
        ASTNode *fn = program->children[fi];
        if (fn->type != AST_FUNCTION || !fn->data.function.body) continue;

        /* Never remove main */
        if (strcmp(fn->data.function.name, "main") == 0) continue;

        /* Only remove static (local-linkage) functions. Non-static functions
           may be called from other translation units, so we must keep them. */
        if (!fn->data.function.is_static) continue;

        /* Check if any other function calls this one */
        int is_called = 0;
        for (size_t ci = 0; ci < program->children_count; ci++) {
            if (ci == fi) continue; /* skip self */
            ASTNode *caller = program->children[ci];
            if (caller->type == AST_FUNCTION && caller->data.function.body) {
                if (func_is_called_in_stmt(caller->data.function.body, fn->data.function.name)) {
                    is_called = 1;
                    break;
                }
            }
        }

        if (!is_called) {
            /* Remove the function by shifting subsequent children down */
            for (size_t j = fi; j < program->children_count - 1; j++)
                program->children[j] = program->children[j + 1];
            program->children_count--;
            fi--; /* re-check the slot (new function moved here) */
        }
    }
}

/* ------------------------------------------------------------------ */
/* Top-level entry point                                              */
/* ------------------------------------------------------------------ */
ASTNode *optimize(ASTNode *program, OptLevel level) {
    if (!program) return program;

    /* __forceinline / __attribute__((always_inline)) must be processed even at -O0 */
    {
        find_inline_candidates(program);
        if (g_inline_cand_count > 0) {
            /* At -O0: only always_inline (hint==2).
             * At -O1: inline + always_inline (hint>=1).
             * At -O2+: all eligible (hint>=0). */
            int min_hint = (level >= OPT_O2) ? 0 : (level >= OPT_O1) ? 1 : 2;
            /* Remove candidates below threshold */
            for (int ci = 0; ci < g_inline_cand_count; ci++) {
                if (g_inline_cands[ci].inline_hint < min_hint) {
                    for (int cj = ci; cj < g_inline_cand_count - 1; cj++)
                        g_inline_cands[cj] = g_inline_cands[cj + 1];
                    g_inline_cand_count--;
                    ci--;
                }
            }
            if (g_inline_cand_count > 0) {
                for (size_t i = 0; i < program->children_count; i++) {
                    ASTNode *child = program->children[i];
                    if (child->type == AST_FUNCTION && child->data.function.body) {
                        inline_stmt(child->data.function.body);
                    }
                }
            }
        }
    }

    if (level < OPT_O1) return program;  /* -O0: no further optimization */

    /* -O1: AST-level optimizations (constant folding, DCE, strength reduction, algebraic) */
    for (size_t i = 0; i < program->children_count; i++) {
        ASTNode *child = program->children[i];
        if (child->type == AST_FUNCTION) {
            if (child->data.function.body) {
                opt_stmt(child->data.function.body);
            }
        }
        /* Global variable initializers */
        if (child->type == AST_VAR_DECL && child->data.var_decl.initializer) {
            child->data.var_decl.initializer = opt_expr(child->data.var_decl.initializer);
        }
    }

    /* -O1: Assert-based value range analysis — extract ranges from assert()
       conditions and use them for constant substitution + strength reduction.
       Must run after O1 folding so assert conditions are simplified. */
    for (size_t i = 0; i < program->children_count; i++) {
        ASTNode *child = program->children[i];
        if (child->type == AST_FUNCTION && child->data.function.body) {
            range_analyze_block(child->data.function.body);
        }
    }

    /* -O2: Within-block constant/copy propagation and dead store elimination */
    if (level >= OPT_O2) {
        for (size_t i = 0; i < program->children_count; i++) {
            ASTNode *child = program->children[i];
            if (child->type == AST_FUNCTION && child->data.function.body) {
                /* Propagate within the function body block */
                o2_propagate_block(child->data.function.body);
            }
        }
    }

    /* -O3: Aggressive optimizations */
    if (level >= OPT_O3) {
        /* Pass 1: Aggressive inlining of multi-statement functions */
        find_aggressive_inline_candidates(program);
        if (g_agg_inline_cand_count > 0) {
            for (size_t i = 0; i < program->children_count; i++) {
                ASTNode *child = program->children[i];
                if (child->type == AST_FUNCTION && child->data.function.body) {
                    /* Don't inline a function into itself */
                    int is_self = 0;
                    for (int ci = 0; ci < g_agg_inline_cand_count; ci++) {
                        if (strcmp(g_agg_inline_cands[ci].name, child->data.function.name) == 0) {
                            is_self = 1;
                            break;
                        }
                    }
                    if (!is_self)
                        o3_aggressive_inline_block(child->data.function.body);
                }
            }
            /* Re-run O1 + O2 passes on the inlined code */
            for (size_t i = 0; i < program->children_count; i++) {
                ASTNode *child = program->children[i];
                if (child->type == AST_FUNCTION && child->data.function.body) {
                    opt_stmt(child->data.function.body);
                    o2_propagate_block(child->data.function.body);
                }
            }
        }

        /* Pass 2: Loop unrolling */
        for (size_t i = 0; i < program->children_count; i++) {
            ASTNode *child = program->children[i];
            if (child->type == AST_FUNCTION && child->data.function.body) {
                o3_unroll_loops(child->data.function.body);
            }
        }

        /* Re-run O1 + O2 passes after unrolling (fold constants, eliminate dead code) */
        for (size_t i = 0; i < program->children_count; i++) {
            ASTNode *child = program->children[i];
            if (child->type == AST_FUNCTION && child->data.function.body) {
                opt_stmt(child->data.function.body);
                o2_propagate_block(child->data.function.body);
            }
        }

        /* Pass 2b: Vectorization — annotate eligible loops for SSE codegen.
           Must run after unrolling + cleanup so loops are in canonical form,
           and before IPA which may modify function boundaries. */
        for (size_t i = 0; i < program->children_count; i++) {
            ASTNode *child = program->children[i];
            if (child->type == AST_FUNCTION && child->data.function.body) {
                o3_vectorize_loops(child->data.function.body);
            }
        }

        /* Pass 3: Interprocedural optimization */

        /* IPA 3a: Return value propagation — replace calls to functions that
           always return the same constant with that constant value. */
        find_rvp_candidates(program);
        if (g_rvp_cand_count > 0) {
            for (size_t i = 0; i < program->children_count; i++) {
                ASTNode *child = program->children[i];
                if (child->type == AST_FUNCTION && child->data.function.body) {
                    rvp_substitute_stmt(child->data.function.body);
                }
            }
        }

        /* IPA 3b: Interprocedural constant propagation — if a parameter is
           always passed the same constant across all call sites, substitute
           it in the function body. */
        ipa_propagate_constants(program);

        /* IPA 3c: Dead argument elimination — remove unused parameters from
           function definitions and their corresponding arguments from call sites. */
        ipa_dead_arg_elimination(program);

        /* IPA 3d: Dead function elimination — remove functions with zero
           callers remaining after inlining and RVP. */
        ipa_dead_function_elimination(program);

        /* Final cleanup: re-run O1 + O2 after IPA passes */
        for (size_t i = 0; i < program->children_count; i++) {
            ASTNode *child = program->children[i];
            if (child->type == AST_FUNCTION && child->data.function.body) {
                opt_stmt(child->data.function.body);
                o2_propagate_block(child->data.function.body);
            }
        }
    }

    return program;
}
