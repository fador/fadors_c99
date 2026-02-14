/*
 * linker.c — Minimal ELF static linker for x86-64 Linux
 *
 * Merges ELF64 relocatable objects (.o) and static archives (.a) into a
 * non-PIE static executable.  Includes a built-in _start stub that calls
 * main() and invokes the exit syscall, so no external CRT files are needed
 * for programs that don't require libc initialisation.
 *
 * Supported relocation types:
 *   R_X86_64_64    (1)  — 64-bit absolute
 *   R_X86_64_PC32  (2)  — 32-bit PC-relative
 *   R_X86_64_PLT32 (4)  — 32-bit PLT-relative (= PC32 for static link)
 *   R_X86_64_32    (10) — 32-bit absolute (zero-extended)
 *   R_X86_64_32S   (11) — 32-bit absolute (sign-extended)
 */

#include "linker.h"
#include "elf.h"
#include "buffer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Constants                                                         */
/* ------------------------------------------------------------------ */

#define BASE_ADDR    0x400000ULL
#define PAGE_SIZE    0x1000ULL

/* Short aliases for section constants */
#define SEC_UNDEF  LINK_SEC_UNDEF
#define SEC_TEXT   LINK_SEC_TEXT
#define SEC_DATA   LINK_SEC_DATA
#define SEC_BSS    LINK_SEC_BSS

/* Built-in _start stub — calls main(argc, argv), then exit(retval).
 *
 *   xor  %ebp, %ebp           ; ABI: mark deepest stack frame
 *   mov  (%rsp), %rdi          ; argc
 *   lea  8(%rsp), %rsi         ; argv
 *   call main                  ; (rel32 displacement patched at link time)
 *   mov  %eax, %edi            ; exit code = return value of main
 *   mov  $60, %eax             ; __NR_exit
 *   syscall
 */
#define START_STUB_SIZE       25
#define START_CALL_DISP_OFF   12   /* offset of the 4-byte call displacement */
#define START_CALL_NEXT_IP    16   /* IP of instruction after the call       */

/*
 * Built-in _start stub (machine code bytes).
 * Can now be expressed as a proper array initialiser because the compiler
 * supports  static const unsigned char name[] = { 0xNN, ... };
 */
static const unsigned char start_stub[START_STUB_SIZE] = {
    0x31, 0xED,                         /* xor %ebp,%ebp     */
    0x48, 0x8B, 0x3C, 0x24,            /* mov (%rsp),%rdi   */
    0x48, 0x8D, 0x74, 0x24, 0x08,      /* lea 8(%rsp),%rsi  */
    0xE8, 0x00, 0x00, 0x00, 0x00,      /* call main (rel32) */
    0x89, 0xC7,                         /* mov %eax,%edi     */
    0xB8, 0x3C, 0x00, 0x00, 0x00,      /* mov $60,%eax      */
    0x0F, 0x05                          /* syscall           */
};

/* ------------------------------------------------------------------ */
/*  Helpers                                                           */
/* ------------------------------------------------------------------ */

static uint64_t lnk_align_up(uint64_t v, uint64_t a) {
    return (v + a - 1) & ~(a - 1);
}

/* ELF SysV hash function for .hash section in dynamic linking */
static unsigned long elf_hash(const char *name) {
    unsigned long h = 0;
    const unsigned char *p = (const unsigned char *)name;
    while (*p) {
        unsigned long g;
        h = (h << 4) + *p++;
        g = h & 0xf0000000UL;
        if (g) h ^= g >> 24;
        h &= ~g;
    }
    return h;
}

/* ------------------------------------------------------------------ */
/*  Symbol table management                                           */
/* ------------------------------------------------------------------ */

/* Find a GLOBAL (or WEAK) symbol by name.  Returns index or -1. */
static int linker_find_global(Linker *l, const char *name) {
    size_t i;
    for (i = 0; i < l->sym_count; i++) {
        if (l->symbols[i].binding != ELF_STB_LOCAL &&
            strcmp(l->symbols[i].name, name) == 0)
            return (int)i;
    }
    return -1;
}

/* Append a new symbol.  Returns its index. */
static uint32_t linker_add_sym(Linker *l, const char *name,
                               uint64_t value, int section,
                               uint8_t binding, uint8_t type,
                               uint64_t size) {
    if (l->sym_count >= l->sym_cap) {
        l->sym_cap = l->sym_cap ? l->sym_cap * 2 : 256;
        l->symbols = realloc(l->symbols, l->sym_cap * sizeof(LinkSymbol));
    }
    LinkSymbol *s = &l->symbols[l->sym_count];
    s->name    = strdup(name);
    s->value   = value;
    s->section = section;
    s->binding = binding;
    s->type    = type;
    s->size    = size;
    return (uint32_t)l->sym_count++;
}

/* ------------------------------------------------------------------ */
/*  Relocation management                                             */
/* ------------------------------------------------------------------ */

static void linker_add_reloc(Linker *l, uint64_t offset, int section,
                             uint32_t sym_index, uint32_t type,
                             int64_t addend) {
    if (l->reloc_count >= l->reloc_cap) {
        l->reloc_cap = l->reloc_cap ? l->reloc_cap * 2 : 256;
        l->relocs = realloc(l->relocs, l->reloc_cap * sizeof(LinkReloc));
    }
    LinkReloc *r = &l->relocs[l->reloc_count++];
    r->offset    = offset;
    r->section   = section;
    r->sym_index = sym_index;
    r->type      = type;
    r->addend    = addend;
}

/* ------------------------------------------------------------------ */
/*  Public: create / destroy                                          */
/* ------------------------------------------------------------------ */

Linker *linker_new(void) {
    Linker *l = calloc(1, sizeof(Linker));
    buffer_init(&l->text);
    buffer_init(&l->data);
    l->debug_source_file = NULL;
    l->debug_comp_dir = NULL;
    l->debug_lines = NULL;
    l->debug_line_count = 0;
    l->debug_line_cap = 0;
    return l;
}

void linker_free(Linker *l) {
    size_t i;
    if (!l) return;
    buffer_free(&l->text);
    buffer_free(&l->data);
    for (i = 0; i < l->sym_count; i++)
        free(l->symbols[i].name);
    free(l->symbols);
    free(l->relocs);
    for (i = 0; i < l->lib_path_count; i++)
        free(l->lib_paths[i]);
    free(l->lib_paths);
    for (i = 0; i < l->lib_count; i++)
        free(l->libraries[i]);
    free(l->libraries);
    free(l->debug_source_file);
    free(l->debug_comp_dir);
    free(l->debug_lines);
    free(l);
}

/* ------------------------------------------------------------------ */
/*  Public: add library path / library                                */
/* ------------------------------------------------------------------ */

void linker_add_lib_path(Linker *l, const char *path) {
    l->lib_paths = realloc(l->lib_paths, (l->lib_path_count + 1) * sizeof(char *));
    l->lib_paths[l->lib_path_count++] = strdup(path);
}

void linker_add_library(Linker *l, const char *name) {
    l->libraries = realloc(l->libraries, (l->lib_count + 1) * sizeof(char *));
    l->libraries[l->lib_count++] = strdup(name);
}

/* ------------------------------------------------------------------ */
/*  ELF .o reader                                                     */
/* ------------------------------------------------------------------ */

/*
 * read_elf_object — Parse an ELF64 relocatable object from memory and
 * merge its .text, .data, .bss, .rodata content into the linker's
 * buffers.  Symbols and relocations are translated to linker-global
 * indices.
 *
 * Returns 0 on success, -1 on error.
 */
