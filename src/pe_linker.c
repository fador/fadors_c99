/*
 * pe_linker.c — Minimal PE/COFF linker for x86-64 Windows
 *
 * Merges COFF relocatable objects (.obj) into a PE executable.
 * Generates import tables for dynamic linking against Windows DLLs
 * (kernel32.dll by default for ExitProcess).
 *
 * Produces a non-PIE console executable with:
 *   .text  — executable code
 *   .rdata — read-only data (import tables)
 *   .data  — initialized data
 *   .bss   — uninitialized data (virtual only)
 *
 * Supported COFF relocation types:
 *   IMAGE_REL_AMD64_ADDR64  (0x0001) — 64-bit absolute
 *   IMAGE_REL_AMD64_ADDR32NB(0x0003) — 32-bit RVA (image-base relative)
 *   IMAGE_REL_AMD64_REL32   (0x0004) — 32-bit RIP-relative
 */

#include "pe_linker.h"
#include "pe.h"
#include "coff.h"
#include "buffer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Constants                                                         */
/* ------------------------------------------------------------------ */

#define PE_DEFAULT_IMAGE_BASE   0x00400000ULL
#define PE_SECTION_ALIGNMENT    0x1000U
#define PE_FILE_ALIGNMENT       0x200U
#define PE_DEFAULT_STACK_RESERVE 0x800000ULL  /* 8 MB */
#define PE_DEFAULT_STACK_COMMIT  0x1000ULL    /* 4 KB */
#define PE_DEFAULT_HEAP_RESERVE  0x100000ULL  /* 1 MB */
#define PE_DEFAULT_HEAP_COMMIT   0x1000ULL    /* 4 KB */

/* Short aliases */
#define SEC_UNDEF PE_LINK_SEC_UNDEF
#define SEC_TEXT  PE_LINK_SEC_TEXT
#define SEC_DATA  PE_LINK_SEC_DATA
#define SEC_BSS   PE_LINK_SEC_BSS
#define SEC_RDATA PE_LINK_SEC_RDATA

/* ------------------------------------------------------------------ */
/*  Helpers                                                           */
/* ------------------------------------------------------------------ */

static uint32_t pe_align_up32(uint32_t v, uint32_t a) {
    return (v + a - 1) & ~(a - 1);
}

#ifndef _MSC_VER
#define pe_strdup strdup
#else
#define pe_strdup _strdup
#endif

/* ------------------------------------------------------------------ */
/*  Symbol management                                                 */
/* ------------------------------------------------------------------ */

static int pe_find_global(PELinker *l, const char *name) {
    size_t i;
    for (i = 0; i < l->sym_count; i++) {
        if (l->symbols[i].storage_class != PE_SYM_CLASS_STATIC &&
            strcmp(l->symbols[i].name, name) == 0)
            return (int)i;
    }
    return -1;
}

static uint32_t pe_add_sym(PELinker *l, const char *name,
                           uint64_t value, int section,
                           uint8_t storage_class, uint16_t type) {
    if (l->sym_count >= l->sym_cap) {
        l->sym_cap = l->sym_cap ? l->sym_cap * 2 : 256;
        l->symbols = realloc(l->symbols, l->sym_cap * sizeof(PELinkSymbol));
    }
    PELinkSymbol *s = &l->symbols[l->sym_count];
    s->name          = pe_strdup(name);
    s->value         = value;
    s->section       = section;
    s->storage_class = storage_class;
    s->type          = type;
    s->size          = 0;
    return (uint32_t)l->sym_count++;
}

/* ------------------------------------------------------------------ */
/*  Relocation management                                             */
/* ------------------------------------------------------------------ */

static void pe_add_reloc(PELinker *l, uint64_t offset, int section,
                         uint32_t sym_index, uint32_t type) {
    if (l->reloc_count >= l->reloc_cap) {
        l->reloc_cap = l->reloc_cap ? l->reloc_cap * 2 : 256;
        l->relocs = realloc(l->relocs, l->reloc_cap * sizeof(PELinkReloc));
    }
    PELinkReloc *r = &l->relocs[l->reloc_count++];
    r->offset    = offset;
    r->section   = section;
    r->sym_index = sym_index;
    r->type      = type;
}

/* ------------------------------------------------------------------ */
/*  Import management                                                 */
/* ------------------------------------------------------------------ */

static PEImportDll *pe_find_or_add_dll(PELinker *l, const char *dll_name) {
    size_t i;
    for (i = 0; i < l->dll_count; i++) {
        if (strcmp(l->import_dlls[i].dll_name, dll_name) == 0)
            return &l->import_dlls[i];
    }
    if (l->dll_count >= l->dll_cap) {
        l->dll_cap = l->dll_cap ? l->dll_cap * 2 : 8;
        l->import_dlls = realloc(l->import_dlls, l->dll_cap * sizeof(PEImportDll));
    }
    PEImportDll *d = &l->import_dlls[l->dll_count++];
    d->dll_name = pe_strdup(dll_name);
    d->import_indices = NULL;
    d->count = 0;
    d->cap = 0;
    return d;
}

