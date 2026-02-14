#include "encoder.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "coff_writer.h"

static COFFWriter *encoder_writer = NULL;

void encoder_set_writer(COFFWriter *writer) {
    encoder_writer = writer;
}

static void emit_reloc(Buffer *buf, const char *label, uint32_t offset) {
    if (!encoder_writer) return;
    int32_t sym_idx = coff_writer_find_symbol(encoder_writer, label);
    if (sym_idx < 0) {
        sym_idx = coff_writer_add_symbol(encoder_writer, label, 0, 0, 0, IMAGE_SYM_CLASS_EXTERNAL);
    }
    // IMAGE_REL_AMD64_REL32 is 0x0004
    coff_writer_add_reloc(encoder_writer, offset, (uint32_t)sym_idx, 0x0004, 1 /* .text */);
}

int get_reg_id(const char *reg) {
    if (strcmp(reg, "rax") == 0 || strcmp(reg, "eax") == 0 || strcmp(reg, "ax") == 0 || strcmp(reg, "al") == 0) return 0;
    if (strcmp(reg, "rcx") == 0 || strcmp(reg, "ecx") == 0 || strcmp(reg, "cx") == 0 || strcmp(reg, "cl") == 0) return 1;
    if (strcmp(reg, "rdx") == 0 || strcmp(reg, "edx") == 0 || strcmp(reg, "dx") == 0 || strcmp(reg, "dl") == 0) return 2;
    if (strcmp(reg, "rbx") == 0 || strcmp(reg, "ebx") == 0 || strcmp(reg, "bx") == 0 || strcmp(reg, "bl") == 0) return 3;
    if (strcmp(reg, "rsp") == 0 || strcmp(reg, "esp") == 0 || strcmp(reg, "sp") == 0) return 4;
    if (strcmp(reg, "rbp") == 0 || strcmp(reg, "ebp") == 0 || strcmp(reg, "bp") == 0) return 5;
    if (strcmp(reg, "rsi") == 0 || strcmp(reg, "esi") == 0 || strcmp(reg, "si") == 0) return 6;
    if (strcmp(reg, "rdi") == 0 || strcmp(reg, "edi") == 0 || strcmp(reg, "di") == 0) return 7;
    // R8-R15
    if (strcmp(reg, "r8") == 0 || strcmp(reg, "r8d") == 0 || strcmp(reg, "r8w") == 0 || strcmp(reg, "r8b") == 0) return 8;
    if (strcmp(reg, "r9") == 0 || strcmp(reg, "r9d") == 0 || strcmp(reg, "r9w") == 0 || strcmp(reg, "r9b") == 0) return 9;
    if (strcmp(reg, "r10") == 0 || strcmp(reg, "r10d") == 0 || strcmp(reg, "r10w") == 0 || strcmp(reg, "r10b") == 0) return 10;
    if (strcmp(reg, "r11") == 0 || strcmp(reg, "r11d") == 0 || strcmp(reg, "r11w") == 0 || strcmp(reg, "r11b") == 0) return 11;
    if (strcmp(reg, "r12") == 0 || strcmp(reg, "r12d") == 0 || strcmp(reg, "r12w") == 0 || strcmp(reg, "r12b") == 0) return 12;
    if (strcmp(reg, "r13") == 0 || strcmp(reg, "r13d") == 0 || strcmp(reg, "r13w") == 0 || strcmp(reg, "r13b") == 0) return 13;
    if (strcmp(reg, "r14") == 0 || strcmp(reg, "r14d") == 0 || strcmp(reg, "r14w") == 0 || strcmp(reg, "r14b") == 0) return 14;
    if (strcmp(reg, "r15") == 0 || strcmp(reg, "r15d") == 0 || strcmp(reg, "r15w") == 0 || strcmp(reg, "r15b") == 0) return 15;
    // XMM registers
    if (strncmp(reg, "xmm", 3) == 0) return atoi(reg + 3);
    // YMM registers (same encoding as XMM, distinguished by VEX.L)
    if (strncmp(reg, "ymm", 3) == 0) return atoi(reg + 3);
    return -1;
}

static void emit_rex(Buffer *buf, int w, int r, int x, int b) {
    // REX prefix: 0100WRXB
    uint8_t rex = 0x40;
    if (w) rex |= 0x08;
    if (r) rex |= 0x04;
    if (x) rex |= 0x02;
    if (b) rex |= 0x01;
    if (rex != 0x40) buffer_write_byte(buf, rex);
}

static void emit_modrm(Buffer *buf, int mod, int reg, int rm) {
    // ModR/M: mod(2) | reg(3) | rm(3)
    uint8_t modrm = ((mod & 3) << 6) | ((reg & 7) << 3) | (rm & 7);
    buffer_write_byte(buf, modrm);
    if ((rm & 7) == 4 && mod != 3) {
        // SIB byte: [RSP] or [R12]
        buffer_write_byte(buf, 0x24); // scale=0, index=4(none), base=4(rsp)
    }
}

/* ---- VEX prefix encoding for AVX/AVX2 instructions ---- */
/* 2-byte VEX: C5 [R vvvv L pp]
 *   R    = inverted REX.R (1 = no extension, 0 = extend ModRM.reg)
 *   vvvv = inverted second source register (1111 = unused)
 *   L    = 0 for 128-bit (xmm), 1 for 256-bit (ymm)
 *   pp   = 00=none, 01=66, 10=F3, 11=F2
 *
 * 3-byte VEX: C4 [R X B mmmmm] [W vvvv L pp]
 *   R,X,B = inverted REX.R, REX.X, REX.B
 *   mmmmm = 00001=0F, 00010=0F38, 00011=0F3A
 *   W     = REX.W (0 for most SSE/AVX)
 */
static void emit_vex2(Buffer *buf, int R, int vvvv, int L, int pp) {
    buffer_write_byte(buf, 0xC5);
    uint8_t byte = 0;
    byte |= (R ? 0 : 0x80);   /* inverted R */
    byte |= ((~vvvv & 0x0F) << 3);
    byte |= (L ? 0x04 : 0);
    byte |= (pp & 3);
    buffer_write_byte(buf, byte);
}

static void emit_vex3(Buffer *buf, int R, int X, int B, int mmmmm,
                       int W, int vvvv, int L, int pp) {
    buffer_write_byte(buf, 0xC4);
    uint8_t b1 = 0;
    b1 |= (R ? 0 : 0x80);
    b1 |= (X ? 0 : 0x40);
    b1 |= (B ? 0 : 0x20);
    b1 |= (mmmmm & 0x1F);
    buffer_write_byte(buf, b1);
    uint8_t b2 = 0;
    b2 |= (W ? 0x80 : 0);
    b2 |= ((~vvvv & 0x0F) << 3);
    b2 |= (L ? 0x04 : 0);
    b2 |= (pp & 3);
    buffer_write_byte(buf, b2);
}

/* Emit VEX prefix — uses 2-byte form when possible, 3-byte otherwise.
 * reg_id, rm_id: register encoding IDs (for R / B bits).
 * vvvv: second source register id (15 = unused).
 * L: 0=128, 1=256. pp: 0=none, 1=66, 2=F3, 3=F2. */
static void emit_vex(Buffer *buf, int reg_id, int rm_id, int vvvv,
                      int L, int pp) {
    int R = (reg_id >= 8) ? 1 : 0;
    int B = (rm_id  >= 8) ? 1 : 0;
    /* 2-byte form only if B=0, X=0, W=0, mmmmm=0F(1) */
    if (!B) {
        emit_vex2(buf, R, vvvv, L, pp);
    } else {
        emit_vex3(buf, R, 0, B, 1 /* 0F */, 0, vvvv, L, pp);
    }
}

