#ifndef ARCH_X86_H
#define ARCH_X86_H

#include "ast.h"
#include "codegen.h"
#include <stdio.h>

void arch_x86_init(FILE *output);
void arch_x86_set_syntax(CodegenSyntax syntax);
void arch_x86_set_target(TargetPlatform target);
void arch_x86_set_writer(COFFWriter *writer);
void arch_x86_generate(ASTNode *program);

#endif // ARCH_X86_H