static void pe_dll_add_import(PEImportDll *d, size_t import_idx) {
    if (d->count >= d->cap) {
        d->cap = d->cap ? d->cap * 2 : 16;
        d->import_indices = realloc(d->import_indices, d->cap * sizeof(size_t));
    }
    d->import_indices[d->count++] = import_idx;
}

/* ------------------------------------------------------------------ */
/*  Public: create / destroy                                          */
/* ------------------------------------------------------------------ */

PELinker *pe_linker_new(void) {
    PELinker *l = calloc(1, sizeof(PELinker));
    buffer_init(&l->text);
    buffer_init(&l->data);
    buffer_init(&l->rdata);
    l->subsystem     = PE_SUBSYSTEM_CONSOLE;
    l->stack_reserve = PE_DEFAULT_STACK_RESERVE;
    l->image_base    = PE_DEFAULT_IMAGE_BASE;
    l->entry_name    = "main";
    return l;
}

void pe_linker_free(PELinker *l) {
    size_t i;
    if (!l) return;
    buffer_free(&l->text);
    buffer_free(&l->data);
    buffer_free(&l->rdata);
    for (i = 0; i < l->sym_count; i++)
        free(l->symbols[i].name);
    free(l->symbols);
    free(l->relocs);
    for (i = 0; i < l->import_count; i++) {
        free(l->imports[i].func_name);
        free(l->imports[i].dll_name);
    }
    free(l->imports);
    for (i = 0; i < l->dll_count; i++) {
        free(l->import_dlls[i].dll_name);
        free(l->import_dlls[i].import_indices);
    }
    free(l->import_dlls);
    free(l);
}

/* ------------------------------------------------------------------ */
/*  Public: import declaration                                        */
/* ------------------------------------------------------------------ */

void pe_linker_add_import(PELinker *l, const char *func_name,
                          const char *dll_name, uint16_t hint) {
    if (l->import_count >= l->import_cap) {
        l->import_cap = l->import_cap ? l->import_cap * 2 : 32;
        l->imports = realloc(l->imports, l->import_cap * sizeof(PEImportEntry));
    }
    PEImportEntry *e = &l->imports[l->import_count];
    e->func_name = pe_strdup(func_name);
    e->dll_name  = pe_strdup(dll_name);
    e->sym_index = (uint32_t)-1;
    e->hint      = hint;

    /* Group into DLL */
    PEImportDll *d = pe_find_or_add_dll(l, dll_name);
    pe_dll_add_import(d, l->import_count);

    l->import_count++;
}

/* ------------------------------------------------------------------ */
/*  COFF .obj reader                                                  */
/* ------------------------------------------------------------------ */

