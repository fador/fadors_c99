#ifndef AST_H
#define AST_H

#include <stddef.h>
#include "lexer.h"
#include "types.h"

typedef enum {
    AST_PROGRAM,
    AST_FUNCTION,
    AST_BLOCK,
    AST_RETURN,
    AST_INTEGER,
    AST_IDENTIFIER,
    AST_BINARY_EXPR,
    AST_VAR_DECL,
    AST_ASSIGN,
    AST_IF,
    AST_WHILE,
    AST_CALL,
    AST_STRUCT_DEF,
    AST_UNION_DEF,
    AST_MEMBER_ACCESS,
    AST_DEREF,
    AST_ADDR_OF,
    AST_STRING,
    AST_FOR,
    AST_ARRAY_ACCESS,
    AST_SWITCH,
    AST_CASE,
    AST_DEFAULT,
    AST_BREAK,
    AST_NEG,
    AST_NOT,
    AST_PRE_INC,
    AST_PRE_DEC,
    AST_POST_INC,
    AST_POST_DEC,
    AST_CAST,
    AST_BITWISE_NOT,
    AST_INIT_LIST,
    AST_GOTO,
    AST_LABEL,
    AST_CONTINUE,
    AST_DO_WHILE,
    AST_UNKNOWN,
    AST_FLOAT
} ASTNodeType;

/* Vectorization metadata (set by optimizer O3 pass) */
typedef struct VecInfo {
    int width;            /* Vector width: 4 for SSE (128-bit, 4x32-bit) */
    int elem_size;        /* Element size in bytes (4 for int/float) */
    int is_float;         /* 1 = float elements, 0 = int elements */
    int op;               /* TokenType: TOKEN_PLUS, TOKEN_MINUS, etc. */
    int iterations;       /* Total loop iteration count */
    const char *loop_var; /* Loop variable name */
    const char *dst;      /* Destination array variable name */
    const char *src1;     /* Source array 1 variable name */
    const char *src2;     /* Source array 2 variable name */
} VecInfo;

typedef struct ASTNode {
    ASTNodeType type;
    struct ASTNode **children;
    size_t children_count;
    Type *resolved_type;
    int line;  // Source line number (for debug info / -g)
    VecInfo *vec_info;  // Vectorization info (NULL = not vectorized)
    
    // For specific nodes
    union {
        struct {
            char *name;
            struct ASTNode *body;
            int inline_hint;  // 0=none, 1=inline, 2=always_inline/__forceinline, -1=noinline
        } function;
        struct {
            long long value;
        } integer;
        struct {
            double value;
        } float_val;
        struct {
            char *name;
        } identifier;
        struct {
            struct ASTNode *body;
        } program;
        struct {
            struct ASTNode *expression;
        } return_stmt;
        struct {
            TokenType op;
            struct ASTNode *left;
            struct ASTNode *right;
        } binary_expr;
        struct {
            char *name;
            struct ASTNode *initializer;
            int is_static;
            int is_extern;
        } var_decl;
        struct {
            struct ASTNode *left;
            struct ASTNode *value;
        } assign;
        struct {
            struct ASTNode *condition;
            struct ASTNode *then_branch;
            struct ASTNode *else_branch;
        } if_stmt;
        struct {
            struct ASTNode *condition;
            struct ASTNode *body;
        } while_stmt;
        struct {
            char *name;
            // args will be in children
        } call;
        struct {
            char *name;
            // members will be in children
        } struct_def;
        struct {
            struct ASTNode *struct_expr;
            char *member_name;
            int is_arrow;
        } member_access;
        struct {
            struct ASTNode *expression;
        } unary;
        struct {
            struct ASTNode *expression;
            Type *target_type;
        } cast;
        struct {
            struct ASTNode *init;
            struct ASTNode *condition;
            struct ASTNode *increment;
            struct ASTNode *body;
        } for_stmt;
        struct {
            struct ASTNode *condition;
            struct ASTNode *body;
        } switch_stmt;
        struct {
            int value;
        } case_stmt;
        struct {
            struct ASTNode *array;
            struct ASTNode *index;
        } array_access;
        struct {
            char *value;
            int length;
        } string;
        struct {
            char *label;
        } goto_stmt;
        struct {
            char *name;
        } label_stmt;
    } data;
} ASTNode;

ASTNode *ast_create_node(ASTNodeType type);
void ast_add_child(ASTNode *parent, ASTNode *child);
void ast_print(ASTNode *node, int indent);

#endif // AST_H
