#ifndef CODEGEN_H
#define CODEGEN_H

#include "ast.h"
#include <stdio.h>

void codegen_init(FILE *output);
void codegen_generate(ASTNode *program);

#endif // CODEGEN_H
