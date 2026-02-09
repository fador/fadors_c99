#define _CRT_SECURE_NO_WARNINGS
#include "parser.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

void parser_init(Parser *parser, Lexer *lexer) {
    parser->lexer = lexer;
    parser->current_token = lexer_next_token(lexer);
    parser->typedefs_count = 0;
    parser->enum_constants_count = 0;
    parser->structs_count = 0;
    parser->structs_count = 0;
    parser->locals_count = 0;
    parser->globals_count = 0;
}

static void add_local(Parser *parser, const char *name, Type *type) {
    if (name && parser->locals_count < 100) {
        parser->locals[parser->locals_count].name = strdup(name);
        parser->locals[parser->locals_count].type = type;
        parser->locals_count++;
    }
}

static void add_global(Parser *parser, const char *name, Type *type) {
    if (name && parser->globals_count < 100) {
        parser->globals[parser->globals_count].name = strdup(name);
        parser->globals[parser->globals_count].type = type;
        parser->globals_count++;
    }
}

static Type *find_local(Parser *parser, const char *name) {
    for (int i = 0; i < parser->locals_count; i++) {
        if (strcmp(parser->locals[i].name, name) == 0) {
            return parser->locals[i].type;
        }
    }
    return NULL;
}

static Type *find_global(Parser *parser, const char *name) {
    for (int i = 0; i < parser->globals_count; i++) {
        if (strcmp(parser->globals[i].name, name) == 0) {
            return parser->globals[i].type;
        }
    }
    return NULL;
}

static Type *find_variable_type(Parser *parser, const char *name) {
    Type *t = find_local(parser, name);
    if (!t) t = find_global(parser, name);
    return t;
}

static void parser_advance(Parser *parser) {
    parser->current_token = lexer_next_token(parser->lexer);
}

static void parser_expect(Parser *parser, TokenType type) {
    if (parser->current_token.type == type) {
        parser_advance(parser);
    } else {
        printf("Syntax Error: Expected token type %d, got %d ('%.*s') at line %d\n", 
               type, parser->current_token.type, (int)parser->current_token.length, parser->current_token.start, parser->current_token.line);
        fflush(stdout);
        exit(1);
    }
}

static int is_typedef_name(Parser *parser, Token token) {
    if (token.type != TOKEN_IDENTIFIER) return 0;
    for (int i = 0; i < parser->typedefs_count; i++) {
        if (strlen(parser->typedefs[i].name) == token.length &&
            strncmp(parser->typedefs[i].name, token.start, token.length) == 0) {
            return 1;
        }
    }
    return 0;
}

static int is_token_type_start(Parser *parser, Token t); // Forward declaration

static ASTNode *parse_expression(Parser *parser);
static ASTNode *parse_logical_or(Parser *parser);
static ASTNode *parse_logical_and(Parser *parser);
static ASTNode *parse_inclusive_or(Parser *parser);
static ASTNode *parse_exclusive_or(Parser *parser);
static ASTNode *parse_and(Parser *parser);
static ASTNode *parse_equality(Parser *parser);
static ASTNode *parse_relational(Parser *parser);
static ASTNode *parse_shift(Parser *parser);
static ASTNode *parse_additive(Parser *parser);
static ASTNode *parse_multiplicative(Parser *parser);
static ASTNode *parse_unary(Parser *parser);
static ASTNode *parse_tag_body(Parser *parser, Type *type);
static ASTNode *parse_enum_body(Parser *parser, Type *type);

static Type *get_expr_type(Parser *parser, ASTNode *node) {
    if (!node) return NULL;
    if (node->type == AST_INTEGER) return type_int();
    if (node->type == AST_FLOAT) return node->resolved_type ? node->resolved_type : type_double();
    if (node->type == AST_STRING) return type_ptr(type_char());
    if (node->type == AST_IDENTIFIER) {
        Type *t = find_variable_type(parser, node->data.identifier.name);
        return t ? t : type_int();
    } else if (node->type == AST_DEREF) {
        Type *t = get_expr_type(parser, node->data.unary.expression);
        return t ? t->data.ptr_to : NULL;
    } else if (node->type == AST_ADDR_OF) {
        Type *t = get_expr_type(parser, node->data.unary.expression);
        return type_ptr(t);
    } else if (node->type == AST_CALL) {
        Type *t = find_variable_type(parser, node->data.call.name);
        return t ? t : type_int();
    } else if (node->type == AST_MEMBER_ACCESS) {
        Type *st = get_expr_type(parser, node->data.member_access.struct_expr);
        if (node->data.member_access.is_arrow && st && st->kind == TYPE_PTR) {
            st = st->data.ptr_to;
        }
        if (st && (st->kind == TYPE_STRUCT || st->kind == TYPE_UNION)) {
            for (int i = 0; i < st->data.struct_data.members_count; i++) {
                if (strcmp(st->data.struct_data.members[i].name, node->data.member_access.member_name) == 0) {
                    return st->data.struct_data.members[i].type;
                }
            }
        }
        return NULL;
    } else if (node->type == AST_BINARY_EXPR) {
        TokenType op = node->data.binary_expr.op;
        if (op == TOKEN_EQUAL_EQUAL || op == TOKEN_BANG_EQUAL ||
            op == TOKEN_LESS || op == TOKEN_GREATER ||
            op == TOKEN_LESS_EQUAL || op == TOKEN_GREATER_EQUAL ||
            op == TOKEN_AMPERSAND_AMPERSAND || op == TOKEN_PIPE_PIPE) {
            return type_int();
        }
        Type *lt = get_expr_type(parser, node->data.binary_expr.left);
        Type *rt = get_expr_type(parser, node->data.binary_expr.right);
        if (!lt) return rt;
        if (!rt) return lt;
        
        // Apply integer promotion: char -> int
        if (lt->kind == TYPE_CHAR) lt = type_int();
        if (rt->kind == TYPE_CHAR) rt = type_int();
        
        if (lt->kind == TYPE_DOUBLE || rt->kind == TYPE_DOUBLE) return type_double();
        if (lt->kind == TYPE_FLOAT || rt->kind == TYPE_FLOAT) return type_float();
        if (lt->kind == TYPE_PTR) return lt; 
        if (rt->kind == TYPE_PTR) return rt;
        return lt;
    } else if (node->type == AST_NEG || node->type == AST_PRE_INC || node->type == AST_PRE_DEC || 
               node->type == AST_POST_INC || node->type == AST_POST_DEC) {
        return get_expr_type(parser, node->data.unary.expression);
    } else if (node->type == AST_NOT) {
        return type_int();
    } else if (node->type == AST_CAST) {
        return node->data.cast.target_type;
    }
    return NULL;
}

