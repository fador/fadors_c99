#include "coff_writer.h"
#include "codegen.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>



void coff_writer_init(COFFWriter *w) {
    buffer_init(&w->text_section);
    buffer_init(&w->data_section);
    buffer_init(&w->string_table);
    
    // String table starts with 4-byte size field
    buffer_write_dword(&w->string_table, 4);
    
    w->symbols_count = 0;
    w->symbols_capacity = 16;
    w->symbols = malloc(w->symbols_capacity * sizeof(SymbolEntry));
    
    w->text_relocs_count = 0;
    w->text_relocs_capacity = 16;
    w->text_relocs = malloc(w->text_relocs_capacity * sizeof(RelocEntry));
    
    w->data_relocs_count = 0;
    w->data_relocs_capacity = 16;
    w->data_relocs = malloc(w->data_relocs_capacity * sizeof(RelocEntry));

    w->debugs_relocs_count = 0;
    w->debugs_relocs_capacity = 16;
    w->debugs_relocs = malloc(w->debugs_relocs_capacity * sizeof(RelocEntry));

    w->debug_source_file = NULL;
    w->debug_comp_dir = NULL;
    w->debug_lines = NULL;
    w->debug_line_count = 0;
    w->debug_line_capacity = 0;
    w->debug_funcs = NULL;
    w->debug_func_count = 0;
    w->debug_func_capacity = 0;
}

void coff_writer_free(COFFWriter *w) {
    buffer_free(&w->text_section);
    buffer_free(&w->data_section);
    buffer_free(&w->string_table);
    
    if (w->symbols) {
        for (size_t i = 0; i < w->symbols_count; i++) {
            free(w->symbols[i].name);
        }
        free(w->symbols);
    }
    free(w->text_relocs);
    free(w->data_relocs);
    free(w->debugs_relocs);
    free(w->debug_source_file);
    free(w->debug_comp_dir);
    free(w->debug_lines);
    if (w->debug_funcs) {
        for (size_t fi = 0; fi < w->debug_func_count; fi++) {
            free(w->debug_funcs[fi].name);
            for (size_t vi = 0; vi < w->debug_funcs[fi].var_count; vi++) {
                free(w->debug_funcs[fi].vars[vi].name);
                free(w->debug_funcs[fi].vars[vi].type_name);
            }
            free(w->debug_funcs[fi].vars);
        }
        free(w->debug_funcs);
    }
}

int32_t coff_writer_find_symbol(COFFWriter *w, const char *name) {
    for (size_t i = 0; i < w->symbols_count; i++) {
        if (strcmp(w->symbols[i].name, name) == 0) {
            return (int32_t)i;
        }
    }
    return -1;
}

uint32_t coff_writer_add_symbol(COFFWriter *w, const char *name, uint32_t value, int16_t section, uint16_t type, uint8_t storage_class) {
    int32_t existing = coff_writer_find_symbol(w, name);
    if (existing >= 0) {
        if (w->symbols[existing].section == 0 && section != 0) {
            w->symbols[existing].value = value;
            w->symbols[existing].section = section;
            w->symbols[existing].type = type;
            w->symbols[existing].storage_class = storage_class;
        }
        return (uint32_t)existing;
    }

    if (w->symbols_count >= w->symbols_capacity) {
        w->symbols_capacity *= 2;
        w->symbols = realloc(w->symbols, w->symbols_capacity * sizeof(SymbolEntry));
    }
    
    uint32_t index = (uint32_t)w->symbols_count++;
    w->symbols[index].name = strdup(name);
    w->symbols[index].value = value;
    w->symbols[index].section = section;
    w->symbols[index].type = type;
    w->symbols[index].storage_class = storage_class;
    
    return index;
}

void coff_writer_add_reloc(COFFWriter *w, uint32_t virtual_address, uint32_t symbol_index, uint16_t type, int section) {
    if (section == 1) {
        if (w->text_relocs_count >= w->text_relocs_capacity) {
            w->text_relocs_capacity *= 2;
            w->text_relocs = realloc(w->text_relocs, w->text_relocs_capacity * sizeof(RelocEntry));
        }
        w->text_relocs[w->text_relocs_count].virtual_address = virtual_address;
        w->text_relocs[w->text_relocs_count].symbol_index = symbol_index;
        w->text_relocs[w->text_relocs_count].type = type;
        w->text_relocs_count++;
    } else {
        if (w->data_relocs_count >= w->data_relocs_capacity) {
            w->data_relocs_capacity *= 2;
            w->data_relocs = realloc(w->data_relocs, w->data_relocs_capacity * sizeof(RelocEntry));
        }
        w->data_relocs[w->data_relocs_count].virtual_address = virtual_address;
        w->data_relocs[w->data_relocs_count].symbol_index = symbol_index;
        w->data_relocs[w->data_relocs_count].type = type;
        w->data_relocs_count++;
    }
}