static int read_elf_object(Linker *l, const unsigned char *data,
                           size_t file_size, const char *filename) {
    size_t i;

    /* --- 1. Validate ELF header ----------------------------------- */
    if (file_size < sizeof(Elf64_Ehdr)) {
        fprintf(stderr, "linker: %s: file too small for ELF header\n", filename);
        return -1;
    }
    const Elf64_Ehdr *ehdr = (const Elf64_Ehdr *)data;
    if (ehdr->e_ident[0] != ELF_ELFMAG0 || ehdr->e_ident[1] != ELF_ELFMAG1 ||
        ehdr->e_ident[2] != ELF_ELFMAG2 || ehdr->e_ident[3] != ELF_ELFMAG3) {
        fprintf(stderr, "linker: %s: not an ELF file\n", filename);
        return -1;
    }
    if (ehdr->e_ident[4] != ELF_ELFCLASS64) {
        fprintf(stderr, "linker: %s: not a 64-bit ELF\n", filename);
        return -1;
    }
    if (ehdr->e_type != ELF_ET_REL) {
        fprintf(stderr, "linker: %s: not a relocatable object (type=%d)\n",
                filename, ehdr->e_type);
        return -1;
    }

    /* --- 2. Locate section headers -------------------------------- */
    if (ehdr->e_shoff == 0 || ehdr->e_shnum == 0) return 0;
    int num_sec = ehdr->e_shnum;
    const Elf64_Shdr *shdrs = (const Elf64_Shdr *)(data + ehdr->e_shoff);

    /* Section-name string table */
    if (ehdr->e_shstrndx >= (uint16_t)num_sec) return -1;
    const char *shstr = (const char *)(data + shdrs[ehdr->e_shstrndx].sh_offset);

    /* --- 3. Identify interesting sections ------------------------- */
    int text_si = -1, data_si = -1, bss_si = -1, rodata_si = -1;
    int symtab_si = -1, strtab_si = -1;
    int fadors_debug_si = -1;

    /* We will collect rela sections in a small array */
    typedef struct { int sec_si; int rela_si; } RelaPair;
    RelaPair rela_pairs[16];
    int rela_pair_count = 0;

    for (i = 0; i < (size_t)num_sec; i++) {
        const char *name = shstr + shdrs[i].sh_name;
        uint32_t stype = shdrs[i].sh_type;
        if (stype == ELF_SHT_PROGBITS || stype == ELF_SHT_NOBITS) {
            if (strcmp(name, ".text") == 0)   text_si   = (int)i;
            else if (strcmp(name, ".data") == 0)   data_si   = (int)i;
            else if (strcmp(name, ".bss") == 0)    bss_si    = (int)i;
            else if (strcmp(name, ".rodata") == 0) rodata_si = (int)i;
            else if (strcmp(name, ".fadors_debug") == 0) fadors_debug_si = (int)i;
        } else if (stype == ELF_SHT_SYMTAB) {
            symtab_si = (int)i;
        } else if (stype == ELF_SHT_RELA) {
            /* .rela.X — sh_info points to the section it applies to */
            if (rela_pair_count < 16) {
                rela_pairs[rela_pair_count].sec_si  = (int)shdrs[i].sh_info;
                rela_pairs[rela_pair_count].rela_si = (int)i;
                rela_pair_count++;
            }
        }
    }

    if (symtab_si >= 0)
        strtab_si = (int)shdrs[symtab_si].sh_link;

    /* --- 4. Build section-index → linker mapping ------------------- */
    int   *sec_id   = calloc((size_t)num_sec, sizeof(int));       /* SEC_* */
    size_t *sec_base = calloc((size_t)num_sec, sizeof(size_t));   /* base in merged buf */

    /* Merge .text */
    size_t text_base = l->text.size;
    if (text_si >= 0) {
        uint64_t align = shdrs[text_si].sh_addralign;
        if (align < 1) align = 1;
        buffer_pad(&l->text, (size_t)align);
        text_base = l->text.size;
        if (shdrs[text_si].sh_type != ELF_SHT_NOBITS)
            buffer_write_bytes(&l->text,
                               data + shdrs[text_si].sh_offset,
                               (size_t)shdrs[text_si].sh_size);
        sec_id[text_si]   = SEC_TEXT;
        sec_base[text_si] = text_base;
    }

    /* Merge .data */
    size_t data_base = l->data.size;
    if (data_si >= 0) {
        uint64_t align = shdrs[data_si].sh_addralign;
        if (align < 1) align = 1;
        buffer_pad(&l->data, (size_t)align);
        data_base = l->data.size;
        if (shdrs[data_si].sh_type != ELF_SHT_NOBITS)
            buffer_write_bytes(&l->data,
                               data + shdrs[data_si].sh_offset,
                               (size_t)shdrs[data_si].sh_size);
        sec_id[data_si]   = SEC_DATA;
        sec_base[data_si] = data_base;
    }

    /* Merge .rodata → data */
    size_t rodata_base = l->data.size;
    if (rodata_si >= 0) {
        uint64_t align = shdrs[rodata_si].sh_addralign;
        if (align < 1) align = 1;
        buffer_pad(&l->data, (size_t)align);
        rodata_base = l->data.size;
        if (shdrs[rodata_si].sh_type != ELF_SHT_NOBITS)
            buffer_write_bytes(&l->data,
                               data + shdrs[rodata_si].sh_offset,
                               (size_t)shdrs[rodata_si].sh_size);
        sec_id[rodata_si]   = SEC_DATA;
        sec_base[rodata_si] = rodata_base;
    }

    /* Merge .bss */
    size_t bss_base = l->bss_size;
    if (bss_si >= 0) {
        uint64_t align = shdrs[bss_si].sh_addralign;
        if (align < 1) align = 1;
        l->bss_size = (size_t)lnk_align_up(l->bss_size, align);
        bss_base = l->bss_size;
        l->bss_size += (size_t)shdrs[bss_si].sh_size;
        sec_id[bss_si]   = SEC_BSS;
        sec_base[bss_si] = bss_base;
    }

    /* --- 5. Process symbols ---------------------------------------- */
    if (symtab_si < 0 || strtab_si < 0) {
        free(sec_id);
        free(sec_base);
        return 0;  /* no symbols — nothing more to do */
    }

    const Elf64_Sym *syms =
        (const Elf64_Sym *)(data + shdrs[symtab_si].sh_offset);
    int sym_count = (int)(shdrs[symtab_si].sh_size / sizeof(Elf64_Sym));
    const char *strtab = (const char *)(data + shdrs[strtab_si].sh_offset);

    uint32_t *sym_map = calloc((size_t)sym_count, sizeof(uint32_t));

    for (i = 1; i < (size_t)sym_count; i++) {   /* skip null symbol */
        const char *name = strtab + syms[i].st_name;
        uint8_t bind = ELF64_ST_BIND(syms[i].st_info);
        uint8_t stype = ELF64_ST_TYPE(syms[i].st_info);
        uint16_t shndx = syms[i].st_shndx;

        int  section = SEC_UNDEF;
        uint64_t value = syms[i].st_value;

        if (shndx == ELF_SHN_ABS) {
            /* Absolute symbol — keep value as-is, mark as "text" for
             * simplicity (won't be relocated). */
            section = SEC_TEXT;
        } else if (shndx == ELF_SHN_COMMON) {
            /* Common symbol — allocate in BSS */
            uint64_t align = syms[i].st_value;
            if (align < 1) align = 1;
            l->bss_size = (size_t)lnk_align_up(l->bss_size, align);
            value   = l->bss_size;
            section = SEC_BSS;
            l->bss_size += (size_t)syms[i].st_size;
        } else if (shndx != ELF_SHN_UNDEF && shndx < (uint16_t)num_sec) {
            if (sec_id[shndx] != SEC_UNDEF) {
                section = sec_id[shndx];
                value  += sec_base[shndx];
            } else if (stype == ELF_STT_SECTION) {
                /* Section symbol for a section we're ignoring (e.g. .eh_frame).
                 * Map to UNDEF so relocations against it will be flagged. */
                section = SEC_UNDEF;
            }
        }

        if (bind == ELF_STB_LOCAL) {
            /* Local symbols are always unique per object file. */
            sym_map[i] = linker_add_sym(l, name, value, section,
                                        bind, stype, syms[i].st_size);
        } else {
            /* Global / weak — merge with existing if possible. */
            int existing = linker_find_global(l, name);
            if (existing >= 0) {
                LinkSymbol *es = &l->symbols[existing];
                if (section != SEC_UNDEF && es->section == SEC_UNDEF) {
                    /* Existing was undefined, now we have a definition. */
                    es->value   = value;
                    es->section = section;
                    es->type    = stype;
                    es->size    = syms[i].st_size;
                    if (bind == ELF_STB_GLOBAL)
                        es->binding = ELF_STB_GLOBAL;
                } else if (section != SEC_UNDEF && es->section != SEC_UNDEF) {
                    /* Both defined — allow if one is weak. */
                    if (es->binding == ELF_STB_WEAK && bind != ELF_STB_WEAK) {
                        es->value   = value;
                        es->section = section;
                        es->type    = stype;
                        es->binding = bind;
                        es->size    = syms[i].st_size;
                    } else if (bind != ELF_STB_WEAK) {
                        fprintf(stderr,
                                "linker: duplicate symbol '%s' in %s\n",
                                name, filename);
                    }
                }
                sym_map[i] = (uint32_t)existing;
            } else {
                sym_map[i] = linker_add_sym(l, name, value, section,
                                            bind, stype, syms[i].st_size);
            }
        }
    }

    /* --- 6. Process relocations ------------------------------------ */
    {
        int p;
        for (p = 0; p < rela_pair_count; p++) {
            int target_si = rela_pairs[p].sec_si;
            int rela_si   = rela_pairs[p].rela_si;

            /* Determine which merged section this relocation applies to */
            int target_sec = SEC_UNDEF;
            size_t target_base = 0;
            if (target_si >= 0 && target_si < num_sec) {
                target_sec  = sec_id[target_si];
                target_base = sec_base[target_si];
            }
            if (target_sec == SEC_UNDEF)
                continue;  /* relocation for a section we don't track */

            const Elf64_Rela *relas =
                (const Elf64_Rela *)(data + shdrs[rela_si].sh_offset);
            int rela_count =
                (int)(shdrs[rela_si].sh_size / sizeof(Elf64_Rela));

            int r;
            for (r = 0; r < rela_count; r++) {
                uint32_t rsym  = ELF64_R_SYM(relas[r].r_info);
                uint32_t rtype = ELF64_R_TYPE(relas[r].r_info);

                if (rsym >= (uint32_t)sym_count) continue;  /* safety */

                linker_add_reloc(l,
                                 relas[r].r_offset + target_base,
                                 target_sec,
                                 sym_map[rsym],
                                 rtype,
                                 relas[r].r_addend);
            }
        }
    }

    /* --- 7. Parse .fadors_debug section (for -g) ------------------- */
    if (fadors_debug_si >= 0) {
        const unsigned char *dbg = data + shdrs[fadors_debug_si].sh_offset;
        size_t dbg_size = (size_t)shdrs[fadors_debug_si].sh_size;
        size_t pos2 = 0;

        if (dbg_size >= 4) {
            /* source filename */
            uint32_t sname_len;
            memcpy(&sname_len, dbg + pos2, 4); pos2 += 4;
            if (pos2 + sname_len <= dbg_size && !l->debug_source_file) {
                l->debug_source_file = strndup((const char *)(dbg + pos2), sname_len);
            }
            pos2 += sname_len;

            /* comp_dir */
            if (pos2 + 4 <= dbg_size) {
                uint32_t cdir_len;
                memcpy(&cdir_len, dbg + pos2, 4); pos2 += 4;
                if (pos2 + cdir_len <= dbg_size && !l->debug_comp_dir) {
                    l->debug_comp_dir = strndup((const char *)(dbg + pos2), cdir_len);
                }
                pos2 += cdir_len;
            }

            /* line entries */
            if (pos2 + 4 <= dbg_size) {
                uint32_t entry_count;
                memcpy(&entry_count, dbg + pos2, 4); pos2 += 4;

                for (uint32_t ei = 0; ei < entry_count && pos2 + 10 <= dbg_size; ei++) {
                    if (l->debug_line_count >= l->debug_line_cap) {
                        l->debug_line_cap = l->debug_line_cap ? l->debug_line_cap * 2 : 256;
                        l->debug_lines = realloc(l->debug_lines,
                                                 l->debug_line_cap * sizeof(LinkDebugLine));
                    }
                    LinkDebugLine *dl = &l->debug_lines[l->debug_line_count++];
                    memcpy(&dl->address, dbg + pos2, 4); pos2 += 4;
                    /* Adjust address by the text base of this object */
                    dl->address += (uint32_t)text_base;
                    memcpy(&dl->line, dbg + pos2, 4); pos2 += 4;
                    dl->is_stmt = dbg[pos2++];
                    dl->end_seq = dbg[pos2++];
                }
            }
        }
    }

    free(sec_id);
    free(sec_base);
    free(sym_map);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Public: add object file                                           */
/* ------------------------------------------------------------------ */

int linker_add_object_file(Linker *l, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "linker: cannot open '%s'\n", path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *buf = malloc((size_t)sz);
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        fprintf(stderr, "linker: read error on '%s'\n", path);
        free(buf);
        fclose(f);
        return -1;
    }
    fclose(f);

    int rc = read_elf_object(l, buf, (size_t)sz, path);
    free(buf);
    return rc;
}