static int eval_constant_expression(ASTNode *node) {
    if (!node) return 0;
    if (node->type == AST_INTEGER) return node->data.integer.value;
    if (node->type == AST_BINARY_EXPR) {
        int left = eval_constant_expression(node->data.binary_expr.left);
        int right = eval_constant_expression(node->data.binary_expr.right);
        switch (node->data.binary_expr.op) {
            case TOKEN_PLUS: return left + right;
            case TOKEN_MINUS: return left - right;
            case TOKEN_STAR: return left * right;
            case TOKEN_SLASH: return right != 0 ? left / right : 0;
            case TOKEN_PERCENT: return right != 0 ? left % right : 0;
            case TOKEN_LESS_LESS: return left << right;
            case TOKEN_GREATER_GREATER: return left >> right;
            default: return 0;
        }
    }
    // Simple fallback
    return 0;
}

static Type *find_struct(Parser *parser, const char *name) {
    for (int i = 0; i < parser->structs_count; i++) {
        if (parser->structs[i]->data.struct_data.name && strcmp(parser->structs[i]->data.struct_data.name, name) == 0) {
            return parser->structs[i];
        }
    }
    return NULL;
}

static Type *parse_type(Parser *parser) {
    Type *type = NULL;
    
    // Skip const qualifier (before type)
    while (parser->current_token.type == TOKEN_KEYWORD_CONST || 
           parser->current_token.type == TOKEN_KEYWORD_STATIC) {
        parser_advance(parser);
    }
    
    if (parser->current_token.type == TOKEN_KEYWORD_UNSIGNED) {
        parser_advance(parser);
        // unsigned int, unsigned char, or just unsigned (= unsigned int)
        if (parser->current_token.type == TOKEN_KEYWORD_INT) {
            parser_advance(parser);
        } else if (parser->current_token.type == TOKEN_KEYWORD_CHAR) {
            type = type_char();
            parser_advance(parser);
            goto done_base;
        }
        type = type_int(); // unsigned int -> int for now
        goto done_base;
    } else if (parser->current_token.type == TOKEN_KEYWORD_INT) {
        type = type_int();
        parser_advance(parser);
    } else if (parser->current_token.type == TOKEN_KEYWORD_CHAR) {
        type = type_char();
        parser_advance(parser);
    } else if (parser->current_token.type == TOKEN_KEYWORD_FLOAT) {
        type = type_float();
        parser_advance(parser);
    } else if (parser->current_token.type == TOKEN_KEYWORD_DOUBLE) {
        type = type_double();
        parser_advance(parser);
    } else if (parser->current_token.type == TOKEN_KEYWORD_VOID) {
        type = type_void();
        parser_advance(parser);
    } else if (parser->current_token.type == TOKEN_KEYWORD_STRUCT || parser->current_token.type == TOKEN_KEYWORD_UNION || parser->current_token.type == TOKEN_KEYWORD_ENUM) {
        TokenType tag_kind = parser->current_token.type;
        parser_advance(parser);
        if (parser->current_token.type == TOKEN_IDENTIFIER) {
            char *name = malloc(parser->current_token.length + 1);
            strncpy(name, parser->current_token.start, parser->current_token.length);
            name[parser->current_token.length] = '\0';
            parser_advance(parser);
            
            if (tag_kind == TOKEN_KEYWORD_ENUM) {
                type = type_enum(name);
            } else if (tag_kind == TOKEN_KEYWORD_UNION) {
                type = find_struct(parser, name); // find_struct handles both struct and union tags
                if (!type) type = type_union(name);
            } else { // TOKEN_KEYWORD_STRUCT
                type = find_struct(parser, name);
                if (!type) type = type_struct(name);
            }
            free(name);
        } else {
            // Anonymous struct/union/enum
            if (tag_kind == TOKEN_KEYWORD_ENUM) type = type_enum(NULL);
            else if (tag_kind == TOKEN_KEYWORD_UNION) type = type_union(NULL);
            else type = type_struct(NULL);
        }
    } else if (parser->current_token.type == TOKEN_IDENTIFIER) {
        // Check for typedefs
        for (int i = 0; i < parser->typedefs_count; i++) {
            if (strlen(parser->typedefs[i].name) == parser->current_token.length &&
                strncmp(parser->typedefs[i].name, parser->current_token.start, parser->current_token.length) == 0) {
                type = parser->typedefs[i].type;
                parser_advance(parser);
                break;
            }
        }
    }
    
    done_base:
    if (!type) return NULL;
    
    // Skip const qualifier (after type, e.g. "int const")
    while (parser->current_token.type == TOKEN_KEYWORD_CONST) {
        parser_advance(parser);
    }
    
    while (parser->current_token.type == TOKEN_STAR) {
        type = type_ptr(type);
        parser_advance(parser);
        // Skip const after pointer (e.g., "const char *const")
        while (parser->current_token.type == TOKEN_KEYWORD_CONST) {
            parser_advance(parser);
        }
    }
    return type;
}

