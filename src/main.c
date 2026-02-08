#include <stdio.h>
#include "lexer.h"
#include "parser.h"
#include "ast.h"
#include "codegen.h"

#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: %s <source.c>\n", argv[0]);
        return 1;
    }
    
    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        printf("Could not open file %s\n", argv[1]);
        return 1;
    }
    
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    char *source = malloc(size + 1);
    fread(source, 1, size, f);
    source[size] = '\0';
    fclose(f);
    
    Lexer lexer;
    lexer_init(&lexer, source);
    
    Parser parser;
    parser_init(&parser, &lexer);
    
    ASTNode *program = parser_parse(&parser);
    
    char out_filename[256];
    strncpy(out_filename, argv[1], 250);
    char *dot = strrchr(out_filename, '.');
    if (dot) *dot = '\0';
    strcat(out_filename, ".s");
    
    FILE *out = fopen(out_filename, "w");
    if (!out) {
        printf("Could not create output file %s\n", out_filename);
        return 1;
    }
    
    codegen_init(out);
    codegen_generate(program);
    fclose(out);
    
    printf("Generated: %s\n", out_filename);
    
    free(source);
    return 0;
}