/* Add a relocation entry for the .debug$S section */
static void debugs_add_reloc(COFFWriter *w, uint32_t virtual_address,
                              uint32_t symbol_index, uint16_t type) {
    if (w->debugs_relocs_count >= w->debugs_relocs_capacity) {
        w->debugs_relocs_capacity *= 2;
        w->debugs_relocs = realloc(w->debugs_relocs, w->debugs_relocs_capacity * sizeof(RelocEntry));
    }
    w->debugs_relocs[w->debugs_relocs_count].virtual_address = virtual_address;
    w->debugs_relocs[w->debugs_relocs_count].symbol_index = symbol_index;
    w->debugs_relocs[w->debugs_relocs_count].type = type;
    w->debugs_relocs_count++;
}

void coff_writer_set_debug_source(COFFWriter *w, const char *filename, const char *comp_dir) {
    free(w->debug_source_file);
    free(w->debug_comp_dir);
    w->debug_source_file = strdup(filename);
    w->debug_comp_dir = comp_dir ? strdup(comp_dir) : NULL;
}

void coff_writer_add_debug_line(COFFWriter *w, uint32_t address, uint32_t line, uint8_t is_stmt) {
    if (w->debug_line_count >= w->debug_line_capacity) {
        w->debug_line_capacity = w->debug_line_capacity ? w->debug_line_capacity * 2 : 256;
        w->debug_lines = realloc(w->debug_lines, w->debug_line_capacity * sizeof(DebugLineEntry));
    }
    DebugLineEntry *e = &w->debug_lines[w->debug_line_count++];
    e->address = address;
    e->line = line;
    e->is_stmt = is_stmt;
    e->end_seq = 0;
}

void coff_writer_begin_debug_func(COFFWriter *w, const char *name, uint32_t start_addr,
                                   uint8_t ret_type_kind, int32_t ret_type_size) {
    if (w->debug_func_count >= w->debug_func_capacity) {
        w->debug_func_capacity = w->debug_func_capacity ? w->debug_func_capacity * 2 : 16;
        w->debug_funcs = realloc(w->debug_funcs, w->debug_func_capacity * sizeof(DebugFuncEntry));
    }
    DebugFuncEntry *fn = &w->debug_funcs[w->debug_func_count++];
    fn->name = strdup(name);
    fn->start_addr = start_addr;
    fn->end_addr = start_addr; /* updated by end_debug_func */
    fn->ret_type_kind = ret_type_kind;
    fn->ret_type_size = ret_type_size;
    fn->vars = NULL;
    fn->var_count = 0;
    fn->var_capacity = 0;
}

void coff_writer_end_debug_func(COFFWriter *w, uint32_t end_addr) {
    if (w->debug_func_count == 0) return;
    w->debug_funcs[w->debug_func_count - 1].end_addr = end_addr;
}

void coff_writer_add_debug_var(COFFWriter *w, const char *name, int32_t rbp_offset,
                                uint8_t is_param, uint8_t type_kind, int32_t type_size,
                                const char *type_name) {
    if (w->debug_func_count == 0) return;
    DebugFuncEntry *fn = &w->debug_funcs[w->debug_func_count - 1];
    if (fn->var_count >= fn->var_capacity) {
        fn->var_capacity = fn->var_capacity ? fn->var_capacity * 2 : 16;
        fn->vars = realloc(fn->vars, fn->var_capacity * sizeof(DebugVarEntry));
    }
    DebugVarEntry *v = &fn->vars[fn->var_count++];
    v->name = strdup(name);
    v->rbp_offset = rbp_offset;
    v->is_param = is_param;
    v->type_kind = type_kind;
    v->type_size = type_size;
    v->type_name = type_name ? strdup(type_name) : NULL;
}

/* ===================================================================
 * CodeView Debug Section Generation
 * =================================================================== */

/* Map a DebugTypeKind + size to a CodeView basic type index */
static uint32_t cv_type_index(uint8_t kind, int32_t size) {
    switch (kind) {
        case DBG_TYPE_VOID:     return T_VOID;
        case DBG_TYPE_CHAR:     return T_CHAR;
        case DBG_TYPE_UCHAR:    return T_UCHAR;
        case DBG_TYPE_SHORT:    return T_SHORT;
        case DBG_TYPE_USHORT:   return T_USHORT;
        case DBG_TYPE_INT:      return (size == 8) ? T_QUAD : T_INT4;
        case DBG_TYPE_UINT:     return (size == 8) ? T_UQUAD : T_UINT4;
        case DBG_TYPE_LONG:     return T_QUAD;
        case DBG_TYPE_ULONG:    return T_UQUAD;
        case DBG_TYPE_LONGLONG: return T_QUAD;
        case DBG_TYPE_ULONGLONG:return T_UQUAD;
        case DBG_TYPE_FLOAT:    return T_REAL32;
        case DBG_TYPE_DOUBLE:   return T_REAL64;
        case DBG_TYPE_PTR:      return T_64PVOID;
        default:                return T_INT4;
    }
}

/* Write a 4-byte-aligned padding to a buffer */
static void cv_pad_align4(Buffer *buf) {
    while (buf->size & 3) buffer_write_byte(buf, 0);
}