static ASTNode *parse_primary(Parser *parser) {
    if (parser->current_token.type == TOKEN_NUMBER) {
        ASTNode *node = ast_create_node(AST_INTEGER);
        char buffer[64];
        size_t len = parser->current_token.length < 63 ? parser->current_token.length : 63;
        strncpy(buffer, parser->current_token.start, len);
        buffer[len] = '\0';
        node->data.integer.value = atoi(buffer);
        parser_advance(parser);
        return node;
    } else if (parser->current_token.type == TOKEN_FLOAT) {
        ASTNode *node = ast_create_node(AST_FLOAT);
        char buffer[64];
        size_t len = parser->current_token.length < 63 ? parser->current_token.length : 63;
        strncpy(buffer, parser->current_token.start, len);
        buffer[len] = '\0';
        node->resolved_type = type_double(); // Default
        if (buffer[len-1] == 'f' || buffer[len-1] == 'F') {
             node->resolved_type = type_float();
        }
        // atof/strtod handles optional 'f' suffix? No, standard atof might stop at 'f'.
        // Let's strip 'f' just in case or rely on stdlib behavior.
        // Actually simple atof("1.0f") works on most platforms, stopping at 'f'.
        node->data.float_val.value = atof(buffer);
        parser_advance(parser);
        return node;
    } else if (parser->current_token.type == TOKEN_IDENTIFIER) {
        char *name = malloc(parser->current_token.length + 1);
        strncpy(name, parser->current_token.start, parser->current_token.length);
        name[parser->current_token.length] = '\0';
        
        // Check for enum constants
        for (int i = 0; i < parser->enum_constants_count; i++) {
            if (strcmp(parser->enum_constants[i].name, name) == 0) {
                ASTNode *node = ast_create_node(AST_INTEGER);
                node->data.integer.value = parser->enum_constants[i].value;
                parser_advance(parser);
                free(name);
                return node;
            }
        }
        
        parser_advance(parser);
        
        if (parser->current_token.type == TOKEN_LPAREN) {
            parser_advance(parser);
            ASTNode *node = ast_create_node(AST_CALL);
            node->data.call.name = name;
            while (parser->current_token.type != TOKEN_RPAREN && parser->current_token.type != TOKEN_EOF) {
                ast_add_child(node, parse_expression(parser));
                if (parser->current_token.type == TOKEN_COMMA) {
                    parser_advance(parser);
                }
            }
            parser_expect(parser, TOKEN_RPAREN);
            return node;
        } else {
            ASTNode *node = ast_create_node(AST_IDENTIFIER);
            node->data.identifier.name = name;
            node->resolved_type = find_variable_type(parser, name);
            return node;
        }
    } else if (parser->current_token.type == TOKEN_STRING) {
        ASTNode *node = ast_create_node(AST_STRING);
        char *val = malloc(parser->current_token.length + 1);
        if (parser->current_token.start != NULL) {
            strncpy(val, parser->current_token.start, parser->current_token.length);
        }
        val[parser->current_token.length] = '\0';
        node->data.string.value = val;
        parser_advance(parser);
        return node;
    } else if (parser->current_token.type == TOKEN_LPAREN) {
        parser_advance(parser);
        ASTNode *node = parse_expression(parser);
        parser_expect(parser, TOKEN_RPAREN);
        return node;
    }
    printf("Syntax Error: Unexpected token %d ('%.*s') in expression at line %d\n", 
           parser->current_token.type, (int)parser->current_token.length, parser->current_token.start, parser->current_token.line);
    fflush(stdout);
    exit(1);
}

static ASTNode *parse_postfix(Parser *parser) {
    ASTNode *node = parse_primary(parser);
    while (parser->current_token.type == TOKEN_DOT || parser->current_token.type == TOKEN_ARROW || 
           parser->current_token.type == TOKEN_LBRACKET || 
           parser->current_token.type == TOKEN_PLUS_PLUS || parser->current_token.type == TOKEN_MINUS_MINUS) {
        if (parser->current_token.type == TOKEN_LBRACKET) {
            parser_advance(parser);
            ASTNode *index = parse_expression(parser);
            parser_expect(parser, TOKEN_RBRACKET);
            
            ASTNode *access = ast_create_node(AST_ARRAY_ACCESS);
            access->data.array_access.array = node;
            access->data.array_access.index = index;
            
            // Resolve type: if node is array or pointer, element is the base type
            if (node->resolved_type && (node->resolved_type->kind == TYPE_ARRAY || node->resolved_type->kind == TYPE_PTR)) {
                access->resolved_type = node->resolved_type->data.ptr_to;
            }
            node = access;
        } else if (parser->current_token.type == TOKEN_PLUS_PLUS) {
            parser_advance(parser);
            ASTNode *inc = ast_create_node(AST_POST_INC);
            inc->data.unary.expression = node;
            if (node->resolved_type) inc->resolved_type = node->resolved_type;
            node = inc;
        } else if (parser->current_token.type == TOKEN_MINUS_MINUS) {
            parser_advance(parser);
            ASTNode *dec = ast_create_node(AST_POST_DEC);
            dec->data.unary.expression = node;
            if (node->resolved_type) dec->resolved_type = node->resolved_type;
            node = dec;
        } else {
            int is_arrow = (parser->current_token.type == TOKEN_ARROW);
            parser_advance(parser);
            if (parser->current_token.type == TOKEN_IDENTIFIER) {
                char *member_name = malloc(parser->current_token.length + 1);
                strncpy(member_name, parser->current_token.start, parser->current_token.length);
                member_name[parser->current_token.length] = '\0';
                parser_advance(parser);
                
                ASTNode *access = ast_create_node(AST_MEMBER_ACCESS);
                access->data.member_access.struct_expr = node;
                access->data.member_access.member_name = member_name;
                access->data.member_access.is_arrow = is_arrow;
                
                // Resolve type
                Type *struct_type = node->resolved_type;
                if (is_arrow && struct_type && struct_type->kind == TYPE_PTR) {
                    struct_type = struct_type->data.ptr_to;
                }
                if (struct_type && struct_type->kind == TYPE_STRUCT) {
                    for (int i = 0; i < struct_type->data.struct_data.members_count; i++) {
                        if (strcmp(struct_type->data.struct_data.members[i].name, member_name) == 0) {
                            access->resolved_type = struct_type->data.struct_data.members[i].type;
                            break;
                        }
                    }
                }
                node = access;
            } else {
                printf("Syntax Error: Expected member name after %s at line %d\n", is_arrow ? "->" : ".", parser->current_token.line);
                exit(1);
            }
        }
    }
    return node;
}

static ASTNode *parse_cast(Parser *parser);

static ASTNode *parse_unary(Parser *parser) {
    if (parser->current_token.type == TOKEN_KEYWORD_SIZEOF) {
        parser_advance(parser);
        int size = 0;
        if (parser->current_token.type == TOKEN_LPAREN) {
            Token next = lexer_peek_token(parser->lexer);
            if (is_token_type_start(parser, next)) {
                parser_advance(parser); // (
                Type *type = parse_type(parser);
                parser_expect(parser, TOKEN_RPAREN);
                size = type->size;
            } else {
                ASTNode *expr = parse_unary(parser);
                Type *t = get_expr_type(parser, expr);
                size = t ? t->size : 1; 
            }
        } else {
            ASTNode *expr = parse_unary(parser);
            Type *t = get_expr_type(parser, expr);
            size = t ? t->size : 1;
        }
        
        ASTNode *node = ast_create_node(AST_INTEGER);
        node->data.integer.value = size;
        return node;
    } else if (parser->current_token.type == TOKEN_PLUS_PLUS) {
        parser_advance(parser);
        ASTNode *node = ast_create_node(AST_PRE_INC);
        node->data.unary.expression = parse_unary(parser);
        return node;
    } else if (parser->current_token.type == TOKEN_MINUS_MINUS) {
        parser_advance(parser);
        ASTNode *node = ast_create_node(AST_PRE_DEC);
        node->data.unary.expression = parse_unary(parser);
        return node;
    } else if (parser->current_token.type == TOKEN_STAR) {
        parser_advance(parser);
        ASTNode *node = ast_create_node(AST_DEREF);
        node->data.unary.expression = parse_cast(parser);
        return node;
    } else if (parser->current_token.type == TOKEN_AMPERSAND) {
        parser_advance(parser);
        ASTNode *node = ast_create_node(AST_ADDR_OF);
        node->data.unary.expression = parse_cast(parser);
        return node;
    } else if (parser->current_token.type == TOKEN_MINUS) {
        parser_advance(parser);
        ASTNode *node = ast_create_node(AST_NEG);
        node->data.unary.expression = parse_cast(parser);
        return node;
    } else if (parser->current_token.type == TOKEN_BANG) {
        parser_advance(parser);
        ASTNode *node = ast_create_node(AST_NOT);
        node->data.unary.expression = parse_cast(parser);
        return node;
    }
    return parse_postfix(parser);
}

