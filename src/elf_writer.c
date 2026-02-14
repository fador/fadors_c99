#include "elf_writer.h"
#include "elf.h"
#include "coff.h"
#include "buffer.h"
#include "codegen.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/*
 * Write padding zeros until file position reaches 'target'.
 */
static void pad_to(FILE *f, uint64_t target) {
    long current = ftell(f);
    while ((uint64_t)current < target) {
        fputc(0, f);
        current++;
    }
}

/*
 * Align a value up to a given power-of-two alignment.
 */
static uint64_t align_up(uint64_t val, uint64_t alignment) {
    return (val + alignment - 1) & ~(alignment - 1);
}

void elf_writer_write(COFFWriter *w, const char *filename) {
    FILE *f = fopen(filename, "wb");
    if (!f) return;

    /*
     * ELF section layout (fixed indices):
     *   0: null
     *   1: .text
     *   2: .data
     *   3: .note.GNU-stack  (non-executable stack)
     *   4: .symtab
     *   5: .strtab
     *   6: .shstrtab
     *   7: .rela.text  (optional)
     *   8: .rela.data  (optional)
     */
    int text_idx     = 1;
    int data_idx     = 2;
    int gnustack_idx = 3;
    int symtab_idx   = 4;
    int strtab_idx   = 5;
    int shstrtab_idx = 6;

    int num_sections = 7;
    int rela_text_idx = 0;
    int rela_data_idx = 0;
    int fadors_debug_idx = 0;
    if (w->text_relocs_count > 0) {
        rela_text_idx = num_sections++;
    }
    if (w->data_relocs_count > 0) {
        rela_data_idx = num_sections++;
    }
    /* Custom debug section: carries raw line entries through to the linker */
    int has_debug = (g_compiler_options.debug_info && w->debug_line_count > 0
                     && w->debug_source_file);
    if (has_debug) {
        fadors_debug_idx = num_sections++;
    }

    /* ---- Build .shstrtab (section name string table) ---- */
    Buffer shstrtab;
    buffer_init(&shstrtab);
    buffer_write_byte(&shstrtab, 0); /* null string at offset 0 */

    uint32_t name_text = (uint32_t)shstrtab.size;
    buffer_write_bytes(&shstrtab, ".text", 6);

    uint32_t name_data = (uint32_t)shstrtab.size;
    buffer_write_bytes(&shstrtab, ".data", 6);

    uint32_t name_symtab = (uint32_t)shstrtab.size;
    buffer_write_bytes(&shstrtab, ".symtab", 8);

    uint32_t name_strtab = (uint32_t)shstrtab.size;
    buffer_write_bytes(&shstrtab, ".strtab", 8);

    uint32_t name_shstrtab = (uint32_t)shstrtab.size;
    buffer_write_bytes(&shstrtab, ".shstrtab", 10);

    uint32_t name_gnustack = (uint32_t)shstrtab.size;
    buffer_write_bytes(&shstrtab, ".note.GNU-stack", 16);
    (void)gnustack_idx;

    uint32_t name_rela_text = 0;
    if (rela_text_idx > 0) {
        name_rela_text = (uint32_t)shstrtab.size;
        buffer_write_bytes(&shstrtab, ".rela.text", 11);
    }

    uint32_t name_rela_data = 0;
    if (rela_data_idx > 0) {
        name_rela_data = (uint32_t)shstrtab.size;
        buffer_write_bytes(&shstrtab, ".rela.data", 11);
    }

    uint32_t name_fadors_debug = 0;
    Buffer fadors_debug_sec;
    buffer_init(&fadors_debug_sec);
    if (has_debug) {
        name_fadors_debug = (uint32_t)shstrtab.size;
        buffer_write_bytes(&shstrtab, ".fadors_debug", 14);

        /* Build custom debug section:
         *   u32 source_name_len (incl. null)
         *   char[] source_name
         *   u32 comp_dir_len (incl. null)
         *   char[] comp_dir
         *   u32 entry_count
         *   DebugLineEntry[] entries (each: u32 address, u32 line, u8 is_stmt, u8 end_seq)
         */
        uint32_t slen = (uint32_t)(strlen(w->debug_source_file) + 1);
        buffer_write_dword(&fadors_debug_sec, slen);
        buffer_write_bytes(&fadors_debug_sec, w->debug_source_file, slen);

        const char *cdir = w->debug_comp_dir ? w->debug_comp_dir : ".";
        uint32_t clen = (uint32_t)(strlen(cdir) + 1);
        buffer_write_dword(&fadors_debug_sec, clen);
        buffer_write_bytes(&fadors_debug_sec, cdir, clen);

        uint32_t count = (uint32_t)w->debug_line_count;
        buffer_write_dword(&fadors_debug_sec, count);
        for (size_t di = 0; di < w->debug_line_count; di++) {
            buffer_write_dword(&fadors_debug_sec, w->debug_lines[di].address);
            buffer_write_dword(&fadors_debug_sec, w->debug_lines[di].line);
            buffer_write_byte(&fadors_debug_sec, w->debug_lines[di].is_stmt);
            buffer_write_byte(&fadors_debug_sec, w->debug_lines[di].end_seq);
        }
    }

    /* ---- Build .strtab and .symtab ---- */
    /* ELF requires local symbols before global symbols.        */
    /* Symbol index 0 is always the null symbol.                 */
    Buffer strtab;
    buffer_init(&strtab);
    buffer_write_byte(&strtab, 0); /* null string at offset 0 */

    /* Count locals and globals */
    size_t num_locals = 0;
    size_t num_globals = 0;
    size_t i;
    for (i = 0; i < w->symbols_count; i++) {
        if (w->symbols[i].storage_class == IMAGE_SYM_CLASS_STATIC)
            num_locals++;
        else
            num_globals++;
    }

    size_t total_syms = 1 + num_locals + num_globals;
    Elf64_Sym *symtab = calloc(total_syms, sizeof(Elf64_Sym));

    /* Old COFF index -> new ELF index */
    uint32_t *sym_map = malloc(w->symbols_count * sizeof(uint32_t));

    /* Pass 1: local symbols */
    uint32_t elf_idx = 1; /* start after null symbol */
    for (i = 0; i < w->symbols_count; i++) {
        if (w->symbols[i].storage_class != IMAGE_SYM_CLASS_STATIC) continue;
        sym_map[i] = elf_idx;

        uint32_t name_off = (uint32_t)strtab.size;
        buffer_write_bytes(&strtab, w->symbols[i].name, strlen(w->symbols[i].name) + 1);

        symtab[elf_idx].st_name  = name_off;
        symtab[elf_idx].st_info  = ELF64_ST_INFO(ELF_STB_LOCAL, ELF_STT_NOTYPE);
        symtab[elf_idx].st_other = ELF_STV_DEFAULT;

        if (w->symbols[i].section == 1)      symtab[elf_idx].st_shndx = (uint16_t)text_idx;
        else if (w->symbols[i].section == 2) symtab[elf_idx].st_shndx = (uint16_t)data_idx;
        else                                  symtab[elf_idx].st_shndx = ELF_SHN_UNDEF;

        symtab[elf_idx].st_value = w->symbols[i].value;
        symtab[elf_idx].st_size  = 0;
        elf_idx++;
    }

    uint32_t first_global = elf_idx;

    /* Pass 2: global symbols */
    for (i = 0; i < w->symbols_count; i++) {
        if (w->symbols[i].storage_class == IMAGE_SYM_CLASS_STATIC) continue;
        sym_map[i] = elf_idx;

        uint32_t name_off = (uint32_t)strtab.size;
        buffer_write_bytes(&strtab, w->symbols[i].name, strlen(w->symbols[i].name) + 1);

        uint8_t sym_type = ELF_STT_NOTYPE;
        if (w->symbols[i].type == 0x20) sym_type = ELF_STT_FUNC;

        symtab[elf_idx].st_name  = name_off;
        symtab[elf_idx].st_info  = ELF64_ST_INFO(ELF_STB_GLOBAL, sym_type);
        symtab[elf_idx].st_other = ELF_STV_DEFAULT;

        if (w->symbols[i].section == 1)      symtab[elf_idx].st_shndx = (uint16_t)text_idx;
        else if (w->symbols[i].section == 2) symtab[elf_idx].st_shndx = (uint16_t)data_idx;
        else                                  symtab[elf_idx].st_shndx = ELF_SHN_UNDEF;

        symtab[elf_idx].st_value = w->symbols[i].value;
        symtab[elf_idx].st_size  = 0;
        elf_idx++;
    }

    /* ---- Build .rela.text ---- */
    size_t rela_text_byte_size = w->text_relocs_count * sizeof(Elf64_Rela);
    Elf64_Rela *rela_text = NULL;
    if (w->text_relocs_count > 0) {
        rela_text = calloc(w->text_relocs_count, sizeof(Elf64_Rela));
        for (i = 0; i < w->text_relocs_count; i++) {
            rela_text[i].r_offset = w->text_relocs[i].virtual_address;

            uint32_t new_sym = sym_map[w->text_relocs[i].symbol_index];
            uint32_t elf_type;
            int64_t  addend;

            if (w->text_relocs[i].type == IMAGE_REL_AMD64_REL32) {
                /* RIP-relative 32-bit: call, jmp, jcc, lea, mov [rip+disp] */
                elf_type = ELF_R_X86_64_PLT32;
                addend   = -4;
            } else if (w->text_relocs[i].type == IMAGE_REL_AMD64_ADDR64) {
                elf_type = ELF_R_X86_64_64;
                addend   = 0;
            } else {
                elf_type = w->text_relocs[i].type;
                addend   = 0;
            }

            rela_text[i].r_info   = ELF64_R_INFO(new_sym, elf_type);
            rela_text[i].r_addend = addend;
        }
    }

    /* ---- Build .rela.data ---- */
    size_t rela_data_byte_size = w->data_relocs_count * sizeof(Elf64_Rela);
    Elf64_Rela *rela_data = NULL;
    if (w->data_relocs_count > 0) {
        rela_data = calloc(w->data_relocs_count, sizeof(Elf64_Rela));
        for (i = 0; i < w->data_relocs_count; i++) {
            rela_data[i].r_offset = w->data_relocs[i].virtual_address;

            uint32_t new_sym = sym_map[w->data_relocs[i].symbol_index];
            uint32_t elf_type;
            int64_t  addend;

            if (w->data_relocs[i].type == IMAGE_REL_AMD64_ADDR64) {
                elf_type = ELF_R_X86_64_64;
                addend   = 0;
            } else if (w->data_relocs[i].type == IMAGE_REL_AMD64_REL32) {
                elf_type = ELF_R_X86_64_PC32;
                addend   = -4;
            } else {
                elf_type = w->data_relocs[i].type;
                addend   = 0;
            }

            rela_data[i].r_info   = ELF64_R_INFO(new_sym, elf_type);
            rela_data[i].r_addend = addend;
        }
    }

    /* ---- Calculate file offsets ---- */
    /* Layout: ehdr | .text | .data | .rela.text | .rela.data |
     *         .fadors_debug | .symtab | .strtab | .shstrtab |
     *         section headers                                    */
    uint64_t offset = sizeof(Elf64_Ehdr); /* 64 */

    uint64_t text_offset = offset;
    uint64_t text_size   = w->text_section.size;
    offset += text_size;

    offset = align_up(offset, 8);
    uint64_t data_offset = offset;
    uint64_t data_size   = w->data_section.size;
    offset += data_size;

    offset = align_up(offset, 8);
    uint64_t rela_text_offset = offset;
    offset += rela_text_byte_size;

    offset = align_up(offset, 8);
    uint64_t rela_data_offset = offset;
    offset += rela_data_byte_size;

    offset = align_up(offset, 8);
    uint64_t fadors_debug_offset = offset;
    uint64_t fadors_debug_size   = fadors_debug_sec.size;
    offset += fadors_debug_size;

    offset = align_up(offset, 8);
    uint64_t symtab_offset   = offset;
    uint64_t symtab_byte_size = total_syms * sizeof(Elf64_Sym);
    offset += symtab_byte_size;

    uint64_t strtab_offset = offset;
    uint64_t strtab_size   = strtab.size;
    offset += strtab_size;

    uint64_t shstrtab_offset = offset;
    uint64_t shstrtab_size   = shstrtab.size;
    offset += shstrtab_size;

    offset = align_up(offset, 8);
    uint64_t shdr_offset = offset;

    /* ---- Write ELF header ---- */
    Elf64_Ehdr ehdr;
    memset(&ehdr, 0, sizeof(ehdr));
    ehdr.e_ident[0]  = ELF_ELFMAG0;
    ehdr.e_ident[1]  = ELF_ELFMAG1;
    ehdr.e_ident[2]  = ELF_ELFMAG2;
    ehdr.e_ident[3]  = ELF_ELFMAG3;
    ehdr.e_ident[4]  = ELF_ELFCLASS64;
    ehdr.e_ident[5]  = ELF_ELFDATA2LSB;
    ehdr.e_ident[6]  = ELF_EV_CURRENT;
    ehdr.e_ident[7]  = ELF_ELFOSABI_NONE;
    ehdr.e_type      = ELF_ET_REL;
    ehdr.e_machine   = ELF_EM_X86_64;
    ehdr.e_version   = ELF_EV_CURRENT;
    ehdr.e_entry     = 0;
    ehdr.e_phoff     = 0;
    ehdr.e_shoff     = shdr_offset;
    ehdr.e_flags     = 0;
    ehdr.e_ehsize    = sizeof(Elf64_Ehdr);
    ehdr.e_phentsize = 0;
    ehdr.e_phnum     = 0;
    ehdr.e_shentsize = sizeof(Elf64_Shdr);
    ehdr.e_shnum     = (uint16_t)num_sections;
    ehdr.e_shstrndx  = (uint16_t)shstrtab_idx;

    fwrite(&ehdr, sizeof(ehdr), 1, f);

    /* ---- Write section data ---- */
    /* .text */
    if (text_size > 0)
        fwrite(w->text_section.data, 1, text_size, f);

    /* .data */
    pad_to(f, data_offset);
    if (data_size > 0)
        fwrite(w->data_section.data, 1, data_size, f);

    /* .rela.text */
    pad_to(f, rela_text_offset);
    if (rela_text_byte_size > 0)
        fwrite(rela_text, 1, rela_text_byte_size, f);

    /* .rela.data */
    pad_to(f, rela_data_offset);
    if (rela_data_byte_size > 0)
        fwrite(rela_data, 1, rela_data_byte_size, f);

    /* .fadors_debug */
    if (has_debug) {
        pad_to(f, fadors_debug_offset);
        fwrite(fadors_debug_sec.data, 1, fadors_debug_sec.size, f);
    }

    /* .symtab */
    pad_to(f, symtab_offset);
    fwrite(symtab, sizeof(Elf64_Sym), total_syms, f);

    /* .strtab */
    fwrite(strtab.data, 1, strtab.size, f);

    /* .shstrtab */
    fwrite(shstrtab.data, 1, shstrtab.size, f);

    /* ---- Write section headers ---- */
    pad_to(f, shdr_offset);

    /* 0: null */
    {
        Elf64_Shdr sh;
        memset(&sh, 0, sizeof(sh));
        fwrite(&sh, sizeof(sh), 1, f);
    }

    /* 1: .text */
    {
        Elf64_Shdr sh;
        memset(&sh, 0, sizeof(sh));
        sh.sh_name      = name_text;
        sh.sh_type      = ELF_SHT_PROGBITS;
        sh.sh_flags     = ELF_SHF_ALLOC | ELF_SHF_EXECINSTR;
        sh.sh_offset    = text_offset;
        sh.sh_size      = text_size;
        sh.sh_addralign = 16;
        fwrite(&sh, sizeof(sh), 1, f);
    }

    /* 2: .data */
    {
        Elf64_Shdr sh;
        memset(&sh, 0, sizeof(sh));
        sh.sh_name      = name_data;
        sh.sh_type      = ELF_SHT_PROGBITS;
        sh.sh_flags     = ELF_SHF_ALLOC | ELF_SHF_WRITE;
        sh.sh_offset    = data_offset;
        sh.sh_size      = data_size;
        sh.sh_addralign = 8;
        fwrite(&sh, sizeof(sh), 1, f);
    }

    /* 3: .note.GNU-stack (marks stack as non-executable) */
    {
        Elf64_Shdr sh;
        memset(&sh, 0, sizeof(sh));
        sh.sh_name      = name_gnustack;
        sh.sh_type      = ELF_SHT_PROGBITS;
        sh.sh_flags     = 0; /* no SHF_EXECINSTR = non-executable stack */
        sh.sh_addralign = 1;
        fwrite(&sh, sizeof(sh), 1, f);
    }

    /* 4: .symtab */
    {
        Elf64_Shdr sh;
        memset(&sh, 0, sizeof(sh));
        sh.sh_name      = name_symtab;
        sh.sh_type      = ELF_SHT_SYMTAB;
        sh.sh_offset    = symtab_offset;
        sh.sh_size      = symtab_byte_size;
        sh.sh_link      = (uint32_t)strtab_idx;
        sh.sh_info      = first_global; /* index of first non-local symbol */
        sh.sh_addralign = 8;
        sh.sh_entsize   = sizeof(Elf64_Sym);
        fwrite(&sh, sizeof(sh), 1, f);
    }

    /* 5: .strtab */
    {
        Elf64_Shdr sh;
        memset(&sh, 0, sizeof(sh));
        sh.sh_name      = name_strtab;
        sh.sh_type      = ELF_SHT_STRTAB;
        sh.sh_offset    = strtab_offset;
        sh.sh_size      = strtab_size;
        sh.sh_addralign = 1;
        fwrite(&sh, sizeof(sh), 1, f);
    }

    /* 6: .shstrtab */
    {
        Elf64_Shdr sh;
        memset(&sh, 0, sizeof(sh));
        sh.sh_name      = name_shstrtab;
        sh.sh_type      = ELF_SHT_STRTAB;
        sh.sh_offset    = shstrtab_offset;
        sh.sh_size      = shstrtab_size;
        sh.sh_addralign = 1;
        fwrite(&sh, sizeof(sh), 1, f);
    }

    /* 7: .rela.text (optional) */
    if (rela_text_idx > 0) {
        Elf64_Shdr sh;
        memset(&sh, 0, sizeof(sh));
        sh.sh_name      = name_rela_text;
        sh.sh_type      = ELF_SHT_RELA;
        sh.sh_flags     = ELF_SHF_INFO_LINK;
        sh.sh_offset    = rela_text_offset;
        sh.sh_size      = rela_text_byte_size;
        sh.sh_link      = (uint32_t)symtab_idx;
        sh.sh_info      = (uint32_t)text_idx;
        sh.sh_addralign = 8;
        sh.sh_entsize   = sizeof(Elf64_Rela);
        fwrite(&sh, sizeof(sh), 1, f);
    }

    /* 8: .rela.data (optional) */
    if (rela_data_idx > 0) {
        Elf64_Shdr sh;
        memset(&sh, 0, sizeof(sh));
        sh.sh_name      = name_rela_data;
        sh.sh_type      = ELF_SHT_RELA;
        sh.sh_flags     = ELF_SHF_INFO_LINK;
        sh.sh_offset    = rela_data_offset;
        sh.sh_size      = rela_data_byte_size;
        sh.sh_link      = (uint32_t)symtab_idx;
        sh.sh_info      = (uint32_t)data_idx;
        sh.sh_addralign = 8;
        sh.sh_entsize   = sizeof(Elf64_Rela);
        fwrite(&sh, sizeof(sh), 1, f);
    }

    /* .fadors_debug (optional) */
    if (has_debug) {
        Elf64_Shdr sh;
        memset(&sh, 0, sizeof(sh));
        sh.sh_name      = name_fadors_debug;
        sh.sh_type      = ELF_SHT_PROGBITS;
        sh.sh_flags     = 0; /* non-loadable */
        sh.sh_offset    = fadors_debug_offset;
        sh.sh_size      = fadors_debug_size;
        sh.sh_addralign = 4;
        fwrite(&sh, sizeof(sh), 1, f);
    }

    fclose(f);

    /* Cleanup */
    free(symtab);
    free(sym_map);
    free(rela_text);
    free(rela_data);
    buffer_free(&strtab);
    buffer_free(&shstrtab);
    buffer_free(&fadors_debug_sec);
}