/*
 * Build the .debug$T section (type information).
 * For each function, emit an LF_ARGLIST + LF_PROCEDURE record.
 * Returns the buffer; caller must free.
 * On output, proc_type_indices[i] = type index for function i.
 */
static void build_debug_t(COFFWriter *w, Buffer *debug_t, uint32_t **proc_type_indices_out) {
    buffer_init(debug_t);
    buffer_write_dword(debug_t, CV_SIGNATURE_C13);

    uint32_t next_type_index = 0x1000; /* user-defined types start here */
    uint32_t *proc_types = malloc(w->debug_func_count * sizeof(uint32_t));

    for (size_t fi = 0; fi < w->debug_func_count; fi++) {
        DebugFuncEntry *fn = &w->debug_funcs[fi];

        /* Count parameters */
        uint32_t param_count = 0;
        for (size_t vi = 0; vi < fn->var_count; vi++) {
            if (fn->vars[vi].is_param) param_count++;
        }

        /* --- LF_ARGLIST record ---
         * uint16_t length (excluding this field)
         * uint16_t leaf = LF_ARGLIST
         * uint32_t count
         * uint32_t arg_type[count]
         */
        uint16_t arglist_data_len = (uint16_t)(2 + 4 + param_count * 4); /* leaf + count + types */
        buffer_write_word(debug_t, arglist_data_len);
        buffer_write_word(debug_t, LF_ARGLIST);
        buffer_write_dword(debug_t, param_count);
        for (size_t vi = 0; vi < fn->var_count; vi++) {
            if (fn->vars[vi].is_param) {
                buffer_write_dword(debug_t, cv_type_index(fn->vars[vi].type_kind,
                                                           fn->vars[vi].type_size));
            }
        }
        cv_pad_align4(debug_t);
        uint32_t arglist_idx = next_type_index++;

        /* --- LF_PROCEDURE record ---
         * uint16_t length
         * uint16_t leaf = LF_PROCEDURE
         * uint32_t return_type
         * uint8_t  calling_convention (0 = near C)
         * uint8_t  func_attributes
         * uint16_t param_count
         * uint32_t arglist_type_index
         */
        uint16_t proc_data_len = 2 + 4 + 1 + 1 + 2 + 4; /* 14 bytes */
        buffer_write_word(debug_t, proc_data_len);
        buffer_write_word(debug_t, LF_PROCEDURE);
        buffer_write_dword(debug_t, cv_type_index(fn->ret_type_kind, fn->ret_type_size));
        buffer_write_byte(debug_t, 0);  /* CC_NEAR_C */
        buffer_write_byte(debug_t, 0);  /* no attributes */
        buffer_write_word(debug_t, (uint16_t)param_count);
        buffer_write_dword(debug_t, arglist_idx);
        cv_pad_align4(debug_t);

        proc_types[fi] = next_type_index++;
    }

    *proc_type_indices_out = proc_types;
}

/*
 * Build the .debug$S section (symbol/line/checksum information).
 * text_sym_index: symbol table index of the .text section symbol
 *                 (used for SECREL/SECTION relocations)
 */