static int is_token_type_start(Parser *parser, Token t) {
    TokenType type = t.type;
    return (type == TOKEN_KEYWORD_INT || 
            type == TOKEN_KEYWORD_CHAR || 
            type == TOKEN_KEYWORD_FLOAT || 
            type == TOKEN_KEYWORD_DOUBLE || 
            type == TOKEN_KEYWORD_VOID || 
            type == TOKEN_KEYWORD_STRUCT || 
            type == TOKEN_KEYWORD_UNION || 
            type == TOKEN_KEYWORD_ENUM || 
            type == TOKEN_KEYWORD_CONST ||
            type == TOKEN_KEYWORD_STATIC ||
            type == TOKEN_KEYWORD_UNSIGNED ||
            is_typedef_name(parser, t));
}

static ASTNode *parse_cast(Parser *parser) {
    if (parser->current_token.type == TOKEN_LPAREN) {
        Token next = lexer_peek_token(parser->lexer);
        if (is_token_type_start(parser, next)) {
            parser_advance(parser); // (
            Type *type = parse_type(parser);
            parser_expect(parser, TOKEN_RPAREN);
            
            ASTNode *node = ast_create_node(AST_CAST);
            node->data.cast.target_type = type;
            node->data.cast.expression = parse_cast(parser);
            node->resolved_type = type;
            return node;
        }
    }
    return parse_unary(parser);
}

static ASTNode *parse_multiplicative(Parser *parser) {
    ASTNode *left = parse_cast(parser);
    while (parser->current_token.type == TOKEN_STAR || parser->current_token.type == TOKEN_SLASH || parser->current_token.type == TOKEN_PERCENT) {
        TokenType op = parser->current_token.type;
        parser_advance(parser);
        ASTNode *node = ast_create_node(AST_BINARY_EXPR);
        node->data.binary_expr.op = op;
        node->data.binary_expr.left = left;
        node->data.binary_expr.right = parse_cast(parser);
        left = node;
    }
    return left;
}

static ASTNode *parse_shift(Parser *parser) {
    ASTNode *left = parse_additive(parser);
    while (parser->current_token.type == TOKEN_LESS_LESS || parser->current_token.type == TOKEN_GREATER_GREATER) {
        TokenType op = parser->current_token.type;
        parser_advance(parser);
        ASTNode *node = ast_create_node(AST_BINARY_EXPR);
        node->data.binary_expr.op = op;
        node->data.binary_expr.left = left;
        node->data.binary_expr.right = parse_additive(parser);
        left = node;
    }
    return left;
}

static ASTNode *parse_additive(Parser *parser) {
    ASTNode *left = parse_multiplicative(parser);
    while (parser->current_token.type == TOKEN_PLUS || parser->current_token.type == TOKEN_MINUS) {
        TokenType op = parser->current_token.type;
        parser_advance(parser);
        ASTNode *node = ast_create_node(AST_BINARY_EXPR);
        node->data.binary_expr.op = op;
        node->data.binary_expr.left = left;
        node->data.binary_expr.right = parse_multiplicative(parser);
        left = node;
    }
    return left;
}


static ASTNode *parse_relational(Parser *parser) {
    ASTNode *left = parse_shift(parser);
    while (parser->current_token.type == TOKEN_LESS || parser->current_token.type == TOKEN_GREATER ||
           parser->current_token.type == TOKEN_LESS_EQUAL || parser->current_token.type == TOKEN_GREATER_EQUAL) {
        TokenType op = parser->current_token.type;
        parser_advance(parser);
        ASTNode *node = ast_create_node(AST_BINARY_EXPR);
        node->data.binary_expr.op = op;
        node->data.binary_expr.left = left;
        node->data.binary_expr.right = parse_shift(parser);
        left = node;
    }
    return left;
}

static ASTNode *parse_equality(Parser *parser) {
    ASTNode *left = parse_relational(parser);
    while (parser->current_token.type == TOKEN_EQUAL_EQUAL || parser->current_token.type == TOKEN_BANG_EQUAL) {
        TokenType op = parser->current_token.type;
        parser_advance(parser);
        ASTNode *node = ast_create_node(AST_BINARY_EXPR);
        node->data.binary_expr.op = op;
        node->data.binary_expr.left = left;
        node->data.binary_expr.right = parse_relational(parser);
        left = node;
    }
    return left;
}

static ASTNode *parse_and(Parser *parser) {
    ASTNode *left = parse_equality(parser);
    while (parser->current_token.type == TOKEN_AMPERSAND) {
        parser_advance(parser);
        ASTNode *node = ast_create_node(AST_BINARY_EXPR);
        node->data.binary_expr.op = TOKEN_AMPERSAND;
        node->data.binary_expr.left = left;
        node->data.binary_expr.right = parse_equality(parser);
        left = node;
    }
    return left;
}

static ASTNode *parse_exclusive_or(Parser *parser) {
    ASTNode *left = parse_and(parser);
    while (parser->current_token.type == TOKEN_CARET) {
        parser_advance(parser);
        ASTNode *node = ast_create_node(AST_BINARY_EXPR);
        node->data.binary_expr.op = TOKEN_CARET;
        node->data.binary_expr.left = left;
        node->data.binary_expr.right = parse_and(parser);
        left = node;
    }
    return left;
}

static ASTNode *parse_inclusive_or(Parser *parser) {
    ASTNode *left = parse_exclusive_or(parser);
    while (parser->current_token.type == TOKEN_PIPE) {
        parser_advance(parser);
        ASTNode *node = ast_create_node(AST_BINARY_EXPR);
        node->data.binary_expr.op = TOKEN_PIPE;
        node->data.binary_expr.left = left;
        node->data.binary_expr.right = parse_exclusive_or(parser);
        left = node;
    }
    return left;
}

static ASTNode *parse_logical_and(Parser *parser) {
    ASTNode *left = parse_inclusive_or(parser);
    while (parser->current_token.type == TOKEN_AMPERSAND_AMPERSAND) {
        parser_advance(parser);
        ASTNode *node = ast_create_node(AST_BINARY_EXPR);
        node->data.binary_expr.op = TOKEN_AMPERSAND_AMPERSAND;
        node->data.binary_expr.left = left;
        node->data.binary_expr.right = parse_inclusive_or(parser);
        left = node;
    }
    return left;
}

