#ifndef CODEGEN_H
#define CODEGEN_H

#include "ast.h"
#include <stdio.h>

typedef enum {
    SYNTAX_ATT,
    SYNTAX_INTEL
} CodegenSyntax;

void codegen_init(FILE *output);
void codegen_set_syntax(CodegenSyntax syntax);
void codegen_generate(ASTNode *program);

#endif // CODEGEN_H
