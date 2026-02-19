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
    int is_intel;
} AsContext;

static void skip_whitespace_on_line(AsContext *ctx) {
    while (ctx->input[ctx->pos] && isspace(ctx->input[ctx->pos]) && ctx->input[ctx->pos] != '\n') {
        ctx->pos++;
    }
}

static void skip_comments_across_lines(AsContext *ctx) {
    while (ctx->input[ctx->pos]) {
        if (isspace(ctx->input[ctx->pos])) {
            ctx->pos++;
        } else if (ctx->input[ctx->pos] == '/' && ctx->input[ctx->pos + 1] == '*') {
            ctx->pos += 2;
            while (ctx->input[ctx->pos] && !(ctx->input[ctx->pos] == '*' && ctx->input[ctx->pos + 1] == '/')) ctx->pos++;
            if (ctx->input[ctx->pos]) ctx->pos += 2;
        } else if (ctx->input[ctx->pos] == '/' && ctx->input[ctx->pos + 1] == '/') {
            while (ctx->input[ctx->pos] && ctx->input[ctx->pos] != '\n') ctx->pos++;
        } else {
            break;
        }
    }
}

static char *get_token_on_line(AsContext *ctx) {
    skip_whitespace_on_line(ctx);
    if (!ctx->input[ctx->pos] || ctx->input[ctx->pos] == '\n') return NULL;

    int start = ctx->pos;
    if (isalnum(ctx->input[ctx->pos]) || ctx->input[ctx->pos] == '_' || ctx->input[ctx->pos] == '.') {
        while (isalnum(ctx->input[ctx->pos]) || ctx->input[ctx->pos] == '_' || ctx->input[ctx->pos] == '.') ctx->pos++;
    } else {
        ctx->pos++;
    }

    int len = ctx->pos - start;
    char *token = malloc(len + 1);
    if (!token) return NULL;
    strncpy(token, &ctx->input[start], len);
    token[len] = '\0';
    return token;
}

static void parse_op(AsContext *ctx, Operand *op) {
    memset(op, 0, sizeof(Operand));
    skip_whitespace_on_line(ctx);
    if (ctx->input[ctx->pos] == '[') {
        ctx->pos++;
        char *base = get_token_on_line(ctx);
        op->type = OP_MEM;
        op->data.mem.base = base;
        skip_whitespace_on_line(ctx);
        if (ctx->input[ctx->pos] == '+') {
            ctx->pos++;
            char *off_str = get_token_on_line(ctx);
            if (off_str) { op->data.mem.offset = (int)strtoll(off_str, NULL, 0); free(off_str); }
        } else if (ctx->input[ctx->pos] == '-') {
            ctx->pos++;
            char *off_str = get_token_on_line(ctx);
            if (off_str) { op->data.mem.offset = -(int)strtoll(off_str, NULL, 0); free(off_str); }
        }
        while (ctx->input[ctx->pos] && ctx->input[ctx->pos] != ']' && ctx->input[ctx->pos] != '\n') ctx->pos++;
        if (ctx->input[ctx->pos] == ']') ctx->pos++;
    } else if (isdigit(ctx->input[ctx->pos]) || ctx->input[ctx->pos] == '-' || ctx->input[ctx->pos] == '\'') {
        op->type = OP_IMM;
        if (ctx->input[ctx->pos] == '\'') {
            ctx->pos++;
            op->data.imm = ctx->input[ctx->pos++];
            if (ctx->input[ctx->pos] == '\'') ctx->pos++;
        } else {
            char *imm_str = get_token_on_line(ctx);
            if (imm_str) { op->data.imm = strtoll(imm_str, NULL, 0); free(imm_str); }
        }
    } else {
        char *name = get_token_on_line(ctx);
        if (!name) return;
        const char *regs[] = {"eax", "ebx", "ecx", "edx", "esi", "edi", "ebp", "esp",
                             "ax", "bx", "cx", "dx", "si", "di", "bp", "sp",
                             "al", "bl", "cl", "dl", "ah", "bh", "ch", "dh", NULL};
        int is_reg = 0;
        for (int i = 0; regs[i]; i++) {
            if (strcmp(name, regs[i]) == 0) { is_reg = 1; break; }
        }
        if (is_reg) { op->type = OP_REG; op->data.reg = name; }
        else { op->type = OP_LABEL; op->data.label = name; }
    }
}

static void free_op(Operand *op) {
    if (op->type == OP_REG) free((void*)op->data.reg);
    else if (op->type == OP_LABEL) free((void*)op->data.label);
    else if (op->type == OP_MEM) free((void*)op->data.mem.base);
}

