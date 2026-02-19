#include "encoder.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "coff_writer.h"

static COFFWriter *encoder_writer = NULL;

static int encoder_bits = 64;

void encoder_set_writer(COFFWriter *writer) {
    encoder_writer = writer;
}

void encoder_set_bitness(int bits) {
    encoder_bits = bits;
}

static void emit_reloc(Buffer *buf, const char *label, uint32_t offset) {
    if (!encoder_writer) return;
    int32_t sym_idx = coff_writer_find_symbol(encoder_writer, label);
    if (sym_idx < 0) {
        sym_idx = coff_writer_add_symbol(encoder_writer, label, 0, 0, 0, IMAGE_SYM_CLASS_EXTERNAL);
    }
    // Use abstract relocation types; specific types are handled by coff_writer based on machine type.
    uint16_t type = COFF_RELOC_RELATIVE;
    coff_writer_add_reloc(encoder_writer, offset, (uint32_t)sym_idx, type, 1 /* .text */);
}

static void emit_reloc_abs(Buffer *buf, const char *label, uint32_t offset) {
    if (!encoder_writer) return;
    int32_t sym_idx = coff_writer_find_symbol(encoder_writer, label);
    if (sym_idx < 0) {
        sym_idx = coff_writer_add_symbol(encoder_writer, label, 0, 0, 0, IMAGE_SYM_CLASS_EXTERNAL);
    }
    // Use abstract relocation types
    uint16_t type = COFF_RELOC_ABSOLUTE;
    coff_writer_add_reloc(encoder_writer, offset, (uint32_t)sym_idx, type, 1 /* .text */);
}

/* Helper to emit 0x66/0x67 prefixes for 16-bit mode */
static void emit_prefixes(Buffer *buf, int op_size, int addr_size) {
    if (encoder_bits == 16) {
        if (op_size == 4 || op_size == 32) buffer_write_byte(buf, 0x66);
        if (addr_size == 4 || addr_size == 32) buffer_write_byte(buf, 0x67);
    } else if (encoder_bits == 32) {
        if (op_size == 2 || op_size == 16) buffer_write_byte(buf, 0x66);
        if (addr_size == 2 || addr_size == 16) buffer_write_byte(buf, 0x67);
    }
}

