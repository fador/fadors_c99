#ifndef ARCH_X86_64_H
#define ARCH_X86_64_H

#include <stdio.h>
#include "ast.h"
#include "codegen.h"
#include "coff_writer.h"

typedef enum {
    OP_REG,
    OP_IMM,
    OP_MEM,
    OP_LABEL
} OperandType;

typedef struct {
    OperandType type;
    union {
        const char *reg;
        int imm;
        struct {
            const char *base;
            int offset;
        } mem;
        const char *label;
    } data;
} Operand;

// Initialize the x86_64 backend
void arch_x86_64_init(FILE *output);

// Set the output syntax (AT&T or Intel/MASM)
void arch_x86_64_set_syntax(CodegenSyntax syntax);

// Set the COFF writer for binary generation
void arch_x86_64_set_writer(COFFWriter *writer);

// Generate code for a program using x86_64
void arch_x86_64_generate(ASTNode *program);

#endif