static ASTNode *parse_logical_or(Parser *parser) {
    ASTNode *left = parse_logical_and(parser);
    while (parser->current_token.type == TOKEN_PIPE_PIPE) {
        parser_advance(parser);
        ASTNode *node = ast_create_node(AST_BINARY_EXPR);
        node->data.binary_expr.op = TOKEN_PIPE_PIPE;
        node->data.binary_expr.left = left;
        node->data.binary_expr.right = parse_logical_and(parser);
        left = node;
    }
    return left;
}

static TokenType get_compound_op(TokenType token) {
    switch (token) {
        case TOKEN_PLUS_EQUAL: return TOKEN_PLUS;
        case TOKEN_MINUS_EQUAL: return TOKEN_MINUS;
        case TOKEN_STAR_EQUAL: return TOKEN_STAR;
        case TOKEN_SLASH_EQUAL: return TOKEN_SLASH;
        case TOKEN_PERCENT_EQUAL: return TOKEN_PERCENT;
        case TOKEN_PIPE_EQUAL: return TOKEN_PIPE;
        case TOKEN_AMPERSAND_EQUAL: return TOKEN_AMPERSAND;
        case TOKEN_CARET_EQUAL: return TOKEN_CARET;
        case TOKEN_LESS_LESS_EQUAL: return TOKEN_LESS_LESS;
        case TOKEN_GREATER_GREATER_EQUAL: return TOKEN_GREATER_GREATER;
        default: return TOKEN_UNKNOWN;
    }
}

static int is_compound_assign(TokenType t) {
    return t == TOKEN_PLUS_EQUAL || t == TOKEN_MINUS_EQUAL ||
           t == TOKEN_STAR_EQUAL || t == TOKEN_SLASH_EQUAL ||
           t == TOKEN_PERCENT_EQUAL || t == TOKEN_PIPE_EQUAL ||
           t == TOKEN_AMPERSAND_EQUAL || t == TOKEN_CARET_EQUAL ||
           t == TOKEN_LESS_LESS_EQUAL || t == TOKEN_GREATER_GREATER_EQUAL;
}

static ASTNode *parse_expression(Parser *parser) {
    ASTNode *left = parse_logical_or(parser);
    
    if (parser->current_token.type == TOKEN_EQUAL) {
        parser_advance(parser);
        ASTNode *node = ast_create_node(AST_ASSIGN);
        node->data.assign.left = left;
        node->data.assign.value = parse_expression(parser);
        return node;
    }
    
    if (is_compound_assign(parser->current_token.type)) {
        // Desugar: x += y  ->  x = x + y
        TokenType op = get_compound_op(parser->current_token.type);
        parser_advance(parser);
        ASTNode *rhs = parse_expression(parser);
        ASTNode *bin = ast_create_node(AST_BINARY_EXPR);
        bin->data.binary_expr.op = op;
        bin->data.binary_expr.left = left;
        bin->data.binary_expr.right = rhs;
        ASTNode *assign = ast_create_node(AST_ASSIGN);
        assign->data.assign.left = left;  // Note: left is shared (evaluated twice)
        assign->data.assign.value = bin;
        return assign;
    }
    
    // Ternary: condition ? true_expr : false_expr
    if (parser->current_token.type == TOKEN_QUESTION) {
        parser_advance(parser);
        ASTNode *node = ast_create_node(AST_IF);
        node->data.if_stmt.condition = left;
        node->data.if_stmt.then_branch = parse_expression(parser);
        parser_expect(parser, TOKEN_COLON);
        node->data.if_stmt.else_branch = parse_expression(parser);
        return node;
    }
    
    return left;
}

static ASTNode *parse_initializer(Parser *parser) {
    if (parser->current_token.type == TOKEN_LBRACE) {
        parser_advance(parser); // consume '{'
        ASTNode *list = ast_create_node(AST_INIT_LIST);
        while (parser->current_token.type != TOKEN_RBRACE && parser->current_token.type != TOKEN_EOF) {
            ASTNode *elem = parse_initializer(parser); // recursive for nested {}'s
            ast_add_child(list, elem);
            if (parser->current_token.type == TOKEN_COMMA) {
                parser_advance(parser);
            } else {
                break;
            }
        }
        parser_expect(parser, TOKEN_RBRACE);
        return list;
    }
    return parse_expression(parser);
}