void encode_inst0(Buffer *buf, const char *mnemonic) {
    if (strcmp(mnemonic, "ret") == 0) {
        buffer_write_byte(buf, 0xC3);
    } else if (strcmp(mnemonic, "leave") == 0) {
        buffer_write_byte(buf, 0xC9);
    } else if (strcmp(mnemonic, "cqo") == 0) {
        emit_rex(buf, 1, 0, 0, 0); // REX.W
        buffer_write_byte(buf, 0x99);
    } else if (strcmp(mnemonic, "vzeroupper") == 0) {
        /* VEX.128.0F.WIG 77 — zero upper 128 bits of all YMM registers */
        emit_vex2(buf, 0, 0, 0, 0);
        buffer_write_byte(buf, 0x77);
    } else if (strcmp(mnemonic, "ud2") == 0) {
        /* 0F 0B — undefined instruction (trap) */
        buffer_write_byte(buf, 0x0F);
        buffer_write_byte(buf, 0x0B);
    }
}

void encode_inst1(Buffer *buf, const char *mnemonic, Operand *op1) {
    if (strcmp(mnemonic, "push") == 0 || strcmp(mnemonic, "pushq") == 0) {
        if (op1->type == OP_REG) {
            int reg = get_reg_id(op1->data.reg);
            if (reg >= 8) emit_rex(buf, 0, 0, 0, 1);
            buffer_write_byte(buf, 0x50 + (reg & 7));
        }
    } else if (strcmp(mnemonic, "pop") == 0 || strcmp(mnemonic, "popq") == 0) {
        if (op1->type == OP_REG) {
            int reg = get_reg_id(op1->data.reg);
            if (reg >= 8) emit_rex(buf, 0, 0, 0, 1);
            buffer_write_byte(buf, 0x58 + (reg & 7));
        }
    } else if (strcmp(mnemonic, "idiv") == 0) {
        if (op1->type == OP_REG) {
            int reg = get_reg_id(op1->data.reg);
            emit_rex(buf, 1, 0, 0, reg >= 8); // REX.W
            buffer_write_byte(buf, 0xF7);
            emit_modrm(buf, 3, 7, reg); // Mod=3 (reg), Opcode extension=7 for IDIV
        }
    } else if (strcmp(mnemonic, "jmp") == 0) {
        if (op1->type == OP_LABEL) {
            buffer_write_byte(buf, 0xE9);
            emit_reloc(buf, op1->data.label, (uint32_t)buf->size);
            buffer_write_dword(buf, 0); // COFF REL32 addend = 0
        }
    } else if (strcmp(mnemonic, "je") == 0 || strcmp(mnemonic, "jz") == 0 ||
               strcmp(mnemonic, "jne") == 0 || strcmp(mnemonic, "jnz") == 0 ||
               strcmp(mnemonic, "jl") == 0 || strcmp(mnemonic, "jge") == 0 ||
               strcmp(mnemonic, "jle") == 0 || strcmp(mnemonic, "jg") == 0 ||
               strcmp(mnemonic, "jb") == 0 || strcmp(mnemonic, "jae") == 0 ||
               strcmp(mnemonic, "jbe") == 0 || strcmp(mnemonic, "ja") == 0) {
        if (op1->type == OP_LABEL) {
            uint8_t opcode = 0x84; // je
            if (strcmp(mnemonic, "jne") == 0 || strcmp(mnemonic, "jnz") == 0) opcode = 0x85;
            else if (strcmp(mnemonic, "jl") == 0) opcode = 0x8C;
            else if (strcmp(mnemonic, "jge") == 0) opcode = 0x8D;
            else if (strcmp(mnemonic, "jle") == 0) opcode = 0x8E;
            else if (strcmp(mnemonic, "jg") == 0) opcode = 0x8F;
            else if (strcmp(mnemonic, "jb") == 0) opcode = 0x82;
            else if (strcmp(mnemonic, "jae") == 0) opcode = 0x83;
            else if (strcmp(mnemonic, "jbe") == 0) opcode = 0x86;
            else if (strcmp(mnemonic, "ja") == 0) opcode = 0x87;
            
            buffer_write_byte(buf, 0x0F);
            buffer_write_byte(buf, opcode);
            emit_reloc(buf, op1->data.label, (uint32_t)buf->size);
            buffer_write_dword(buf, 0); // COFF REL32 addend = 0
        }
    } else if (strcmp(mnemonic, "call") == 0) {
        if (op1->type == OP_LABEL) {
            buffer_write_byte(buf, 0xE8);
            emit_reloc(buf, op1->data.label, (uint32_t)buf->size);
            buffer_write_dword(buf, 0); // COFF REL32 addend = 0
        }
    } else if (strncmp(mnemonic, "set", 3) == 0) {
        // sete, setne, setl, setle, setg, setge
        if (op1->type == OP_REG) {
            int reg = get_reg_id(op1->data.reg);
            // setcc only works on 1-byte registers (AL, CL, DL, BL, etc.)
            buffer_write_byte(buf, 0x0F);
            if (strcmp(mnemonic, "sete") == 0 || strcmp(mnemonic, "setz") == 0) buffer_write_byte(buf, 0x94);
            else if (strcmp(mnemonic, "setne") == 0 || strcmp(mnemonic, "setnz") == 0) buffer_write_byte(buf, 0x95);
            else if (strcmp(mnemonic, "setl") == 0) buffer_write_byte(buf, 0x9C);
            else if (strcmp(mnemonic, "setle") == 0) buffer_write_byte(buf, 0x9E);
            else if (strcmp(mnemonic, "setg") == 0) buffer_write_byte(buf, 0x9F);
            else if (strcmp(mnemonic, "setge") == 0) buffer_write_byte(buf, 0x9D);
            else if (strcmp(mnemonic, "setb") == 0) buffer_write_byte(buf, 0x92);
            else if (strcmp(mnemonic, "setbe") == 0) buffer_write_byte(buf, 0x96);
            else if (strcmp(mnemonic, "seta") == 0) buffer_write_byte(buf, 0x97);
            else if (strcmp(mnemonic, "setae") == 0) buffer_write_byte(buf, 0x93);
            emit_modrm(buf, 3, 0, reg);
        }
    } else if (strcmp(mnemonic, "neg") == 0) {
        if (op1->type == OP_REG) {
            int reg = get_reg_id(op1->data.reg);
            emit_rex(buf, 1, 0, 0, reg >= 8);
            buffer_write_byte(buf, 0xF7);
            emit_modrm(buf, 3, 3, reg); // Mod=3, Opcode extension=3 for NEG
        }
    } else if (strcmp(mnemonic, "not") == 0) {
        if (op1->type == OP_REG) {
            int reg = get_reg_id(op1->data.reg);
            emit_rex(buf, 1, 0, 0, reg >= 8);
            buffer_write_byte(buf, 0xF7);
            emit_modrm(buf, 3, 2, reg); // Mod=3, Opcode extension=2 for NOT
        }
    }
}