static void build_debug_s(COFFWriter *w, Buffer *debug_s,
                           uint32_t text_sym_index,
                           uint32_t *proc_type_indices) {
    buffer_init(debug_s);
    buffer_write_dword(debug_s, CV_SIGNATURE_C13);

    const char *src_file = w->debug_source_file ? w->debug_source_file : "unknown.c";

    /* ==== 1. DEBUG_S_STRINGTABLE subsection ==== */
    /* String table: offset 0 = empty, then the source filename */
    {
        uint32_t strtab_start = (uint32_t)debug_s->size;
        buffer_write_dword(debug_s, DEBUG_S_STRINGTABLE);
        uint32_t len_pos = (uint32_t)debug_s->size;
        buffer_write_dword(debug_s, 0); /* placeholder for length */

        uint32_t sub_start = (uint32_t)debug_s->size;
        buffer_write_byte(debug_s, 0); /* empty string at offset 0 */
        /* Source filename at offset 1 */
        buffer_write_bytes(debug_s, src_file, strlen(src_file) + 1);
        uint32_t sub_len = (uint32_t)(debug_s->size - sub_start);
        /* Patch length */
        memcpy(debug_s->data + len_pos, &sub_len, 4);
        cv_pad_align4(debug_s);
        (void)strtab_start;
    }

    /* ==== 2. DEBUG_S_FILECHKSMS subsection ==== */
    /* File checksum entry: references string at offset 1 */
    {
        buffer_write_dword(debug_s, DEBUG_S_FILECHKSMS);
        uint32_t len_pos = (uint32_t)debug_s->size;
        buffer_write_dword(debug_s, 0); /* placeholder */

        uint32_t sub_start = (uint32_t)debug_s->size;
        /* FileChecksumEntry:
         *   uint32_t FileNameOffset (index into string table)
         *   uint8_t  ChecksumSize
         *   uint8_t  ChecksumType
         *   [checksum bytes]
         *   Padded to 4-byte boundary (padding IS part of entry/subsection)
         */
        buffer_write_dword(debug_s, 1); /* offset into string table = 1 (after null byte) */
        buffer_write_byte(debug_s, 0);  /* checksum size = 0 (no checksum) */
        buffer_write_byte(debug_s, CHKSUM_TYPE_NONE);
        /* Pad individual entry to 4-byte boundary (included in subsection length) */
        cv_pad_align4(debug_s);
        uint32_t sub_len = (uint32_t)(debug_s->size - sub_start);
        memcpy(debug_s->data + len_pos, &sub_len, 4);
        /* Subsection already aligned after entry padding */
    }

    /* ==== 3. DEBUG_S_SYMBOLS subsection ==== */
    {
        buffer_write_dword(debug_s, DEBUG_S_SYMBOLS);
        uint32_t len_pos = (uint32_t)debug_s->size;
        buffer_write_dword(debug_s, 0); /* placeholder */
        uint32_t sub_start = (uint32_t)debug_s->size;

        /* --- S_OBJNAME record --- */
        {
            size_t name_len = strlen(src_file) + 1;
            uint16_t rec_len = (uint16_t)(2 + 4 + name_len); /* rectyp + signature + name */
            buffer_write_word(debug_s, rec_len);
            buffer_write_word(debug_s, S_OBJNAME);
            buffer_write_dword(debug_s, 0); /* signature */
            buffer_write_bytes(debug_s, src_file, name_len);
            /* Symbol records within DEBUG_S_SYMBOLS are packed (NOT individually aligned) */
        }

        /* --- S_COMPILE3 record --- */
        {
            const char *ver_str = "Fador's C99 Compiler";
            size_t ver_len = strlen(ver_str) + 1;
            uint16_t rec_len = (uint16_t)(2 + 4 + 2 + 8 * 2 + ver_len);
            /* rectyp(2) + flags(4) + machine(2) + 8 version words(16) + string */
            buffer_write_word(debug_s, rec_len);
            buffer_write_word(debug_s, S_COMPILE3);
            /* flags: language=C (0), no special flags */
            buffer_write_dword(debug_s, CV_CFL_C); /* iLanguage=0 (C), rest=0 */
            buffer_write_word(debug_s, CV_CFL_AMD64); /* machine */
            /* Version numbers: FE major, FE minor, FE build, FE QFE */
            buffer_write_word(debug_s, 1); /* FE major */
            buffer_write_word(debug_s, 0); /* FE minor */
            buffer_write_word(debug_s, 0); /* FE build */
            buffer_write_word(debug_s, 0); /* FE QFE */
            /* BE major, minor, build, QFE */
            buffer_write_word(debug_s, 1); /* major */
            buffer_write_word(debug_s, 0); /* minor */
            buffer_write_word(debug_s, 0); /* build */
            buffer_write_word(debug_s, 0); /* QFE */
            buffer_write_bytes(debug_s, ver_str, ver_len);
        }

        /* --- Function symbols: S_GPROC32 + S_REGREL32 + S_FRAMEPROC + S_END --- */
        for (size_t fi = 0; fi < w->debug_func_count; fi++) {
            DebugFuncEntry *fn = &w->debug_funcs[fi];
            uint32_t func_len = fn->end_addr - fn->start_addr;
            uint32_t proc_type = proc_type_indices ? proc_type_indices[fi] : T_NOTYPE;
            size_t name_len = strlen(fn->name) + 1;

            /* S_GPROC32:
             *   reclen(2) rectyp(2) parent(4) end(4) next(4)
             *   len(4) dbgstart(4) dbgend(4) typind(4)
             *   offset(4) segment(2) flags(1) name(NUL)
             */
            uint16_t gproc_reclen = (uint16_t)(2 + 4+4+4 + 4+4+4+4 + 4+2+1 + name_len);
            buffer_write_word(debug_s, gproc_reclen);
            buffer_write_word(debug_s, S_GPROC32);
            buffer_write_dword(debug_s, 0); /* parent */
            buffer_write_dword(debug_s, 0); /* end (placeholder, patched later or left 0) */
            buffer_write_dword(debug_s, 0); /* next */
            buffer_write_dword(debug_s, func_len); /* procedure length */
            buffer_write_dword(debug_s, 0); /* debug start offset (after prologue) */
            buffer_write_dword(debug_s, func_len); /* debug end offset */
            buffer_write_dword(debug_s, proc_type); /* type index */

            /* Offset field — needs IMAGE_REL_AMD64_SECREL relocation */
            uint32_t offset_pos = (uint32_t)debug_s->size;
            buffer_write_dword(debug_s, fn->start_addr);
            debugs_add_reloc(w, offset_pos - 4 /* relative to section start (after CV sig) */,
                              text_sym_index, IMAGE_REL_AMD64_SECREL);
            /* Wait — the virtual_address in relocation is relative to section raw data start,
             * but the CV signature (4 bytes) is part of the section data. So the reloc VA
             * is just the byte position within the section data. */
            /* Actually, let me fix this: relocation VA is offset from start of section raw data */
            w->debugs_relocs[w->debugs_relocs_count - 1].virtual_address = offset_pos;

            /* Segment field — needs IMAGE_REL_AMD64_SECTION relocation */
            uint32_t seg_pos = (uint32_t)debug_s->size;
            buffer_write_word(debug_s, 0); /* segment (filled by linker) */
            debugs_add_reloc(w, seg_pos, text_sym_index, IMAGE_REL_AMD64_SECTION);

            buffer_write_byte(debug_s, CV_PFLAG_NONE); /* flags */
            buffer_write_bytes(debug_s, fn->name, name_len);

            /* S_FRAMEPROC (required by debuggers after S_GPROC32):
             *   reclen(2) rectyp(2) cbFrame(4) cbPad(4) offPad(4)
             *   cbSaveRegs(4) offExHdlr(4) secExHdlr(2) flags(4)
             */
            {
                uint16_t fp_reclen = 2 + 4+4+4+4+4+2+4; /* 28 */
                buffer_write_word(debug_s, fp_reclen);
                buffer_write_word(debug_s, S_FRAMEPROC);
                /* Estimate frame size from stack variables */
                uint32_t frame_size = 0;
                for (size_t vi = 0; vi < fn->var_count; vi++) {
                    if (fn->vars[vi].rbp_offset < 0) {
                        uint32_t abs_off = (uint32_t)(-fn->vars[vi].rbp_offset);
                        if (abs_off > frame_size) frame_size = abs_off;
                    }
                }
                buffer_write_dword(debug_s, frame_size);  /* cbFrame */
                buffer_write_dword(debug_s, 0);  /* cbPad */
                buffer_write_dword(debug_s, 0);  /* offPad */
                buffer_write_dword(debug_s, 0);  /* cbSaveRegs */
                buffer_write_dword(debug_s, 0);  /* offExHdlr */
                buffer_write_word(debug_s, 0);   /* secExHdlr */
                /* EncodedLocalBasePointer (bits 14-15): 2 = FP (RBP)
                 * EncodedParamBasePointer (bits 16-17): 2 = FP (RBP) */
                buffer_write_dword(debug_s, (2u << 14) | (2u << 16));
            }

            /* S_REGREL32 for each variable/parameter */
            for (size_t vi = 0; vi < fn->var_count; vi++) {
                DebugVarEntry *v = &fn->vars[vi];
                size_t vname_len = strlen(v->name) + 1;
                uint32_t type_idx = cv_type_index(v->type_kind, v->type_size);

                /* S_REGREL32:
                 *   reclen(2) rectyp(2) offset(4) typind(4) reg(2) name(NUL)
                 */
                uint16_t reg_reclen = (uint16_t)(2 + 4 + 4 + 2 + vname_len);
                buffer_write_word(debug_s, reg_reclen);
                buffer_write_word(debug_s, S_REGREL32);
                /* offset from RBP (signed, written as uint32_t) */
                {
                    uint32_t off_u;
                    memcpy(&off_u, &v->rbp_offset, 4);
                    buffer_write_dword(debug_s, off_u);
                }
                buffer_write_dword(debug_s, type_idx);
                buffer_write_word(debug_s, CV_AMD64_RBP); /* register = RBP */
                buffer_write_bytes(debug_s, v->name, vname_len);
            }

            /* S_END */
            buffer_write_word(debug_s, 2); /* reclen */
            buffer_write_word(debug_s, S_END);
        }

        uint32_t sub_len = (uint32_t)(debug_s->size - sub_start);
        memcpy(debug_s->data + len_pos, &sub_len, 4);
        cv_pad_align4(debug_s);
    }

    /* ==== 4. DEBUG_S_LINES subsection ==== */
    if (w->debug_line_count > 0) {
        buffer_write_dword(debug_s, DEBUG_S_LINES);
        uint32_t len_pos = (uint32_t)debug_s->size;
        buffer_write_dword(debug_s, 0); /* placeholder */
        uint32_t sub_start = (uint32_t)debug_s->size;

        /* Lines header:
         *   offCon(4) segCon(2) flags(2) cbCon(4)
         */
        uint32_t offcon_pos = (uint32_t)debug_s->size;
        buffer_write_dword(debug_s, 0); /* offCon — needs SECREL reloc */
        debugs_add_reloc(w, offcon_pos, text_sym_index, IMAGE_REL_AMD64_SECREL);

        uint32_t segcon_pos = (uint32_t)debug_s->size;
        buffer_write_word(debug_s, 0); /* segCon — needs SECTION reloc */
        debugs_add_reloc(w, segcon_pos, text_sym_index, IMAGE_REL_AMD64_SECTION);

        buffer_write_word(debug_s, 0); /* flags (no columns) */

        /* cbCon: total code size */
        uint32_t code_size = (uint32_t)w->text_section.size;
        buffer_write_dword(debug_s, code_size);

        /* File block:
         *   offFile(4) nLines(4) cbBlock(4)
         *   followed by CV_Line_t entries
         */
        uint32_t nlines = (uint32_t)w->debug_line_count;
        uint32_t block_size = 12 + nlines * 8; /* header(12) + lines(8 each) */
        buffer_write_dword(debug_s, 0); /* offFile = 0 (offset into FILECHKSMS) */
        buffer_write_dword(debug_s, nlines);
        buffer_write_dword(debug_s, block_size);

        /* CV_Line_t entries:
         *   offset(4)  lineinfo(4)
         * lineinfo: linenumStart:24, deltaLineEnd:7, fStatement:1
         */
        for (size_t li = 0; li < w->debug_line_count; li++) {
            buffer_write_dword(debug_s, w->debug_lines[li].address);
            /* Pack line number: bits 0-23 = line, bit 31 = statement flag */
            uint32_t lineinfo = (w->debug_lines[li].line & 0x00FFFFFF)
                              | (w->debug_lines[li].is_stmt ? 0x80000000u : 0);
            buffer_write_dword(debug_s, lineinfo);
        }

        uint32_t sub_len = (uint32_t)(debug_s->size - sub_start);
        memcpy(debug_s->data + len_pos, &sub_len, 4);
        cv_pad_align4(debug_s);
    }
}