static ASTNode *parse_statement(Parser *parser) {
    if (parser->current_token.type == TOKEN_KEYWORD_RETURN) {
        parser_advance(parser);
        ASTNode *node = ast_create_node(AST_RETURN);
        if (parser->current_token.type == TOKEN_SEMICOLON) {
            node->data.return_stmt.expression = NULL;
        } else {
            node->data.return_stmt.expression = parse_expression(parser);
        }
        parser_expect(parser, TOKEN_SEMICOLON);
        return node;
    } else if (parser->current_token.type == TOKEN_KEYWORD_TYPEDEF) {
        parser_advance(parser);
        Type *type = parse_type(parser);
        if (!type) {
            printf("Syntax Error: Expected type after 'typedef' at line %d\n", parser->current_token.line);
            exit(1);
        }
        if (parser->current_token.type == TOKEN_IDENTIFIER) {
            char *name = malloc(parser->current_token.length + 1);
            strncpy(name, parser->current_token.start, parser->current_token.length);
            name[parser->current_token.length] = '\0';
            parser_advance(parser);

            if (parser->typedefs_count < 100) {
                parser->typedefs[parser->typedefs_count].name = name;
                parser->typedefs[parser->typedefs_count].type = type;
                parser->typedefs_count++;
            }
            parser_expect(parser, TOKEN_SEMICOLON);
            return NULL; // typedef doesn't produce an AST node for execution
        } else {
            printf("Syntax Error: Expected name for typedef at line %d\n", parser->current_token.line);
            exit(1);
        }
    } else if (parser->current_token.type == TOKEN_KEYWORD_INT || 
               parser->current_token.type == TOKEN_KEYWORD_CHAR ||
               parser->current_token.type == TOKEN_KEYWORD_FLOAT ||
               parser->current_token.type == TOKEN_KEYWORD_DOUBLE ||
               parser->current_token.type == TOKEN_KEYWORD_VOID ||
               parser->current_token.type == TOKEN_KEYWORD_EXTERN ||
               parser->current_token.type == TOKEN_KEYWORD_STRUCT ||
               parser->current_token.type == TOKEN_KEYWORD_UNION ||
               parser->current_token.type == TOKEN_KEYWORD_ENUM ||
               parser->current_token.type == TOKEN_KEYWORD_CONST ||
               parser->current_token.type == TOKEN_KEYWORD_STATIC ||
               parser->current_token.type == TOKEN_KEYWORD_UNSIGNED ||
               is_typedef_name(parser, parser->current_token)) {
        // Variable or struct/union/enum declaration/definition
        if (parser->current_token.type == TOKEN_KEYWORD_STRUCT || parser->current_token.type == TOKEN_KEYWORD_UNION || parser->current_token.type == TOKEN_KEYWORD_ENUM) {
            TokenType tag_kind = parser->current_token.type;
            Token next = lexer_peek_token(parser->lexer);
            
            if (next.type == TOKEN_LBRACE) {
                // Anonymous definition: struct { ... }
                parser_advance(parser); // Consume keyword
                Type *type;
                if (tag_kind == TOKEN_KEYWORD_ENUM) {
                    type = type_enum(NULL);
                    return parse_enum_body(parser, type);
                } else {
                    type = (tag_kind == TOKEN_KEYWORD_UNION) ? type_union(NULL) : type_struct(NULL);
                    return parse_tag_body(parser, type);
                }
            } else if (next.type == TOKEN_IDENTIFIER) {
                // Potential definition or just type usage
                parser_advance(parser); // Consume keyword
                char *name = malloc(parser->current_token.length + 1);
                strncpy(name, parser->current_token.start, parser->current_token.length);
                name[parser->current_token.length] = '\0';
                
                Token check = lexer_peek_token(parser->lexer);
                if (check.type == TOKEN_LBRACE) {
                    parser_advance(parser); // Consume Name
                    Type *type;
                    if (tag_kind == TOKEN_KEYWORD_ENUM) {
                        type = type_enum(name);
                        return parse_enum_body(parser, type);
                    } else {
                        type = (tag_kind == TOKEN_KEYWORD_UNION) ? type_union(name) : type_struct(name);
                        return parse_tag_body(parser, type);
                    }
                }
                
                // Variable declaration: struct Name var;
                Type *type;
                if (tag_kind == TOKEN_KEYWORD_ENUM) type = type_enum(name);
                else {
                    type = find_struct(parser, name);
                    if (!type) {
                        type = (tag_kind == TOKEN_KEYWORD_UNION) ? type_union(name) : type_struct(name);
                    }
                }
                free(name);
                parser_advance(parser); // Consume Name
                
                // Now parse pointers/variable name
                while (parser->current_token.type == TOKEN_STAR) {
                    type = type_ptr(type);
                    parser_advance(parser);
                }
                
                ASTNode *node = ast_create_node(AST_VAR_DECL);
                node->resolved_type = type;
                if (parser->current_token.type == TOKEN_IDENTIFIER) {
                    node->data.var_decl.name = malloc(parser->current_token.length + 1);
                    strncpy(node->data.var_decl.name, parser->current_token.start, parser->current_token.length);
                    node->data.var_decl.name[parser->current_token.length] = '\0';
                    parser_advance(parser);
                    add_local(parser, node->data.var_decl.name, node->resolved_type);
                    
                    if (parser->current_token.type == TOKEN_EQUAL) {
                        parser_advance(parser);
                        node->data.var_decl.initializer = parse_initializer(parser);
                    } else {
                        node->data.var_decl.initializer = NULL;
                    }
                    parser_expect(parser, TOKEN_SEMICOLON);
                    return node;
                } else if (parser->current_token.type == TOKEN_SEMICOLON) {
                    parser_advance(parser);
                    return NULL;
                }
            }
        }

        Type *type = parse_type(parser);
        if (!type) {
            // Error handling...
        }
        
        ASTNode *node = ast_create_node(AST_VAR_DECL);
        node->resolved_type = type;
        if (parser->current_token.type == TOKEN_IDENTIFIER) {
            node->data.var_decl.name = malloc(parser->current_token.length + 1);
            memcpy(node->data.var_decl.name, parser->current_token.start, parser->current_token.length);
            node->data.var_decl.name[parser->current_token.length] = '\0';
            parser_advance(parser);
            if (parser->current_token.type == TOKEN_LBRACKET) {
                parser_advance(parser);
                if (parser->current_token.type != TOKEN_RBRACKET) {
                    ASTNode *size_expr = parse_expression(parser);
                    int size = eval_constant_expression(size_expr);
                    node->resolved_type = type_array(node->resolved_type, size);
                }
                parser_expect(parser, TOKEN_RBRACKET);
            }

            add_local(parser, node->data.var_decl.name, node->resolved_type);

            if (parser->current_token.type == TOKEN_EQUAL) {
                parser_advance(parser);
                node->data.var_decl.initializer = parse_initializer(parser);
            } else {
                node->data.var_decl.initializer = NULL;
            }
            parser_expect(parser, TOKEN_SEMICOLON);
            return node;
        } else {
            // Not a variable declaration, maybe a stray struct definition or error?
            // If it was 'struct Point {', parse_type consumed 'struct Point'.
            // Current token is '{'.
            // We should ideally error out or backtrack, but for now let's just error
            printf("Syntax Error: Expected variable name after type at line %d. (Struct definitions inside functions not fully supported yet)\n", parser->current_token.line);
            exit(1); 
        }
    } else if (parser->current_token.type == TOKEN_KEYWORD_WHILE) {
        parser_advance(parser);
        ASTNode *node = ast_create_node(AST_WHILE);
        parser_expect(parser, TOKEN_LPAREN);
        node->data.while_stmt.condition = parse_expression(parser);
        parser_expect(parser, TOKEN_RPAREN);
        node->data.while_stmt.body = parse_statement(parser);
        return node;
    } else if (parser->current_token.type == TOKEN_KEYWORD_FOR) {
        parser_advance(parser);
        ASTNode *node = ast_create_node(AST_FOR);
        parser_expect(parser, TOKEN_LPAREN);

        // Init
        if (parser->current_token.type == TOKEN_SEMICOLON) {
            node->data.for_stmt.init = NULL;
            parser_advance(parser);
        } else {
            if (parser->current_token.type == TOKEN_KEYWORD_INT || 
                parser->current_token.type == TOKEN_KEYWORD_CHAR ||
                parser->current_token.type == TOKEN_KEYWORD_FLOAT ||
                parser->current_token.type == TOKEN_KEYWORD_DOUBLE ||
                parser->current_token.type == TOKEN_KEYWORD_VOID ||
                parser->current_token.type == TOKEN_KEYWORD_STRUCT ||
                parser->current_token.type == TOKEN_KEYWORD_CONST ||
                parser->current_token.type == TOKEN_KEYWORD_STATIC ||
                parser->current_token.type == TOKEN_KEYWORD_UNSIGNED) {
                node->data.for_stmt.init = parse_statement(parser);
            } else {
                node->data.for_stmt.init = parse_expression(parser);
                parser_expect(parser, TOKEN_SEMICOLON);
            }
        }

        // Condition
        if (parser->current_token.type == TOKEN_SEMICOLON) {
            node->data.for_stmt.condition = NULL;
            parser_advance(parser);
        } else {
            node->data.for_stmt.condition = parse_expression(parser);
            parser_expect(parser, TOKEN_SEMICOLON);
        }

        // Increment
        if (parser->current_token.type == TOKEN_RPAREN) {
            node->data.for_stmt.increment = NULL;
        } else {
            node->data.for_stmt.increment = parse_expression(parser);
        }
        parser_expect(parser, TOKEN_RPAREN);

        node->data.for_stmt.body = parse_statement(parser);
        return node;
    } else if (parser->current_token.type == TOKEN_KEYWORD_IF) {
        parser_advance(parser);
        ASTNode *node = ast_create_node(AST_IF);
        parser_expect(parser, TOKEN_LPAREN);
        node->data.if_stmt.condition = parse_expression(parser);
        parser_expect(parser, TOKEN_RPAREN);
        node->data.if_stmt.then_branch = parse_statement(parser);
        if (parser->current_token.type == TOKEN_KEYWORD_ELSE) {
            parser_advance(parser);
            node->data.if_stmt.else_branch = parse_statement(parser);
        } else {
            node->data.if_stmt.else_branch = NULL;
        }
        return node;
    } else if (parser->current_token.type == TOKEN_KEYWORD_SWITCH) {
        parser_advance(parser);
        ASTNode *node = ast_create_node(AST_SWITCH);
        parser_expect(parser, TOKEN_LPAREN);
        node->data.switch_stmt.condition = parse_expression(parser);
        parser_expect(parser, TOKEN_RPAREN);
        node->data.switch_stmt.body = parse_statement(parser);
        return node;
    } else if (parser->current_token.type == TOKEN_KEYWORD_CASE) {
        parser_advance(parser);
        ASTNode *node = ast_create_node(AST_CASE);
        // Expect a constant expression (integer for now)
        if (parser->current_token.type == TOKEN_NUMBER) {
            char buffer[64];
            size_t len = parser->current_token.length < 63 ? parser->current_token.length : 63;
            strncpy(buffer, parser->current_token.start, len);
            buffer[len] = '\0';
            node->data.case_stmt.value = atoi(buffer);
            parser_advance(parser);
        } else {
            printf("Syntax Error: Expected constant integer after 'case' at line %d\n", parser->current_token.line);
            exit(1);
        }
        parser_expect(parser, TOKEN_COLON);
        return node;
    } else if (parser->current_token.type == TOKEN_KEYWORD_DEFAULT) {
        parser_advance(parser);
        ASTNode *node = ast_create_node(AST_DEFAULT);
        parser_expect(parser, TOKEN_COLON);
        return node;
    } else if (parser->current_token.type == TOKEN_KEYWORD_BREAK) {
        parser_advance(parser);
        ASTNode *node = ast_create_node(AST_BREAK);
        parser_expect(parser, TOKEN_SEMICOLON);
        return node;
    } else if (parser->current_token.type == TOKEN_LBRACE) {
        // Handle blocks as statements
        extern ASTNode *parse_block(Parser *parser);
        return parse_block(parser);
    }
    
    // Assignment or expression statement?
    ASTNode *expr = parse_expression(parser);
    parser_expect(parser, TOKEN_SEMICOLON);
    return expr;
}