/* ------------------------------------------------------------------ */
/*  Archive (.a) reader                                               */
/* ------------------------------------------------------------------ */

/*
 * Returns 1 if there is at least one undefined global symbol.
 */
/* Check whether any relocation actually references this symbol index. */
static int symbol_is_referenced(Linker *l, size_t sym_idx) {
    size_t j;
    for (j = 0; j < l->reloc_count; j++) {
        if (l->relocs[j].sym_index == (uint32_t)sym_idx)
            return 1;
    }
    return 0;
}

static int has_undefined_symbols(Linker *l) {
    size_t i;
    for (i = 0; i < l->sym_count; i++) {
        if (l->symbols[i].binding != ELF_STB_LOCAL &&
            l->symbols[i].section == SEC_UNDEF &&
            l->symbols[i].name[0] != '\0' &&
            symbol_is_referenced(l, i))
            return 1;
    }
    return 0;
}

/*
 * Read a big-endian 32-bit integer.
 */
static uint32_t read_be32(const unsigned char *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

/*
 * Process an ar archive: look at the archive symbol table and load
 * members that define currently-undefined symbols.  Iterates until
 * no further progress is made.
 *
 * Returns 0 on success, -1 on error.
 */
static int process_archive(Linker *l, const unsigned char *ar_data,
                           size_t ar_size, const char *ar_path) {
    /* Check magic "!<arch>\n" */
    if (ar_size < 8 || memcmp(ar_data, "!<arch>\n", 8) != 0) {
        fprintf(stderr, "linker: %s: not an ar archive\n", ar_path);
        return -1;
    }

    /* --- Parse the archive symbol table (first member, named "/") --- */
    if (ar_size < 8 + 60) return 0;  /* empty archive */

    size_t pos = 8;
    /* ar member header: 16 name + 12 date + 6 uid + 6 gid + 8 mode + 10 size + 2 fmag = 60 */
    char sizestr[11];
    memcpy(sizestr, ar_data + pos + 48, 10);
    sizestr[10] = '\0';
    long membsz = atol(sizestr);
    size_t content_off = pos + 60;

    /* Is the first member the symbol table? (name starts with "/" followed by space) */
    int has_symidx = (ar_data[pos] == '/' &&
                      (ar_data[pos + 1] == ' ' || ar_data[pos + 1] == '\0'));

    if (!has_symidx || membsz < 4) {
        /* No symbol index — we would have to scan every member.
         * For now, skip this archive. */
        fprintf(stderr, "linker: %s: no archive symbol index\n", ar_path);
        return 0;
    }

    const unsigned char *idx = ar_data + content_off;
    uint32_t nsyms = read_be32(idx);
    const unsigned char *offsets_p = idx + 4;
    const char *names_p = (const char *)(idx + 4 + nsyms * 4);

    /* Build a simple table: (name, member_offset) */
    /* Loaded-member tracking (avoid double-loading) */
    size_t *loaded_offsets = NULL;
    int loaded_count = 0;

    int changed = 1;
    while (changed && has_undefined_symbols(l)) {
        changed = 0;
        const char *np = names_p;
        uint32_t si;

        for (si = 0; si < nsyms; si++) {
            uint32_t member_off = read_be32(offsets_p + si * 4);
            if (np >= (const char *)(ar_data + ar_size)) break;
            size_t nlen = strlen(np);

            /* Is this symbol currently undefined in our table? */
            int idx2 = linker_find_global(l, np);
            if (idx2 >= 0 && l->symbols[idx2].section == SEC_UNDEF) {
                /* Check if we already loaded this member */
                int already = 0;
                int j;
                for (j = 0; j < loaded_count; j++) {
                    if (loaded_offsets[j] == (size_t)member_off) {
                        already = 1;
                        break;
                    }
                }

                if (!already && (size_t)member_off + 60 <= ar_size) {
                    /* Parse member header to get size */
                    char msz_str[11];
                    memcpy(msz_str, ar_data + member_off + 48, 10);
                    msz_str[10] = '\0';
                    long msz = atol(msz_str);
                    size_t mcontent = (size_t)member_off + 60;

                    if (mcontent + (size_t)msz <= ar_size) {
                        /* Load this ELF member */
                        int rc = read_elf_object(l,
                                                 ar_data + mcontent,
                                                 (size_t)msz,
                                                 ar_path);
                        if (rc == 0) changed = 1;
                    }

                    loaded_offsets = realloc(loaded_offsets,
                                            ((size_t)loaded_count + 1) * sizeof(size_t));
                    loaded_offsets[loaded_count++] = (size_t)member_off;
                }
            }

            np += nlen + 1;
        }
    }

    free(loaded_offsets);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Library search and loading                                        */
/* ------------------------------------------------------------------ */

static int file_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f) { fclose(f); return 1; }
    return 0;
}

/*
 * Search library paths for lib<name>.a .  Returns pointer to a static
 * buffer containing the path, or NULL if not found.
 */
static const char *find_library_file(Linker *l, const char *name) {
    static char buf[512];
    size_t i;
    for (i = 0; i < l->lib_path_count; i++) {
        snprintf(buf, sizeof(buf), "%s/lib%s.a", l->lib_paths[i], name);
        if (file_exists(buf)) return buf;
    }
    return NULL;
}

/*
 * Load an archive file and resolve symbols from it.
 */
static int load_archive(Linker *l, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "linker: cannot open archive '%s'\n", path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *buf = malloc((size_t)sz);
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf);
        fclose(f);
        return -1;
    }
    fclose(f);

    int rc = process_archive(l, buf, (size_t)sz, path);
    free(buf);
    return rc;
}

/* ------------------------------------------------------------------ */
/*  Default library search paths                                      */
/* ------------------------------------------------------------------ */

static void add_default_lib_paths(Linker *l) {
    /* Try to find GCC library path (for libgcc.a) */
    int ver;
    char path[256];
    for (ver = 14; ver >= 7; ver--) {
        snprintf(path, sizeof(path),
                 "/usr/lib/gcc/x86_64-linux-gnu/%d", ver);
        snprintf(path + strlen(path), sizeof(path) - strlen(path),
                 "/libgcc.a");
        if (file_exists(path)) {
            /* Truncate to directory */
            char *slash = strrchr(path, '/');
            if (slash) *slash = '\0';
            linker_add_lib_path(l, path);
            break;
        }
    }

    /* Standard system library directories */
    if (file_exists("/usr/lib/x86_64-linux-gnu/libc.a"))
        linker_add_lib_path(l, "/usr/lib/x86_64-linux-gnu");
    else if (file_exists("/usr/lib64/libc.a"))
        linker_add_lib_path(l, "/usr/lib64");

    if (file_exists("/usr/lib/libc.a"))
        linker_add_lib_path(l, "/usr/lib");
    if (file_exists("/lib/x86_64-linux-gnu/libc.a"))
        linker_add_lib_path(l, "/lib/x86_64-linux-gnu");
}

/* ------------------------------------------------------------------ */
/*  DWARF 4 debug section generation                                  */
/* ------------------------------------------------------------------ */

/* Write a ULEB128 value to a buffer */
static void write_uleb128(Buffer *b, uint64_t val) {
    do {
        uint8_t byte = val & 0x7F;
        val >>= 7;
        if (val != 0) byte |= 0x80;
        buffer_write_byte(b, byte);
    } while (val != 0);
}

/* Write a SLEB128 value to a buffer */
static void write_sleb128(Buffer *b, int64_t val) {
    int more = 1;
    while (more) {
        uint8_t byte = val & 0x7F;
        val >>= 7;
        if ((val == 0 && !(byte & 0x40)) ||
            (val == -1 && (byte & 0x40))) {
            more = 0;
        } else {
            byte |= 0x80;
        }
        buffer_write_byte(b, byte);
    }
}

