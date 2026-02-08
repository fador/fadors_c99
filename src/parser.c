#include "parser.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

void parser_init(Parser *parser, Lexer *lexer) {
    parser->lexer = lexer;
    parser->current_token = lexer_next_token(lexer);
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
        exit(1);
    }
}

static ASTNode *parse_expression(Parser *parser);
static ASTNode *parse_struct_body(Parser *parser, char *name);
static ASTNode *parse_struct_def(Parser *parser);

static Type *parse_type(Parser *parser) {
    Type *type = NULL;
    if (parser->current_token.type == TOKEN_KEYWORD_INT) {
        type = type_int();
        parser_advance(parser);
    } else if (parser->current_token.type == TOKEN_KEYWORD_STRUCT) {
        parser_advance(parser);
        if (parser->current_token.type == TOKEN_IDENTIFIER) {
            char buffer[256];
            size_t len = parser->current_token.length < 255 ? parser->current_token.length : 255;
            strncpy(buffer, parser->current_token.start, len);
            buffer[len] = '\0';
            type = type_struct(buffer);
            parser_advance(parser);
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
    } else if (parser->current_token.type == TOKEN_IDENTIFIER) {
        char *name = malloc(parser->current_token.length + 1);
        strncpy(name, parser->current_token.start, parser->current_token.length);
        name[parser->current_token.length] = '\0';
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
    } else if (parser->current_token.type == TOKEN_LPAREN) {
        parser_advance(parser);
        ASTNode *node = parse_expression(parser);
        parser_expect(parser, TOKEN_RPAREN);
        return node;
    }
    printf("Syntax Error: Unexpected token %d ('%.*s') in expression at line %d\n", 
           parser->current_token.type, (int)parser->current_token.length, parser->current_token.start, parser->current_token.line);
    exit(1);
}

static ASTNode *parse_postfix(Parser *parser) {
    ASTNode *node = parse_primary(parser);
    while (parser->current_token.type == TOKEN_DOT || parser->current_token.type == TOKEN_ARROW) {
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
    }
    return parse_postfix(parser);
}

static ASTNode *parse_multiplicative(Parser *parser) {
    ASTNode *left = parse_unary(parser);
    while (parser->current_token.type == TOKEN_STAR || parser->current_token.type == TOKEN_SLASH) {
        TokenType op = parser->current_token.type;
        parser_advance(parser);
        ASTNode *node = ast_create_node(AST_BINARY_EXPR);
        node->data.binary_expr.op = op;
        node->data.binary_expr.left = left;
        node->data.binary_expr.right = parse_primary(parser);
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
    ASTNode *left = parse_additive(parser);
    while (parser->current_token.type == TOKEN_LESS || parser->current_token.type == TOKEN_GREATER ||
           parser->current_token.type == TOKEN_LESS_EQUAL || parser->current_token.type == TOKEN_GREATER_EQUAL) {
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

static ASTNode *parse_expression(Parser *parser) {
    ASTNode *left = parse_equality(parser);
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
        node->data.return_stmt.expression = parse_expression(parser);
        parser_expect(parser, TOKEN_SEMICOLON);
        return node;
    } else if (parser->current_token.type == TOKEN_KEYWORD_INT || parser->current_token.type == TOKEN_KEYWORD_STRUCT) {
        // Variable or struct declaration/definition
        if (parser->current_token.type == TOKEN_KEYWORD_STRUCT) {
            Token next = lexer_peek_token(parser->lexer);
            
            if (next.type == TOKEN_LBRACE) {
                // Anonymous struct definition: struct { ... }
                parser_advance(parser); // Consume struct
                return parse_struct_body(parser, NULL);
            } else if (next.type == TOKEN_IDENTIFIER) {
                // Check if it's 'struct Name {'
                // We advance to identifier
                 parser_advance(parser); // struct
                 // Now at Name
                 char *name = malloc(parser->current_token.length + 1);
                 strncpy(name, parser->current_token.start, parser->current_token.length);
                 name[parser->current_token.length] = '\0';
                 
                 Token check = lexer_peek_token(parser->lexer);
                 
                 if (check.type == TOKEN_LBRACE) {
                     parser_advance(parser); // Consume Name
                     return parse_struct_body(parser, name);
                 }
                 
                 // Variable declaration: struct Name ...
                 // We already consumed 'struct'. Current is 'Name'.
                 // Create the type manually.
                 Type *type = type_struct(name);
                 free(name);
                 
                 parser_advance(parser); // Consume Name
                 
                 // Now parsing possible pointers *
                 while (parser->current_token.type == TOKEN_STAR) {
                    type = type_ptr(type);
                    parser_advance(parser);
                 }
                 
                 // Now create the VAR_DECL node
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
                 } else {
                     printf("Syntax Error: Expected variable name after struct type at line %d\n", parser->current_token.line);
                     exit(1);
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

static ASTNode *parse_function(Parser *parser) {
    parser_expect(parser, TOKEN_KEYWORD_INT);
    
    ASTNode *node = ast_create_node(AST_FUNCTION);
    if (parser->current_token.type == TOKEN_IDENTIFIER) {
        node->data.function.name = malloc(parser->current_token.length + 1);
        strncpy(node->data.function.name, parser->current_token.start, parser->current_token.length);
        node->data.function.name[parser->current_token.length] = '\0';
        parser_advance(parser);
    } else {
        printf("Syntax Error: Expected function name at line %d\n", parser->current_token.line);
        exit(1);
    }
    
    parser_expect(parser, TOKEN_LPAREN);
    parser_expect(parser, TOKEN_RPAREN);
    
    node->data.function.body = parse_block(parser);
    
    return node;
}

// Helper to parse struct body: { int x; int y; } ;
static ASTNode *parse_struct_body(Parser *parser, char *name) {
    Type *struct_type = name ? type_struct(name) : NULL;
    
    parser_expect(parser, TOKEN_LBRACE);
    ASTNode *node = ast_create_node(AST_STRUCT_DEF);
    node->data.struct_def.name = name;
    
    int current_offset = 0;
    while (parser->current_token.type != TOKEN_RBRACE && parser->current_token.type != TOKEN_EOF) {
        Type *member_type = parse_type(parser);
        if (parser->current_token.type == TOKEN_IDENTIFIER) {
            ASTNode *member = ast_create_node(AST_VAR_DECL);
            member->data.var_decl.name = malloc(parser->current_token.length + 1);
            strncpy(member->data.var_decl.name, parser->current_token.start, parser->current_token.length);
            member->data.var_decl.name[parser->current_token.length] = '\0';
            parser_advance(parser);
            ast_add_child(node, member);
            
            if (struct_type) {
                Member *m = &struct_type->data.struct_data.members[struct_type->data.struct_data.members_count++];
                m->name = strdup(member->data.var_decl.name);
                m->type = member_type;
                m->offset = current_offset;
                current_offset += member_type->size;
                struct_type->size = current_offset;
            }
            
            parser_expect(parser, TOKEN_SEMICOLON);
        }
    }
    parser_expect(parser, TOKEN_RBRACE);
    parser_expect(parser, TOKEN_SEMICOLON);
    return node;
}

static ASTNode *parse_struct_def(Parser *parser) {
    parser_expect(parser, TOKEN_KEYWORD_STRUCT);
    char *name = NULL;
    if (parser->current_token.type == TOKEN_IDENTIFIER) {
        name = malloc(parser->current_token.length + 1);
        strncpy(name, parser->current_token.start, parser->current_token.length);
        name[parser->current_token.length] = '\0';
        parser_advance(parser);
    }
    
    if (parser->current_token.type == TOKEN_LBRACE) {
        return parse_struct_body(parser, name);
    }
    return NULL;
}

ASTNode *parser_parse(Parser *parser) {
    ASTNode *program = ast_create_node(AST_PROGRAM);
    while (parser->current_token.type != TOKEN_EOF) {
        if (parser->current_token.type == TOKEN_KEYWORD_STRUCT) {
            Token next = lexer_peek_token(parser->lexer);
            // If it's 'struct name {' or 'struct {' it's a definition
            // Actually, just try to parse struct def if we see struct
            ASTNode *def = parse_struct_def(parser);
            if (def) ast_add_child(program, def);
        } else {
            ast_add_child(program, parse_function(parser));
        }
    }
    return program;
}