int get_reg_id(const char *reg) {
    if (strcmp(reg, "rax") == 0 || strcmp(reg, "eax") == 0 || strcmp(reg, "ax") == 0 || strcmp(reg, "al") == 0) return 0;
    if (strcmp(reg, "rcx") == 0 || strcmp(reg, "ecx") == 0 || strcmp(reg, "cx") == 0 || strcmp(reg, "cl") == 0) return 1;
    if (strcmp(reg, "rdx") == 0 || strcmp(reg, "edx") == 0 || strcmp(reg, "dx") == 0 || strcmp(reg, "dl") == 0) return 2;
    if (strcmp(reg, "rbx") == 0 || strcmp(reg, "ebx") == 0 || strcmp(reg, "bx") == 0 || strcmp(reg, "bl") == 0) return 3;
    if (strcmp(reg, "rsp") == 0 || strcmp(reg, "esp") == 0 || strcmp(reg, "sp") == 0 || strcmp(reg, "spl") == 0 || strcmp(reg, "ah") == 0) return 4;
    if (strcmp(reg, "rbp") == 0 || strcmp(reg, "ebp") == 0 || strcmp(reg, "bp") == 0 || strcmp(reg, "bpl") == 0 || strcmp(reg, "ch") == 0) return 5;
    if (strcmp(reg, "rsi") == 0 || strcmp(reg, "esi") == 0 || strcmp(reg, "si") == 0 || strcmp(reg, "sil") == 0 || strcmp(reg, "dh") == 0) return 6;
    if (strcmp(reg, "rdi") == 0 || strcmp(reg, "edi") == 0 || strcmp(reg, "di") == 0 || strcmp(reg, "dil") == 0 || strcmp(reg, "bh") == 0) return 7;
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

int get_reg_size(const char *reg) {
    if (reg[0] == 'r' && reg[1] >= '0' && reg[1] <= '9') {
        int len = strlen(reg);
        if (reg[len-1] == 'd') return 4;
        if (reg[len-1] == 'w') return 2;
        if (reg[len-1] == 'b') return 1;
        return 8;
    }
    if (reg[0] == 'e') return 4;
    if (reg[0] == 'r') return 8;
    if (strlen(reg) == 2 && (reg[1] == 'x' || reg[1] == 'i' || reg[1] == 'p')) return 2;
    if (strlen(reg) == 2 && (reg[1] == 'l' || reg[1] == 'h')) return 1;
    if (strncmp(reg, "xmm", 3) == 0) return 16;
    if (strncmp(reg, "ymm", 3) == 0) return 32;
    return 0;
}

static void emit_rex(Buffer *buf, int w, int r, int x, int b) {
    if (encoder_bits != 64) return; // No REX in 32-bit/16-bit
    if (w || r || x || b) {
        buffer_write_byte(buf, 0x40 | (w << 3) | (r << 2) | (x << 1) | b);
    }
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
        /* `call` ... if we use 0x66 call, it pushes EIP? */
        /* This is tricky. 32-bit code in 16-bit mode (unreal/flat) usually assumes 32-bit offsets. */
        /* BUT we are restricted to 64K segments in pure real mode. */
        /* So IP is 16-bit. */
        buffer_write_byte(buf, 0xC3);
    } else if (strcmp(mnemonic, "leave") == 0) {
        /* leave = mov sp, bp; pop bp */
        /* In 32-bit: mov esp, ebp; pop ebp */
        /* In 16-bit with 0x66: mov esp, ebp; pop ebp (32-bit stack ops) */
        emit_prefixes(buf, 32, 0); // 32-bit operand size for EBP/ESP
        buffer_write_byte(buf, 0xC9);
    } else if (strcmp(mnemonic, "cqo") == 0) {
        emit_rex(buf, 1, 0, 0, 0); // REX.W
        buffer_write_byte(buf, 0x99);
    } else if (strcmp(mnemonic, "cdq") == 0 || strcmp(mnemonic, "cltd") == 0) {
        emit_prefixes(buf, 32, 0);
        buffer_write_byte(buf, 0x99);
    } else if (strcmp(mnemonic, "vzeroupper") == 0) {
        /* VEX.128.0F.WIG 77 — zero upper 128 bits of all YMM registers */
        emit_vex2(buf, 0, 0, 0, 0);
        buffer_write_byte(buf, 0x77);
    } else if (strcmp(mnemonic, "ud2") == 0) {
        /* 0F 0B — undefined instruction (trap) */
        buffer_write_byte(buf, 0x0F);
        buffer_write_byte(buf, 0x0B);
    } else if (strcmp(mnemonic, "syscall") == 0) {
        /* 0F 05 — syscall */
        buffer_write_byte(buf, 0x0F);
        buffer_write_byte(buf, 0x05);
    } else if (strcmp(mnemonic, "hlt") == 0) {
        /* F4 — hlt */
        buffer_write_byte(buf, 0xF4);
    }
}