/* DWARF constants */
#define DW_TAG_compile_unit     0x11
#define DW_CHILDREN_no          0x00
#define DW_AT_name              0x03
#define DW_AT_stmt_list         0x10
#define DW_AT_low_pc            0x11
#define DW_AT_high_pc           0x12
#define DW_AT_language          0x13
#define DW_AT_comp_dir          0x1b
#define DW_AT_producer          0x25
#define DW_FORM_addr            0x01
#define DW_FORM_data2           0x05
#define DW_FORM_data4           0x06
#define DW_FORM_data8           0x07
#define DW_FORM_string          0x08
#define DW_FORM_sec_offset      0x17
#define DW_LANG_C99             0x000c
#define DW_LNS_copy             1
#define DW_LNS_advance_pc       2
#define DW_LNS_advance_line     3
#define DW_LNS_set_file         4
#define DW_LNS_set_column       5
#define DW_LNS_negate_stmt      6
#define DW_LNS_set_basic_block  7
#define DW_LNS_const_add_pc     8
#define DW_LNS_fixed_advance_pc 9
#define DW_LNS_set_prologue_end 10
#define DW_LNS_set_epilogue_begin 11
#define DW_LNS_set_isa          12
#define DW_LNE_end_sequence     1
#define DW_LNE_set_address      2

/*
 * Build DWARF 4 .debug_abbrev, .debug_info, and .debug_line sections.
 * text_vaddr is the virtual address of the .text segment in the final
 * executable.
 */
static void build_dwarf_sections(Linker *l, uint64_t text_vaddr,
                                  uint64_t text_size,
                                  Buffer *debug_abbrev,
                                  Buffer *debug_info,
                                  Buffer *debug_line) {
    size_t i;
    buffer_init(debug_abbrev);
    buffer_init(debug_info);
    buffer_init(debug_line);

    /* ---- .debug_abbrev ---- */
    /* Abbreviation 1: DW_TAG_compile_unit, no children */
    write_uleb128(debug_abbrev, 1);                     /* abbrev code */
    write_uleb128(debug_abbrev, DW_TAG_compile_unit);   /* tag */
    buffer_write_byte(debug_abbrev, DW_CHILDREN_no);    /* has children */
    /* attributes */
    write_uleb128(debug_abbrev, DW_AT_producer);
    write_uleb128(debug_abbrev, DW_FORM_string);
    write_uleb128(debug_abbrev, DW_AT_language);
    write_uleb128(debug_abbrev, DW_FORM_data2);
    write_uleb128(debug_abbrev, DW_AT_name);
    write_uleb128(debug_abbrev, DW_FORM_string);
    write_uleb128(debug_abbrev, DW_AT_comp_dir);
    write_uleb128(debug_abbrev, DW_FORM_string);
    write_uleb128(debug_abbrev, DW_AT_low_pc);
    write_uleb128(debug_abbrev, DW_FORM_addr);
    write_uleb128(debug_abbrev, DW_AT_high_pc);
    write_uleb128(debug_abbrev, DW_FORM_data8);
    write_uleb128(debug_abbrev, DW_AT_stmt_list);
    write_uleb128(debug_abbrev, DW_FORM_sec_offset);
    /* end of attributes */
    write_uleb128(debug_abbrev, 0);
    write_uleb128(debug_abbrev, 0);
    /* end of abbreviation table */
    buffer_write_byte(debug_abbrev, 0);

    /* ---- .debug_info ---- */
    /* Reserve 4 bytes for unit_length, fill later */
    size_t info_len_off = debug_info->size;
    buffer_write_dword(debug_info, 0); /* placeholder for unit_length */
    buffer_write_word(debug_info, 4);  /* DWARF version 4 */
    buffer_write_dword(debug_info, 0); /* debug_abbrev_offset */
    buffer_write_byte(debug_info, 8);  /* address_size = 8 (64-bit) */

    /* CU DIE: abbrev code 1 = compile_unit */
    write_uleb128(debug_info, 1);

    /* DW_AT_producer (string) */
    {
        const char *producer = "fadors99 C compiler";
        buffer_write_bytes(debug_info, producer, strlen(producer) + 1);
    }
    /* DW_AT_language (data2) */
    buffer_write_word(debug_info, DW_LANG_C99);
    /* DW_AT_name (string) */
    {
        const char *name = l->debug_source_file ? l->debug_source_file : "unknown";
        buffer_write_bytes(debug_info, name, strlen(name) + 1);
    }
    /* DW_AT_comp_dir (string) */
    {
        const char *cdir = l->debug_comp_dir ? l->debug_comp_dir : ".";
        buffer_write_bytes(debug_info, cdir, strlen(cdir) + 1);
    }
    /* DW_AT_low_pc (addr) */
    buffer_write_qword(debug_info, text_vaddr);
    /* DW_AT_high_pc (data8 = size relative to low_pc) */
    buffer_write_qword(debug_info, text_size);
    /* DW_AT_stmt_list (sec_offset, 4 bytes = offset into .debug_line) */
    buffer_write_dword(debug_info, 0);

    /* Patch unit_length = total size - 4 (the length field itself) */
    {
        uint32_t info_len = (uint32_t)(debug_info->size - info_len_off - 4);
        memcpy(debug_info->data + info_len_off, &info_len, 4);
    }

    /* ---- .debug_line ---- */
    /* DWARF 4 line number program */
    size_t line_unit_off = debug_line->size;
    buffer_write_dword(debug_line, 0); /* placeholder for unit_length */
    buffer_write_word(debug_line, 4);  /* version = DWARF 4 */
    size_t header_len_off = debug_line->size;
    buffer_write_dword(debug_line, 0); /* placeholder for header_length */
    size_t header_start = debug_line->size;

    buffer_write_byte(debug_line, 1);  /* minimum_instruction_length */
    buffer_write_byte(debug_line, 1);  /* maximum_operations_per_instruction */
    buffer_write_byte(debug_line, 1);  /* default_is_stmt */
    buffer_write_byte(debug_line, (uint8_t)(int8_t)-5);  /* line_base */
    buffer_write_byte(debug_line, 14); /* line_range */
    buffer_write_byte(debug_line, 13); /* opcode_base */

    /* standard_opcode_lengths[1..12] */
    buffer_write_byte(debug_line, 0);  /* DW_LNS_copy */
    buffer_write_byte(debug_line, 1);  /* DW_LNS_advance_pc */
    buffer_write_byte(debug_line, 1);  /* DW_LNS_advance_line */
    buffer_write_byte(debug_line, 1);  /* DW_LNS_set_file */
    buffer_write_byte(debug_line, 1);  /* DW_LNS_set_column */
    buffer_write_byte(debug_line, 0);  /* DW_LNS_negate_stmt */
    buffer_write_byte(debug_line, 0);  /* DW_LNS_set_basic_block */
    buffer_write_byte(debug_line, 0);  /* DW_LNS_const_add_pc */
    buffer_write_byte(debug_line, 1);  /* DW_LNS_fixed_advance_pc */
    buffer_write_byte(debug_line, 0);  /* DW_LNS_set_prologue_end */
    buffer_write_byte(debug_line, 0);  /* DW_LNS_set_epilogue_begin */
    buffer_write_byte(debug_line, 1);  /* DW_LNS_set_isa */

    /* include_directories: just end with 0 (no directories) */
    buffer_write_byte(debug_line, 0);

    /* file_names: one entry, then end with 0 */
    {
        const char *fname = l->debug_source_file ? l->debug_source_file : "unknown";
        buffer_write_bytes(debug_line, fname, strlen(fname) + 1);
        write_uleb128(debug_line, 0); /* directory index (0 = comp_dir) */
        write_uleb128(debug_line, 0); /* last modification time */
        write_uleb128(debug_line, 0); /* file size */
    }
    buffer_write_byte(debug_line, 0); /* end of file_names */

    /* Patch header_length */
    {
        uint32_t hdr_len = (uint32_t)(debug_line->size - header_start);
        memcpy(debug_line->data + header_len_off, &hdr_len, 4);
    }

    /* ---- Line number program ---- */
    /* Initial state: address=0, file=1, line=1, column=0, is_stmt=1 */
    int cur_line = 1;
    uint64_t cur_addr = 0;

    /* Emit DW_LNE_set_address to set the initial address */
    buffer_write_byte(debug_line, 0); /* extended opcode marker */
    write_uleb128(debug_line, 9);     /* length: 1 (opcode) + 8 (addr) */
    buffer_write_byte(debug_line, DW_LNE_set_address);
    buffer_write_qword(debug_line, text_vaddr);
    cur_addr = text_vaddr;

    for (i = 0; i < l->debug_line_count; i++) {
        uint64_t new_addr = text_vaddr + l->debug_lines[i].address;
        int new_line = (int)l->debug_lines[i].line;
        if (new_line <= 0) continue;

        int64_t addr_delta = (int64_t)(new_addr - cur_addr);
        int64_t line_delta = (int64_t)new_line - (int64_t)cur_line;

        /* Try special opcode first */
        int64_t adj_line = line_delta - (-5); /* line_base = -5 */
        int64_t opcode_val = adj_line + 14 * addr_delta + 13;
        if (adj_line >= 0 && adj_line < 14 && opcode_val >= 13 && opcode_val <= 255) {
            /* Use special opcode */
            buffer_write_byte(debug_line, (uint8_t)opcode_val);
        } else {
            /* Use explicit advance_pc + advance_line + copy */
            if (addr_delta > 0) {
                buffer_write_byte(debug_line, DW_LNS_advance_pc);
                write_uleb128(debug_line, (uint64_t)addr_delta);
            }
            if (line_delta != 0) {
                buffer_write_byte(debug_line, DW_LNS_advance_line);
                write_sleb128(debug_line, line_delta);
            }
            buffer_write_byte(debug_line, DW_LNS_copy);
        }

        cur_addr = new_addr;
        cur_line = new_line;
    }

    /* End sequence: advance PC to end of text, then end_sequence */
    {
        uint64_t end_addr = text_vaddr + text_size;
        int64_t end_delta = (int64_t)(end_addr - cur_addr);
        if (end_delta > 0) {
            buffer_write_byte(debug_line, DW_LNS_advance_pc);
            write_uleb128(debug_line, (uint64_t)end_delta);
        }
        buffer_write_byte(debug_line, 0); /* extended opcode marker */
        write_uleb128(debug_line, 1);     /* length = 1 */
        buffer_write_byte(debug_line, DW_LNE_end_sequence);
    }

    /* Patch unit_length */
    {
        uint32_t unit_len = (uint32_t)(debug_line->size - line_unit_off - 4);
        memcpy(debug_line->data + line_unit_off, &unit_len, 4);
    }
}