ASTNode *parse_block(Parser *parser) {
    parser_expect(parser, TOKEN_LBRACE);
    ASTNode *node = ast_create_node(AST_BLOCK);
    while (parser->current_token.type != TOKEN_RBRACE && parser->current_token.type != TOKEN_EOF) {
        ASTNode *stmt = parse_statement(parser);
        if (stmt) {
            ast_add_child(node, stmt);
        }
    }
    parser_expect(parser, TOKEN_RBRACE);
    return node;
}

static ASTNode *parse_external_declaration(Parser *parser) {
    if (parser->current_token.type == TOKEN_KEYWORD_EXTERN) {
        parser_advance(parser);
    }
    
    Type *type = parse_type(parser);
    if (!type) {
        printf("Syntax Error: Expected return type or variable type at line %d\n", parser->current_token.line);
        exit(1);
    }
    
    ASTNode *node;
    
    if (parser->current_token.type == TOKEN_IDENTIFIER) {
        char *name = malloc(parser->current_token.length + 1);
        strncpy(name, parser->current_token.start, parser->current_token.length);
        name[parser->current_token.length] = '\0';
        parser_advance(parser);

        // Check for function or variable
        if (parser->current_token.type == TOKEN_LPAREN) {
            // Function
            parser->locals_count = 0; // Clear locals for new function
            node = ast_create_node(AST_FUNCTION);
            node->resolved_type = type;
            node->data.function.name = name;
            
            parser_advance(parser); // Consume '('
            while (parser->current_token.type != TOKEN_RPAREN && parser->current_token.type != TOKEN_EOF) {
                Type *param_type = parse_type(parser);
                if (!param_type) {
                    if (parser->current_token.type == TOKEN_ELLIPSIS) {
                        parser_advance(parser);
                        parser_expect(parser, TOKEN_RPAREN); // Must be last
                        // Mark function as variadic if we had a way, but type system assumes it for now
                        // We need to return the node, but we consumed ')'
                        // The loop condition checks for RPAREN, so breaking is trickier if we already consumed it.
                        // Let's just return here.
                        
                        if (parser->current_token.type == TOKEN_SEMICOLON) {
                            parser_advance(parser);
                            node->data.function.body = NULL;
                        } else {
                            node->data.function.body = parse_block(parser);
                        }
                        return node;
                        // Actually, the loop logic expects to continue if not RPAREN.
                        // But ... must be last.
                    }
                    printf("Syntax Error: Expected parameter type or '...' at line %d\n", parser->current_token.line);
                    exit(1);
                }
                
                ASTNode *param = ast_create_node(AST_VAR_DECL);
                param->resolved_type = param_type;
                
                if (parser->current_token.type == TOKEN_IDENTIFIER) {
                    param->data.var_decl.name = malloc(parser->current_token.length + 1);
                    strncpy(param->data.var_decl.name, parser->current_token.start, parser->current_token.length);
                    param->data.var_decl.name[parser->current_token.length] = '\0';
                    parser_advance(parser);
                    add_local(parser, param->data.var_decl.name, param->resolved_type);
                } else {
                    param->data.var_decl.name = NULL;
                }
                
                ast_add_child(node, param);
                
                if (parser->current_token.type == TOKEN_COMMA) {
                    parser_advance(parser);
                } else if (parser->current_token.type != TOKEN_RPAREN) {
                    printf("Syntax Error: Expected ',' or ')' in parameter list at line %d\n", parser->current_token.line);
                    exit(1);
                }
            }
            parser_expect(parser, TOKEN_RPAREN);
            
            if (parser->current_token.type == TOKEN_SEMICOLON) {
                parser_advance(parser);
                node->data.function.body = NULL;
            } else {
                node->data.function.body = parse_block(parser);
            }
            return node;
        } else {
            // Global Variable
            node = ast_create_node(AST_VAR_DECL);
            node->resolved_type = type;
            node->data.var_decl.name = name;
            // Array support: int a[10];
            if (parser->current_token.type == TOKEN_LBRACKET) {
                parser_advance(parser);
                if (parser->current_token.type != TOKEN_RBRACKET) {
                    ASTNode *size_expr = parse_expression(parser);
                    int size = eval_constant_expression(size_expr);
                    node->resolved_type = type_array(node->resolved_type, size);
                }
                parser_expect(parser, TOKEN_RBRACKET);
            }

            add_global(parser, name, node->resolved_type);

            if (parser->current_token.type == TOKEN_EQUAL) {
                parser_advance(parser);
                node->data.var_decl.initializer = parse_initializer(parser);
            } else {
                node->data.var_decl.initializer = NULL;
            }
            parser_expect(parser, TOKEN_SEMICOLON);
            return node;
        }
    } else {
        printf("Syntax Error: Expected identifier at line %d\n", parser->current_token.line);
        exit(1);
    }
}