void coff_writer_write(COFFWriter *w, const char *filename) {
    FILE *f = fopen(filename, "wb");
    if (!f) return;

    /* Determine if we have debug info to emit */
    int has_debug = (g_compiler_options.debug_info &&
                     w->debug_source_file &&
                     (w->debug_line_count > 0 || w->debug_func_count > 0));

    /* Build debug section buffers if needed */
    Buffer debug_s_buf, debug_t_buf;
    uint32_t *proc_type_indices = NULL;
    buffer_init(&debug_s_buf);
    buffer_init(&debug_t_buf);

    /*
     * When debug info is present, we prepend section symbols (with aux records)
     * to the symbol table. Each section symbol occupies 2 slots (symbol + aux).
     * This shifts all existing symbol indices. We need to adjust relocations.
     *
     * Section symbols added:
     *   - .text section symbol (always, if .text exists)
     *   - .data section symbol (if .data exists)
     */
    int has_text = (w->text_section.size > 0);
    int has_data = (w->data_section.size > 0);

    /* Number of extra symbol-table entries prepended (each section sym = 2 entries) */
    uint32_t sym_shift = 0;
    uint32_t text_sym_slot = 0;  /* symbol index of .text section symbol */
    uint32_t data_sym_slot = 0;  /* symbol index of .data section symbol */

    if (has_debug) {
        if (has_text) {
            text_sym_slot = sym_shift;
            sym_shift += 2; /* symbol + 1 aux */
        }
        if (has_data) {
            data_sym_slot = sym_shift;
            sym_shift += 2;
        }

        /* Adjust existing relocation symbol indices */
        for (size_t i = 0; i < w->text_relocs_count; i++)
            w->text_relocs[i].symbol_index += sym_shift;
        for (size_t i = 0; i < w->data_relocs_count; i++)
            w->data_relocs[i].symbol_index += sym_shift;

        /* Build CodeView sections.
         * build_debug_s uses text_sym_slot for its relocations (which go into
         * w->debugs_relocs). These symbol indices are already correct because
         * the section symbols are at the start of the table. */
        build_debug_t(w, &debug_t_buf, &proc_type_indices);
        build_debug_s(w, &debug_s_buf, text_sym_slot, proc_type_indices);
    }

    /* --- Compute section count and numbers --- */
    COFFHeader header = {0};
    header.Machine = IMAGE_FILE_MACHINE_AMD64;
    header.NumberOfSections = 0;

    int16_t text_sec_num = 0, data_sec_num = 0;
    int16_t debugs_sec_num = 0, debugt_sec_num = 0;

    if (has_text) { header.NumberOfSections++; text_sec_num = header.NumberOfSections; }
    if (has_data) { header.NumberOfSections++; data_sec_num = header.NumberOfSections; }
    if (has_debug && debug_s_buf.size > 0) { header.NumberOfSections++; debugs_sec_num = header.NumberOfSections; }
    if (has_debug && debug_t_buf.size > 0) { header.NumberOfSections++; debugt_sec_num = header.NumberOfSections; }

    header.TimeDateStamp = (uint32_t)time(NULL);
    header.SizeOfOptionalHeader = 0;
    header.Characteristics = 0;

    /* --- Compute file layout --- */
    uint32_t section_headers_pos = sizeof(COFFHeader);
    uint32_t current_data_pos = section_headers_pos +
        sizeof(COFFSectionHeader) * (uint32_t)header.NumberOfSections;

    uint32_t text_data_pos = 0, text_relocs_pos = 0;
    uint32_t data_data_pos = 0, data_relocs_pos = 0;
    uint32_t debugs_data_pos = 0, debugs_relocs_pos = 0;
    uint32_t debugt_data_pos = 0;

    if (has_text) {
        text_data_pos = current_data_pos;
        current_data_pos += (uint32_t)w->text_section.size;
        if (w->text_relocs_count > 0) {
            text_relocs_pos = current_data_pos;
            current_data_pos += (uint32_t)w->text_relocs_count * sizeof(COFFRelocation);
        }
    }

    if (has_data) {
        data_data_pos = current_data_pos;
        current_data_pos += (uint32_t)w->data_section.size;
        if (w->data_relocs_count > 0) {
            data_relocs_pos = current_data_pos;
            current_data_pos += (uint32_t)w->data_relocs_count * sizeof(COFFRelocation);
        }
    }

    if (has_debug && debug_s_buf.size > 0) {
        debugs_data_pos = current_data_pos;
        current_data_pos += (uint32_t)debug_s_buf.size;
        if (w->debugs_relocs_count > 0) {
            debugs_relocs_pos = current_data_pos;
            current_data_pos += (uint32_t)w->debugs_relocs_count * sizeof(COFFRelocation);
        }
    }

    if (has_debug && debug_t_buf.size > 0) {
        debugt_data_pos = current_data_pos;
        current_data_pos += (uint32_t)debug_t_buf.size;
    }

    /* Total symbol count: section symbols + existing symbols */
    header.PointerToSymbolTable = current_data_pos;
    header.NumberOfSymbols = (uint32_t)(sym_shift + w->symbols_count);

    /* === Write COFF Header === */
    fwrite(&header, sizeof(header), 1, f);

    /* === Write Section Headers === */
    if (has_text) {
        COFFSectionHeader text_sh = {0};
        memcpy(text_sh.Name, ".text\0\0\0", 8);
        text_sh.SizeOfRawData = (uint32_t)w->text_section.size;
        text_sh.PointerToRawData = text_data_pos;
        text_sh.PointerToRelocations = text_relocs_pos;
        text_sh.NumberOfRelocations = (uint16_t)w->text_relocs_count;
        text_sh.Characteristics = IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE |
                                  IMAGE_SCN_MEM_READ | IMAGE_SCN_ALIGN_16BYTES;
        fwrite(&text_sh, sizeof(text_sh), 1, f);
    }

    if (has_data) {
        COFFSectionHeader data_sh = {0};
        memcpy(data_sh.Name, ".data\0\0\0", 8);
        data_sh.SizeOfRawData = (uint32_t)w->data_section.size;
        data_sh.PointerToRawData = data_data_pos;
        data_sh.PointerToRelocations = data_relocs_pos;
        data_sh.NumberOfRelocations = (uint16_t)w->data_relocs_count;
        data_sh.Characteristics = IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ |
                                  IMAGE_SCN_MEM_WRITE | IMAGE_SCN_ALIGN_4BYTES;
        fwrite(&data_sh, sizeof(data_sh), 1, f);
    }

    if (has_debug && debug_s_buf.size > 0) {
        COFFSectionHeader debugs_sh = {0};
        /* ".debug$S" is exactly 8 chars */
        memcpy(debugs_sh.Name, ".debug$S", 8);
        debugs_sh.SizeOfRawData = (uint32_t)debug_s_buf.size;
        debugs_sh.PointerToRawData = debugs_data_pos;
        debugs_sh.PointerToRelocations = debugs_relocs_pos;
        debugs_sh.NumberOfRelocations = (uint16_t)w->debugs_relocs_count;
        debugs_sh.Characteristics = IMAGE_SCN_CNT_INITIALIZED_DATA |
                                    IMAGE_SCN_MEM_READ |
                                    IMAGE_SCN_MEM_DISCARDABLE |
                                    IMAGE_SCN_ALIGN_1BYTES;
        fwrite(&debugs_sh, sizeof(debugs_sh), 1, f);
    }

    if (has_debug && debug_t_buf.size > 0) {
        COFFSectionHeader debugt_sh = {0};
        memcpy(debugt_sh.Name, ".debug$T", 8);
        debugt_sh.SizeOfRawData = (uint32_t)debug_t_buf.size;
        debugt_sh.PointerToRawData = debugt_data_pos;
        debugt_sh.PointerToRelocations = 0;
        debugt_sh.NumberOfRelocations = 0;
        debugt_sh.Characteristics = IMAGE_SCN_CNT_INITIALIZED_DATA |
                                    IMAGE_SCN_MEM_READ |
                                    IMAGE_SCN_MEM_DISCARDABLE |
                                    IMAGE_SCN_ALIGN_1BYTES;
        fwrite(&debugt_sh, sizeof(debugt_sh), 1, f);
    }

    /* === Write Section Raw Data + Relocations === */

    /* .text */
    if (has_text) {
        fwrite(w->text_section.data, 1, w->text_section.size, f);
        for (size_t i = 0; i < w->text_relocs_count; i++) {
            COFFRelocation r;
            r.VirtualAddress = w->text_relocs[i].virtual_address;
            r.SymbolTableIndex = w->text_relocs[i].symbol_index;
            r.Type = w->text_relocs[i].type;
            fwrite(&r, sizeof(r), 1, f);
        }
    }

    /* .data */
    if (has_data) {
        fwrite(w->data_section.data, 1, w->data_section.size, f);
        for (size_t i = 0; i < w->data_relocs_count; i++) {
            COFFRelocation r;
            r.VirtualAddress = w->data_relocs[i].virtual_address;
            r.SymbolTableIndex = w->data_relocs[i].symbol_index;
            r.Type = w->data_relocs[i].type;
            fwrite(&r, sizeof(r), 1, f);
        }
    }

    /* .debug$S */
    if (has_debug && debug_s_buf.size > 0) {
        fwrite(debug_s_buf.data, 1, debug_s_buf.size, f);
        for (size_t i = 0; i < w->debugs_relocs_count; i++) {
            COFFRelocation r;
            r.VirtualAddress = w->debugs_relocs[i].virtual_address;
            r.SymbolTableIndex = w->debugs_relocs[i].symbol_index;
            r.Type = w->debugs_relocs[i].type;
            fwrite(&r, sizeof(r), 1, f);
        }
    }

    /* .debug$T */
    if (has_debug && debug_t_buf.size > 0) {
        fwrite(debug_t_buf.data, 1, debug_t_buf.size, f);
    }

    /* === Write Symbol Table === */

    /* First: section symbols with auxiliary records (if debug) */
    if (has_debug) {
        /* .text section symbol */
        if (has_text) {
            COFFSymbol sym = {0};
            memcpy(sym.N.ShortName, ".text\0\0\0", 8);
            sym.Value = 0;
            sym.SectionNumber = text_sec_num;
            sym.Type = 0;
            sym.StorageClass = IMAGE_SYM_CLASS_STATIC;
            sym.NumberOfAuxSymbols = 1;
            fwrite(&sym, sizeof(sym), 1, f);

            /* Auxiliary record for section symbol (18 bytes) */
            uint8_t aux[18];
            memset(aux, 0, sizeof(aux));
            /* Length (4 bytes) */
            uint32_t sec_len = (uint32_t)w->text_section.size;
            memcpy(aux, &sec_len, 4);
            /* NumberOfRelocations (2 bytes) */
            uint16_t nrelocs = (uint16_t)w->text_relocs_count;
            memcpy(aux + 4, &nrelocs, 2);
            /* NumberOfLinenumbers (2 bytes) = 0 */
            /* Checksum (4 bytes) = 0 */
            /* Number (2 bytes) = 0 */
            /* Selection (1 byte) = 0 */
            fwrite(aux, sizeof(aux), 1, f);
        }

        /* .data section symbol */
        if (has_data) {
            COFFSymbol sym = {0};
            memcpy(sym.N.ShortName, ".data\0\0\0", 8);
            sym.Value = 0;
            sym.SectionNumber = data_sec_num;
            sym.Type = 0;
            sym.StorageClass = IMAGE_SYM_CLASS_STATIC;
            sym.NumberOfAuxSymbols = 1;
            fwrite(&sym, sizeof(sym), 1, f);

            uint8_t aux[18];
            memset(aux, 0, sizeof(aux));
            uint32_t sec_len = (uint32_t)w->data_section.size;
            memcpy(aux, &sec_len, 4);
            uint16_t nrelocs = (uint16_t)w->data_relocs_count;
            memcpy(aux + 4, &nrelocs, 2);
            fwrite(aux, sizeof(aux), 1, f);
        }
    }

    /* Then: existing symbols */
    for (size_t i = 0; i < w->symbols_count; i++) {
        COFFSymbol sym = {0};
        if (strlen(w->symbols[i].name) <= 8) {
            memcpy(sym.N.ShortName, w->symbols[i].name, strlen(w->symbols[i].name));
        } else {
            sym.N.Name.Zeroes = 0;
            sym.N.Name.Offset = (uint32_t)w->string_table.size;
            buffer_write_bytes(&w->string_table, w->symbols[i].name,
                             strlen(w->symbols[i].name) + 1);
            uint32_t total_size = (uint32_t)w->string_table.size;
            memcpy(w->string_table.data, &total_size, 4);
        }
        sym.Value = w->symbols[i].value;
        sym.SectionNumber = w->symbols[i].section;
        sym.Type = w->symbols[i].type;
        sym.StorageClass = w->symbols[i].storage_class;
        sym.NumberOfAuxSymbols = 0;
        fwrite(&sym, sizeof(sym), 1, f);
    }

    /* === Write String Table === */
    fwrite(w->string_table.data, 1, w->string_table.size, f);
    fclose(f);

    /* Cleanup */
    buffer_free(&debug_s_buf);
    buffer_free(&debug_t_buf);
    free(proc_type_indices);

    /* Un-shift relocation indices so the COFFWriter state isn't corrupted
     * (in case write is called again or data is inspected afterwards) */
    if (has_debug && sym_shift > 0) {
        for (size_t i = 0; i < w->text_relocs_count; i++)
            w->text_relocs[i].symbol_index -= sym_shift;
        for (size_t i = 0; i < w->data_relocs_count; i++)
            w->data_relocs[i].symbol_index -= sym_shift;
    }
}
