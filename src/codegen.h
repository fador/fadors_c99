#ifndef CODEGEN_H
#define CODEGEN_H

#include "ast.h"
#include <stdio.h>

#include "coff_writer.h"

typedef enum {
    SYNTAX_ATT,
    SYNTAX_INTEL
} CodegenSyntax;

typedef enum {
    TARGET_LINUX,
    TARGET_WINDOWS
} TargetPlatform;

void codegen_init(FILE *output);
void codegen_set_syntax(CodegenSyntax syntax);
void codegen_set_target(TargetPlatform target);
void codegen_set_writer(COFFWriter *writer);
void codegen_generate(ASTNode *program);

#endif // CODEGEN_H
