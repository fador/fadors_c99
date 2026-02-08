#ifndef ARCH_X86_64_H
#define ARCH_X86_64_H

#include <stdio.h>
#include "ast.h"
#include "codegen.h"

// Initialize the x86_64 backend
void arch_x86_64_init(FILE *output);

// Set the output syntax (AT&T or Intel/MASM)
void arch_x86_64_set_syntax(CodegenSyntax syntax);

// Generate code for a program using x86_64
void arch_x86_64_generate(ASTNode *program);

#endif