void encode_inst1(Buffer *buf, const char *mnemonic, Operand *op1) {
    if (strcmp(mnemonic, "push") == 0 || strcmp(mnemonic, "pushq") == 0) {
        if (op1->type == OP_REG) {
            int reg = get_reg_id(op1->data.reg);
            emit_prefixes(buf, 32, 0); // PUSH/POP 32-bit reg
            if (reg >= 8) emit_rex(buf, 0, 0, 0, 1);
            buffer_write_byte(buf, 0x50 + (reg & 7));
        }
    } else if (strcmp(mnemonic, "pop") == 0 || strcmp(mnemonic, "popq") == 0) {
        if (op1->type == OP_REG) {
            int reg = get_reg_id(op1->data.reg);
            emit_prefixes(buf, 32, 0); // PUSH/POP 32-bit reg
            if (reg >= 8) emit_rex(buf, 0, 0, 0, 1);
            buffer_write_byte(buf, 0x58 + (reg & 7));
        }
    } else if (strcmp(mnemonic, "idiv") == 0 || strcmp(mnemonic, "div") == 0) {
        if (op1->type == OP_REG) {
            int reg = get_reg_id(op1->data.reg);
            int is_idiv = (strcmp(mnemonic, "idiv") == 0);
            int size = get_reg_size(op1->data.reg);
            if (size == 1) {
                buffer_write_byte(buf, 0xF6);
                emit_modrm(buf, 3, is_idiv ? 7 : 6, reg);
            } else {
                emit_prefixes(buf, size, 0); 
                emit_rex(buf, size == 64, 0, 0, reg >= 8);
                buffer_write_byte(buf, 0xF7);
                emit_modrm(buf, 3, is_idiv ? 7 : 6, reg); // /7 for IDIV, /6 for DIV
            }
        }
    } else if (strcmp(mnemonic, "inc") == 0 || strcmp(mnemonic, "incl") == 0 || strcmp(mnemonic, "incq") == 0) {
        if (op1->type == OP_REG) {
            int reg = get_reg_id(op1->data.reg);
            int size = get_reg_size(op1->data.reg);
            if (size == 1) {
                buffer_write_byte(buf, 0xFE);
                emit_modrm(buf, 3, 0, reg);
            } else {
                emit_prefixes(buf, size, 0);
                if (encoder_bits != 64 && size != 1) {
                    buffer_write_byte(buf, 0x40 + (reg & 7));
                } else {
                    emit_rex(buf, size == 64, 0, 0, reg >= 8);
                    buffer_write_byte(buf, 0xFF);
                    emit_modrm(buf, 3, 0, reg);
                }
            }
        }
    } else if (strcmp(mnemonic, "dec") == 0 || strcmp(mnemonic, "decl") == 0 || strcmp(mnemonic, "decq") == 0) {
        if (op1->type == OP_REG) {
            int reg = get_reg_id(op1->data.reg);
            int size = get_reg_size(op1->data.reg);
            if (size == 1) {
                buffer_write_byte(buf, 0xFE);
                emit_modrm(buf, 3, 1, reg);
            } else {
                emit_prefixes(buf, size, 0);
                if (encoder_bits != 64 && size != 1) {
                    buffer_write_byte(buf, 0x48 + (reg & 7));
                } else {
                    emit_rex(buf, size == 64, 0, 0, reg >= 8);
                    buffer_write_byte(buf, 0xFF);
                    emit_modrm(buf, 3, 1, reg);
                }
            }
        }
    } else if (strcmp(mnemonic, "jmp") == 0) {
        if (op1->type == OP_LABEL) {
            emit_prefixes(buf, 32, 0);
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
            else if (strcmp(mnemonic, "jae") == 0 || strcmp(mnemonic, "jnc") == 0) opcode = 0x83;
            else if (strcmp(mnemonic, "jbe") == 0) opcode = 0x86;
            else if (strcmp(mnemonic, "ja") == 0) opcode = 0x87;
            else if (strcmp(mnemonic, "jns") == 0) opcode = 0x89;
            else if (strcmp(mnemonic, "js") == 0) opcode = 0x88;
            else if (strcmp(mnemonic, "jc") == 0) opcode = 0x82;
            
            emit_prefixes(buf, 32, 0);
            buffer_write_byte(buf, 0x0F);
            buffer_write_byte(buf, opcode);
            emit_reloc(buf, op1->data.label, (uint32_t)buf->size);
            buffer_write_dword(buf, 0); // COFF REL32 addend = 0
        }
    } else if (strcmp(mnemonic, "call") == 0) {
        if (op1->type == OP_LABEL) {
            emit_prefixes(buf, 32, 0);
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
            int size = get_reg_size(op1->data.reg);
            if (size == 1) {
                buffer_write_byte(buf, 0xF6);
                emit_modrm(buf, 3, 3, reg);
            } else {
                emit_prefixes(buf, size, 0);
                emit_rex(buf, size == 64, 0, 0, reg >= 8);
                buffer_write_byte(buf, 0xF7);
                emit_modrm(buf, 3, 3, reg);
            }
        }
    } else if (strcmp(mnemonic, "int") == 0) {
        if (op1->type == OP_IMM) {
            buffer_write_byte(buf, 0xCD);
            buffer_write_byte(buf, (uint8_t)op1->data.imm);
        }
    } else if (strcmp(mnemonic, "loop") == 0) {
        if (op1->type == OP_LABEL) {
            /* loop rel8. This usually expects a short jump. */
            /* Our relocations are rel32. For now, let's use a 
               hack: emit a placeholder for rel8 and a relocation.
               Or better, the assembler should handle rel8 if possible. */
            buffer_write_byte(buf, 0xE2);
            emit_reloc(buf, op1->data.label, (uint32_t)buf->size);
            buffer_write_byte(buf, 0); // placeholder for rel8
            /* WARNING: The linker MUST support rel8 relocations for this to work! */
        }
    }
}

