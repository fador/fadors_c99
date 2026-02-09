#ifndef ENCODER_H
#define ENCODER_H

#include "buffer.h"
#include "arch_x86_64.h"

// Note: Using Operand from arch_x86_64.c might be tricky if it's static.
// I should probably move Operand definition to arch_x86_64.h or a shared header.

void encode_inst0(Buffer *buf, const char *mnemonic);
void encode_inst1(Buffer *buf, const char *mnemonic, Operand op1);
void encode_inst2(Buffer *buf, const char *mnemonic, Operand src, Operand dest);

#endif