void encode_inst2(Buffer *buf, const char *mnemonic, Operand *src, Operand *dest) {
    int is_64 = (mnemonic[strlen(mnemonic)-1] == 'q' || strcmp(mnemonic, "mov") == 0 || strcmp(mnemonic, "add") == 0 || strcmp(mnemonic, "sub") == 0 || strcmp(mnemonic, "cmp") == 0);
    if (strcmp(mnemonic, "mov") == 0 || strcmp(mnemonic, "movq") == 0 || strcmp(mnemonic, "movl") == 0) {
        is_64 = (mnemonic[3] != 'l');
        if (src->type == OP_IMM && dest->type == OP_REG) {
            int reg = get_reg_id(dest->data.reg);
            emit_rex(buf, is_64, 0, 0, reg >= 8);
            buffer_write_byte(buf, 0xB8 + (reg & 7));
            if (is_64) buffer_write_qword(buf, (uint64_t)src->data.imm);
            else buffer_write_dword(buf, (uint32_t)src->data.imm);
        } else if (src->type == OP_REG && dest->type == OP_REG) {
            int s = get_reg_id(src->data.reg);
            int d = get_reg_id(dest->data.reg);
            emit_rex(buf, is_64, s >= 8, 0, d >= 8);
            buffer_write_byte(buf, 0x89);
            emit_modrm(buf, 3, s, d);
        } else if (src->type == OP_REG && dest->type == OP_LABEL) {
            int s = get_reg_id(src->data.reg);
            emit_rex(buf, is_64, s >= 8, 0, 0);
            buffer_write_byte(buf, 0x89);
            emit_modrm(buf, 0, s, 5);
            emit_reloc(buf, dest->data.label, (uint32_t)buf->size);
            buffer_write_dword(buf, 0); // COFF REL32 addend = 0
        } else if (src->type == OP_LABEL && dest->type == OP_REG) {
            int d = get_reg_id(dest->data.reg);
            emit_rex(buf, is_64, d >= 8, 0, 0);
            buffer_write_byte(buf, 0x8B);
            emit_modrm(buf, 0, d, 5);
            emit_reloc(buf, src->data.label, (uint32_t)buf->size);
            buffer_write_dword(buf, 0); // COFF REL32 addend = 0
        } else if (src->type == OP_REG && dest->type == OP_MEM) {
            int s = get_reg_id(src->data.reg);
            int d = get_reg_id(dest->data.mem.base);
            emit_rex(buf, is_64, s >= 8, 0, d >= 8);
            buffer_write_byte(buf, 0x89);
            if (dest->data.mem.offset == 0) emit_modrm(buf, 0, s, d);
            else { emit_modrm(buf, 2, s, d); buffer_write_dword(buf, dest->data.mem.offset); }
        } else if (src->type == OP_MEM && dest->type == OP_REG) {
            int s = get_reg_id(src->data.mem.base);
            int d = get_reg_id(dest->data.reg);
            emit_rex(buf, is_64, d >= 8, 0, s >= 8);
            buffer_write_byte(buf, 0x8B);
            if (src->data.mem.offset == 0) emit_modrm(buf, 0, d, s);
            else { emit_modrm(buf, 2, d, s); buffer_write_dword(buf, src->data.mem.offset); }
        } else if (src->type == OP_IMM && dest->type == OP_MEM) {
            // MOV r/m32, imm32 (C7 /0) or REX.W MOV r/m64, imm32 (sign-extended)
            int d = get_reg_id(dest->data.mem.base);
            emit_rex(buf, is_64, 0, 0, d >= 8);
            buffer_write_byte(buf, 0xC7);
            if (dest->data.mem.offset == 0) emit_modrm(buf, 0, 0, d);
            else { emit_modrm(buf, 2, 0, d); buffer_write_dword(buf, dest->data.mem.offset); }
            buffer_write_dword(buf, (uint32_t)src->data.imm);
        }
    } else if (strcmp(mnemonic, "movw") == 0) {
        buffer_write_byte(buf, 0x66);
        if (src->type == OP_REG && dest->type == OP_REG) {
            int s = get_reg_id(src->data.reg);
            int d = get_reg_id(dest->data.reg);
            emit_rex(buf, 0, s >= 8, 0, d >= 8);
            buffer_write_byte(buf, 0x89);
            emit_modrm(buf, 3, s, d);
        } else if (src->type == OP_REG && dest->type == OP_MEM) {
            int s = get_reg_id(src->data.reg);
            int d = get_reg_id(dest->data.mem.base);
            emit_rex(buf, 0, s >= 8, 0, d >= 8);
            buffer_write_byte(buf, 0x89);
            if (dest->data.mem.offset == 0) emit_modrm(buf, 0, s, d);
            else { emit_modrm(buf, 2, s, d); buffer_write_dword(buf, dest->data.mem.offset); }
        } else if (src->type == OP_MEM && dest->type == OP_REG) {
            int s = get_reg_id(src->data.mem.base);
            int d = get_reg_id(dest->data.reg);
            emit_rex(buf, 0, d >= 8, 0, s >= 8);
            buffer_write_byte(buf, 0x8B);
            if (src->data.mem.offset == 0) emit_modrm(buf, 0, d, s);
            else { emit_modrm(buf, 2, d, s); buffer_write_dword(buf, src->data.mem.offset); }
        } else if (src->type == OP_REG && dest->type == OP_LABEL) {
            int s = get_reg_id(src->data.reg);
            emit_rex(buf, 0, s >= 8, 0, 0);
            buffer_write_byte(buf, 0x89);
            emit_modrm(buf, 0, s, 5); // RIP-relative
            emit_reloc(buf, dest->data.label, (uint32_t)buf->size);
            buffer_write_dword(buf, 0); // COFF REL32 addend = 0
        }
    } else if (strcmp(mnemonic, "movb") == 0) {
        // 8-bit MOV r/m8, r8 (opcode 0x88) or MOV r8, r/m8 (opcode 0x8A)
        if (src->type == OP_REG && dest->type == OP_MEM) {
            int s = get_reg_id(src->data.reg);
            int d = get_reg_id(dest->data.mem.base);
            // Need REX for sil/dil/spl/bpl (regs 4-7 in byte mode) or r8-r15
            if (s >= 4 || d >= 8) emit_rex(buf, 0, s >= 8, 0, d >= 8);
            buffer_write_byte(buf, 0x88);
            if (dest->data.mem.offset == 0) emit_modrm(buf, 0, s, d);
            else { emit_modrm(buf, 2, s, d); buffer_write_dword(buf, dest->data.mem.offset); }
        } else if (src->type == OP_REG && dest->type == OP_LABEL) {
            int s = get_reg_id(src->data.reg);
            if (s >= 4) emit_rex(buf, 0, s >= 8, 0, 0);
            buffer_write_byte(buf, 0x88);
            emit_modrm(buf, 0, s, 5); // RIP-relative
            emit_reloc(buf, dest->data.label, (uint32_t)buf->size);
            buffer_write_dword(buf, 0); // COFF REL32 addend = 0
        } else if (src->type == OP_MEM && dest->type == OP_REG) {
            int s = get_reg_id(src->data.mem.base);
            int d = get_reg_id(dest->data.reg);
            if (d >= 4 || s >= 8) emit_rex(buf, 0, d >= 8, 0, s >= 8);
            buffer_write_byte(buf, 0x8A);
            if (src->data.mem.offset == 0) emit_modrm(buf, 0, d, s);
            else { emit_modrm(buf, 2, d, s); buffer_write_dword(buf, src->data.mem.offset); }
        }
    } else if (strcmp(mnemonic, "add") == 0 || strcmp(mnemonic, "addq") == 0 || strcmp(mnemonic, "addl") == 0) {
        is_64 = (mnemonic[3] != 'l');
        if (src->type == OP_REG && dest->type == OP_REG) {
            int s = get_reg_id(src->data.reg);
            int d = get_reg_id(dest->data.reg);
            emit_rex(buf, is_64, s >= 8, 0, d >= 8);
            buffer_write_byte(buf, 0x01);
            emit_modrm(buf, 3, s, d);
        } else if (src->type == OP_IMM && dest->type == OP_REG) {
             int d = get_reg_id(dest->data.reg);
             emit_rex(buf, is_64, 0, 0, d >= 8);
             if (src->data.imm >= -128 && src->data.imm <= 127) {
                 buffer_write_byte(buf, 0x83);
                 emit_modrm(buf, 3, 0, d);
                 buffer_write_byte(buf, (uint8_t)src->data.imm);
             } else {
                 buffer_write_byte(buf, 0x81);
                 emit_modrm(buf, 3, 0, d);
                 buffer_write_dword(buf, (uint32_t)src->data.imm);
             }
        }
    } else if (strcmp(mnemonic, "sub") == 0 || strcmp(mnemonic, "subq") == 0 || strcmp(mnemonic, "subl") == 0) {
        is_64 = (mnemonic[3] != 'l');
        if (src->type == OP_REG && dest->type == OP_REG) {
            int s = get_reg_id(src->data.reg);
            int d = get_reg_id(dest->data.reg);
            emit_rex(buf, is_64, s >= 8, 0, d >= 8);
            buffer_write_byte(buf, 0x29);
            emit_modrm(buf, 3, s, d);
        } else if (src->type == OP_IMM && dest->type == OP_REG) {
             int d = get_reg_id(dest->data.reg);
             emit_rex(buf, is_64, 0, 0, d >= 8);
             if (src->data.imm >= -128 && src->data.imm <= 127) {
                 buffer_write_byte(buf, 0x83);
                 emit_modrm(buf, 3, 5, d);
                 buffer_write_byte(buf, (uint8_t)src->data.imm);
             } else {
                 buffer_write_byte(buf, 0x81);
                 emit_modrm(buf, 3, 5, d);
                 buffer_write_dword(buf, (uint32_t)src->data.imm);
             }
        }
    } else if (strcmp(mnemonic, "imul") == 0 || strcmp(mnemonic, "imulq") == 0 || strcmp(mnemonic, "imull") == 0) {
        is_64 = (mnemonic[4] != 'l');
        if (src->type == OP_REG && dest->type == OP_REG) {
            int s = get_reg_id(src->data.reg);
            int d = get_reg_id(dest->data.reg);
            emit_rex(buf, is_64, d >= 8, 0, s >= 8);
            buffer_write_byte(buf, 0x0F);
            buffer_write_byte(buf, 0xAF);
            emit_modrm(buf, 3, d, s);
        } else if (src->type == OP_IMM && dest->type == OP_REG) {
            int d = get_reg_id(dest->data.reg);
            emit_rex(buf, is_64, d >= 8, 0, d >= 8);
            if (src->data.imm >= -128 && src->data.imm <= 127) {
                buffer_write_byte(buf, 0x6B);
                emit_modrm(buf, 3, d, d);
                buffer_write_byte(buf, (uint8_t)src->data.imm);
            } else {
                buffer_write_byte(buf, 0x69);
                emit_modrm(buf, 3, d, d);
                buffer_write_dword(buf, (uint32_t)src->data.imm);
            }
        }
    } else if (strcmp(mnemonic, "cmp") == 0 || strcmp(mnemonic, "cmpq") == 0 || strcmp(mnemonic, "cmpl") == 0) {
        is_64 = (mnemonic[3] != 'l');
        if (src->type == OP_REG && dest->type == OP_REG) {
            int s = get_reg_id(src->data.reg);
            int d = get_reg_id(dest->data.reg);
            emit_rex(buf, is_64, s >= 8, 0, d >= 8);
            buffer_write_byte(buf, 0x39);
            emit_modrm(buf, 3, s, d);
        } else if (src->type == OP_IMM && dest->type == OP_REG) {
            int d = get_reg_id(dest->data.reg);
            emit_rex(buf, is_64, 0, 0, d >= 8);
            if (src->data.imm >= -128 && src->data.imm <= 127) {
                buffer_write_byte(buf, 0x83);
                emit_modrm(buf, 3, 7, d);
                buffer_write_byte(buf, (uint8_t)src->data.imm);
            } else {
                buffer_write_byte(buf, 0x81);
                emit_modrm(buf, 3, 7, d);
                buffer_write_dword(buf, (uint32_t)src->data.imm);
            }
        }
    } else if (strcmp(mnemonic, "lea") == 0 || strcmp(mnemonic, "leaq") == 0 || strcmp(mnemonic, "leal") == 0) {
        is_64 = (mnemonic[3] != 'l');
        if (src->type == OP_MEM && dest->type == OP_REG) {
            int s = get_reg_id(src->data.mem.base);
            int d = get_reg_id(dest->data.reg);
            emit_rex(buf, is_64, d >= 8, 0, s >= 8);
            buffer_write_byte(buf, 0x8D);
            if (src->data.mem.offset == 0) emit_modrm(buf, 0, d, s);
            else { emit_modrm(buf, 2, d, s); buffer_write_dword(buf, src->data.mem.offset); }
        } else if (src->type == OP_LABEL && dest->type == OP_REG) {
            int d = get_reg_id(dest->data.reg);
            emit_rex(buf, is_64, d >= 8, 0, 0);
            buffer_write_byte(buf, 0x8D);
            emit_modrm(buf, 0, d, 5); // RIP-relative
            emit_reloc(buf, src->data.label, (uint32_t)buf->size);
            buffer_write_dword(buf, 0); // COFF REL32 addend = 0
        }
    } else if (strcmp(mnemonic, "movzbq") == 0) {
        if (src->type == OP_MEM && dest->type == OP_REG) {
            int s = get_reg_id(src->data.mem.base);
            int d = get_reg_id(dest->data.reg);
            emit_rex(buf, 1, d >= 8, 0, s >= 8);
            buffer_write_byte(buf, 0x0F);
            buffer_write_byte(buf, 0xB6);
            if (src->data.mem.offset == 0) {
                emit_modrm(buf, 0, d, s);
            } else {
                emit_modrm(buf, 2, d, s);
                buffer_write_dword(buf, src->data.mem.offset);
            }
        } else if (src->type == OP_REG && dest->type == OP_REG) {
            int s = get_reg_id(src->data.reg);
            int d = get_reg_id(dest->data.reg);
            emit_rex(buf, 1, d >= 8, 0, s >= 8); // dest is r64, src is r8
            buffer_write_byte(buf, 0x0F);
            buffer_write_byte(buf, 0xB6);
            emit_modrm(buf, 3, d, s);
        } else if (src->type == OP_LABEL && dest->type == OP_REG) {
            int d = get_reg_id(dest->data.reg);
            emit_rex(buf, 1, d >= 8, 0, 0);
            buffer_write_byte(buf, 0x0F);
            buffer_write_byte(buf, 0xB6);
            emit_modrm(buf, 0, d, 5); // RIP-relative
            emit_reloc(buf, src->data.label, (uint32_t)buf->size);
            buffer_write_dword(buf, 0); // COFF REL32 addend = 0
        }
    } else if (strcmp(mnemonic, "movsbq") == 0) {
        if (src->type == OP_MEM && dest->type == OP_REG) {
            int s = get_reg_id(src->data.mem.base);
            int d = get_reg_id(dest->data.reg);
            emit_rex(buf, 1, d >= 8, 0, s >= 8);
            buffer_write_byte(buf, 0x0F);
            buffer_write_byte(buf, 0xBE);
            if (src->data.mem.offset == 0) {
                emit_modrm(buf, 0, d, s);
            } else {
                emit_modrm(buf, 2, d, s);
                buffer_write_dword(buf, src->data.mem.offset);
            }
        } else if (src->type == OP_REG && dest->type == OP_REG) {
            int s = get_reg_id(src->data.reg);
            int d = get_reg_id(dest->data.reg);
            emit_rex(buf, 1, d >= 8, 0, s >= 8); // dest is r64, src is r8
            buffer_write_byte(buf, 0x0F);
            buffer_write_byte(buf, 0xBE);
            emit_modrm(buf, 3, d, s);
        } else if (src->type == OP_LABEL && dest->type == OP_REG) {
            int d = get_reg_id(dest->data.reg);
            emit_rex(buf, 1, d >= 8, 0, 0);
            buffer_write_byte(buf, 0x0F);
            buffer_write_byte(buf, 0xBE);
            emit_modrm(buf, 0, d, 5); // RIP-relative
            emit_reloc(buf, src->data.label, (uint32_t)buf->size);
            buffer_write_dword(buf, 0); // COFF REL32 addend = 0
        }
    } else if (strcmp(mnemonic, "movzwq") == 0) {
        if (src->type == OP_MEM && dest->type == OP_REG) {
            int s = get_reg_id(src->data.mem.base);
            int d = get_reg_id(dest->data.reg);
            emit_rex(buf, 1, d >= 8, 0, s >= 8);
            buffer_write_byte(buf, 0x0F);
            buffer_write_byte(buf, 0xB7);
            if (src->data.mem.offset == 0) emit_modrm(buf, 0, d, s);
            else { emit_modrm(buf, 2, d, s); buffer_write_dword(buf, src->data.mem.offset); }
        } else if (src->type == OP_REG && dest->type == OP_REG) {
            int s = get_reg_id(src->data.reg);
            int d = get_reg_id(dest->data.reg);
            emit_rex(buf, 1, d >= 8, 0, s >= 8);
            buffer_write_byte(buf, 0x0F);
            buffer_write_byte(buf, 0xB7);
            emit_modrm(buf, 3, d, s);
        } else if (src->type == OP_LABEL && dest->type == OP_REG) {
            int d = get_reg_id(dest->data.reg);
            emit_rex(buf, 1, d >= 8, 0, 0);
            buffer_write_byte(buf, 0x0F);
            buffer_write_byte(buf, 0xB7);
            emit_modrm(buf, 0, d, 5); // RIP-relative
            emit_reloc(buf, src->data.label, (uint32_t)buf->size);
            buffer_write_dword(buf, 0); // COFF REL32 addend = 0
        }
    } else if (strcmp(mnemonic, "movswq") == 0) {
        if (src->type == OP_MEM && dest->type == OP_REG) {
            int s = get_reg_id(src->data.mem.base);
            int d = get_reg_id(dest->data.reg);
            emit_rex(buf, 1, d >= 8, 0, s >= 8);
            buffer_write_byte(buf, 0x0F);
            buffer_write_byte(buf, 0xBF);
            if (src->data.mem.offset == 0) emit_modrm(buf, 0, d, s);
            else { emit_modrm(buf, 2, d, s); buffer_write_dword(buf, src->data.mem.offset); }
        } else if (src->type == OP_REG && dest->type == OP_REG) {
            int s = get_reg_id(src->data.reg);
            int d = get_reg_id(dest->data.reg);
            emit_rex(buf, 1, d >= 8, 0, s >= 8);
            buffer_write_byte(buf, 0x0F);
            buffer_write_byte(buf, 0xBF);
            emit_modrm(buf, 3, d, s);
        } else if (src->type == OP_LABEL && dest->type == OP_REG) {
            int d = get_reg_id(dest->data.reg);
            emit_rex(buf, 1, d >= 8, 0, 0);
            buffer_write_byte(buf, 0x0F);
            buffer_write_byte(buf, 0xBF);
            emit_modrm(buf, 0, d, 5); // RIP-relative
            emit_reloc(buf, src->data.label, (uint32_t)buf->size);
            buffer_write_dword(buf, 0); // COFF REL32 addend = 0
        }
    } else if (strcmp(mnemonic, "movslq") == 0) {
        /* MOVSXD: sign-extend dword (32-bit) to qword (64-bit).  REX.W + 0x63 /r */
        if (src->type == OP_MEM && dest->type == OP_REG) {
            int s = get_reg_id(src->data.mem.base);
            int d = get_reg_id(dest->data.reg);
            emit_rex(buf, 1, d >= 8, 0, s >= 8);
            buffer_write_byte(buf, 0x63);
            if (src->data.mem.offset == 0) emit_modrm(buf, 0, d, s);
            else { emit_modrm(buf, 2, d, s); buffer_write_dword(buf, src->data.mem.offset); }
        } else if (src->type == OP_REG && dest->type == OP_REG) {
            int s = get_reg_id(src->data.reg);
            int d = get_reg_id(dest->data.reg);
            emit_rex(buf, 1, d >= 8, 0, s >= 8);
            buffer_write_byte(buf, 0x63);
            emit_modrm(buf, 3, d, s);
        } else if (src->type == OP_LABEL && dest->type == OP_REG) {
            int d = get_reg_id(dest->data.reg);
            emit_rex(buf, 1, d >= 8, 0, 0);
            buffer_write_byte(buf, 0x63);
            emit_modrm(buf, 0, d, 5); /* RIP-relative */
            emit_reloc(buf, src->data.label, (uint32_t)buf->size);
            buffer_write_dword(buf, 0);
        }
    } else if (strcmp(mnemonic, "xor") == 0 || strcmp(mnemonic, "and") == 0 || strcmp(mnemonic, "or") == 0 || strcmp(mnemonic, "test") == 0 ||
               strcmp(mnemonic, "xorl") == 0 || strcmp(mnemonic, "andl") == 0 || strcmp(mnemonic, "orl") == 0) {
        int w = (mnemonic[strlen(mnemonic)-1] != 'l'); // 64-bit unless suffix 'l'
        if (src->type == OP_REG && dest->type == OP_REG) {
            int s = get_reg_id(src->data.reg);
            int d = get_reg_id(dest->data.reg);
            emit_rex(buf, w, s >= 8, 0, d >= 8);
            uint8_t opcode = 0x31; // xor
            if (strncmp(mnemonic, "and", 3) == 0) opcode = 0x21;
            else if (strncmp(mnemonic, "or", 2) == 0 && (mnemonic[2] == '\0' || mnemonic[2] == 'l')) opcode = 0x09;
            else if (strcmp(mnemonic, "test") == 0) opcode = 0x85;
            buffer_write_byte(buf, opcode);
            emit_modrm(buf, 3, s, d);
        } else if (src->type == OP_IMM && dest->type == OP_REG) {
            int d = get_reg_id(dest->data.reg);
            emit_rex(buf, w, 0, 0, d >= 8);
            uint8_t extension = 4; // and
            if (strncmp(mnemonic, "or", 2) == 0 && (mnemonic[2] == '\0' || mnemonic[2] == 'l')) extension = 1;
            else if (strncmp(mnemonic, "xor", 3) == 0) extension = 6;
            
            if (strcmp(mnemonic, "test") == 0) {
                buffer_write_byte(buf, 0xF7);
                emit_modrm(buf, 3, 0, d);
                buffer_write_dword(buf, src->data.imm);
            } else {
                if (src->data.imm >= -128 && src->data.imm <= 127) {
                    buffer_write_byte(buf, 0x83);
                    emit_modrm(buf, 3, extension, d);
                    buffer_write_byte(buf, (uint8_t)src->data.imm);
                } else {
                    buffer_write_byte(buf, 0x81);
                    emit_modrm(buf, 3, extension, d);
                    buffer_write_dword(buf, src->data.imm);
                }
            }
        }
    } else if (strcmp(mnemonic, "shl") == 0 || strcmp(mnemonic, "sar") == 0 || strcmp(mnemonic, "shr") == 0) {
        if (src->type == OP_REG && dest->type == OP_REG && strcmp(src->data.reg, "cl") == 0) {
            int d = get_reg_id(dest->data.reg);
            emit_rex(buf, 1, 0, 0, d >= 8);
            buffer_write_byte(buf, 0xD3);
            uint8_t extension = 4; // shl
            if (strcmp(mnemonic, "sar") == 0) extension = 7;
            else if (strcmp(mnemonic, "shr") == 0) extension = 5;
            emit_modrm(buf, 3, extension, d);
        } else if (src->type == OP_IMM && dest->type == OP_REG) {
            int d = get_reg_id(dest->data.reg);
            emit_rex(buf, 1, 0, 0, d >= 8);
            buffer_write_byte(buf, 0xC1);
            uint8_t extension = 4; // shl
            if (strcmp(mnemonic, "sar") == 0) extension = 7;
            else if (strcmp(mnemonic, "shr") == 0) extension = 5;
            emit_modrm(buf, 3, extension, d);
            buffer_write_byte(buf, (uint8_t)src->data.imm);
        }
    } else if (strcmp(mnemonic, "idiv") == 0) {
        // Unary idiv for modulo/div: idiv r/m64
        if (dest->type == OP_REG) {
            int d = get_reg_id(dest->data.reg);
            emit_rex(buf, 1, 0, 0, d >= 8);
            buffer_write_byte(buf, 0xF7);
            emit_modrm(buf, 3, 7, d); // /7 extension
        }
    } else if (strcmp(mnemonic, "movss") == 0 || strcmp(mnemonic, "movsd") == 0) {
        int is_double = (strcmp(mnemonic, "movsd") == 0);
        buffer_write_byte(buf, is_double ? 0xF2 : 0xF3);
        
        if (src->type == OP_REG && dest->type == OP_REG) {
            int s = get_reg_id(src->data.reg);
            int d = get_reg_id(dest->data.reg);
            emit_rex(buf, 0, d >= 8, 0, s >= 8);
            buffer_write_byte(buf, 0x0F);
            buffer_write_byte(buf, 0x10);
            emit_modrm(buf, 3, d, s);
        } else if (src->type == OP_REG && dest->type == OP_MEM) {
            int s = get_reg_id(src->data.reg);
            int d = get_reg_id(dest->data.mem.base);
            emit_rex(buf, 0, s >= 8, 0, d >= 8);
            buffer_write_byte(buf, 0x0F);
            buffer_write_byte(buf, 0x11);
            if (dest->data.mem.offset == 0) emit_modrm(buf, 0, s, d);
            else { emit_modrm(buf, 2, s, d); buffer_write_dword(buf, dest->data.mem.offset); }
        } else if (src->type == OP_MEM && dest->type == OP_REG) {
            int s = get_reg_id(src->data.mem.base);
            int d = get_reg_id(dest->data.reg);
            emit_rex(buf, 0, d >= 8, 0, s >= 8);
            buffer_write_byte(buf, 0x0F);
            buffer_write_byte(buf, 0x10);
            if (src->data.mem.offset == 0) emit_modrm(buf, 0, d, s);
            else { emit_modrm(buf, 2, d, s); buffer_write_dword(buf, src->data.mem.offset); }
        } else if (src->type == OP_LABEL && dest->type == OP_REG) {
            int d = get_reg_id(dest->data.reg);
            emit_rex(buf, 0, d >= 8, 0, 0);
            buffer_write_byte(buf, 0x0F);
            buffer_write_byte(buf, 0x10);
            emit_modrm(buf, 0, d, 5); // RIP-relative
            emit_reloc(buf, src->data.label, (uint32_t)buf->size);
            buffer_write_dword(buf, 0); // COFF REL32 addend = 0
        } else if (src->type == OP_REG && dest->type == OP_LABEL) {
            int s = get_reg_id(src->data.reg);
            emit_rex(buf, 0, s >= 8, 0, 0);
            buffer_write_byte(buf, 0x0F);
            buffer_write_byte(buf, 0x11);
            emit_modrm(buf, 0, s, 5); // RIP-relative
            emit_reloc(buf, dest->data.label, (uint32_t)buf->size);
            buffer_write_dword(buf, 0); // COFF REL32 addend = 0
        }
    } else if (strcmp(mnemonic, "addss") == 0 || strcmp(mnemonic, "addsd") == 0 ||
               strcmp(mnemonic, "subss") == 0 || strcmp(mnemonic, "subsd") == 0 ||
               strcmp(mnemonic, "mulss") == 0 || strcmp(mnemonic, "mulsd") == 0 ||
               strcmp(mnemonic, "divss") == 0 || strcmp(mnemonic, "divsd") == 0) {
        int is_double = (mnemonic[4] == 'd');
        uint8_t opcode = 0x58; // ADD
        if (mnemonic[0] == 's') opcode = 0x5C;
        else if (mnemonic[0] == 'm') opcode = 0x59;
        else if (mnemonic[0] == 'd') opcode = 0x5E;
        
        buffer_write_byte(buf, is_double ? 0xF2 : 0xF3);
        if (src->type == OP_REG && dest->type == OP_REG) {
            int s = get_reg_id(src->data.reg);
            int d = get_reg_id(dest->data.reg);
            emit_rex(buf, 0, d >= 8, 0, s >= 8);
            buffer_write_byte(buf, 0x0F);
            buffer_write_byte(buf, opcode);
            emit_modrm(buf, 3, d, s);
        }
    } else if (strcmp(mnemonic, "ucomiss") == 0 || strcmp(mnemonic, "ucomisd") == 0) {
        int is_double = (strcmp(mnemonic, "ucomisd") == 0);
        if (is_double) buffer_write_byte(buf, 0x66);
        if (src->type == OP_REG && dest->type == OP_REG) {
            int s = get_reg_id(src->data.reg);
            int d = get_reg_id(dest->data.reg);
            emit_rex(buf, 0, d >= 8, 0, s >= 8);
            buffer_write_byte(buf, 0x0F);
            buffer_write_byte(buf, 0x2E);
            emit_modrm(buf, 3, d, s);
        }
    } else if (strcmp(mnemonic, "cvtsi2ss") == 0 || strcmp(mnemonic, "cvtsi2sd") == 0) {
        int is_double = (strcmp(mnemonic, "cvtsi2sd") == 0);
        buffer_write_byte(buf, is_double ? 0xF2 : 0xF3);
        if (src->type == OP_REG && dest->type == OP_REG) {
            int s = get_reg_id(src->data.reg);
            int d = get_reg_id(dest->data.reg);
            emit_rex(buf, 1, d >= 8, 0, s >= 8); // s is GPR (r64), d is XMM
            buffer_write_byte(buf, 0x0F);
            buffer_write_byte(buf, 0x2A);
            emit_modrm(buf, 3, d, s);
        }
    } else if (strcmp(mnemonic, "cvttss2si") == 0 || strcmp(mnemonic, "cvttsd2si") == 0) {
        int is_double = (strcmp(mnemonic, "cvttsd2si") == 0);
        buffer_write_byte(buf, is_double ? 0xF2 : 0xF3);
        if (src->type == OP_REG && dest->type == OP_REG) {
            int s = get_reg_id(src->data.reg);
            int d = get_reg_id(dest->data.reg);
            emit_rex(buf, 1, d >= 8, 0, s >= 8); // s is XMM, d is GPR (r64)
            buffer_write_byte(buf, 0x0F);
            buffer_write_byte(buf, 0x2C);
            emit_modrm(buf, 3, d, s);
        }
    } else if (strcmp(mnemonic, "cvtss2sd") == 0 || strcmp(mnemonic, "cvtsd2ss") == 0) {
        int to_double = (strcmp(mnemonic, "cvtss2sd") == 0);
        buffer_write_byte(buf, to_double ? 0xF3 : 0xF2);
        if (src->type == OP_REG && dest->type == OP_REG) {
            int s = get_reg_id(src->data.reg);
            int d = get_reg_id(dest->data.reg);
            emit_rex(buf, 0, d >= 8, 0, s >= 8);
            buffer_write_byte(buf, 0x0F);
            buffer_write_byte(buf, 0x5A);
            emit_modrm(buf, 3, d, s);
        }
    /* ---- Packed SSE/SSE2 instructions for vectorization ---- */
    } else if (strcmp(mnemonic, "movups") == 0) {
        /* Unaligned packed single-precision float move */
        /* No mandatory prefix (NP) + 0x0F 0x10 (load) / 0x0F 0x11 (store) */
        if (src->type == OP_REG && dest->type == OP_REG) {
            int s = get_reg_id(src->data.reg);
            int d = get_reg_id(dest->data.reg);
            emit_rex(buf, 0, d >= 8, 0, s >= 8);
            buffer_write_byte(buf, 0x0F);
            buffer_write_byte(buf, 0x10);
            emit_modrm(buf, 3, d, s);
        } else if (src->type == OP_MEM && dest->type == OP_REG) {
            int s = get_reg_id(src->data.mem.base);
            int d = get_reg_id(dest->data.reg);
            emit_rex(buf, 0, d >= 8, 0, s >= 8);
            buffer_write_byte(buf, 0x0F);
            buffer_write_byte(buf, 0x10);
            if ((s & 7) == 5 && src->data.mem.offset == 0) {
                emit_modrm(buf, 1, d, s); buffer_write_byte(buf, 0);
            } else if (src->data.mem.offset == 0) {
                emit_modrm(buf, 0, d, s);
            } else if (src->data.mem.offset >= -128 && src->data.mem.offset <= 127) {
                emit_modrm(buf, 1, d, s); buffer_write_byte(buf, (uint8_t)src->data.mem.offset);
            } else {
                emit_modrm(buf, 2, d, s); buffer_write_dword(buf, src->data.mem.offset);
            }
        } else if (src->type == OP_REG && dest->type == OP_MEM) {
            int s = get_reg_id(src->data.reg);
            int d = get_reg_id(dest->data.mem.base);
            emit_rex(buf, 0, s >= 8, 0, d >= 8);
            buffer_write_byte(buf, 0x0F);
            buffer_write_byte(buf, 0x11);
            if ((d & 7) == 5 && dest->data.mem.offset == 0) {
                emit_modrm(buf, 1, s, d); buffer_write_byte(buf, 0);
            } else if (dest->data.mem.offset == 0) {
                emit_modrm(buf, 0, s, d);
            } else if (dest->data.mem.offset >= -128 && dest->data.mem.offset <= 127) {
                emit_modrm(buf, 1, s, d); buffer_write_byte(buf, (uint8_t)dest->data.mem.offset);
            } else {
                emit_modrm(buf, 2, s, d); buffer_write_dword(buf, dest->data.mem.offset);
            }
        }
    } else if (strcmp(mnemonic, "movdqu") == 0) {
        /* Unaligned packed integer move: F3 0F 6F (load) / F3 0F 7F (store) */
        buffer_write_byte(buf, 0xF3);
        if (src->type == OP_REG && dest->type == OP_REG) {
            int s = get_reg_id(src->data.reg);
            int d = get_reg_id(dest->data.reg);
            emit_rex(buf, 0, d >= 8, 0, s >= 8);
            buffer_write_byte(buf, 0x0F);
            buffer_write_byte(buf, 0x6F);
            emit_modrm(buf, 3, d, s);
        } else if (src->type == OP_MEM && dest->type == OP_REG) {
            int s = get_reg_id(src->data.mem.base);
            int d = get_reg_id(dest->data.reg);
            emit_rex(buf, 0, d >= 8, 0, s >= 8);
            buffer_write_byte(buf, 0x0F);
            buffer_write_byte(buf, 0x6F);
            if ((s & 7) == 5 && src->data.mem.offset == 0) {
                emit_modrm(buf, 1, d, s); buffer_write_byte(buf, 0);
            } else if (src->data.mem.offset == 0) {
                emit_modrm(buf, 0, d, s);
            } else if (src->data.mem.offset >= -128 && src->data.mem.offset <= 127) {
                emit_modrm(buf, 1, d, s); buffer_write_byte(buf, (uint8_t)src->data.mem.offset);
            } else {
                emit_modrm(buf, 2, d, s); buffer_write_dword(buf, src->data.mem.offset);
            }
        } else if (src->type == OP_REG && dest->type == OP_MEM) {
            int s = get_reg_id(src->data.reg);
            int d = get_reg_id(dest->data.mem.base);
            emit_rex(buf, 0, s >= 8, 0, d >= 8);
            buffer_write_byte(buf, 0x0F);
            buffer_write_byte(buf, 0x7F);
            if ((d & 7) == 5 && dest->data.mem.offset == 0) {
                emit_modrm(buf, 1, s, d); buffer_write_byte(buf, 0);
            } else if (dest->data.mem.offset == 0) {
                emit_modrm(buf, 0, s, d);
            } else if (dest->data.mem.offset >= -128 && dest->data.mem.offset <= 127) {
                emit_modrm(buf, 1, s, d); buffer_write_byte(buf, (uint8_t)dest->data.mem.offset);
            } else {
                emit_modrm(buf, 2, s, d); buffer_write_dword(buf, dest->data.mem.offset);
            }
        }
    } else if (strcmp(mnemonic, "addps") == 0 || strcmp(mnemonic, "subps") == 0 ||
               strcmp(mnemonic, "mulps") == 0 || strcmp(mnemonic, "divps") == 0) {
        /* Packed single-precision float arithmetic: NP 0F {58,5C,59,5E} */
        uint8_t opcode = 0x58; /* addps */
        if (mnemonic[0] == 's') opcode = 0x5C;
        else if (mnemonic[0] == 'm') opcode = 0x59;
        else if (mnemonic[0] == 'd') opcode = 0x5E;
        /* No mandatory prefix for packed single */
        if (src->type == OP_REG && dest->type == OP_REG) {
            int s = get_reg_id(src->data.reg);
            int d = get_reg_id(dest->data.reg);
            emit_rex(buf, 0, d >= 8, 0, s >= 8);
            buffer_write_byte(buf, 0x0F);
            buffer_write_byte(buf, opcode);
            emit_modrm(buf, 3, d, s);
        }
    } else if (strcmp(mnemonic, "paddd") == 0 || strcmp(mnemonic, "psubd") == 0) {
        /* Packed int32 add/sub: 66 0F FE (paddd) / 66 0F FA (psubd) */
        uint8_t opcode = (mnemonic[1] == 'a') ? 0xFE : 0xFA;
        buffer_write_byte(buf, 0x66);
        if (src->type == OP_REG && dest->type == OP_REG) {
            int s = get_reg_id(src->data.reg);
            int d = get_reg_id(dest->data.reg);
            emit_rex(buf, 0, d >= 8, 0, s >= 8);
            buffer_write_byte(buf, 0x0F);
            buffer_write_byte(buf, opcode);
            emit_modrm(buf, 3, d, s);
        }
    /* ---- AVX/AVX2 2-operand instructions (VEX-encoded) ---- */
    } else if (strcmp(mnemonic, "vmovups") == 0) {
        /* VEX.{128,256}.0F.WIG 10/11 — unaligned packed float move */
        int is_ymm = 0;
        if (src->type == OP_REG && strncmp(src->data.reg, "ymm", 3) == 0) is_ymm = 1;
        if (dest->type == OP_REG && strncmp(dest->data.reg, "ymm", 3) == 0) is_ymm = 1;
        if (src->type == OP_REG && dest->type == OP_REG) {
            int s = get_reg_id(src->data.reg);
            int d = get_reg_id(dest->data.reg);
            emit_vex(buf, d, s, 0, is_ymm, 0); /* pp=0(none) */
            buffer_write_byte(buf, 0x10);
            emit_modrm(buf, 3, d, s);
        } else if (src->type == OP_MEM && dest->type == OP_REG) {
            int s = get_reg_id(src->data.mem.base);
            int d = get_reg_id(dest->data.reg);
            emit_vex(buf, d, s, 0, is_ymm, 0);
            buffer_write_byte(buf, 0x10);
            if ((s & 7) == 5 && src->data.mem.offset == 0) {
                emit_modrm(buf, 1, d, s); buffer_write_byte(buf, 0);
            } else if (src->data.mem.offset == 0) {
                emit_modrm(buf, 0, d, s);
            } else if (src->data.mem.offset >= -128 && src->data.mem.offset <= 127) {
                emit_modrm(buf, 1, d, s); buffer_write_byte(buf, (uint8_t)src->data.mem.offset);
            } else {
                emit_modrm(buf, 2, d, s); buffer_write_dword(buf, src->data.mem.offset);
            }
        } else if (src->type == OP_REG && dest->type == OP_MEM) {
            int s = get_reg_id(src->data.reg);
            int d = get_reg_id(dest->data.mem.base);
            emit_vex(buf, s, d, 0, is_ymm, 0);
            buffer_write_byte(buf, 0x11);
            if ((d & 7) == 5 && dest->data.mem.offset == 0) {
                emit_modrm(buf, 1, s, d); buffer_write_byte(buf, 0);
            } else if (dest->data.mem.offset == 0) {
                emit_modrm(buf, 0, s, d);
            } else if (dest->data.mem.offset >= -128 && dest->data.mem.offset <= 127) {
                emit_modrm(buf, 1, s, d); buffer_write_byte(buf, (uint8_t)dest->data.mem.offset);
            } else {
                emit_modrm(buf, 2, s, d); buffer_write_dword(buf, dest->data.mem.offset);
            }
        }
    } else if (strcmp(mnemonic, "vmovdqu") == 0) {
        /* VEX.{128,256}.F3.0F.WIG 6F/7F — unaligned packed integer move */
        int is_ymm = 0;
        if (src->type == OP_REG && strncmp(src->data.reg, "ymm", 3) == 0) is_ymm = 1;
        if (dest->type == OP_REG && strncmp(dest->data.reg, "ymm", 3) == 0) is_ymm = 1;
        if (src->type == OP_REG && dest->type == OP_REG) {
            int s = get_reg_id(src->data.reg);
            int d = get_reg_id(dest->data.reg);
            emit_vex(buf, d, s, 0, is_ymm, 2); /* pp=2(F3) */
            buffer_write_byte(buf, 0x6F);
            emit_modrm(buf, 3, d, s);
        } else if (src->type == OP_MEM && dest->type == OP_REG) {
            int s = get_reg_id(src->data.mem.base);
            int d = get_reg_id(dest->data.reg);
            emit_vex(buf, d, s, 0, is_ymm, 2);
            buffer_write_byte(buf, 0x6F);
            if ((s & 7) == 5 && src->data.mem.offset == 0) {
                emit_modrm(buf, 1, d, s); buffer_write_byte(buf, 0);
            } else if (src->data.mem.offset == 0) {
                emit_modrm(buf, 0, d, s);
            } else if (src->data.mem.offset >= -128 && src->data.mem.offset <= 127) {
                emit_modrm(buf, 1, d, s); buffer_write_byte(buf, (uint8_t)src->data.mem.offset);
            } else {
                emit_modrm(buf, 2, d, s); buffer_write_dword(buf, src->data.mem.offset);
            }
        } else if (src->type == OP_REG && dest->type == OP_MEM) {
            int s = get_reg_id(src->data.reg);
            int d = get_reg_id(dest->data.mem.base);
            emit_vex(buf, s, d, 0, is_ymm, 2);
            buffer_write_byte(buf, 0x7F);
            if ((d & 7) == 5 && dest->data.mem.offset == 0) {
                emit_modrm(buf, 1, s, d); buffer_write_byte(buf, 0);
            } else if (dest->data.mem.offset == 0) {
                emit_modrm(buf, 0, s, d);
            } else if (dest->data.mem.offset >= -128 && dest->data.mem.offset <= 127) {
                emit_modrm(buf, 1, s, d); buffer_write_byte(buf, (uint8_t)dest->data.mem.offset);
            } else {
                emit_modrm(buf, 2, s, d); buffer_write_dword(buf, dest->data.mem.offset);
            }
        }
    }
}