void encode_inst2(Buffer *buf, const char *short_mnemonic, Operand *ops_src, Operand *ops_dest) {
    char mnemonic[64];
    strncpy(mnemonic, short_mnemonic, 63); mnemonic[63] = '\0';

    /* Strip AT&T size suffixes (l, w, b, q) from arithmetic/mov mnemonics.
     * The operand size is already determined from register operands. */
    {
        int mlen = (int)strlen(mnemonic);
        if (mlen > 1) {
            char last = mnemonic[mlen - 1];
            if (last == 'l' || last == 'w' || last == 'b' || last == 'q') {
                /* Don't strip from known short mnemonics like "call", "jl", "shl", etc. */
                const char *base = mnemonic;
                if (strcmp(base, "subl") == 0 || strcmp(base, "addl") == 0 ||
                    strcmp(base, "cmpl") == 0 || strcmp(base, "andl") == 0 ||
                    strcmp(base, "orl") == 0  || strcmp(base, "xorl") == 0 ||
                    strcmp(base, "testl") == 0 || strcmp(base, "imull") == 0 ||
                    strcmp(base, "shll") == 0 || strcmp(base, "shrl") == 0 ||
                    strcmp(base, "sarl") == 0 ||
                    strcmp(base, "subq") == 0 || strcmp(base, "addq") == 0 ||
                    strcmp(base, "cmpq") == 0 || strcmp(base, "andq") == 0 ||
                    strcmp(base, "orq") == 0  || strcmp(base, "xorq") == 0 ||
                    strcmp(base, "testq") == 0) {
                    mnemonic[mlen - 1] = '\0';
                }
            }
        }
    }
    
    int size = 0;
    if (ops_dest->type == OP_REG) size = get_reg_size(ops_dest->data.reg);
    else if (ops_src->type == OP_REG) size = get_reg_size(ops_src->data.reg);
    
    int addr_size = 0;
    if (ops_dest->type == OP_MEM) addr_size = get_reg_size(ops_dest->data.mem.base);
    if (ops_src->type == OP_MEM) addr_size = get_reg_size(ops_src->data.mem.base);

    if (strcmp(mnemonic, "mov") == 0 || strcmp(mnemonic, "movq") == 0 || strcmp(mnemonic, "movl") == 0 || strcmp(mnemonic, "movw") == 0 || strcmp(mnemonic, "movb") == 0) {
        if ((ops_src->type == OP_IMM || ops_src->type == OP_LABEL) && ops_dest->type == OP_REG) {
            int reg = get_reg_id(ops_dest->data.reg);
            if (size == 1) {
                buffer_write_byte(buf, 0xB0 + (reg & 7));
                buffer_write_byte(buf, (uint8_t)ops_src->data.imm);
            } else {
                emit_prefixes(buf, size, addr_size);
                emit_rex(buf, size == 64, 0, 0, reg >= 8);
                buffer_write_byte(buf, 0xB8 + (reg & 7));
                if (ops_src->type == OP_LABEL) {
                    emit_reloc_abs(buf, ops_src->data.label, (uint32_t)buf->size);
                    buffer_write_dword(buf, 0);
                } else {
                    if (size == 64) buffer_write_qword(buf, (uint64_t)ops_src->data.imm);
                    else if (size == 2) buffer_write_word(buf, (uint16_t)ops_src->data.imm);
                    else buffer_write_dword(buf, (uint32_t)ops_src->data.imm);
                }
            }
        } else if (ops_src->type == OP_MEM_LABEL && ops_dest->type == OP_REG) {
             /* mov reg, [label] — load VALUE from memory at label address */
             int reg = get_reg_id(ops_dest->data.reg);
             emit_prefixes(buf, size, 4); /* address is always 32-bit for labels */
             emit_rex(buf, size == 64, reg >= 8, 0, 0);
             if (reg == 0 && (size == 4 || size == 32)) {
                 /* eax shortcut: opcode A1 */
                 buffer_write_byte(buf, (size == 1) ? 0xA0 : 0xA1);
             } else {
                 buffer_write_byte(buf, (size == 1) ? 0x8A : 0x8B);
                 emit_modrm(buf, 0, reg, 5); /* mod=00, rm=101 → disp32 */
             }
             emit_reloc_abs(buf, ops_src->data.label, (uint32_t)buf->size);
             buffer_write_dword(buf, 0);
        } else if (ops_src->type == OP_REG && ops_dest->type == OP_MEM_LABEL) {
             /* mov [label], reg — store VALUE to memory at label address */
             int reg = get_reg_id(ops_src->data.reg);
             emit_prefixes(buf, size, 4); /* address is always 32-bit for labels */
             emit_rex(buf, size == 64, reg >= 8, 0, 0);
             if (reg == 0 && (size == 4 || size == 32)) {
                 /* eax shortcut: opcode A3 */
                 buffer_write_byte(buf, (size == 1) ? 0xA2 : 0xA3);
             } else {
                 buffer_write_byte(buf, (size == 1) ? 0x88 : 0x89);
                 emit_modrm(buf, 0, reg, 5); /* mod=00, rm=101 → disp32 */
             }
             emit_reloc_abs(buf, ops_dest->data.label, (uint32_t)buf->size);
             buffer_write_dword(buf, 0);
        } else if (ops_src->type == OP_REG && ops_dest->type == OP_REG) {
            int s = get_reg_id(ops_src->data.reg);
            int d = get_reg_id(ops_dest->data.reg);
            emit_prefixes(buf, size, addr_size);
            emit_rex(buf, size == 64, s >= 8, 0, d >= 8);
            buffer_write_byte(buf, (size == 1) ? 0x88 : 0x89);
            emit_modrm(buf, 3, s, d);
        } else if (ops_src->type == OP_MEM && ops_dest->type == OP_REG) {
            int s = get_reg_id(ops_src->data.mem.base);
            int d = get_reg_id(ops_dest->data.reg);
            emit_prefixes(buf, size, addr_size);
            emit_rex(buf, size == 64, d >= 8, 0, s >= 8);
            buffer_write_byte(buf, (size == 1) ? 0x8A : 0x8B);
            if (ops_src->data.mem.offset == 0 && (s & 7) != 5) emit_modrm(buf, 0, d, s);
            else if (ops_src->data.mem.offset >= -128 && ops_src->data.mem.offset <= 127) {
                emit_modrm(buf, 1, d, s);
                buffer_write_byte(buf, (int8_t)ops_src->data.mem.offset);
            } else { emit_modrm(buf, 2, d, s); buffer_write_dword(buf, ops_src->data.mem.offset); }
        } else if (ops_src->type == OP_REG && ops_dest->type == OP_MEM) {
            int s = get_reg_id(ops_src->data.reg);
            int d = get_reg_id(ops_dest->data.mem.base);
            emit_prefixes(buf, size, addr_size);
            emit_rex(buf, size == 64, s >= 8, 0, d >= 8);
            buffer_write_byte(buf, (size == 1) ? 0x88 : 0x89);
            if (ops_dest->data.mem.offset == 0 && (d & 7) != 5) emit_modrm(buf, 0, s, d);
            else if (ops_dest->data.mem.offset >= -128 && ops_dest->data.mem.offset <= 127) {
                emit_modrm(buf, 1, s, d);
                buffer_write_byte(buf, (int8_t)ops_dest->data.mem.offset);
            } else { emit_modrm(buf, 2, s, d); buffer_write_dword(buf, ops_dest->data.mem.offset); }
        } else if (ops_src->type == OP_IMM && ops_dest->type == OP_MEM) {
            int d = get_reg_id(ops_dest->data.mem.base);
            emit_prefixes(buf, size, addr_size);
            emit_rex(buf, size == 64, 0, 0, d >= 8);
            buffer_write_byte(buf, (size == 1) ? 0xC6 : 0xC7);
            if (ops_dest->data.mem.offset == 0 && (d & 7) != 5) emit_modrm(buf, 0, 0, d);
            else if (ops_dest->data.mem.offset >= -128 && ops_dest->data.mem.offset <= 127) {
                emit_modrm(buf, 1, 0, d);
                buffer_write_byte(buf, (int8_t)ops_dest->data.mem.offset);
            } else { emit_modrm(buf, 2, 0, d); buffer_write_dword(buf, ops_dest->data.mem.offset); }
            if (size == 1) buffer_write_byte(buf, (uint8_t)ops_src->data.imm);
            else if (size == 2) buffer_write_word(buf, (uint16_t)ops_src->data.imm);
            else buffer_write_dword(buf, (uint32_t)ops_src->data.imm);
         }
    } else if (strcmp(mnemonic, "movzbl") == 0 || strcmp(mnemonic, "movzwl") == 0) {
        /* movzbl: 0F B6 /r  (zero-extend byte to dword)
         * movzwl: 0F B7 /r  (zero-extend word to dword) */
        int is_byte = (strcmp(mnemonic, "movzbl") == 0);
        if (ops_src->type == OP_REG && ops_dest->type == OP_REG) {
            int s = get_reg_id(ops_src->data.reg);
            int d = get_reg_id(ops_dest->data.reg);
            emit_prefixes(buf, 4, addr_size); /* result is always 32-bit */
            buffer_write_byte(buf, 0x0F);
            buffer_write_byte(buf, is_byte ? 0xB6 : 0xB7);
            emit_modrm(buf, 3, d, s);
        } else if (ops_src->type == OP_MEM && ops_dest->type == OP_REG) {
            int s = get_reg_id(ops_src->data.mem.base);
            int d = get_reg_id(ops_dest->data.reg);
            emit_prefixes(buf, 4, addr_size);
            buffer_write_byte(buf, 0x0F);
            buffer_write_byte(buf, is_byte ? 0xB6 : 0xB7);
            if (ops_src->data.mem.offset == 0 && (s & 7) != 5) emit_modrm(buf, 0, d, s);
            else if (ops_src->data.mem.offset >= -128 && ops_src->data.mem.offset <= 127) {
                emit_modrm(buf, 1, d, s);
                buffer_write_byte(buf, (int8_t)ops_src->data.mem.offset);
            } else { emit_modrm(buf, 2, d, s); buffer_write_dword(buf, ops_src->data.mem.offset); }
        } else if (ops_src->type == OP_MEM_LABEL && ops_dest->type == OP_REG) {
            int d = get_reg_id(ops_dest->data.reg);
            emit_prefixes(buf, 4, 4);
            buffer_write_byte(buf, 0x0F);
            buffer_write_byte(buf, is_byte ? 0xB6 : 0xB7);
            emit_modrm(buf, 0, d, 5); /* disp32 */
            emit_reloc_abs(buf, ops_src->data.label, (uint32_t)buf->size);
            buffer_write_dword(buf, 0);
        }
    } else if (strcmp(mnemonic, "xor") == 0 || strcmp(mnemonic, "and") == 0 || strcmp(mnemonic, "or") == 0 || strcmp(mnemonic, "add") == 0 || strcmp(mnemonic, "sub") == 0 || strcmp(mnemonic, "cmp") == 0) {
        uint8_t base_op = 0;
        int ext = 0;
        if (strcmp(mnemonic, "add") == 0) { base_op = 0x00; ext = 0; }
        else if (strcmp(mnemonic, "or") == 0) { base_op = 0x08; ext = 1; }
        else if (strcmp(mnemonic, "and") == 0) { base_op = 0x20; ext = 4; }
        else if (strcmp(mnemonic, "sub") == 0) { base_op = 0x28; ext = 5; }
        else if (strcmp(mnemonic, "xor") == 0) { base_op = 0x30; ext = 6; }
        else if (strcmp(mnemonic, "cmp") == 0) { base_op = 0x38; ext = 7; }

        if (ops_src->type == OP_REG && ops_dest->type == OP_REG) {
            int s = get_reg_id(ops_src->data.reg);
            int d = get_reg_id(ops_dest->data.reg);
            emit_prefixes(buf, size, addr_size);
            emit_rex(buf, size == 64, s >= 8, 0, d >= 8);
            buffer_write_byte(buf, base_op + (size == 1 ? 0 : 1));
            emit_modrm(buf, 3, s, d);
        } else if (ops_src->type == OP_IMM && ops_dest->type == OP_REG) {
            int d = get_reg_id(ops_dest->data.reg);
            emit_prefixes(buf, size, addr_size);
            emit_rex(buf, size == 64, 0, 0, d >= 8);
            if (size == 1) {
                buffer_write_byte(buf, 0x80);
                emit_modrm(buf, 3, ext, d);
                buffer_write_byte(buf, (uint8_t)ops_src->data.imm);
            } else if (ops_src->data.imm >= -128 && ops_src->data.imm <= 127 && strcmp(mnemonic, "and") != 0) {
                buffer_write_byte(buf, 0x83);
                emit_modrm(buf, 3, ext, d);
                buffer_write_byte(buf, (uint8_t)ops_src->data.imm);
            } else {
                buffer_write_byte(buf, 0x81);
                emit_modrm(buf, 3, ext, d);
                if (size == 2) buffer_write_word(buf, (uint16_t)ops_src->data.imm);
                else buffer_write_dword(buf, (uint32_t)ops_src->data.imm);
            }
        }
    } else if (strcmp(mnemonic, "test") == 0) {
        if (ops_src->type == OP_REG && ops_dest->type == OP_REG) {
            int s = get_reg_id(ops_src->data.reg);
            int d = get_reg_id(ops_dest->data.reg);
            emit_prefixes(buf, size, addr_size);
            emit_rex(buf, size == 64, s >= 8, 0, d >= 8);
            buffer_write_byte(buf, (size == 1) ? 0x84 : 0x85);
            emit_modrm(buf, 3, s, d);
        }
    } else if (strcmp(mnemonic, "shl") == 0 || strcmp(mnemonic, "shr") == 0 || strcmp(mnemonic, "sar") == 0) {
        /* shl/shr/sar imm8, reg: C1 /ext ib */
        int ext = 4; /* shl */
        if (strcmp(mnemonic, "shr") == 0) ext = 5;
        else if (strcmp(mnemonic, "sar") == 0) ext = 7;
        if (ops_src->type == OP_IMM && ops_dest->type == OP_REG) {
            int d = get_reg_id(ops_dest->data.reg);
            emit_prefixes(buf, size, addr_size);
            emit_rex(buf, size == 64, 0, 0, d >= 8);
            if (ops_src->data.imm == 1) {
                buffer_write_byte(buf, 0xD1);
                emit_modrm(buf, 3, ext, d);
            } else {
                buffer_write_byte(buf, 0xC1);
                emit_modrm(buf, 3, ext, d);
                buffer_write_byte(buf, (uint8_t)ops_src->data.imm);
            }
        } else if (ops_src->type == OP_REG && ops_dest->type == OP_REG &&
                   (strcmp(ops_src->data.reg, "cl") == 0 || strcmp(ops_src->data.reg, "ecx") == 0)) {
            /* shl %cl, %reg: D3 /ext */
            int d = get_reg_id(ops_dest->data.reg);
            emit_prefixes(buf, size, addr_size);
            emit_rex(buf, size == 64, 0, 0, d >= 8);
            buffer_write_byte(buf, 0xD3);
            emit_modrm(buf, 3, ext, d);
        }
    } else if (strcmp(mnemonic, "imul") == 0) {
        if (ops_src->type == OP_IMM && ops_dest->type == OP_REG) {
            /* imul $imm, %reg → 3-operand form: 6B /r ib or 69 /r id */
            int d = get_reg_id(ops_dest->data.reg);
            emit_prefixes(buf, size, addr_size);
            emit_rex(buf, size == 64, d >= 8, 0, d >= 8);
            if (ops_src->data.imm >= -128 && ops_src->data.imm <= 127) {
                buffer_write_byte(buf, 0x6B);
                emit_modrm(buf, 3, d, d); /* same reg for src and dst */
                buffer_write_byte(buf, (uint8_t)ops_src->data.imm);
            } else {
                buffer_write_byte(buf, 0x69);
                emit_modrm(buf, 3, d, d);
                buffer_write_dword(buf, (uint32_t)ops_src->data.imm);
            }
        } else if (ops_src->type == OP_REG && ops_dest->type == OP_REG) {
            /* imul %src, %dst: 0F AF /r */
            int s = get_reg_id(ops_src->data.reg);
            int d = get_reg_id(ops_dest->data.reg);
            emit_prefixes(buf, size, addr_size);
            emit_rex(buf, size == 64, d >= 8, 0, s >= 8);
            buffer_write_byte(buf, 0x0F);
            buffer_write_byte(buf, 0xAF);
            emit_modrm(buf, 3, d, s);
        }
    } else if (strcmp(mnemonic, "lea") == 0) {
        if (ops_src->type == OP_MEM && ops_dest->type == OP_REG) {
            int d = get_reg_id(ops_dest->data.reg);
            int s = get_reg_id(ops_src->data.mem.base);
            emit_prefixes(buf, size, addr_size);
            emit_rex(buf, size == 64, d >= 8, 0, s >= 8);
            buffer_write_byte(buf, 0x8D);
            if (ops_src->data.mem.offset == 0 && (s & 7) != 5) emit_modrm(buf, 0, d, s);
            else if (ops_src->data.mem.offset >= -128 && ops_src->data.mem.offset <= 127) {
                emit_modrm(buf, 1, d, s);
                buffer_write_byte(buf, (int8_t)ops_src->data.mem.offset);
            } else { emit_modrm(buf, 2, d, s); buffer_write_dword(buf, ops_src->data.mem.offset); }
        } else if ((ops_src->type == OP_LABEL || ops_src->type == OP_MEM_LABEL) && ops_dest->type == OP_REG) {
            int reg = get_reg_id(ops_dest->data.reg);
            emit_prefixes(buf, size, addr_size);
            emit_rex(buf, size == 64, 0, 0, reg >= 8);
            buffer_write_byte(buf, 0xB8 + (reg & 7));
            emit_reloc_abs(buf, ops_src->data.label, (uint32_t)buf->size);
            buffer_write_dword(buf, 0);
        }
    }
}
void encode_inst3(Buffer *buf, const char *mnemonic, Operand *op1, Operand *op2, Operand *op3) {
    (void)buf; (void)mnemonic; (void)op1; (void)op2; (void)op3;
}
