#define _CRT_SECURE_NO_WARNINGS
#include "as_parser.h"
#include "encoder.h"
#include "buffer.h"
#include "coff_writer.h"
#include "codegen.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

typedef struct {
    char *input;
    int pos;
    TargetPlatform target;
    COFFWriter *writer;
    Buffer *current_section;
    int bitness;
} AsContext;

static void skip_whitespace(AsContext *ctx) {
    while (ctx->input[ctx->pos] && isspace(ctx->input[ctx->pos])) {
        ctx->pos++;
    }
    // Skip comments
    if (ctx->input[ctx->pos] == '/' && ctx->input[ctx->pos+1] == '*') {
        ctx->pos += 2;
        while (ctx->input[ctx->pos] && !(ctx->input[ctx->pos] == '*' && ctx->input[ctx->pos+1] == '/')) {
            ctx->pos++;
        }
        if (ctx->input[ctx->pos]) ctx->pos += 2;
        skip_whitespace(ctx);
    }
    if (ctx->input[ctx->pos] == '/' && ctx->input[ctx->pos+1] == '/') {
        while (ctx->input[ctx->pos] && ctx->input[ctx->pos] != '\n') {
            ctx->pos++;
        }
        skip_whitespace(ctx);
    }
}

static char *get_token(AsContext *ctx) {
    skip_whitespace(ctx);
    if (!ctx->input[ctx->pos]) return NULL;
    
    int start = ctx->pos;
    if (isalnum(ctx->input[ctx->pos]) || ctx->input[ctx->pos] == '_' || ctx->input[ctx->pos] == '.') {
        while (isalnum(ctx->input[ctx->pos]) || ctx->input[ctx->pos] == '_' || ctx->input[ctx->pos] == '.') {
            ctx->pos++;
        }
    } else {
        ctx->pos++;
    }
    
    int len = ctx->pos - start;
    char *token = malloc(len + 1);
    strncpy(token, &ctx->input[start], len);
    token[len] = '\0';
    return token;
}

static int expect_token(AsContext *ctx, const char *expected) {
    char *t = get_token(ctx);
    if (!t) return 0;
    int match = (strcmp(t, expected) == 0);
    free(t);
    return match;
}

static Operand parse_operand(AsContext *ctx) {
    Operand op;
    memset(&op, 0, sizeof(op));
    
    skip_whitespace(ctx);
    if (ctx->input[ctx->pos] == '[') {
        // Memory operand: [ebp+8]
        ctx->pos++;
        char *base = get_token(ctx);
        op.type = OP_MEM;
        op.data.mem.base = base;
        skip_whitespace(ctx);
        if (ctx->input[ctx->pos] == '+') {
            ctx->pos++;
            char *off_str = get_token(ctx);
            op.data.mem.offset = (int)strtoll(off_str, NULL, 0);
            free(off_str);
        } else if (ctx->input[ctx->pos] == '-') {
            ctx->pos++;
            char *off_str = get_token(ctx);
            op.data.mem.offset = -(int)strtoll(off_str, NULL, 0);
            free(off_str);
        }
        expect_token(ctx, "]");
    } else if (isdigit(ctx->input[ctx->pos]) || ctx->input[ctx->pos] == '-' || ctx->input[ctx->pos] == '\'') {
        // Immediate or character literal
        op.type = OP_IMM;
        if (ctx->input[ctx->pos] == '\'') {
            ctx->pos++;
            op.data.imm = ctx->input[ctx->pos++];
            if (ctx->input[ctx->pos] == '\'') ctx->pos++;
        } else {
            char *imm_str = get_token(ctx);
            if (imm_str) {
                op.data.imm = strtoll(imm_str, NULL, 0);
                free(imm_str);
            }
        }
    } else {
        // Register or Label
        char *name = get_token(ctx);
        // Very simple register check
        const char *regs[] = {"eax", "ebx", "ecx", "edx", "esi", "edi", "ebp", "esp",
                             "ax", "bx", "cx", "dx", "si", "di", "bp", "sp",
                             "al", "bl", "cl", "dl", "ah", "bh", "ch", "dh", NULL};
        int is_reg = 0;
        for (int i = 0; regs[i]; i++) {
            if (strcmp(name, regs[i]) == 0) {
                is_reg = 1;
                break;
            }
        }
        if (is_reg) {
            op.type = OP_REG;
            op.data.reg = name;
        } else {
            op.type = OP_LABEL;
            op.data.label = name;
        }
    }
    return op;
}

