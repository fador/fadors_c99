#include <stdio.h>
#include "lexer.h"
#include "parser.h"
#include "ast.h"
#include "codegen.h"

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    
    // Hardcoded source for now
    const char *source = "int main() { return 0; }";
    
    Lexer lexer;
    lexer_init(&lexer, source);
    
    Parser parser;
    parser_init(&parser, &lexer);
    
    ASTNode *program = parser_parse(&parser);
    
    printf("Generating assembly for: %s\n", source);
    printf("--------------------------------------------------\n");
    
    codegen_init(stdout);
    codegen_generate(program);
    
    printf("--------------------------------------------------\n");
    
    return 0;
}