static int read_coff_object(PELinker *l, const unsigned char *data,
                            size_t file_size, const char *filename) {
    size_t i;

    /* --- 1. Validate COFF header ---------------------------------- */
    if (file_size < sizeof(COFFHeader)) {
        fprintf(stderr, "pe_linker: %s: file too small\n", filename);
        return -1;
    }
    const COFFHeader *hdr = (const COFFHeader *)data;
    if (hdr->Machine != IMAGE_FILE_MACHINE_AMD64) {
        fprintf(stderr, "pe_linker: %s: not an AMD64 COFF object (machine=0x%04X)\n",
                filename, hdr->Machine);
        return -1;
    }

    int num_sec = hdr->NumberOfSections;
    const COFFSectionHeader *shdrs = (const COFFSectionHeader *)(data + sizeof(COFFHeader));

    /* --- 2. Locate symbol table and string table ------------------ */
    const COFFSymbol *symtab = NULL;
    const char *strtab = NULL;
    int sym_count = 0;

    if (hdr->PointerToSymbolTable != 0) {
        symtab = (const COFFSymbol *)(data + hdr->PointerToSymbolTable);
        sym_count = (int)hdr->NumberOfSymbols;
        /* String table immediately follows symbol table */
        strtab = (const char *)(data + hdr->PointerToSymbolTable +
                                (size_t)sym_count * sizeof(COFFSymbol));
    }

    /* --- 3. Get section name helper ------------------------------- */
    /* COFF section names > 8 chars use /N format pointing to strtab */

    /* --- 4. Build section-index → linker mapping ------------------- */
    int   *sec_id   = calloc((size_t)num_sec, sizeof(int));
    size_t *sec_base = calloc((size_t)num_sec, sizeof(size_t));

    for (i = 0; i < (size_t)num_sec; i++) {
        char sec_name[16];
        memset(sec_name, 0, sizeof(sec_name));

        if (shdrs[i].Name[0] == '/') {
            /* Long name: /offset into string table */
            int off = atoi((const char *)shdrs[i].Name + 1);
            if (strtab) strncpy(sec_name, strtab + off, 15);
        } else {
            memcpy(sec_name, shdrs[i].Name, 8);
        }

        uint32_t chars = shdrs[i].Characteristics;
        if (strcmp(sec_name, ".text") == 0 ||
            (chars & IMAGE_SCN_CNT_CODE)) {
            buffer_pad(&l->text, 16);
            sec_base[i] = l->text.size;
            sec_id[i] = SEC_TEXT;
            if (shdrs[i].SizeOfRawData > 0 && shdrs[i].PointerToRawData > 0)
                buffer_write_bytes(&l->text,
                                   data + shdrs[i].PointerToRawData,
                                   shdrs[i].SizeOfRawData);
        } else if (strcmp(sec_name, ".data") == 0 ||
                   (chars & IMAGE_SCN_CNT_INITIALIZED_DATA &&
                    chars & IMAGE_SCN_MEM_WRITE)) {
            buffer_pad(&l->data, 8);
            sec_base[i] = l->data.size;
            sec_id[i] = SEC_DATA;
            if (shdrs[i].SizeOfRawData > 0 && shdrs[i].PointerToRawData > 0)
                buffer_write_bytes(&l->data,
                                   data + shdrs[i].PointerToRawData,
                                   shdrs[i].SizeOfRawData);
        } else if (strcmp(sec_name, ".rdata") == 0 ||
                   (chars & IMAGE_SCN_CNT_INITIALIZED_DATA &&
                    !(chars & IMAGE_SCN_MEM_WRITE) &&
                    !(chars & IMAGE_SCN_CNT_CODE))) {
            buffer_pad(&l->rdata, 8);
            sec_base[i] = l->rdata.size;
            sec_id[i] = SEC_RDATA;
            if (shdrs[i].SizeOfRawData > 0 && shdrs[i].PointerToRawData > 0)
                buffer_write_bytes(&l->rdata,
                                   data + shdrs[i].PointerToRawData,
                                   shdrs[i].SizeOfRawData);
        } else if (strcmp(sec_name, ".bss") == 0 ||
                   (chars & PE_SCN_CNT_UNINITIALIZED_DATA)) {
            sec_base[i] = l->bss_size;
            sec_id[i] = SEC_BSS;
            l->bss_size += shdrs[i].VirtualSize ? shdrs[i].VirtualSize
                                                 : shdrs[i].SizeOfRawData;
        } else {
            sec_id[i] = SEC_UNDEF;
        }
    }

    /* --- 5. Process symbols ---------------------------------------- */
    uint32_t *sym_map = NULL;
    if (sym_count > 0) {
        sym_map = calloc((size_t)sym_count, sizeof(uint32_t));

        for (i = 0; i < (size_t)sym_count; i++) {
            const COFFSymbol *cs = &symtab[i];

            /* Get symbol name */
            char name_buf[256];
            if (cs->N.Name.Zeroes == 0) {
                /* Long name in string table */
                if (strtab)
                    strncpy(name_buf, strtab + cs->N.Name.Offset, 255);
                else
                    name_buf[0] = '\0';
            } else {
                memset(name_buf, 0, sizeof(name_buf));
                memcpy(name_buf, cs->N.ShortName, 8);
            }
            name_buf[255] = '\0';

            /* Skip aux symbols */
            if (cs->StorageClass == 0x67 /* IMAGE_SYM_CLASS_FILE */ ||
                name_buf[0] == '\0') {
                sym_map[i] = (uint32_t)-1;
                i += cs->NumberOfAuxSymbols;
                continue;
            }

            int section = SEC_UNDEF;
            uint64_t value = cs->Value;

            /* Map COFF section number (1-based) to our section ID */
            if (cs->SectionNumber > 0 && cs->SectionNumber <= num_sec) {
                int sec_idx = cs->SectionNumber - 1;
                section = sec_id[sec_idx];
                value += sec_base[sec_idx];
            }

            if (cs->StorageClass == IMAGE_SYM_CLASS_STATIC) {
                sym_map[i] = pe_add_sym(l, name_buf, value, section,
                                        PE_SYM_CLASS_STATIC, cs->Type);
            } else {
                /* External / global */
                int existing = pe_find_global(l, name_buf);
                if (existing >= 0) {
                    PELinkSymbol *es = &l->symbols[existing];
                    if (section != SEC_UNDEF && es->section == SEC_UNDEF) {
                        es->value   = value;
                        es->section = section;
                        es->type    = cs->Type;
                    }
                    sym_map[i] = (uint32_t)existing;
                } else {
                    sym_map[i] = pe_add_sym(l, name_buf, value, section,
                                            PE_SYM_CLASS_EXTERNAL, cs->Type);
                }
            }

            /* Skip aux symbols */
            i += cs->NumberOfAuxSymbols;
        }
    }

    /* --- 6. Process relocations ------------------------------------ */
    for (i = 0; i < (size_t)num_sec; i++) {
        if (sec_id[i] == SEC_UNDEF) continue;
        if (shdrs[i].NumberOfRelocations == 0) continue;
        if (shdrs[i].PointerToRelocations == 0) continue;

        const COFFRelocation *relocs =
            (const COFFRelocation *)(data + shdrs[i].PointerToRelocations);
        int rcount = shdrs[i].NumberOfRelocations;
        int r;

        for (r = 0; r < rcount; r++) {
            uint32_t orig_sym = relocs[r].SymbolTableIndex;
            if (orig_sym >= (uint32_t)sym_count) continue;
            uint32_t new_sym = sym_map ? sym_map[orig_sym] : (uint32_t)-1;
            if (new_sym == (uint32_t)-1) continue;

            pe_add_reloc(l,
                         (uint64_t)relocs[r].VirtualAddress + sec_base[i],
                         sec_id[i],
                         new_sym,
                         relocs[r].Type);
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

int pe_linker_add_object_file(PELinker *l, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "pe_linker: cannot open '%s'\n", path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *buf = malloc((size_t)sz);
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        fprintf(stderr, "pe_linker: read error on '%s'\n", path);
        free(buf);
        fclose(f);
        return -1;
    }
    fclose(f);

    int rc = read_coff_object(l, buf, (size_t)sz, path);
    free(buf);
    return rc;
}

/* ------------------------------------------------------------------ */
/*  Default imports                                                   */
/* ------------------------------------------------------------------ */

static void pe_add_default_imports(PELinker *l) {
    /* ExitProcess from kernel32.dll — used by our entry stub */
    pe_linker_add_import(l, "ExitProcess", "kernel32.dll", 0);
}

/* ------------------------------------------------------------------ */
/*  Resolve imports: match undefined symbols to import entries        */
/* ------------------------------------------------------------------ */

static void pe_resolve_imports(PELinker *l) {
    size_t i, j;

    /* For each undefined external symbol, check if there's an import */
    for (i = 0; i < l->sym_count; i++) {
        if (l->symbols[i].section != SEC_UNDEF) continue;
        if (l->symbols[i].storage_class == PE_SYM_CLASS_STATIC) continue;
        if (l->symbols[i].name[0] == '\0') continue;

        /* Check if already imported */
        int found = 0;
        for (j = 0; j < l->import_count; j++) {
            if (strcmp(l->imports[j].func_name, l->symbols[i].name) == 0) {
                l->imports[j].sym_index = (uint32_t)i;
                found = 1;
                break;
            }
        }

        if (!found) {
            /* Unknown undefined symbol — try to auto-import from common DLLs.
             * This is a simple heuristic: if the symbol is referenced by
             * a relocation, it might be a DLL import. For now, report error. */
            int referenced = 0;
            size_t k;
            for (k = 0; k < l->reloc_count; k++) {
                if (l->relocs[k].sym_index == (uint32_t)i) {
                    referenced = 1;
                    break;
                }
            }
            if (referenced) {
                fprintf(stderr, "pe_linker: undefined symbol: '%s'\n",
                        l->symbols[i].name);
            }
        }
    }

    /* Assign sym_index for imports that weren't matched yet */
    for (j = 0; j < l->import_count; j++) {
        if (l->imports[j].sym_index == (uint32_t)-1) {
            /* Create a placeholder undefined symbol */
            int existing = pe_find_global(l, l->imports[j].func_name);
            if (existing >= 0) {
                l->imports[j].sym_index = (uint32_t)existing;
            } else {
                l->imports[j].sym_index = pe_add_sym(
                    l, l->imports[j].func_name, 0, SEC_UNDEF,
                    PE_SYM_CLASS_EXTERNAL, 0);
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Build import tables in .rdata                                     */
/* ------------------------------------------------------------------ */

/*
 * PE import table layout in .rdata:
 *
 *   Import Directory Table (array of PE_ImportDescriptor, null-terminated)
 *   Import Lookup Table (ILT) — per DLL, array of 8-byte RVAs, null-term
 *   Import Address Table (IAT) — same layout as ILT (loader overwrites)
 *   Hint/Name Table — array of { uint16_t hint; char name[]; }
 *   DLL name strings
 */
typedef struct {
    uint32_t idt_rva;     /* RVA of import directory table */
    uint32_t idt_size;    /* Size of IDT */
    uint32_t iat_rva;     /* RVA of IAT */
    uint32_t iat_size;    /* Size of IAT */
    size_t   rdata_start; /* Offset into rdata buffer where import data begins */
} ImportTableInfo;

static ImportTableInfo pe_build_import_tables(PELinker *l,
                                              uint32_t rdata_rva_base) {
    ImportTableInfo info;
    memset(&info, 0, sizeof(info));

    if (l->import_count == 0) return info;

    size_t rdata_start = l->rdata.size;
    buffer_pad(&l->rdata, 8);
    rdata_start = l->rdata.size;
    info.rdata_start = rdata_start;

    /*
     * We'll compute offsets relative to rdata_start.
     * After sizing all components, convert to RVAs using rdata_rva_base.
     *
     * Layout order:
     *   1. IDT entries: (dll_count + 1) * 20 bytes  (null terminator)
     *   2. ILT entries: for each DLL, (count + 1) * 8 bytes
     *   3. IAT entries: same as ILT
     *   4. Hint/Name entries: variable
     *   5. DLL name strings: variable
     */

    /* Step 1: Calculate sizes */
    size_t idt_size = (l->dll_count + 1) * sizeof(PE_ImportDescriptor);

    size_t total_ilt_entries = 0;
    size_t d;
    for (d = 0; d < l->dll_count; d++)
        total_ilt_entries += l->import_dlls[d].count + 1; /* +1 for null terminator */
    size_t ilt_size = total_ilt_entries * 8;
    size_t iat_size = ilt_size; /* IAT is same size as ILT */

    /* Hint/Name table: for each import, 2 bytes hint + name + null + padding */
    size_t hnt_size = 0;
    size_t j;
    for (j = 0; j < l->import_count; j++) {
        size_t entry_size = 2 + strlen(l->imports[j].func_name) + 1;
        if (entry_size & 1) entry_size++; /* Align to 2 bytes */
        hnt_size += entry_size;
    }

    /* DLL name strings */
    size_t dll_names_size = 0;
    for (d = 0; d < l->dll_count; d++)
        dll_names_size += strlen(l->import_dlls[d].dll_name) + 1;

    /* Step 2: Calculate offsets from rdata_start */
    size_t idt_off = 0;
    size_t ilt_off = idt_off + idt_size;
    size_t iat_off = ilt_off + ilt_size;
    size_t hnt_off = iat_off + iat_size;
    size_t dllname_off = hnt_off + hnt_size;
    size_t total_import_size = dllname_off + dll_names_size;

    /* Reserve space in rdata */
    size_t orig_rdata_size = l->rdata.size;
    {
        size_t pad_amount = total_import_size;
        size_t k;
        for (k = 0; k < pad_amount; k++)
            buffer_write_byte(&l->rdata, 0);
    }

    unsigned char *base = l->rdata.data + rdata_start;

    /* Step 3: Fill Hint/Name table and DLL names, record their offsets */
    size_t *hnt_offsets = malloc(l->import_count * sizeof(size_t));
    {
        size_t pos = hnt_off;
        for (j = 0; j < l->import_count; j++) {
            hnt_offsets[j] = pos;
            uint16_t hint = l->imports[j].hint;
            memcpy(base + pos, &hint, 2);
            pos += 2;
            size_t nlen = strlen(l->imports[j].func_name);
            memcpy(base + pos, l->imports[j].func_name, nlen + 1);
            pos += nlen + 1;
            if (pos & 1) { base[pos] = 0; pos++; } /* Pad to even */
        }
    }

    size_t *dllname_offsets = malloc(l->dll_count * sizeof(size_t));
    {
        size_t pos = dllname_off;
        for (d = 0; d < l->dll_count; d++) {
            dllname_offsets[d] = pos;
            size_t nlen = strlen(l->import_dlls[d].dll_name);
            memcpy(base + pos, l->import_dlls[d].dll_name, nlen + 1);
            pos += nlen + 1;
        }
    }

    /* Step 4: Fill ILT and IAT */
    {
        size_t ilt_pos = ilt_off;
        size_t iat_pos = iat_off;

        for (d = 0; d < l->dll_count; d++) {
            PEImportDll *dll = &l->import_dlls[d];
            size_t k;
            for (k = 0; k < dll->count; k++) {
                size_t imp_idx = dll->import_indices[k];
                /* ILT/IAT entry: RVA to Hint/Name (bit 63 = 0 for name import) */
                uint64_t rva = rdata_rva_base + (uint64_t)hnt_offsets[imp_idx];
                memcpy(base + ilt_pos, &rva, 8);
                memcpy(base + iat_pos, &rva, 8);
                ilt_pos += 8;
                iat_pos += 8;

                /* Update the symbol to point to this IAT entry */
                uint32_t sym_idx = l->imports[imp_idx].sym_index;
                if (sym_idx != (uint32_t)-1 && sym_idx < (uint32_t)l->sym_count) {
                    l->symbols[sym_idx].section = SEC_RDATA;
                    l->symbols[sym_idx].value = rdata_start + iat_off +
                        (ilt_pos - ilt_off - 8); /* IAT entry offset matches ILT */
                    /* Actually, the IAT offset for this entry is:
                     * iat_off + (same position as ilt within this DLL) */
                }
            }
            /* Null terminator */
            ilt_pos += 8;
            iat_pos += 8;
        }
    }

    /* Fix IAT symbol offsets: recalculate properly */
    {
        size_t iat_pos = iat_off;
        for (d = 0; d < l->dll_count; d++) {
            PEImportDll *dll = &l->import_dlls[d];
            size_t k;
            for (k = 0; k < dll->count; k++) {
                size_t imp_idx = dll->import_indices[k];
                uint32_t sym_idx = l->imports[imp_idx].sym_index;
                if (sym_idx != (uint32_t)-1 && sym_idx < (uint32_t)l->sym_count) {
                    l->symbols[sym_idx].section = SEC_RDATA;
                    l->symbols[sym_idx].value = rdata_start + iat_pos;
                }
                iat_pos += 8;
            }
            iat_pos += 8; /* null terminator */
        }
    }

    /* Step 5: Fill IDT (Import Directory Table) */
    {
        size_t ilt_pos = ilt_off;
        size_t iat_pos = iat_off;

        for (d = 0; d < l->dll_count; d++) {
            PE_ImportDescriptor *desc = (PE_ImportDescriptor *)(base + idt_off +
                d * sizeof(PE_ImportDescriptor));
            desc->OriginalFirstThunk = (uint32_t)(rdata_rva_base + ilt_pos);
            desc->TimeDateStamp = 0;
            desc->ForwarderChain = 0;
            desc->Name = (uint32_t)(rdata_rva_base + dllname_offsets[d]);
            desc->FirstThunk = (uint32_t)(rdata_rva_base + iat_pos);

            size_t entries = l->import_dlls[d].count + 1; /* +1 null term */
            ilt_pos += entries * 8;
            iat_pos += entries * 8;
        }
        /* Null terminator IDT entry (already zeroed) */
    }

    /* Set info for header */
    info.idt_rva  = (uint32_t)(rdata_rva_base + idt_off);
    info.idt_size = (uint32_t)idt_size;
    info.iat_rva  = (uint32_t)(rdata_rva_base + iat_off);
    info.iat_size = (uint32_t)iat_size;

    free(hnt_offsets);
    free(dllname_offsets);
    (void)orig_rdata_size;

    return info;
}

/* ------------------------------------------------------------------ */
/*  pe_linker_link — the main linking driver                          */
/* ------------------------------------------------------------------ */

int pe_linker_link(PELinker *l, const char *output_path) {
    size_t i;

    /* ---- 0. Add default imports ---------------------------------- */
    if (!l->no_default_imports)
        pe_add_default_imports(l);

    /* ---- 1. Resolve imports -------------------------------------- */
    pe_resolve_imports(l);

    /* ---- 2. Check for undefined symbols -------------------------- */
    {
        int has_errors = 0;
        for (i = 0; i < l->sym_count; i++) {
            if (l->symbols[i].section == SEC_UNDEF &&
                l->symbols[i].storage_class != PE_SYM_CLASS_STATIC &&
                l->symbols[i].name[0] != '\0') {
                /* Check if referenced */
                size_t k;
                int referenced = 0;
                for (k = 0; k < l->reloc_count; k++) {
                    if (l->relocs[k].sym_index == (uint32_t)i) {
                        referenced = 1;
                        break;
                    }
                }
                /* Check if it's an import that got resolved */
                int is_import = 0;
                size_t j;
                for (j = 0; j < l->import_count; j++) {
                    if (l->imports[j].sym_index == (uint32_t)i) {
                        is_import = 1;
                        break;
                    }
                }
                if (referenced && !is_import) {
                    fprintf(stderr, "pe_linker: error: undefined symbol '%s'\n",
                            l->symbols[i].name);
                    has_errors = 1;
                }
            }
        }
        if (has_errors) return 1;
    }

    /* ---- 3. Find entry point ------------------------------------- */
    int entry_idx = pe_find_global(l, l->entry_name);
    if (entry_idx < 0) {
        fprintf(stderr, "pe_linker: undefined entry point: %s\n", l->entry_name);
        return 1;
    }

    /* ---- 4. Layout: compute section RVAs and file offsets --------- */
    /*
     * PE layout:
     *   DOS header + stub
     *   PE signature
     *   COFF file header
     *   Optional header (PE32+)
     *   Section headers
     *   --- file alignment padding ---
     *   .text section
     *   .rdata section (includes import tables)
     *   .data section
     *   (no .bss on disk — virtual only)
     */

    /* Headers size */
    uint32_t dos_stub_size = sizeof(PE_DosHeader);
    uint32_t pe_sig_size = 4;
    uint32_t file_hdr_size = sizeof(PE_FileHeader);
    uint32_t opt_hdr_size = sizeof(PE_OptionalHeader64);

    /* Count sections */
    int num_sections = 0;
    int text_sec_idx = -1, rdata_sec_idx = -1, data_sec_idx = -1;

    if (l->text.size > 0)  { text_sec_idx  = num_sections++; }
    if (1 /* always have rdata for imports */) { rdata_sec_idx = num_sections++; }
    if (l->data.size > 0 || l->bss_size > 0) { data_sec_idx  = num_sections++; }

    uint32_t section_hdrs_size = (uint32_t)num_sections * sizeof(PE_SectionHeader);
    uint32_t headers_raw_size = dos_stub_size + pe_sig_size + file_hdr_size +
                                opt_hdr_size + section_hdrs_size;
    uint32_t headers_size = pe_align_up32(headers_raw_size, PE_FILE_ALIGNMENT);

    /* Section RVAs and file offsets */
    uint32_t current_rva = pe_align_up32(headers_raw_size, PE_SECTION_ALIGNMENT);
    uint32_t current_foff = headers_size;

    /* .text */
    uint32_t text_rva = 0, text_foff = 0, text_vsize = 0, text_rsize = 0;
    if (text_sec_idx >= 0) {
        text_rva    = current_rva;
        text_foff   = current_foff;
        text_vsize  = (uint32_t)l->text.size;
        text_rsize  = pe_align_up32(text_vsize, PE_FILE_ALIGNMENT);
        current_rva  = pe_align_up32(text_rva + text_vsize, PE_SECTION_ALIGNMENT);
        current_foff += text_rsize;
    }

    /* .rdata — build import tables now that we know the rdata RVA */
    uint32_t rdata_rva = current_rva;
    ImportTableInfo import_info = pe_build_import_tables(l, rdata_rva);

    uint32_t rdata_foff = current_foff;
    uint32_t rdata_vsize = (uint32_t)l->rdata.size;
    uint32_t rdata_rsize = pe_align_up32(rdata_vsize, PE_FILE_ALIGNMENT);
    current_rva  = pe_align_up32(rdata_rva + rdata_vsize, PE_SECTION_ALIGNMENT);
    current_foff += rdata_rsize;

    /* .data */
    uint32_t data_rva = 0, data_foff = 0, data_vsize = 0, data_rsize = 0;
    if (data_sec_idx >= 0) {
        data_rva    = current_rva;
        data_foff   = current_foff;
        data_vsize  = (uint32_t)(l->data.size + l->bss_size);
        data_rsize  = pe_align_up32((uint32_t)l->data.size, PE_FILE_ALIGNMENT);
        if (data_rsize == 0 && l->bss_size > 0)
            data_rsize = 0; /* BSS is virtual only */
        current_rva  = pe_align_up32(data_rva + data_vsize, PE_SECTION_ALIGNMENT);
        current_foff += data_rsize;
    }

    uint32_t image_size = current_rva;

    /* ---- 5. Compute final virtual addresses for symbols ---------- */
    for (i = 0; i < l->sym_count; i++) {
        switch (l->symbols[i].section) {
        case SEC_TEXT:
            l->symbols[i].value += text_rva;
            break;
        case SEC_DATA:
            l->symbols[i].value += data_rva;
            break;
        case SEC_RDATA:
            l->symbols[i].value += rdata_rva;
            break;
        case SEC_BSS:
            l->symbols[i].value += data_rva + l->data.size;
            break;
        default:
            break;
        }
    }

    uint32_t entry_rva = (uint32_t)l->symbols[entry_idx].value;

    /* ---- 6. Apply relocations ------------------------------------ */
    for (i = 0; i < l->reloc_count; i++) {
        PELinkReloc *r = &l->relocs[i];
        PELinkSymbol *sym = &l->symbols[r->sym_index];

        uint32_t S = (uint32_t)sym->value;       /* Symbol RVA */
        uint64_t S64 = sym->value + l->image_base; /* Symbol VA */
        uint32_t P_rva;
        unsigned char *patch;

        if (r->section == SEC_TEXT) {
            P_rva = text_rva + (uint32_t)r->offset;
            patch = l->text.data + r->offset;
        } else if (r->section == SEC_DATA) {
            P_rva = data_rva + (uint32_t)r->offset;
            patch = l->data.data + r->offset;
        } else if (r->section == SEC_RDATA) {
            P_rva = rdata_rva + (uint32_t)r->offset;
            patch = l->rdata.data + r->offset;
        } else {
            continue;
        }

        switch (r->type) {
        case IMAGE_REL_AMD64_ADDR64: {
            /* 64-bit absolute virtual address */
            uint64_t val = S64;
            memcpy(patch, &val, 8);
            break;
        }
        case IMAGE_REL_AMD64_REL32: {
            /* 32-bit RIP-relative: S - (P + 4) */
            /* In COFF REL32, the formula is: S + addend - (P + 4)
             * The addend is already embedded in the object's bytes.
             * We read the existing 4 bytes as the addend. */
            int32_t addend;
            memcpy(&addend, patch, 4);
            int32_t val = (int32_t)(S - (P_rva + 4)) + addend;
            memcpy(patch, &val, 4);
            break;
        }
        case 0x0003: { /* IMAGE_REL_AMD64_ADDR32NB — 32-bit RVA */
            int32_t addend;
            memcpy(&addend, patch, 4);
            uint32_t val = S + (uint32_t)addend;
            memcpy(patch, &val, 4);
            break;
        }
        default:
            fprintf(stderr, "pe_linker: unsupported relocation type 0x%04X "
                    "for '%s'\n", r->type, sym->name);
            return 1;
        }
    }

    /* ---- 7. Write PE file ---------------------------------------- */
    FILE *f = fopen(output_path, "wb");
    if (!f) {
        fprintf(stderr, "pe_linker: cannot create '%s'\n", output_path);
        return 1;
    }

    /* --- DOS header --- */
    PE_DosHeader dos;
    memset(&dos, 0, sizeof(dos));
    dos.e_magic  = PE_DOS_MAGIC;
    dos.e_lfanew = (uint32_t)sizeof(PE_DosHeader);
    fwrite(&dos, sizeof(dos), 1, f);

    /* --- PE signature --- */
    {
        uint32_t sig = PE_SIGNATURE;
        fwrite(&sig, 4, 1, f);
    }

    /* --- COFF file header --- */
    PE_FileHeader fhdr;
    memset(&fhdr, 0, sizeof(fhdr));
    fhdr.Machine            = PE_FILE_MACHINE_AMD64;
    fhdr.NumberOfSections   = (uint16_t)num_sections;
    fhdr.TimeDateStamp      = 0;
    fhdr.PointerToSymbolTable = 0;
    fhdr.NumberOfSymbols    = 0;
    fhdr.SizeOfOptionalHeader = sizeof(PE_OptionalHeader64);
    fhdr.Characteristics    = PE_FILE_EXECUTABLE_IMAGE | PE_FILE_LARGE_ADDRESS_AWARE;
    fwrite(&fhdr, sizeof(fhdr), 1, f);

    /* --- Optional header --- */
    PE_OptionalHeader64 opt;
    memset(&opt, 0, sizeof(opt));
    opt.Magic                       = PE_OPT_MAGIC_PE32PLUS;
    opt.MajorLinkerVersion          = 1;
    opt.MinorLinkerVersion          = 0;
    opt.SizeOfCode                  = text_rsize;
    opt.SizeOfInitializedData       = rdata_rsize + data_rsize;
    opt.SizeOfUninitializedData     = (uint32_t)l->bss_size;
    opt.AddressOfEntryPoint         = entry_rva;
    opt.BaseOfCode                  = text_rva;
    opt.ImageBase                   = l->image_base;
    opt.SectionAlignment            = PE_SECTION_ALIGNMENT;
    opt.FileAlignment               = PE_FILE_ALIGNMENT;
    opt.MajorOperatingSystemVersion = 6;
    opt.MinorOperatingSystemVersion = 0;
    opt.MajorSubsystemVersion       = 6;
    opt.MinorSubsystemVersion       = 0;
    opt.SizeOfImage                 = image_size;
    opt.SizeOfHeaders               = headers_size;
    opt.Subsystem                   = l->subsystem;
    opt.DllCharacteristics          = PE_DLLCHAR_NX_COMPAT;
    opt.SizeOfStackReserve          = l->stack_reserve;
    opt.SizeOfStackCommit           = PE_DEFAULT_STACK_COMMIT;
    opt.SizeOfHeapReserve           = PE_DEFAULT_HEAP_RESERVE;
    opt.SizeOfHeapCommit            = PE_DEFAULT_HEAP_COMMIT;
    opt.NumberOfRvaAndSizes         = PE_NUM_DATA_DIRS;

    /* Import directory */
    if (l->import_count > 0) {
        opt.DataDirectory[PE_DIR_IMPORT].VirtualAddress = import_info.idt_rva;
        opt.DataDirectory[PE_DIR_IMPORT].Size           = import_info.idt_size;
        opt.DataDirectory[PE_DIR_IAT].VirtualAddress    = import_info.iat_rva;
        opt.DataDirectory[PE_DIR_IAT].Size              = import_info.iat_size;
    }

    fwrite(&opt, sizeof(opt), 1, f);

    /* --- Section headers --- */
    if (text_sec_idx >= 0) {
        PE_SectionHeader sh;
        memset(&sh, 0, sizeof(sh));
        memcpy(sh.Name, ".text\0\0\0", 8);
        sh.VirtualSize      = text_vsize;
        sh.VirtualAddress    = text_rva;
        sh.SizeOfRawData     = text_rsize;
        sh.PointerToRawData  = text_foff;
        sh.Characteristics   = PE_SCN_CNT_CODE | PE_SCN_MEM_EXECUTE | PE_SCN_MEM_READ;
        fwrite(&sh, sizeof(sh), 1, f);
    }

    if (rdata_sec_idx >= 0) {
        PE_SectionHeader sh;
        memset(&sh, 0, sizeof(sh));
        memcpy(sh.Name, ".rdata\0\0", 8);
        sh.VirtualSize      = rdata_vsize;
        sh.VirtualAddress    = rdata_rva;
        sh.SizeOfRawData     = rdata_rsize;
        sh.PointerToRawData  = rdata_foff;
        sh.Characteristics   = PE_SCN_CNT_INITIALIZED_DATA | PE_SCN_MEM_READ;
        fwrite(&sh, sizeof(sh), 1, f);
    }

    if (data_sec_idx >= 0) {
        PE_SectionHeader sh;
        memset(&sh, 0, sizeof(sh));
        memcpy(sh.Name, ".data\0\0\0", 8);
        sh.VirtualSize      = data_vsize;
        sh.VirtualAddress    = data_rva;
        sh.SizeOfRawData     = data_rsize;
        sh.PointerToRawData  = data_foff;
        sh.Characteristics   = PE_SCN_CNT_INITIALIZED_DATA | PE_SCN_MEM_READ | PE_SCN_MEM_WRITE;
        fwrite(&sh, sizeof(sh), 1, f);
    }

    /* --- Padding to first section --- */
    {
        long cur = ftell(f);
        while ((uint32_t)cur < headers_size) {
            fputc(0, f);
            cur++;
        }
    }

    /* --- .text section data --- */
    if (text_sec_idx >= 0) {
        fwrite(l->text.data, 1, l->text.size, f);
        /* Pad to file alignment */
        {
            long cur = ftell(f);
            uint32_t target = text_foff + text_rsize;
            while ((uint32_t)cur < target) { fputc(0, f); cur++; }
        }
    }

    /* --- .rdata section data --- */
    if (rdata_sec_idx >= 0) {
        fwrite(l->rdata.data, 1, l->rdata.size, f);
        {
            long cur = ftell(f);
            uint32_t target = rdata_foff + rdata_rsize;
            while ((uint32_t)cur < target) { fputc(0, f); cur++; }
        }
    }

    /* --- .data section data --- */
    if (data_sec_idx >= 0 && l->data.size > 0) {
        fwrite(l->data.data, 1, l->data.size, f);
        {
            long cur = ftell(f);
            uint32_t target = data_foff + data_rsize;
            while ((uint32_t)cur < target) { fputc(0, f); cur++; }
        }
    }

    fclose(f);

    printf("PE Linked: %s  (text=%u, rdata=%u, data=%u, bss=%u, imports=%u)\n",
           output_path,
           text_vsize,
           rdata_vsize,
           (uint32_t)l->data.size,
           (uint32_t)l->bss_size,
           (uint32_t)l->import_count);

    return 0;
}