void assemble_line(AsContext *ctx) {
    skip_whitespace(ctx);
    if (!ctx->input[ctx->pos] || ctx->input[ctx->pos] == '\n') {
        if (ctx->input[ctx->pos] == '\n') ctx->pos++;
        return;
    }
    
    char *token = get_token(ctx);
    if (!token) return;
    
    // Check for label
    skip_whitespace(ctx);
    if (ctx->input[ctx->pos] == ':') {
        ctx->pos++;
        coff_writer_add_symbol(ctx->writer, token, (uint32_t)ctx->current_section->size, 1, 0, IMAGE_SYM_CLASS_EXTERNAL);
        free(token);
        assemble_line(ctx);
        return;
    }
    
    // Check for directives
    if (token[0] == '.') {
        if (strcmp(token, ".global") == 0) {
            char *name = get_token(ctx);
            coff_writer_add_symbol(ctx->writer, name, 0, 0, 0, IMAGE_SYM_CLASS_EXTERNAL); // External
            free(name);
        } else if (strcmp(token, ".intel_syntax") == 0) {
            get_token(ctx); // noprefix
        } else if (strcmp(token, ".code16") == 0) {
            ctx->bitness = 16;
            encoder_set_bitness(16);
        } else if (strcmp(token, ".section") == 0) {
            char *section_name = get_token(ctx);
            if (strcmp(section_name, ".text") == 0) ctx->current_section = &ctx->writer->text_section;
            else if (strcmp(section_name, ".data") == 0) ctx->current_section = &ctx->writer->data_section;
            free(section_name);
        } else if (strcmp(token, ".byte") == 0 || strcmp(token, ".word") == 0 || strcmp(token, ".long") == 0) {
            int size = 1;
            if (strcmp(token, ".word") == 0) size = 2;
            else if (strcmp(token, ".long") == 0) size = 4;
            
            do {
                skip_whitespace(ctx);
                char *val_str = get_token(ctx);
                if (val_str) {
                    long long val = strtoll(val_str, NULL, 0);
                    if (size == 4) buffer_write_dword(ctx->current_section, (uint32_t)val);
                    else if (size == 2) buffer_write_word(ctx->current_section, (uint16_t)val);
                    else buffer_write_byte(ctx->current_section, (uint8_t)val);
                    free(val_str);
                }
                skip_whitespace(ctx);
            } while (ctx->input[ctx->pos] == ',' && (ctx->pos++, 1));
        }
        free(token);
        return;
    }
    
    // Instruction
    char *mnemonic = token;
    Operand ops[3];
    int op_count = 0;
    
    skip_whitespace(ctx);
    if (ctx->input[ctx->pos] && ctx->input[ctx->pos] != '\n') {
        ops[op_count++] = parse_operand(ctx);
        skip_whitespace(ctx);
        while (ctx->input[ctx->pos] == ',') {
            ctx->pos++;
            ops[op_count++] = parse_operand(ctx);
            skip_whitespace(ctx);
        }
    }
    
    if (op_count == 0) {
        encode_inst0(ctx->current_section, mnemonic);
    } else if (op_count == 1) {
        encode_inst1(ctx->current_section, mnemonic, &ops[0]);
    } else if (op_count == 2) {
        // encoder.c uses (src, dest) order for Intel? 
        // Wait, encoder.c: encode_inst2(Buffer *buf, const char *mnemonic, Operand *src, Operand *dest)
        // Intel syntax: mnemonic dest, src
        // So we swap them.
        encode_inst2(ctx->current_section, mnemonic, &ops[1], &ops[0]);
    }
    
    // Clean up
    free(mnemonic);
    for (int i = 0; i < op_count; i++) {
        if (ops[i].type == OP_REG) free((void*)ops[i].data.reg);
        if (ops[i].type == OP_LABEL) free((void*)ops[i].data.label);
        if (ops[i].type == OP_MEM) free((void*)ops[i].data.mem.base);
    }
}

int assemble_file(const char *input_file, const char *output_file, TargetPlatform target) {
    FILE *f = fopen(input_file, "r");
    if (!f) return 1;
    
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *input = malloc(size + 1);
    fread(input, 1, size, f);
    input[size] = '\0';
    fclose(f);
    
    AsContext ctx;
    ctx.input = input;
    ctx.pos = 0;
    ctx.target = target;
    ctx.writer = malloc(sizeof(COFFWriter));
    coff_writer_init(ctx.writer);
    ctx.current_section = &ctx.writer->text_section;
    ctx.bitness = 32;
    
    encoder_set_writer(ctx.writer);
    encoder_set_bitness(32);
    
    while (ctx.input[ctx.pos]) {
        assemble_line(&ctx);
    }
    
    if (target == TARGET_DOS) {
        coff_writer_set_machine(ctx.writer, IMAGE_FILE_MACHINE_I386);
    }
    
    coff_writer_write(ctx.writer, output_file);
    
    free(input);
    // Note: coff_writer_free(ctx.writer) might be needed depending on implementation
    return 0;
}
