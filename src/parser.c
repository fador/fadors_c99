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
    if (parser->current_token.type == TOKEN_KEYWORD_INT) {
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
    
    if (!type) return NULL;
    
    while (parser->current_token.type == TOKEN_STAR) {
        type = type_ptr(type);
        parser_advance(parser);
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
    while (parser->current_token.type == TOKEN_DOT || parser->current_token.type == TOKEN_ARROW || parser->current_token.type == TOKEN_LBRACKET) {
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

static ASTNode *parse_unary(Parser *parser) {
    if (parser->current_token.type == TOKEN_STAR) {
        parser_advance(parser);
        ASTNode *node = ast_create_node(AST_DEREF);
        node->data.unary.expression = parse_unary(parser);
        return node;
    } else if (parser->current_token.type == TOKEN_AMPERSAND) {
        parser_advance(parser);
        ASTNode *node = ast_create_node(AST_ADDR_OF);
        node->data.unary.expression = parse_unary(parser);
        return node;
    } else if (parser->current_token.type == TOKEN_MINUS) {
        parser_advance(parser);
        ASTNode *node = ast_create_node(AST_NEG);
        node->data.unary.expression = parse_unary(parser);
        return node;
    } else if (parser->current_token.type == TOKEN_BANG) {
        parser_advance(parser);
        ASTNode *node = ast_create_node(AST_NOT);
        node->data.unary.expression = parse_unary(parser);
        return node;
    }
    return parse_postfix(parser);
}

static ASTNode *parse_multiplicative(Parser *parser) {
    ASTNode *left = parse_unary(parser);
    while (parser->current_token.type == TOKEN_STAR || parser->current_token.type == TOKEN_SLASH || parser->current_token.type == TOKEN_PERCENT) {
        TokenType op = parser->current_token.type;
        parser_advance(parser);
        ASTNode *node = ast_create_node(AST_BINARY_EXPR);
        node->data.binary_expr.op = op;
        node->data.binary_expr.left = left;
        node->data.binary_expr.right = parse_unary(parser);
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

static ASTNode *parse_expression(Parser *parser) {
    ASTNode *left = parse_logical_or(parser);
    if (parser->current_token.type == TOKEN_EQUAL) {
        parser_advance(parser);
        ASTNode *node = ast_create_node(AST_ASSIGN);
        node->data.assign.left = left;
        node->data.assign.value = parse_expression(parser);
        return node;
    }
    return left;
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
                    
                    if (parser->current_token.type == TOKEN_EQUAL) {
                        parser_advance(parser);
                        node->data.var_decl.initializer = parse_expression(parser);
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

            // Array support: int a[10];
            if (parser->current_token.type == TOKEN_LBRACKET) {
                parser_advance(parser);
                if (parser->current_token.type == TOKEN_NUMBER) {
                    char buffer[64];
                    size_t len = parser->current_token.length < 63 ? parser->current_token.length : 63;
                    strncpy(buffer, parser->current_token.start, len);
                    buffer[len] = '\0';
                    int size = atoi(buffer);
                    node->resolved_type = type_array(node->resolved_type, size);
                    parser_advance(parser);
                }
                parser_expect(parser, TOKEN_RBRACKET);
            }

            if (parser->current_token.type == TOKEN_EQUAL) {
                parser_advance(parser);
                node->data.var_decl.initializer = parse_expression(parser);
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
                parser->current_token.type == TOKEN_KEYWORD_STRUCT) {
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
            node = ast_create_node(AST_FUNCTION);
            node->resolved_type = type;
            node->data.function.name = name;
            
            parser_advance(parser); // Consume '('
            while (parser->current_token.type != TOKEN_RPAREN && parser->current_token.type != TOKEN_EOF) {
                Type *param_type = parse_type(parser);
                if (!param_type) {
                    printf("Syntax Error: Expected parameter type at line %d\n", parser->current_token.line);
                    exit(1);
                }
                
                ASTNode *param = ast_create_node(AST_VAR_DECL);
                param->resolved_type = param_type;
                
                if (parser->current_token.type == TOKEN_IDENTIFIER) {
                    param->data.var_decl.name = malloc(parser->current_token.length + 1);
                    strncpy(param->data.var_decl.name, parser->current_token.start, parser->current_token.length);
                    param->data.var_decl.name[parser->current_token.length] = '\0';
                    parser_advance(parser);
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
                if (parser->current_token.type == TOKEN_NUMBER) {
                    char buffer[64];
                    size_t len = parser->current_token.length < 63 ? parser->current_token.length : 63;
                    strncpy(buffer, parser->current_token.start, len);
                    buffer[len] = '\0';
                    int size = atoi(buffer);
                    node->resolved_type = type_array(node->resolved_type, size);
                    parser_advance(parser);
                }
                parser_expect(parser, TOKEN_RBRACKET);
            }

            if (parser->current_token.type == TOKEN_EQUAL) {
                parser_advance(parser);
                node->data.var_decl.initializer = parse_expression(parser);
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
            
            // Array support in members
            if (parser->current_token.type == TOKEN_LBRACKET) {
                parser_advance(parser);
                if (parser->current_token.type == TOKEN_NUMBER) {
                    char buffer[64];
                    strncpy(buffer, parser->current_token.start, parser->current_token.length);
                    buffer[parser->current_token.length] = '\0';
                    int len = atoi(buffer);
                    member_type = type_array(member_type, len);
                    parser_advance(parser);
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
