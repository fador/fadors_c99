#ifndef ENCODER_H
#define ENCODER_H

#include "buffer.h"
#include "coff_writer.h"

/* Simple x86-64 machine code encoder */

typedef enum {
    OP_REG,
    OP_IMM,
    OP_MEM,
    OP_LABEL,
    OP_MEM_LABEL,
    OP_MEM_SIB
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
            const char *base;
            const char *index;
            int scale;
            int disp;
        } sib;
    } data;
} Operand;

void encoder_set_writer(COFFWriter *writer);
void encoder_set_bitness(int bits);

void encode_inst0(Buffer *buf, const char *mnemonic);
void encode_inst1(Buffer *buf, const char *mnemonic, Operand *op1);
void encode_inst2(Buffer *buf, const char *mnemonic, Operand *src, Operand *dest);
void encode_inst3(Buffer *buf, const char *mnemonic, Operand *op1, Operand *op2, Operand *op3);

#endif