/* ---- 3-operand instruction encoder for AVX/AVX2 ---- */
/* AVX arithmetic: dest = src2 OP src1  (AT&T order: src1, src2, dest) */
void encode_inst3(Buffer *buf, const char *mnemonic,
                  Operand *src1, Operand *src2, Operand *dest) {
    /* All 3-operand AVX instructions: VEX.NDS.{128,256}.{pp}.0F.WIG opcode /r
     * dest = ModRM.reg, src2 = VEX.vvvv, src1 = ModRM.r/m */
    if (strcmp(mnemonic, "vaddps") == 0 || strcmp(mnemonic, "vsubps") == 0 ||
        strcmp(mnemonic, "vmulps") == 0 || strcmp(mnemonic, "vdivps") == 0) {
        uint8_t opcode = 0x58; /* vaddps */
        if (mnemonic[1] == 's') opcode = 0x5C;
        else if (mnemonic[1] == 'm') opcode = 0x59;
        else if (mnemonic[1] == 'd') opcode = 0x5E;
        int is_ymm = 0;
        if (dest->type == OP_REG && strncmp(dest->data.reg, "ymm", 3) == 0) is_ymm = 1;
        if (src1->type == OP_REG && dest->type == OP_REG && src2->type == OP_REG) {
            int s1 = get_reg_id(src1->data.reg);
            int s2 = get_reg_id(src2->data.reg);
            int d  = get_reg_id(dest->data.reg);
            emit_vex(buf, d, s1, s2, is_ymm, 0); /* pp=0(none) for packed single */
            buffer_write_byte(buf, opcode);
            emit_modrm(buf, 3, d, s1);
        }
    } else if (strcmp(mnemonic, "vpaddd") == 0 || strcmp(mnemonic, "vpsubd") == 0) {
        /* VEX.NDS.{128,256}.66.0F.WIG FE/FA */
        uint8_t opcode = (mnemonic[2] == 'a') ? 0xFE : 0xFA;
        int is_ymm = 0;
        if (dest->type == OP_REG && strncmp(dest->data.reg, "ymm", 3) == 0) is_ymm = 1;
        if (src1->type == OP_REG && dest->type == OP_REG && src2->type == OP_REG) {
            int s1 = get_reg_id(src1->data.reg);
            int s2 = get_reg_id(src2->data.reg);
            int d  = get_reg_id(dest->data.reg);
            emit_vex(buf, d, s1, s2, is_ymm, 1); /* pp=1(66) for packed integer */
            buffer_write_byte(buf, opcode);
            emit_modrm(buf, 3, d, s1);
        }
    }
}
