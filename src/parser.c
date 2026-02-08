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

static ASTNode *parse_multiplicative(Parser *parser) {
    ASTNode *left = parse_primary(parser);
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
    // Basic assignment: identifier = expression
    if (parser->current_token.type == TOKEN_IDENTIFIER) {
        Token next = lexer_peek_token(parser->lexer);
        if (next.type == TOKEN_EQUAL) {
            char *name = malloc(parser->current_token.length + 1);
            strncpy(name, parser->current_token.start, parser->current_token.length);
            name[parser->current_token.length] = '\0';
            
            parser_advance(parser); // consume identifier
            parser_advance(parser); // consume '='
            
            ASTNode *node = ast_create_node(AST_ASSIGN);
            node->data.assign.name = name;
            node->data.assign.value = parse_expression(parser);
            return node;
        }
    }
    return parse_equality(parser);
}

static ASTNode *parse_statement(Parser *parser) {
    if (parser->current_token.type == TOKEN_KEYWORD_RETURN) {
        parser_advance(parser);
        ASTNode *node = ast_create_node(AST_RETURN);
        node->data.return_stmt.expression = parse_expression(parser);
        parser_expect(parser, TOKEN_SEMICOLON);
        return node;
    } else if (parser->current_token.type == TOKEN_KEYWORD_INT) {
        // Variable declaration
        parser_advance(parser);
        ASTNode *node = ast_create_node(AST_VAR_DECL);
        if (parser->current_token.type == TOKEN_IDENTIFIER) {
            node->data.var_decl.name = malloc(parser->current_token.length + 1);
            strncpy(node->data.var_decl.name, parser->current_token.start, parser->current_token.length);
            node->data.var_decl.name[parser->current_token.length] = '\0';
            parser_advance(parser);
        }
        if (parser->current_token.type == TOKEN_EQUAL) {
            parser_advance(parser);
            node->data.var_decl.initializer = parse_expression(parser);
        } else {
            node->data.var_decl.initializer = NULL;
        }
        parser_expect(parser, TOKEN_SEMICOLON);
        return node;
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

ASTNode *parser_parse(Parser *parser) {
    ASTNode *program = ast_create_node(AST_PROGRAM);
    while (parser->current_token.type != TOKEN_EOF) {
        ast_add_child(program, parse_function(parser));
    }
    return program;
}
