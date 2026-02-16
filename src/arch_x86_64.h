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
    OP_LABEL,
    OP_MEM_SIB      /* SIB addressing: base + index*scale + disp */
} OperandType;

typedef struct {
    OperandType type;
    union {
        const char *reg;
        long long imm;
        struct {
            const char *base;
            int offset;
        } mem;
        const char *label;
        struct {
            const char *base;    /* base register (64-bit) */
            const char *index;   /* index register (64-bit) */
            int scale;           /* 1, 2, 4, or 8 */
            int disp;            /* displacement */
        } sib;
    } data;
} Operand;

// Initialize the x86_64 backend
void arch_x86_64_init(FILE *output);

// Set the output syntax (AT&T or Intel/MASM)
void arch_x86_64_set_syntax(CodegenSyntax syntax);

// Set the target platform / ABI (Linux SysV or Windows Win64)
void arch_x86_64_set_target(TargetPlatform target);

// Set the COFF writer for binary generation
void arch_x86_64_set_writer(COFFWriter *writer);

// Generate code for a program using x86_64
void arch_x86_64_generate(ASTNode *program);

#endif
