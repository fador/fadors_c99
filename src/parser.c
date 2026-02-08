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
        printf("Syntax Error: Expected token type %d, got %d at line %d\n", type, parser->current_token.type, parser->current_token.line);
        exit(1);
    }
}

static ASTNode *parse_expression(Parser *parser) {
    ASTNode *node = ast_create_node(AST_INTEGER);
    if (parser->current_token.type == TOKEN_NUMBER) {
        char buffer[64];
        size_t len = parser->current_token.length < 63 ? parser->current_token.length : 63;
        strncpy(buffer, parser->current_token.start, len);
        buffer[len] = '\0';
        node->data.integer.value = atoi(buffer);
        parser_advance(parser);
    } else {
        printf("Syntax Error: Expected number at line %d\n", parser->current_token.line);
        exit(1);
    }
    return node;
}

static ASTNode *parse_statement(Parser *parser) {
    if (parser->current_token.type == TOKEN_KEYWORD_RETURN) {
        parser_advance(parser);
        ASTNode *node = ast_create_node(AST_RETURN);
        ASTNode *expr = parse_expression(parser);
        ast_add_child(node, expr);
        parser_expect(parser, TOKEN_SEMICOLON);
        return node;
    }
    return NULL;
}

static ASTNode *parse_block(Parser *parser) {
    parser_expect(parser, TOKEN_LBRACE);
    ASTNode *node = ast_create_node(AST_BLOCK);
    while (parser->current_token.type != TOKEN_RBRACE && parser->current_token.type != TOKEN_EOF) {
        ASTNode *stmt = parse_statement(parser);
        if (stmt) {
            ast_add_child(node, stmt);
        } else {
            // Error recovery or skip? For now, exit
            printf("Syntax Error: Unexpected token in block at line %d\n", parser->current_token.line);
            exit(1);
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
    
    ast_add_child(node, parse_block(parser));
    
    return node;
}

ASTNode *parser_parse(Parser *parser) {
    ASTNode *program = ast_create_node(AST_PROGRAM);
    while (parser->current_token.type != TOKEN_EOF) {
        ast_add_child(program, parse_function(parser));
    }
    return program;
}
