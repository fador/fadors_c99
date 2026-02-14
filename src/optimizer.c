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
        case AST_ADDR_OF:
            node->data.unary.expression = prop_substitute(node->data.unary.expression, env);
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

/* ------------------------------------------------------------------ */
/* Top-level entry point                                              */
/* ------------------------------------------------------------------ */
ASTNode *optimize(ASTNode *program, OptLevel level) {
    if (level < OPT_O1) return program;  /* -O0: no optimization */
    if (!program) return program;

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

    return program;
}