/* ------------------------------------------------------------------ */
/*  linker_link — the main linking driver                             */
/* ------------------------------------------------------------------ */

int linker_link(Linker *l, const char *output_path) {
    size_t i;

    /* ---- 0. Add default library search paths --------------------- */
    add_default_lib_paths(l);

    /* ---- 1. Add _start stub at the beginning of .text ------------ */
    /*  We prepend the stub, then move existing .text content after it.
     *  Since text may already contain data from objects loaded earlier,
     *  we create a new buffer.                                        */
    {
        Buffer new_text;
        buffer_init(&new_text);

        /* Write stub */
        buffer_write_bytes(&new_text, start_stub, START_STUB_SIZE);

        /* Pad to 16-byte alignment for subsequent code */
        buffer_pad(&new_text, 16);

        size_t stub_padded = new_text.size;

        /* Copy existing .text content after the stub */
        if (l->text.size > 0)
            buffer_write_bytes(&new_text, l->text.data, l->text.size);

        /* Adjust all symbol values in .text by +stub_padded */
        for (i = 0; i < l->sym_count; i++) {
            if (l->symbols[i].section == SEC_TEXT)
                l->symbols[i].value += stub_padded;
        }

        /* Adjust all .text relocation offsets by +stub_padded */
        for (i = 0; i < l->reloc_count; i++) {
            if (l->relocs[i].section == SEC_TEXT)
                l->relocs[i].offset += stub_padded;
        }

        /* Adjust debug line addresses by +stub_padded */
        for (i = 0; i < l->debug_line_count; i++)
            l->debug_lines[i].address += (uint32_t)stub_padded;

        /* Replace text buffer */
        buffer_free(&l->text);
        memcpy(&l->text, &new_text, sizeof(Buffer));

        /* Add _start symbol at offset 0 */
        linker_add_sym(l, "_start", 0, SEC_TEXT,
                       ELF_STB_GLOBAL, ELF_STT_FUNC, START_STUB_SIZE);
    }

    /* ---- 2. Process explicit -l libraries ------------------------ */
    for (i = 0; i < l->lib_count; i++) {
        if (!has_undefined_symbols(l)) break;
        const char *path = find_library_file(l, l->libraries[i]);
        if (path) {
            load_archive(l, path);
        } else {
            fprintf(stderr, "linker: cannot find -l%s\n", l->libraries[i]);
        }
    }

    /* ---- 3. Detect dynamic linking need -------------------------- */
    /*  If there are referenced undefined symbols after explicit -l
     *  libraries, use dynamic linking (libc.so.6) instead of static
     *  archives.  This avoids the cascading dependency problem with
     *  glibc's libc.a.                                               */
    int need_dynamic = 0;
    size_t *dyn_indices = NULL;
    int dyn_count = 0;
    size_t plt_text_off = 0;
    size_t got_data_off = 0;
    size_t libc_name_off = 0;
    size_t *dyn_name_offs = NULL;
    Buffer interp_sec, hash_sec, dynsym_sec, dynstr_sec, rela_plt_sec, dyn_sec;

    for (i = 0; i < l->sym_count; i++) {
        if (l->symbols[i].section == SEC_UNDEF &&
            l->symbols[i].binding != ELF_STB_LOCAL &&
            l->symbols[i].name[0] != '\0' &&
            symbol_is_referenced(l, i)) {
            dyn_indices = realloc(dyn_indices,
                                  ((size_t)dyn_count + 1) * sizeof(size_t));
            dyn_indices[dyn_count++] = i;
        }
    }
    need_dynamic = (dyn_count > 0);

    if (need_dynamic) {
        /* ---- 3a. Build dynamic linking structures ---------------- */

        /* .interp — path to the dynamic linker */
        buffer_init(&interp_sec);
        {
            const char *ip = "/lib64/ld-linux-x86-64.so.2";
            buffer_write_bytes(&interp_sec, ip, strlen(ip) + 1);
        }

        /* .dynstr — dynamic string table */
        buffer_init(&dynstr_sec);
        buffer_write_byte(&dynstr_sec, 0); /* null string at index 0 */
        libc_name_off = dynstr_sec.size;
        buffer_write_bytes(&dynstr_sec, "libc.so.6", 10);
        dyn_name_offs = malloc((size_t)dyn_count * sizeof(size_t));
        for (i = 0; i < (size_t)dyn_count; i++) {
            dyn_name_offs[i] = dynstr_sec.size;
            {
                const char *nm = l->symbols[dyn_indices[i]].name;
                buffer_write_bytes(&dynstr_sec, nm, strlen(nm) + 1);
            }
        }

        /* .dynsym — dynamic symbol table */
        buffer_init(&dynsym_sec);
        {
            /* Null entry (24 bytes) */
            unsigned char zsym[24];
            memset(zsym, 0, 24);
            buffer_write_bytes(&dynsym_sec, zsym, 24);
        }
        for (i = 0; i < (size_t)dyn_count; i++) {
            unsigned char sd[24];
            uint32_t st_name = (uint32_t)dyn_name_offs[i];
            uint8_t st_info = ELF64_ST_INFO(ELF_STB_GLOBAL, ELF_STT_FUNC);
            memset(sd, 0, 24);
            memcpy(sd, &st_name, 4);
            sd[4] = st_info;
            buffer_write_bytes(&dynsym_sec, sd, 24);
        }

        /* .hash — ELF SysV hash table */
        buffer_init(&hash_sec);
        {
            uint32_t nbuckets = (uint32_t)(dyn_count > 0 ? dyn_count : 1);
            uint32_t nchain = (uint32_t)(dyn_count + 1);
            uint32_t *buckets = calloc(nbuckets, sizeof(uint32_t));
            uint32_t *chains  = calloc(nchain,   sizeof(uint32_t));

            for (i = 0; i < (size_t)dyn_count; i++) {
                uint32_t si2 = (uint32_t)(i + 1);
                unsigned long h = elf_hash(l->symbols[dyn_indices[i]].name);
                uint32_t bkt = (uint32_t)(h % nbuckets);
                chains[si2] = buckets[bkt];
                buckets[bkt] = si2;
            }

            buffer_write_dword(&hash_sec, nbuckets);
            buffer_write_dword(&hash_sec, nchain);
            for (i = 0; i < nbuckets; i++)
                buffer_write_dword(&hash_sec, buckets[i]);
            for (i = 0; i < nchain; i++)
                buffer_write_dword(&hash_sec, chains[i]);

            free(buckets);
            free(chains);
        }

        /* PLT: append stub code to .text */
        buffer_pad(&l->text, 16);
        plt_text_off = l->text.size;
        {
            unsigned char z16[16];
            memset(z16, 0, 16);
            /* PLT0 placeholder (16 bytes) — filled after layout */
            buffer_write_bytes(&l->text, z16, 16);
            /* PLTn placeholders (16 bytes each) */
            for (i = 0; i < (size_t)dyn_count; i++)
                buffer_write_bytes(&l->text, z16, 16);
        }

        /* GOT.PLT: append to .data */
        buffer_pad(&l->data, 8);
        got_data_off = l->data.size;
        {
            /* GOT[0..2] = placeholders (filled after layout) */
            buffer_write_qword(&l->data, 0);
            buffer_write_qword(&l->data, 0);
            buffer_write_qword(&l->data, 0);
            /* GOT[3+n] = placeholders */
            for (i = 0; i < (size_t)dyn_count; i++)
                buffer_write_qword(&l->data, 0);
        }

        /* .rela.plt — placeholder (filled after layout) */
        buffer_init(&rela_plt_sec);
        for (i = 0; i < (size_t)dyn_count; i++) {
            unsigned char z24[24];
            memset(z24, 0, 24);
            buffer_write_bytes(&rela_plt_sec, z24, 24);
        }

        /* .dynamic — placeholder (11 entries × 16 bytes, filled later) */
        buffer_init(&dyn_sec);
        for (i = 0; i < 11; i++) {
            buffer_write_qword(&dyn_sec, 0);
            buffer_write_qword(&dyn_sec, 0);
        }

        /* Mark dynamic symbols as resolved to their PLT entries.
         * value is offset within .text — adjusted to vaddr later. */
        for (i = 0; i < (size_t)dyn_count; i++) {
            l->symbols[dyn_indices[i]].section = SEC_TEXT;
            l->symbols[dyn_indices[i]].value = plt_text_off + 16 + i * 16;
        }
    }

    /* ---- 4. Layout: assign virtual addresses --------------------- */
    /*
     * Static layout:
     *   ELF header + 2 phdrs → .text → (page pad) → .data
     *
     * Dynamic layout:
     *   ELF header + 4 phdrs → .interp → .hash → .dynsym → .dynstr
     *   → .rela.plt → .text (incl. PLT) → (page pad)
     *   → .data (incl. GOT.PLT) → .dynamic
     */
    uint64_t text_file_off, text_vaddr, text_size;
    uint64_t data_file_off, data_vaddr, data_size;
    uint64_t bss_vaddr, entry_point;
    uint64_t interp_foff = 0, hash_foff = 0, dynsym_foff = 0;
    uint64_t dynstr_foff = 0, rela_plt_foff = 0;
    uint64_t dynamic_foff = 0, dynamic_vaddr = 0;

    if (need_dynamic) {
        uint64_t off = sizeof(Elf64_Ehdr) + 4 * sizeof(Elf64_Phdr);
        interp_foff = off;  off += interp_sec.size;
        off = lnk_align_up(off, 4);
        hash_foff = off;    off += hash_sec.size;
        off = lnk_align_up(off, 8);
        dynsym_foff = off;  off += dynsym_sec.size;
        dynstr_foff = off;  off += dynstr_sec.size;
        off = lnk_align_up(off, 8);
        rela_plt_foff = off; off += rela_plt_sec.size;
        off = lnk_align_up(off, 16);
        text_file_off = off;
    } else {
        text_file_off = sizeof(Elf64_Ehdr) + 2 * sizeof(Elf64_Phdr);
    }

    text_vaddr = BASE_ADDR + text_file_off;
    text_size  = l->text.size;

    data_file_off = lnk_align_up(text_file_off + text_size, PAGE_SIZE);
    data_vaddr    = BASE_ADDR + data_file_off;
    data_size     = l->data.size;

    if (need_dynamic) {
        dynamic_foff  = data_file_off + data_size;
        dynamic_vaddr = BASE_ADDR + dynamic_foff;
        bss_vaddr     = dynamic_vaddr + dyn_sec.size;
    } else {
        bss_vaddr = data_vaddr + data_size;
    }

    entry_point = text_vaddr;  /* _start is at offset 0 in .text */

    /* Compute final virtual addresses for all symbols */
    for (i = 0; i < l->sym_count; i++) {
        switch (l->symbols[i].section) {
        case SEC_TEXT:
            l->symbols[i].value += text_vaddr;
            break;
        case SEC_DATA:
            l->symbols[i].value += data_vaddr;
            break;
        case SEC_BSS:
            l->symbols[i].value += bss_vaddr;
            break;
        default:
            break;
        }
    }

    /* ---- 4a. Fill PLT/GOT with final addresses ---- */
    if (need_dynamic) {
        uint64_t plt_va = text_vaddr + plt_text_off;
        uint64_t got_va = data_vaddr + got_data_off;
        int32_t d;

        /* PLT0: push [GOT+8](%rip), jmp *[GOT+16](%rip), nop4 */
        {
            unsigned char *p0 = l->text.data + plt_text_off;
            p0[0] = 0xFF; p0[1] = 0x35;
            d = (int32_t)((int64_t)(got_va + 8) - (int64_t)(plt_va + 6));
            memcpy(p0 + 2, &d, 4);
            p0[6] = 0xFF; p0[7] = 0x25;
            d = (int32_t)((int64_t)(got_va + 16) - (int64_t)(plt_va + 12));
            memcpy(p0 + 8, &d, 4);
            p0[12] = 0x0F; p0[13] = 0x1F; p0[14] = 0x40; p0[15] = 0x00;
        }

        /* PLTn entries: jmp *GOT[3+n](%rip), push $n, jmp PLT0 */
        for (i = 0; i < (size_t)dyn_count; i++) {
            unsigned char *pn = l->text.data + plt_text_off + 16 + i * 16;
            uint64_t pn_va = plt_va + 16 + i * 16;
            uint64_t gn_va = got_va + (3 + i) * 8;

            pn[0] = 0xFF; pn[1] = 0x25;
            d = (int32_t)((int64_t)gn_va - (int64_t)(pn_va + 6));
            memcpy(pn + 2, &d, 4);
            pn[6] = 0x68;
            { uint32_t ri = (uint32_t)i; memcpy(pn + 7, &ri, 4); }
            pn[11] = 0xE9;
            d = (int32_t)((int64_t)plt_va - (int64_t)(pn_va + 16));
            memcpy(pn + 12, &d, 4);
        }

        /* GOT.PLT entries */
        {
            size_t goff = got_data_off;
            uint64_t val;
            val = dynamic_vaddr;
            memcpy(l->data.data + goff, &val, 8); goff += 8;
            val = 0;
            memcpy(l->data.data + goff, &val, 8); goff += 8;
            memcpy(l->data.data + goff, &val, 8); goff += 8;
            for (i = 0; i < (size_t)dyn_count; i++) {
                val = plt_va + 16 + i * 16 + 6; /* pushq instruction */
                memcpy(l->data.data + goff, &val, 8); goff += 8;
            }
        }

        /* .rela.plt entries */
        for (i = 0; i < (size_t)dyn_count; i++) {
            unsigned char *rp = rela_plt_sec.data + i * 24;
            uint64_t r_off = got_va + (3 + i) * 8;
            uint64_t r_inf = ((uint64_t)(i + 1) << 32) | 7;
            int64_t  r_add = 0;
            memcpy(rp,      &r_off, 8);
            memcpy(rp + 8,  &r_inf, 8);
            memcpy(rp + 16, &r_add, 8);
        }

        /* .dynamic entries */
        {
            unsigned char *dp = dyn_sec.data;
            size_t di = 0;
            int64_t tag; uint64_t val;

            tag=1;  val=libc_name_off;
            memcpy(dp+di,&tag,8); memcpy(dp+di+8,&val,8); di+=16;
            tag=4;  val=BASE_ADDR+hash_foff;
            memcpy(dp+di,&tag,8); memcpy(dp+di+8,&val,8); di+=16;
            tag=5;  val=BASE_ADDR+dynstr_foff;
            memcpy(dp+di,&tag,8); memcpy(dp+di+8,&val,8); di+=16;
            tag=6;  val=BASE_ADDR+dynsym_foff;
            memcpy(dp+di,&tag,8); memcpy(dp+di+8,&val,8); di+=16;
            tag=10; val=dynstr_sec.size;
            memcpy(dp+di,&tag,8); memcpy(dp+di+8,&val,8); di+=16;
            tag=11; val=24;
            memcpy(dp+di,&tag,8); memcpy(dp+di+8,&val,8); di+=16;
            tag=3;  val=got_va;
            memcpy(dp+di,&tag,8); memcpy(dp+di+8,&val,8); di+=16;
            tag=2;  val=rela_plt_sec.size;
            memcpy(dp+di,&tag,8); memcpy(dp+di+8,&val,8); di+=16;
            tag=20; val=7;
            memcpy(dp+di,&tag,8); memcpy(dp+di+8,&val,8); di+=16;
            tag=23; val=BASE_ADDR+rela_plt_foff;
            memcpy(dp+di,&tag,8); memcpy(dp+di+8,&val,8); di+=16;
            tag=0;  val=0;
            memcpy(dp+di,&tag,8); memcpy(dp+di+8,&val,8); di+=16;
        }
    }

    /* ---- 5b. Build DWARF debug sections (if -g data present) ----- */
    Buffer debug_abbrev, debug_info, debug_line;
    int has_debug_output = (l->debug_line_count > 0 && l->debug_source_file);
    buffer_init(&debug_abbrev);
    buffer_init(&debug_info);
    buffer_init(&debug_line);
    if (has_debug_output) {
        build_dwarf_sections(l, text_vaddr, text_size,
                             &debug_abbrev, &debug_info, &debug_line);
    }

    /* ---- 6. Patch _start call displacement ----------------------- */
    {
        int main_idx = linker_find_global(l, "main");
        if (main_idx < 0) {
            fprintf(stderr, "linker: undefined symbol: main\n");
            return 1;
        }
        uint64_t main_addr = l->symbols[main_idx].value;
        /* The call instruction is at text_vaddr + START_CALL_DISP_OFF - 1.
         * Its next IP = text_vaddr + START_CALL_NEXT_IP.
         * Displacement = target - next_IP. */
        int32_t disp = (int32_t)(main_addr - (text_vaddr + START_CALL_NEXT_IP));
        memcpy(l->text.data + START_CALL_DISP_OFF, &disp, 4);
    }

    /* ---- 7. Apply relocations ------------------------------------ */
    for (i = 0; i < l->reloc_count; i++) {
        LinkReloc *r = &l->relocs[i];

        uint64_t S = l->symbols[r->sym_index].value;
        int64_t  A = r->addend;
        uint64_t P;
        unsigned char *patch;

        if (r->section == SEC_TEXT) {
            P     = text_vaddr + r->offset;
            patch = l->text.data + r->offset;
        } else {
            P     = data_vaddr + r->offset;
            patch = l->data.data + r->offset;
        }

        switch (r->type) {
        case ELF_R_X86_64_64: {
            uint64_t val = S + (uint64_t)A;
            memcpy(patch, &val, 8);
            break;
        }
        case ELF_R_X86_64_PC32:
        case ELF_R_X86_64_PLT32: {
            int64_t val = (int64_t)(S + (uint64_t)A) - (int64_t)P;
            if (val < -2147483648LL || val > 2147483647LL) {
                fprintf(stderr,
                        "linker: PC32 relocation overflow for '%s' "
                        "(delta=%lld)\n",
                        l->symbols[r->sym_index].name,
                        (long long)val);
                return 1;
            }
            int32_t v32 = (int32_t)val;
            memcpy(patch, &v32, 4);
            break;
        }
        case ELF_R_X86_64_32: {
            uint64_t val = S + (uint64_t)A;
            if (val > 0xFFFFFFFFULL) {
                fprintf(stderr,
                        "linker: R_X86_64_32 overflow for '%s'\n",
                        l->symbols[r->sym_index].name);
                return 1;
            }
            uint32_t v32 = (uint32_t)val;
            memcpy(patch, &v32, 4);
            break;
        }
        case ELF_R_X86_64_32S: {
            int64_t val = (int64_t)(S + (uint64_t)A);
            if (val < -2147483648LL || val > 2147483647LL) {
                fprintf(stderr,
                        "linker: R_X86_64_32S overflow for '%s'\n",
                        l->symbols[r->sym_index].name);
                return 1;
            }
            int32_t v32 = (int32_t)val;
            memcpy(patch, &v32, 4);
            break;
        }
        default:
            fprintf(stderr,
                    "linker: unsupported relocation type %u for '%s'\n",
                    r->type, l->symbols[r->sym_index].name);
            return 1;
        }
    }

    /* ---- 7. Write ELF executable --------------------------------- */
    FILE *f = fopen(output_path, "wb");
    if (!f) {
        fprintf(stderr, "linker: cannot create '%s'\n", output_path);
        return 1;
    }

    /* --- Compute debug / section header layout ---- */
    uint64_t dbg_abbrev_foff = 0, dbg_info_foff = 0, dbg_line_foff = 0;
    uint64_t dbg_shstrtab_foff = 0;
    uint64_t shdr_foff = 0;
    int shnum = 0;
    int shstrtab_idx = 0;
    uint32_t sn_text = 0, sn_data = 0;
    uint32_t sn_dabbrev = 0, sn_dinfo = 0, sn_dline = 0, sn_shstrtab = 0;
    uint32_t sn_symtab = 0, sn_strtab = 0;

    Buffer link_shstrtab;
    buffer_init(&link_shstrtab);
    Buffer link_symtab;
    buffer_init(&link_symtab);
    Buffer link_strtab;
    buffer_init(&link_strtab);

    uint64_t dbg_symtab_foff = 0, dbg_strtab_foff = 0;
    int symtab_shidx = 0, strtab_shidx = 0;
    uint32_t sym_first_global = 0;

    if (has_debug_output) {
        /*
         * Section headers:
         *   0: null
         *   1: .text
         *   2: .data
         *   3: .debug_abbrev
         *   4: .debug_info
         *   5: .debug_line
         *   6: .symtab
         *   7: .strtab
         *   8: .shstrtab
         */
        shnum = 9;
        shstrtab_idx = 8;
        symtab_shidx = 6;
        strtab_shidx = 7;

        buffer_write_byte(&link_shstrtab, 0);
        sn_text = (uint32_t)link_shstrtab.size;
        buffer_write_bytes(&link_shstrtab, ".text", 6);
        sn_data = (uint32_t)link_shstrtab.size;
        buffer_write_bytes(&link_shstrtab, ".data", 6);
        sn_dabbrev = (uint32_t)link_shstrtab.size;
        buffer_write_bytes(&link_shstrtab, ".debug_abbrev", 14);
        sn_dinfo = (uint32_t)link_shstrtab.size;
        buffer_write_bytes(&link_shstrtab, ".debug_info", 12);
        sn_dline = (uint32_t)link_shstrtab.size;
        buffer_write_bytes(&link_shstrtab, ".debug_line", 12);
        sn_symtab = (uint32_t)link_shstrtab.size;
        buffer_write_bytes(&link_shstrtab, ".symtab", 8);
        sn_strtab = (uint32_t)link_shstrtab.size;
        buffer_write_bytes(&link_shstrtab, ".strtab", 8);
        sn_shstrtab = (uint32_t)link_shstrtab.size;
        buffer_write_bytes(&link_shstrtab, ".shstrtab", 10);

        /* Build .symtab and .strtab from linker symbols */
        buffer_write_byte(&link_strtab, 0); /* null string at offset 0 */
        {
            /* Null symbol entry (24 bytes for 64-bit ELF) */
            Elf64_Sym null_sym;
            memset(&null_sym, 0, sizeof(null_sym));
            buffer_write_bytes(&link_symtab, &null_sym, sizeof(null_sym));
        }

        /* First pass: local symbols */
        uint32_t eidx = 1;
        for (i = 0; i < l->sym_count; i++) {
            if (l->symbols[i].binding != ELF_STB_LOCAL) continue;
            if (l->symbols[i].name[0] == '\0') continue;
            Elf64_Sym sym;
            memset(&sym, 0, sizeof(sym));
            sym.st_name = (uint32_t)link_strtab.size;
            buffer_write_bytes(&link_strtab, l->symbols[i].name, strlen(l->symbols[i].name) + 1);
            sym.st_info = ELF64_ST_INFO(ELF_STB_LOCAL, l->symbols[i].type);
            sym.st_other = ELF_STV_DEFAULT;
            if (l->symbols[i].section == SEC_TEXT)      sym.st_shndx = 1;
            else if (l->symbols[i].section == SEC_DATA) sym.st_shndx = 2;
            else                                         sym.st_shndx = ELF_SHN_ABS;
            sym.st_value = l->symbols[i].value;
            sym.st_size  = l->symbols[i].size;
            buffer_write_bytes(&link_symtab, &sym, sizeof(sym));
            eidx++;
        }
        sym_first_global = eidx;

        /* Second pass: global symbols */
        for (i = 0; i < l->sym_count; i++) {
            if (l->symbols[i].binding == ELF_STB_LOCAL) continue;
            if (l->symbols[i].name[0] == '\0') continue;
            Elf64_Sym sym;
            memset(&sym, 0, sizeof(sym));
            sym.st_name = (uint32_t)link_strtab.size;
            buffer_write_bytes(&link_strtab, l->symbols[i].name, strlen(l->symbols[i].name) + 1);
            sym.st_info = ELF64_ST_INFO(l->symbols[i].binding, l->symbols[i].type);
            sym.st_other = ELF_STV_DEFAULT;
            if (l->symbols[i].section == SEC_TEXT)      sym.st_shndx = 1;
            else if (l->symbols[i].section == SEC_DATA) sym.st_shndx = 2;
            else if (l->symbols[i].section == SEC_BSS)  sym.st_shndx = ELF_SHN_ABS;
            else                                         sym.st_shndx = ELF_SHN_UNDEF;
            sym.st_value = l->symbols[i].value;
            sym.st_size  = l->symbols[i].size;
            buffer_write_bytes(&link_symtab, &sym, sizeof(sym));
            eidx++;
        }

        uint64_t doff;
        if (need_dynamic)
            doff = dynamic_foff + dyn_sec.size;
        else
            doff = data_file_off + data_size;

        doff = lnk_align_up(doff, 4);
        dbg_abbrev_foff = doff; doff += debug_abbrev.size;
        doff = lnk_align_up(doff, 4);
        dbg_info_foff = doff;   doff += debug_info.size;
        doff = lnk_align_up(doff, 4);
        dbg_line_foff = doff;   doff += debug_line.size;
        doff = lnk_align_up(doff, 8);
        dbg_symtab_foff = doff; doff += link_symtab.size;
        dbg_strtab_foff = doff; doff += link_strtab.size;
        dbg_shstrtab_foff = doff; doff += link_shstrtab.size;
        doff = lnk_align_up(doff, 8);
        shdr_foff = doff;
    }

    /* --- ELF header --- */
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
    ehdr.e_type      = ELF_ET_EXEC;
    ehdr.e_machine   = ELF_EM_X86_64;
    ehdr.e_version   = ELF_EV_CURRENT;
    ehdr.e_entry     = entry_point;
    ehdr.e_phoff     = sizeof(Elf64_Ehdr);
    ehdr.e_shoff     = shdr_foff;
    ehdr.e_flags     = 0;
    ehdr.e_ehsize    = sizeof(Elf64_Ehdr);
    ehdr.e_phentsize = sizeof(Elf64_Phdr);
    ehdr.e_phnum     = (uint16_t)(need_dynamic ? 4 : 2);
    ehdr.e_shentsize = sizeof(Elf64_Shdr);
    ehdr.e_shnum     = (uint16_t)shnum;
    ehdr.e_shstrndx  = (uint16_t)shstrtab_idx;

    fwrite(&ehdr, sizeof(ehdr), 1, f);

    /* --- Program headers --- */
    Elf64_Phdr phdr;

    /* PT_INTERP (dynamic only) */
    if (need_dynamic) {
        memset(&phdr, 0, sizeof(phdr));
        phdr.p_type   = ELF_PT_INTERP;
        phdr.p_flags  = ELF_PF_R;
        phdr.p_offset = interp_foff;
        phdr.p_vaddr  = BASE_ADDR + interp_foff;
        phdr.p_paddr  = BASE_ADDR + interp_foff;
        phdr.p_filesz = interp_sec.size;
        phdr.p_memsz  = interp_sec.size;
        phdr.p_align  = 1;
        fwrite(&phdr, sizeof(phdr), 1, f);
    }

    /* PT_LOAD: text segment (R+X) — includes ELF header + phdrs */
    memset(&phdr, 0, sizeof(phdr));
    phdr.p_type   = ELF_PT_LOAD;
    phdr.p_flags  = ELF_PF_R | ELF_PF_X;
    phdr.p_offset = 0;
    phdr.p_vaddr  = BASE_ADDR;
    phdr.p_paddr  = BASE_ADDR;
    phdr.p_filesz = text_file_off + text_size;
    phdr.p_memsz  = text_file_off + text_size;
    phdr.p_align  = PAGE_SIZE;
    fwrite(&phdr, sizeof(phdr), 1, f);

    /* PT_LOAD: data segment (R+W) */
    memset(&phdr, 0, sizeof(phdr));
    phdr.p_type   = ELF_PT_LOAD;
    phdr.p_flags  = ELF_PF_R | ELF_PF_W;
    phdr.p_offset = data_file_off;
    phdr.p_vaddr  = data_vaddr;
    phdr.p_paddr  = data_vaddr;
    if (need_dynamic) {
        phdr.p_filesz = data_size + dyn_sec.size;
        phdr.p_memsz  = data_size + dyn_sec.size + l->bss_size;
    } else {
        phdr.p_filesz = data_size;
        phdr.p_memsz  = data_size + l->bss_size;
    }
    phdr.p_align  = PAGE_SIZE;
    fwrite(&phdr, sizeof(phdr), 1, f);

    /* PT_DYNAMIC (dynamic only) */
    if (need_dynamic) {
        memset(&phdr, 0, sizeof(phdr));
        phdr.p_type   = ELF_PT_DYNAMIC;
        phdr.p_flags  = ELF_PF_R | ELF_PF_W;
        phdr.p_offset = dynamic_foff;
        phdr.p_vaddr  = dynamic_vaddr;
        phdr.p_paddr  = dynamic_vaddr;
        phdr.p_filesz = dyn_sec.size;
        phdr.p_memsz  = dyn_sec.size;
        phdr.p_align  = 8;
        fwrite(&phdr, sizeof(phdr), 1, f);
    }

    /* --- Dynamic linking sections (before .text) --- */
    if (need_dynamic) {
        /* Pad to .interp offset */
        { long cur = ftell(f); while ((uint64_t)cur < interp_foff)  { fputc(0, f); cur++; } }
        fwrite(interp_sec.data,   1, interp_sec.size,   f);

        /* .hash */
        { long cur = ftell(f); while ((uint64_t)cur < hash_foff)    { fputc(0, f); cur++; } }
        fwrite(hash_sec.data,     1, hash_sec.size,     f);

        /* .dynsym */
        { long cur = ftell(f); while ((uint64_t)cur < dynsym_foff)  { fputc(0, f); cur++; } }
        fwrite(dynsym_sec.data,   1, dynsym_sec.size,   f);

        /* .dynstr (immediately after .dynsym, no extra padding) */
        fwrite(dynstr_sec.data,   1, dynstr_sec.size,   f);

        /* .rela.plt */
        { long cur = ftell(f); while ((uint64_t)cur < rela_plt_foff) { fputc(0, f); cur++; } }
        fwrite(rela_plt_sec.data, 1, rela_plt_sec.size, f);
    }

    /* Pad to .text offset */
    { long cur = ftell(f); while ((uint64_t)cur < text_file_off) { fputc(0, f); cur++; } }

    /* --- .text section content --- */
    if (text_size > 0)
        fwrite(l->text.data, 1, (size_t)text_size, f);

    /* --- Padding to data_file_off --- */
    {
        long cur = ftell(f);
        while ((uint64_t)cur < data_file_off) {
            fputc(0, f);
            cur++;
        }
    }

    /* --- .data section content --- */
    if (data_size > 0)
        fwrite(l->data.data, 1, (size_t)data_size, f);

    /* --- .dynamic section (dynamic only) --- */
    if (need_dynamic)
        fwrite(dyn_sec.data, 1, dyn_sec.size, f);

    /* --- Debug sections and section headers (when -g) --- */
    if (has_debug_output) {
        /* Write debug section data */
        { long cur = ftell(f); while ((uint64_t)cur < dbg_abbrev_foff) { fputc(0, f); cur++; } }
        fwrite(debug_abbrev.data, 1, debug_abbrev.size, f);

        { long cur = ftell(f); while ((uint64_t)cur < dbg_info_foff) { fputc(0, f); cur++; } }
        fwrite(debug_info.data, 1, debug_info.size, f);

        { long cur = ftell(f); while ((uint64_t)cur < dbg_line_foff) { fputc(0, f); cur++; } }
        fwrite(debug_line.data, 1, debug_line.size, f);

        /* .symtab */
        { long cur = ftell(f); while ((uint64_t)cur < dbg_symtab_foff) { fputc(0, f); cur++; } }
        fwrite(link_symtab.data, 1, link_symtab.size, f);

        /* .strtab */
        fwrite(link_strtab.data, 1, link_strtab.size, f);

        /* .shstrtab */
        fwrite(link_shstrtab.data, 1, link_shstrtab.size, f);

        /* Write section headers */
        { long cur = ftell(f); while ((uint64_t)cur < shdr_foff) { fputc(0, f); cur++; } }

        /* Section 0: null */
        {
            Elf64_Shdr sh;
            memset(&sh, 0, sizeof(sh));
            fwrite(&sh, sizeof(sh), 1, f);
        }

        /* Section 1: .text */
        {
            Elf64_Shdr sh;
            memset(&sh, 0, sizeof(sh));
            sh.sh_name      = sn_text;
            sh.sh_type      = ELF_SHT_PROGBITS;
            sh.sh_flags     = ELF_SHF_ALLOC | ELF_SHF_EXECINSTR;
            sh.sh_addr      = text_vaddr;
            sh.sh_offset    = text_file_off;
            sh.sh_size      = text_size;
            sh.sh_addralign = 16;
            fwrite(&sh, sizeof(sh), 1, f);
        }

        /* Section 2: .data */
        {
            Elf64_Shdr sh;
            memset(&sh, 0, sizeof(sh));
            sh.sh_name      = sn_data;
            sh.sh_type      = ELF_SHT_PROGBITS;
            sh.sh_flags     = ELF_SHF_ALLOC | ELF_SHF_WRITE;
            sh.sh_addr      = data_vaddr;
            sh.sh_offset    = data_file_off;
            sh.sh_size      = data_size;
            sh.sh_addralign = 8;
            fwrite(&sh, sizeof(sh), 1, f);
        }

        /* Section 3: .debug_abbrev */
        {
            Elf64_Shdr sh;
            memset(&sh, 0, sizeof(sh));
            sh.sh_name      = sn_dabbrev;
            sh.sh_type      = ELF_SHT_PROGBITS;
            sh.sh_offset    = dbg_abbrev_foff;
            sh.sh_size      = debug_abbrev.size;
            sh.sh_addralign = 1;
            fwrite(&sh, sizeof(sh), 1, f);
        }

        /* Section 4: .debug_info */
        {
            Elf64_Shdr sh;
            memset(&sh, 0, sizeof(sh));
            sh.sh_name      = sn_dinfo;
            sh.sh_type      = ELF_SHT_PROGBITS;
            sh.sh_offset    = dbg_info_foff;
            sh.sh_size      = debug_info.size;
            sh.sh_addralign = 1;
            fwrite(&sh, sizeof(sh), 1, f);
        }

        /* Section 5: .debug_line */
        {
            Elf64_Shdr sh;
            memset(&sh, 0, sizeof(sh));
            sh.sh_name      = sn_dline;
            sh.sh_type      = ELF_SHT_PROGBITS;
            sh.sh_offset    = dbg_line_foff;
            sh.sh_size      = debug_line.size;
            sh.sh_addralign = 1;
            fwrite(&sh, sizeof(sh), 1, f);
        }

        /* Section 6: .symtab */
        {
            Elf64_Shdr sh;
            memset(&sh, 0, sizeof(sh));
            sh.sh_name      = sn_symtab;
            sh.sh_type      = ELF_SHT_SYMTAB;
            sh.sh_offset    = dbg_symtab_foff;
            sh.sh_size      = link_symtab.size;
            sh.sh_link      = (uint32_t)strtab_shidx;
            sh.sh_info      = sym_first_global;
            sh.sh_addralign = 8;
            sh.sh_entsize   = sizeof(Elf64_Sym);
            fwrite(&sh, sizeof(sh), 1, f);
        }

        /* Section 7: .strtab */
        {
            Elf64_Shdr sh;
            memset(&sh, 0, sizeof(sh));
            sh.sh_name      = sn_strtab;
            sh.sh_type      = ELF_SHT_STRTAB;
            sh.sh_offset    = dbg_strtab_foff;
            sh.sh_size      = link_strtab.size;
            sh.sh_addralign = 1;
            fwrite(&sh, sizeof(sh), 1, f);
        }

        /* Section 8: .shstrtab */
        {
            Elf64_Shdr sh;
            memset(&sh, 0, sizeof(sh));
            sh.sh_name      = sn_shstrtab;
            sh.sh_type      = ELF_SHT_STRTAB;
            sh.sh_offset    = dbg_shstrtab_foff;
            sh.sh_size      = link_shstrtab.size;
            sh.sh_addralign = 1;
            fwrite(&sh, sizeof(sh), 1, f);
        }
    }

    fclose(f);

    /* Make the file executable (chmod +x) */
#ifndef _WIN32
    {
        char cmd[600];
        snprintf(cmd, sizeof(cmd), "chmod +x \"%s\"", output_path);
        system(cmd);
    }
#endif

    /* Cleanup dynamic linking buffers */
    if (need_dynamic) {
        buffer_free(&interp_sec);
        buffer_free(&hash_sec);
        buffer_free(&dynsym_sec);
        buffer_free(&dynstr_sec);
        buffer_free(&rela_plt_sec);
        buffer_free(&dyn_sec);
        free(dyn_name_offs);
    }
    free(dyn_indices);
    buffer_free(&debug_abbrev);
    buffer_free(&debug_info);
    buffer_free(&debug_line);
    buffer_free(&link_shstrtab);
    buffer_free(&link_symtab);
    buffer_free(&link_strtab);

    printf("Linked: %s  (text=%lu, data=%lu, bss=%lu%s%s)\n",
           output_path,
           (unsigned long)text_size,
           (unsigned long)data_size,
           (unsigned long)l->bss_size,
           need_dynamic ? ", dynamic" : "",
           has_debug_output ? ", debug" : "");

    return 0;
}
