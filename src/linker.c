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
    ehdr.e_phoff     = sizeof(Elf64_Ehdr);       /* right after header */
    ehdr.e_shoff     = 0;                         /* no section headers */
    ehdr.e_flags     = 0;
    ehdr.e_ehsize    = sizeof(Elf64_Ehdr);
    ehdr.e_phentsize = sizeof(Elf64_Phdr);
    ehdr.e_phnum     = (uint16_t)(need_dynamic ? 4 : 2);
    ehdr.e_shentsize = sizeof(Elf64_Shdr);
    ehdr.e_shnum     = 0;
    ehdr.e_shstrndx  = 0;

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

    printf("Linked: %s  (text=%lu, data=%lu, bss=%lu%s)\n",
           output_path,
           (unsigned long)text_size,
           (unsigned long)data_size,
           (unsigned long)l->bss_size,
           need_dynamic ? ", dynamic" : "");

    return 0;
}