void assemble_line(AsContext *ctx) {
    skip_comments_across_lines(ctx);
    if (!ctx->input[ctx->pos]) return;

    char *token = get_token_on_line(ctx);
    if (!token) {
        if (ctx->input[ctx->pos] == '\n') ctx->pos++;
        return;
    }

    skip_whitespace_on_line(ctx);
    if (ctx->input[ctx->pos] == ':') {
        ctx->pos++;
        coff_writer_add_symbol(ctx->writer, token, (uint32_t)ctx->current_section->size, 1, 0, IMAGE_SYM_CLASS_EXTERNAL);
        free(token);
        assemble_line(ctx);
        return;
    }

    if (token[0] == '.') {
        if (strcmp(token, ".global") == 0) {
            char *name = get_token_on_line(ctx);
            if (name) { coff_writer_add_symbol(ctx->writer, name, 0, 0, 0, IMAGE_SYM_CLASS_EXTERNAL); free(name); }
        } else if (strcmp(token, ".code16") == 0) {
            ctx->bitness = 16;
            encoder_set_bitness(16);
        } else if (strcmp(token, ".intel_syntax") == 0) {
            ctx->is_intel = 1;
            char *noprefix = get_token_on_line(ctx); if (noprefix) free(noprefix);
        } else if (strcmp(token, ".section") == 0) {
            char *name = get_token_on_line(ctx);
            if (name) {
                if (strcmp(name, ".text") == 0) ctx->current_section = &ctx->writer->text_section;
                else if (strcmp(name, ".data") == 0) ctx->current_section = &ctx->writer->data_section;
                free(name);
            }
        } else if (strcmp(token, ".byte") == 0 || strcmp(token, ".word") == 0 || strcmp(token, ".long") == 0) {
            int sz = (strcmp(token, ".word") == 0) ? 2 : (strcmp(token, ".long") == 0 ? 4 : 1);
            do {
                skip_whitespace_on_line(ctx);
                char *vstr = get_token_on_line(ctx);
                if (vstr) {
                    long long val = strtoll(vstr, NULL, 0);
                    if (sz == 4) buffer_write_dword(ctx->current_section, (uint32_t)val);
                    else if (sz == 2) buffer_write_word(ctx->current_section, (uint16_t)val);
                    else buffer_write_byte(ctx->current_section, (uint8_t)val);
                    free(vstr);
                }
                skip_whitespace_on_line(ctx);
            } while (ctx->input[ctx->pos] == ',' && (ctx->pos++, 1));
        }
        free(token);
        return;
    }

    char *mnemonic = token;
    Operand o0, o1, o2;
    memset(&o0, 0, sizeof(o0)); memset(&o1, 0, sizeof(o1)); memset(&o2, 0, sizeof(o2));
    int op_count = 0;

    skip_whitespace_on_line(ctx);
    if (ctx->input[ctx->pos] && ctx->input[ctx->pos] != '\n') {
        parse_op(ctx, &o0);
        op_count = 1;
        skip_whitespace_on_line(ctx);
        if (ctx->input[ctx->pos] == ',') {
            ctx->pos++;
            parse_op(ctx, &o1);
            op_count = 2;
            skip_whitespace_on_line(ctx);
            if (ctx->input[ctx->pos] == ',') {
                ctx->pos++;
                parse_op(ctx, &o2);
                op_count = 3;
            }
        }
    }

    if (op_count == 0) encode_inst0(ctx->current_section, mnemonic);
    else if (op_count == 1) encode_inst1(ctx->current_section, mnemonic, &o0);
    else if (op_count == 2) {
        if (ctx->is_intel) encode_inst2(ctx->current_section, mnemonic, &o1, &o0);
        else encode_inst2(ctx->current_section, mnemonic, &o0, &o1);
    } else if (op_count == 3) {
        if (ctx->is_intel) encode_inst3(ctx->current_section, mnemonic, &o2, &o1, &o0);
        else encode_inst3(ctx->current_section, mnemonic, &o0, &o1, &o2);
    }

    free(mnemonic);
    if (op_count >= 1) free_op(&o0);
    if (op_count >= 2) free_op(&o1);
    if (op_count >= 3) free_op(&o2);
}

int assemble_file(const char *input_file, const char *output_file, TargetPlatform target) {
    FILE *f = fopen(input_file, "r");
    if (!f) return 1;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *input = malloc(size + 1);
    if (!input) { fclose(f); return 1; }
    if (fread(input, 1, size, f) != (size_t)size) { free(input); fclose(f); return 1; }
    input[size] = '\0';
    fclose(f);

    AsContext ctx = {input, 0, target, malloc(sizeof(COFFWriter)), NULL, 32, 0};
    coff_writer_init(ctx.writer);
    if (target == TARGET_DOS) coff_writer_set_machine(ctx.writer, IMAGE_FILE_MACHINE_I386);
    ctx.current_section = &ctx.writer->text_section;
    encoder_set_writer(ctx.writer);
    encoder_set_bitness(32);

    while (ctx.input[ctx.pos]) {
        assemble_line(&ctx);
        while (ctx.input[ctx.pos] && (isspace(ctx.input[ctx.pos]) || ctx.input[ctx.pos] == '\n')) {
            if (ctx.input[ctx.pos] == '\n') { ctx.pos++; break; }
            ctx.pos++;
        }
    }

    coff_writer_write(ctx.writer, output_file);
    free(input);
    return 0;
}