// Helper to parse struct/union body: { int x; int y; } ;
static ASTNode *parse_tag_body(Parser *parser, Type *type) {
    parser_expect(parser, TOKEN_LBRACE);
    ASTNode *node = ast_create_node(type->kind == TYPE_UNION ? AST_UNION_DEF : AST_STRUCT_DEF);
    node->data.struct_def.name = type->data.struct_data.name;
    
    int current_offset = 0;
    int max_size = 0;
    
    type->data.struct_data.members = malloc(sizeof(Member) * 100);
    type->data.struct_data.members_count = 0;
    
    while (parser->current_token.type != TOKEN_RBRACE && parser->current_token.type != TOKEN_EOF) {
        Type *member_type = parse_type(parser);
        if (parser->current_token.type == TOKEN_IDENTIFIER) {
            ASTNode *member = ast_create_node(AST_VAR_DECL);
            member->data.var_decl.name = malloc(parser->current_token.length + 1);
            strncpy(member->data.var_decl.name, parser->current_token.start, parser->current_token.length);
            member->data.var_decl.name[parser->current_token.length] = '\0';
            parser_advance(parser);
            
            // Array support: int a[10];
            if (parser->current_token.type == TOKEN_LBRACKET) {
                parser_advance(parser);
                if (parser->current_token.type != TOKEN_RBRACKET) {
                    ASTNode *size_expr = parse_expression(parser);
                    int size = eval_constant_expression(size_expr);
                    member_type = type_array(member_type, size);
                }
                parser_expect(parser, TOKEN_RBRACKET);
            }
            
            member->resolved_type = member_type;
            ast_add_child(node, member);
            
            // Add to type members
            type->data.struct_data.members[type->data.struct_data.members_count].name = strdup(member->data.var_decl.name);
            type->data.struct_data.members[type->data.struct_data.members_count].type = member_type;
            
            if (type->kind == TYPE_UNION) {
                type->data.struct_data.members[type->data.struct_data.members_count].offset = 0;
                if (member_type->size > max_size) max_size = member_type->size;
            } else {
                type->data.struct_data.members[type->data.struct_data.members_count].offset = current_offset;
                current_offset += member_type->size;
            }
            type->data.struct_data.members_count++;
            
            parser_expect(parser, TOKEN_SEMICOLON);
        }
    }
    parser_expect(parser, TOKEN_RBRACE);
    
    type->size = (type->kind == TYPE_UNION) ? max_size : current_offset;
    
    if (type->data.struct_data.name && parser->structs_count < 100) {
        parser->structs[parser->structs_count++] = type;
    }
    
    return node;
}

static ASTNode *parse_enum_body(Parser *parser, Type *type) {
    parser_expect(parser, TOKEN_LBRACE);
    int current_value = 0;
    
    while (parser->current_token.type != TOKEN_RBRACE && parser->current_token.type != TOKEN_EOF) {
        if (parser->current_token.type == TOKEN_IDENTIFIER) {
            char *name = malloc(parser->current_token.length + 1);
            strncpy(name, parser->current_token.start, parser->current_token.length);
            name[parser->current_token.length] = '\0';
            parser_advance(parser);
            
            if (parser->current_token.type == TOKEN_EQUAL) {
                parser_advance(parser);
                if (parser->current_token.type == TOKEN_NUMBER) {
                    char buffer[64];
                    strncpy(buffer, parser->current_token.start, parser->current_token.length);
                    buffer[parser->current_token.length] = '\0';
                    current_value = atoi(buffer);
                    parser_advance(parser);
                }
            }
            
            if (parser->enum_constants_count < 200) {
                parser->enum_constants[parser->enum_constants_count].name = name;
                parser->enum_constants[parser->enum_constants_count].value = current_value;
                parser->enum_constants_count++;
            }
            current_value++;
            
            if (parser->current_token.type == TOKEN_COMMA) {
                parser_advance(parser);
            }
        } else {
            break;
        }
    }
    parser_expect(parser, TOKEN_RBRACE);
    return NULL;
}


ASTNode *parser_parse(Parser *parser) {
    ASTNode *program = ast_create_node(AST_PROGRAM);
    while (parser->current_token.type != TOKEN_EOF) {
        if (parser->current_token.type == TOKEN_SEMICOLON) {
            parser_advance(parser);
            continue;
        }
        
        if (parser->current_token.type == TOKEN_KEYWORD_TYPEDEF) {
            parse_statement(parser);
        } else if (parser->current_token.type == TOKEN_KEYWORD_STRUCT || 
                   parser->current_token.type == TOKEN_KEYWORD_UNION || 
                   parser->current_token.type == TOKEN_KEYWORD_ENUM) {
            ASTNode *node = parse_statement(parser);
            if (node) ast_add_child(program, node);
        } else {
            ast_add_child(program, parse_external_declaration(parser));
        }
    }
    return program;
}
