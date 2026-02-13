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
#define SEC_ABS   PE_LINK_SEC_ABS
#define SEC_RDATA PE_LINK_SEC_RDATA

/* ------------------------------------------------------------------ */
/*  Helpers                                                           */
/* ------------------------------------------------------------------ */

static uint32_t pe_align_up32(uint32_t v, uint32_t a) {
    return (v + a - 1) & ~(a - 1);
}

#define pe_strdup strdup

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
    for (i = 0; i < l->lib_path_count; i++)
        free(l->lib_paths[i]);
    free(l->lib_paths);
    for (i = 0; i < l->lib_count; i++)
        free(l->libraries[i]);
    free(l->libraries);
    free(l);
}

/* ------------------------------------------------------------------ */
/*  Public: import declaration                                        */
/* ------------------------------------------------------------------ */

void pe_linker_add_import(PELinker *l, const char *func_name,
                          const char *dll_name, uint16_t hint) {
    /* Check for duplicate (same function + same DLL) */
    size_t di;
    for (di = 0; di < l->import_count; di++) {
        if (strcmp(l->imports[di].func_name, func_name) == 0 &&
            strcmp(l->imports[di].dll_name, dll_name) == 0)
            return; /* already exists */
    }

    if (l->import_count >= l->import_cap) {
        l->import_cap = l->import_cap ? l->import_cap * 2 : 32;
        l->imports = realloc(l->imports, l->import_cap * sizeof(PEImportEntry));
    }
    PEImportEntry *e = &l->imports[l->import_count];
    e->func_name = pe_strdup(func_name);
    e->dll_name  = pe_strdup(dll_name);
    e->sym_index = (uint32_t)-1;
    e->imp_sym_index = (uint32_t)-1;
    e->hint      = hint;
    e->iat_rdata_offset = 0;
    e->thunk_text_offset = 0;

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
    /* Validate that section headers fit within file */
    if (sizeof(COFFHeader) + (size_t)num_sec * sizeof(COFFSectionHeader) > file_size) {
        return -1;
    }
    const COFFSectionHeader *shdrs = (const COFFSectionHeader *)(data + sizeof(COFFHeader));

    /* --- 2. Locate symbol table and string table ------------------ */
    const COFFSymbol *symtab = NULL;
    const char *strtab = NULL;
    size_t strtab_size = 0;  /* total size of string table (includes 4-byte length prefix) */
    int sym_count = 0;

    if (hdr->PointerToSymbolTable != 0) {
        size_t sym_end = (size_t)hdr->PointerToSymbolTable +
                         (size_t)hdr->NumberOfSymbols * sizeof(COFFSymbol);
        if (sym_end <= file_size) {
            symtab = (const COFFSymbol *)(data + hdr->PointerToSymbolTable);
            sym_count = (int)hdr->NumberOfSymbols;
            /* String table immediately follows symbol table */
            if (sym_end + 4 <= file_size) {
                strtab = (const char *)(data + sym_end);
                /* First 4 bytes of string table = total size */
                uint32_t st_sz;
                memcpy(&st_sz, strtab, 4);
                if (sym_end + st_sz <= file_size)
                    strtab_size = st_sz;
                else
                    strtab_size = file_size - sym_end;
            }
        }
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
            if (strtab && off >= 0 && (size_t)off + 1 <= strtab_size) strncpy(sec_name, strtab + off, 15);
        } else {
            memcpy(sec_name, shdrs[i].Name, 8);
        }

        uint32_t chars = shdrs[i].Characteristics;

        /* Bounds check: ensure raw data is within file */
        if (shdrs[i].SizeOfRawData > 0 && shdrs[i].PointerToRawData > 0) {
            if ((size_t)shdrs[i].PointerToRawData + (size_t)shdrs[i].SizeOfRawData > file_size) {
                sec_id[i] = SEC_UNDEF;
                continue;
            }
        }

        /* Parse .drectve / info sections first (they may have REMOVE flag too) */
        if (strcmp(sec_name, ".drectve") == 0 ||
            (chars & IMAGE_SCN_LNK_INFO)) {
            goto handle_drectve;
        }

        /* Skip sections with remove flag (except .drectve handled above) */
        if (chars & IMAGE_SCN_LNK_REMOVE) {
            sec_id[i] = SEC_UNDEF;
            continue;
        }

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
        } else if (strcmp(sec_name, ".drectve") == 0 ||
                   (chars & IMAGE_SCN_LNK_INFO)) {
handle_drectve:
            /* Linker directive section: parse /DEFAULTLIB entries */
            sec_id[i] = SEC_UNDEF;
            if (shdrs[i].SizeOfRawData > 0 && shdrs[i].PointerToRawData > 0) {
                /* Copy to a NUL-terminated buffer */
                size_t dlen = shdrs[i].SizeOfRawData;
                char *drectve = malloc(dlen + 1);
                memcpy(drectve, data + shdrs[i].PointerToRawData, dlen);
                drectve[dlen] = '\0';
                /* Parse space-separated tokens looking for -defaultlib: or /DEFAULTLIB: */
                char *tok = drectve;
                while (*tok) {
                    while (*tok == ' ' || *tok == '\t' || *tok == '\n' || *tok == '\r') tok++;
                    if (*tok == '\0') break;
                    char *end = tok;
                    while (*end && *end != ' ' && *end != '\t' && *end != '\n' && *end != '\r') end++;
                    char saved = *end;
                    *end = '\0';
                    /* Match -defaultlib: or /DEFAULTLIB: (case-insensitive) */
                    if ((strncmp(tok, "-defaultlib:", 12) == 0) ||
                        (strncmp(tok, "/defaultlib:", 12) == 0) ||
                        (strncmp(tok, "/DEFAULTLIB:", 12) == 0) ||
                        (strncmp(tok, "-DEFAULTLIB:", 12) == 0)) {
                        const char *libname = tok + 12;
                        /* Strip quotes if present */
                        if (libname[0] == '"') {
                            libname++;
                            char *q = strchr(libname, '"');
                            if (q) *q = '\0';
                        }
                        /* Add library if not already in the list */
                        {
                            int dup = 0;
                            size_t li;
                            for (li = 0; li < l->lib_count; li++) {
                                if (strcmp(l->libraries[li], libname) == 0) {
                                    dup = 1;
                                    break;
                                }
                            }
                            if (!dup) {
                                /* Skip static CRT libraries — we use
                                 * the DLL import versions instead
                                 * (ucrt.lib, vcruntime.lib) which are
                                 * auto-added by the CRT detection. */
                                if (strcmp(libname, "libucrt.lib") == 0 ||
                                    strcmp(libname, "LIBUCRT") == 0 ||
                                    strcmp(libname, "libucrt") == 0 ||
                                    strcmp(libname, "libvcruntime.lib") == 0 ||
                                    strcmp(libname, "LIBVCRUNTIME") == 0 ||
                                    strcmp(libname, "libvcruntime") == 0 ||
                                    strcmp(libname, "libucrtd.lib") == 0 ||
                                    strcmp(libname, "libvcruntimed.lib") == 0) {
                                    /* skip */
                                } else {
                                    pe_linker_add_library(l, libname);
                                }
                        }
                        }
                    }
                    *end = saved;
                    tok = end;
                }
                free(drectve);
            }
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
                if (strtab && cs->N.Name.Offset > 0 && cs->N.Name.Offset < strtab_size) {
                    size_t max_len = strtab_size - cs->N.Name.Offset;
                    if (max_len > 255) max_len = 255;
                    strncpy(name_buf, strtab + cs->N.Name.Offset, max_len);
                    name_buf[max_len] = '\0';
                } else
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

        /* Bounds check for relocation data */
        if ((size_t)shdrs[i].PointerToRelocations + (size_t)rcount * sizeof(COFFRelocation) > file_size)
            continue;
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
/*  Public: library path / library / entry                            */
/* ------------------------------------------------------------------ */

void pe_linker_add_lib_path(PELinker *l, const char *path) {
    l->lib_paths = realloc(l->lib_paths, (l->lib_path_count + 1) * sizeof(char *));
    l->lib_paths[l->lib_path_count++] = pe_strdup(path);
}

void pe_linker_add_library(PELinker *l, const char *name) {
    l->libraries = realloc(l->libraries, (l->lib_count + 1) * sizeof(char *));
    l->libraries[l->lib_count++] = pe_strdup(name);
}

void pe_linker_set_entry(PELinker *l, const char *name) {
    l->entry_name = name;
}

/* ------------------------------------------------------------------ */
/*  COFF import object (short import) detection and parsing           */
/* ------------------------------------------------------------------ */

static int is_coff_import_object(const unsigned char *data, size_t size) {
    if (size < 20) return 0;
    uint16_t sig1, sig2;
    memcpy(&sig1, data, 2);
    memcpy(&sig2, data + 2, 2);
    return (sig1 == 0x0000 && sig2 == 0xFFFF);
}

/*
 * Process a short import object from a COFF import library.
 *
 * Header (20 bytes):
 *   uint16  Sig1       = 0x0000
 *   uint16  Sig2       = 0xFFFF
 *   uint16  Version
 *   uint16  Machine
 *   uint32  TimeDateStamp
 *   uint32  SizeOfData
 *   uint16  OrdinalHint
 *   uint16  Type       (bits 0-1: import type, bits 2-4: name type)
 * Followed by: symbol name (NUL) + DLL name (NUL)
 */
static int process_coff_import_object(PELinker *l, const unsigned char *data,
                                      size_t size) {
    if (size < 20) return -1;

    uint16_t ordinal_hint, type_field;
    memcpy(&ordinal_hint, data + 16, 2);
    memcpy(&type_field, data + 18, 2);

    const char *sym_name = (const char *)(data + 20);
    size_t sym_name_len = strlen(sym_name);
    if (20 + sym_name_len + 1 >= size) return -1;
    const char *dll_name = sym_name + sym_name_len + 1;

    /* Name type (bits 2-4) determines how the import name is derived */
    int name_type = (type_field >> 2) & 0x7;
    const char *import_name = sym_name;
    char undec_buf[256];

    if (name_type == 2) {
        /* IMPORT_NAME_NOPREFIX: strip leading _, ?, @ */
        if (sym_name[0] == '_' || sym_name[0] == '?' || sym_name[0] == '@')
            import_name = sym_name + 1;
    } else if (name_type == 3) {
        /* IMPORT_NAME_UNDECORATE: strip to first @ */
        strncpy(undec_buf, sym_name, 255);
        undec_buf[255] = '\0';
        {
            char *at = strchr(undec_buf, '@');
            if (at) *at = '\0';
        }
        if (undec_buf[0] == '_')
            import_name = undec_buf + 1;
        else
            import_name = undec_buf;
    }

    /* Check if the regular symbol is undefined */
    int existing = pe_find_global(l, sym_name);
    if (existing >= 0 && l->symbols[existing].section == SEC_UNDEF) {
        pe_linker_add_import(l, import_name, dll_name, ordinal_hint);
        /* Link this import to the existing symbol */
        l->imports[l->import_count - 1].sym_index = (uint32_t)existing;
    }

    /* Also handle __imp_ prefixed symbols (used by MSVC-compiled code).
     * __imp_XXX symbols resolve directly to the IAT entry. */
    {
        char imp_name[280];
        sprintf(imp_name, "__imp_%s", sym_name);
        int imp_existing = pe_find_global(l, imp_name);
        if (imp_existing >= 0 && l->symbols[imp_existing].section == SEC_UNDEF) {
            /* Find or create the import entry for this function */
            size_t imp_idx;
            size_t k;
            int found = 0;
            for (k = 0; k < l->import_count; k++) {
                if (strcmp(l->imports[k].func_name, import_name) == 0 &&
                    strcmp(l->imports[k].dll_name, dll_name) == 0) {
                    imp_idx = k;
                    found = 1;
                    break;
                }
            }
            if (!found) {
                pe_linker_add_import(l, import_name, dll_name, ordinal_hint);
                imp_idx = l->import_count - 1;
            }
            l->imports[imp_idx].imp_sym_index = (uint32_t)imp_existing;
        }
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/*  COFF archive (.lib) reader                                        */
/* ------------------------------------------------------------------ */

static int pe_symbol_is_referenced(PELinker *l, size_t sym_idx) {
    size_t j;
    for (j = 0; j < l->reloc_count; j++) {
        if (l->relocs[j].sym_index == (uint32_t)sym_idx)
            return 1;
    }
    return 0;
}

static int pe_has_undefined_symbols(PELinker *l) {
    size_t i;
    for (i = 0; i < l->sym_count; i++) {
        if (l->symbols[i].storage_class != PE_SYM_CLASS_STATIC &&
            l->symbols[i].section == SEC_UNDEF &&
            l->symbols[i].name[0] != '\0' &&
            pe_symbol_is_referenced(l, i))
            return 1;
    }
    return 0;
}

static uint32_t pe_read_be32(const unsigned char *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

/*
 * Process a COFF archive (.lib):  Scan the symbol index and load members
 * that define currently-undefined symbols.  Members may be regular COFF
 * objects or short import objects.  Iterates until no further progress.
 */
static int pe_process_archive(PELinker *l, const unsigned char *ar_data,
                              size_t ar_size, const char *ar_path) {
    /* Verify "!<arch>\n" magic */
    if (ar_size < 8 || memcmp(ar_data, "!<arch>\n", 8) != 0) {
        fprintf(stderr, "pe_linker: %s: not a valid archive\n", ar_path);
        return -1;
    }
    if (ar_size < 68) return 0;

    size_t pos = 8;
    char sizestr[11];
    memcpy(sizestr, ar_data + pos + 48, 10);
    sizestr[10] = '\0';
    long membsz = atol(sizestr);
    size_t content_off = pos + 60;

    int has_symidx = (ar_data[pos] == '/' &&
                      (ar_data[pos + 1] == ' ' || ar_data[pos + 1] == '\0'));
    if (!has_symidx || membsz < 4) {
        fprintf(stderr, "pe_linker: %s: no archive symbol index\n", ar_path);
        return 0;
    }

    const unsigned char *idx = ar_data + content_off;
    uint32_t nsyms = pe_read_be32(idx);
    const unsigned char *offsets_p = idx + 4;
    const char *names_p = (const char *)(idx + 4 + nsyms * 4);

    size_t *loaded_offsets = NULL;
    int loaded_count = 0;

    int changed = 1;
    int pass = 0;
    while (changed) {
        changed = 0;
        int loaded_in_pass = 0;
        const char *np = names_p;
        uint32_t si;

        for (si = 0; si < nsyms; si++) {
            uint32_t member_off = pe_read_be32(offsets_p + si * 4);
            if (np >= (const char *)(ar_data + ar_size)) break;
            size_t nlen = strlen(np);

            int idx2 = pe_find_global(l, np);
            int need_load = (idx2 >= 0 && l->symbols[idx2].section == SEC_UNDEF);

            /* Also check for __imp_ prefixed version:
             * CRT code may reference __imp_Foo while the archive index lists Foo */
            if (!need_load) {
                char imp_name[512];
                sprintf(imp_name, "__imp_%s", np);
                int idx3 = pe_find_global(l, imp_name);
                if (idx3 >= 0 && l->symbols[idx3].section == SEC_UNDEF)
                    need_load = 1;
            }

            if (need_load) {
                int already = 0;
                int j;
                for (j = 0; j < loaded_count; j++) {
                    if (loaded_offsets[j] == (size_t)member_off) {
                        already = 1;
                        break;
                    }
                }

                if (!already && (size_t)member_off + 60 <= ar_size) {
                    char msz_str[11];
                    memcpy(msz_str, ar_data + member_off + 48, 10);
                    msz_str[10] = '\0';
                    long msz = atol(msz_str);
                    size_t mcontent = (size_t)member_off + 60;

                    if (mcontent + (size_t)msz <= ar_size) {
                        const unsigned char *mdata = ar_data + mcontent;
                        if (is_coff_import_object(mdata, (size_t)msz)) {
                            process_coff_import_object(l, mdata, (size_t)msz);
                            changed = 1;
                            loaded_in_pass++;
                        } else {
                            int rc = read_coff_object(l, mdata, (size_t)msz,
                                                      ar_path);
                            if (rc == 0) { changed = 1; loaded_in_pass++; }
                        }
                    }

                    loaded_offsets = realloc(loaded_offsets,
                                            ((size_t)loaded_count + 1) *
                                            sizeof(size_t));
                    loaded_offsets[loaded_count++] = (size_t)member_off;
                }
            }

            np += nlen + 1;
        }
        pass++;
    }

    free(loaded_offsets);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Library search & loading                                          */
/* ------------------------------------------------------------------ */

static int pe_file_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f) { fclose(f); return 1; }
    return 0;
}

static const char *pe_find_lib_file(PELinker *l, const char *name) {
    static char buf[512];
    size_t i;

    /* Try name as-is in each search path */
    for (i = 0; i < l->lib_path_count; i++) {
        sprintf(buf, "%s\\%s", l->lib_paths[i], name);
        if (pe_file_exists(buf)) return buf;
        /* Also try forward slashes */
        sprintf(buf, "%s/%s", l->lib_paths[i], name);
        if (pe_file_exists(buf)) return buf;
    }
    /* Try with .lib extension */
    for (i = 0; i < l->lib_path_count; i++) {
        sprintf(buf, "%s\\%s.lib", l->lib_paths[i], name);
        if (pe_file_exists(buf)) return buf;
        sprintf(buf, "%s/%s.lib", l->lib_paths[i], name);
        if (pe_file_exists(buf)) return buf;
    }
    return NULL;
}

static int pe_load_library(PELinker *l, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "pe_linker: cannot open library '%s'\n", path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *buf = malloc((size_t)sz);
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf); fclose(f); return -1;
    }
    fclose(f);
    int rc = pe_process_archive(l, buf, (size_t)sz, path);
    free(buf);
    return rc;
}

static void pe_add_default_lib_paths(PELinker *l) {
    /* Parse the LIB environment variable (semicolon-separated paths)
     * set by vcvars64.bat / Visual Studio Developer Command Prompt */
    const char *env_lib = getenv("LIB");
    if (!env_lib) return;
    char *copy = pe_strdup(env_lib);
    char *p = copy;
    while (*p) {
        char *semi = strchr(p, ';');
        if (semi) *semi = '\0';
        if (p[0] != '\0')
            pe_linker_add_lib_path(l, p);
        if (semi)
            p = semi + 1;
        else
            break;
    }
    free(copy);
}

/* ------------------------------------------------------------------ */
/*  Default imports                                                   */
/* ------------------------------------------------------------------ */

static void pe_add_default_imports(PELinker *l) {
    /* ExitProcess from kernel32.dll — used by our entry stub */
    pe_linker_add_import(l, "ExitProcess", "kernel32.dll", 0);
}

/* ------------------------------------------------------------------ */
/*  Built-in CRT/Win32 import table for cross-compilation             */
/*  Maps common function names to their Windows DLL when .lib files   */
/*  are not available (e.g. cross-compiling from Linux).              */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *func_name;
    const char *dll_name;
} BuiltinImportEntry;

static const BuiltinImportEntry builtin_imports[] = {
    /* kernel32.dll */
    {"ExitProcess",              "kernel32.dll"},
    {"GetStdHandle",             "kernel32.dll"},
    {"WriteFile",                "kernel32.dll"},
    {"ReadFile",                 "kernel32.dll"},
    {"GetLastError",             "kernel32.dll"},
    {"SetLastError",             "kernel32.dll"},
    {"CloseHandle",              "kernel32.dll"},
    {"CreateFileA",              "kernel32.dll"},
    {"CreateFileW",              "kernel32.dll"},
    {"GetFileSize",              "kernel32.dll"},
    {"GetFileSizeEx",            "kernel32.dll"},
    {"VirtualAlloc",             "kernel32.dll"},
    {"VirtualFree",              "kernel32.dll"},
    {"GetProcessHeap",           "kernel32.dll"},
    {"HeapAlloc",                "kernel32.dll"},
    {"HeapFree",                 "kernel32.dll"},
    {"HeapReAlloc",              "kernel32.dll"},
    {"GetCommandLineA",          "kernel32.dll"},
    {"GetCommandLineW",          "kernel32.dll"},
    {"GetEnvironmentVariableA",  "kernel32.dll"},
    {"GetEnvironmentVariableW",  "kernel32.dll"},
    {"GetModuleHandleA",         "kernel32.dll"},
    {"GetModuleHandleW",         "kernel32.dll"},
    {"LoadLibraryA",             "kernel32.dll"},
    {"GetProcAddress",           "kernel32.dll"},
    {"QueryPerformanceCounter",  "kernel32.dll"},
    {"QueryPerformanceFrequency","kernel32.dll"},
    {"Sleep",                    "kernel32.dll"},
    {"GetTickCount",             "kernel32.dll"},
    {"MultiByteToWideChar",      "kernel32.dll"},
    {"WideCharToMultiByte",      "kernel32.dll"},
    {"GetCurrentDirectoryA",     "kernel32.dll"},
    {"SetCurrentDirectoryA",     "kernel32.dll"},
    {"CreateProcessA",           "kernel32.dll"},
    {"WaitForSingleObject",      "kernel32.dll"},
    {"GetExitCodeProcess",       "kernel32.dll"},

    /* ucrtbase.dll — C stdio */
    {"__acrt_iob_func",          "ucrtbase.dll"},
    {"printf",                   "ucrtbase.dll"},
    {"sprintf",                  "ucrtbase.dll"},
    {"snprintf",                 "ucrtbase.dll"},
    {"fprintf",                  "ucrtbase.dll"},
    {"sscanf",                   "ucrtbase.dll"},
    {"fscanf",                   "ucrtbase.dll"},
    {"scanf",                    "ucrtbase.dll"},
    {"vprintf",                  "ucrtbase.dll"},
    {"vfprintf",                 "ucrtbase.dll"},
    {"vsprintf",                 "ucrtbase.dll"},
    {"vsnprintf",                "ucrtbase.dll"},
    {"puts",                     "ucrtbase.dll"},
    {"fputs",                    "ucrtbase.dll"},
    {"fputc",                    "ucrtbase.dll"},
    {"putchar",                  "ucrtbase.dll"},
    {"fgets",                    "ucrtbase.dll"},
    {"fgetc",                    "ucrtbase.dll"},
    {"getchar",                  "ucrtbase.dll"},
    {"fopen",                    "ucrtbase.dll"},
    {"fclose",                   "ucrtbase.dll"},
    {"fread",                    "ucrtbase.dll"},
    {"fwrite",                   "ucrtbase.dll"},
    {"fseek",                    "ucrtbase.dll"},
    {"ftell",                    "ucrtbase.dll"},
    {"fflush",                   "ucrtbase.dll"},
    {"feof",                     "ucrtbase.dll"},
    {"ferror",                   "ucrtbase.dll"},
    {"rewind",                   "ucrtbase.dll"},
    {"remove",                   "ucrtbase.dll"},
    {"rename",                   "ucrtbase.dll"},
    {"tmpfile",                  "ucrtbase.dll"},
    {"tmpnam",                   "ucrtbase.dll"},
    {"perror",                   "ucrtbase.dll"},
    {"setvbuf",                  "ucrtbase.dll"},

    /* ucrtbase.dll — C stdlib */
    {"malloc",                   "ucrtbase.dll"},
    {"calloc",                   "ucrtbase.dll"},
    {"realloc",                  "ucrtbase.dll"},
    {"free",                     "ucrtbase.dll"},
    {"atoi",                     "ucrtbase.dll"},
    {"atol",                     "ucrtbase.dll"},
    {"atof",                     "ucrtbase.dll"},
    {"strtol",                   "ucrtbase.dll"},
    {"strtoul",                  "ucrtbase.dll"},
    {"strtoll",                  "ucrtbase.dll"},
    {"strtoull",                 "ucrtbase.dll"},
    {"strtod",                   "ucrtbase.dll"},
    {"strtof",                   "ucrtbase.dll"},
    {"abs",                      "ucrtbase.dll"},
    {"labs",                     "ucrtbase.dll"},
    {"exit",                     "ucrtbase.dll"},
    {"_exit",                    "ucrtbase.dll"},
    {"abort",                    "ucrtbase.dll"},
    {"atexit",                   "ucrtbase.dll"},
    {"getenv",                   "ucrtbase.dll"},
    {"system",                   "ucrtbase.dll"},
    {"qsort",                    "ucrtbase.dll"},
    {"bsearch",                  "ucrtbase.dll"},
    {"rand",                     "ucrtbase.dll"},
    {"srand",                    "ucrtbase.dll"},

    /* ucrtbase.dll — C string */
    {"memcpy",                   "ucrtbase.dll"},
    {"memset",                   "ucrtbase.dll"},
    {"memcmp",                   "ucrtbase.dll"},
    {"memmove",                  "ucrtbase.dll"},
    {"memchr",                   "ucrtbase.dll"},
    {"strlen",                   "ucrtbase.dll"},
    {"strcmp",                    "ucrtbase.dll"},
    {"strncmp",                  "ucrtbase.dll"},
    {"strcpy",                   "ucrtbase.dll"},
    {"strncpy",                  "ucrtbase.dll"},
    {"strcat",                   "ucrtbase.dll"},
    {"strncat",                  "ucrtbase.dll"},
    {"strchr",                   "ucrtbase.dll"},
    {"strrchr",                  "ucrtbase.dll"},
    {"strstr",                   "ucrtbase.dll"},
    {"strpbrk",                  "ucrtbase.dll"},
    {"strspn",                   "ucrtbase.dll"},
    {"strcspn",                  "ucrtbase.dll"},
    {"strtok",                   "ucrtbase.dll"},
    {"strerror",                 "ucrtbase.dll"},
    {"_strdup",                  "ucrtbase.dll"},

    /* ucrtbase.dll — C ctype */
    {"isalpha",                  "ucrtbase.dll"},
    {"isdigit",                  "ucrtbase.dll"},
    {"isalnum",                  "ucrtbase.dll"},
    {"isspace",                  "ucrtbase.dll"},
    {"isupper",                  "ucrtbase.dll"},
    {"islower",                  "ucrtbase.dll"},
    {"isprint",                  "ucrtbase.dll"},
    {"ispunct",                  "ucrtbase.dll"},
    {"isxdigit",                 "ucrtbase.dll"},
    {"toupper",                  "ucrtbase.dll"},
    {"tolower",                  "ucrtbase.dll"},

    /* ucrtbase.dll — C time */
    {"time",                     "ucrtbase.dll"},
    {"_time64",                  "ucrtbase.dll"},
    {"_time32",                  "ucrtbase.dll"},
    {"clock",                    "ucrtbase.dll"},
    {"difftime",                 "ucrtbase.dll"},
    {"mktime",                   "ucrtbase.dll"},
    {"localtime",                "ucrtbase.dll"},
    {"gmtime",                   "ucrtbase.dll"},
    {"strftime",                 "ucrtbase.dll"},

    /* ucrtbase.dll — C math (some may also be in ucrtbase) */
    {"ceil",                     "ucrtbase.dll"},
    {"floor",                    "ucrtbase.dll"},
    {"sqrt",                     "ucrtbase.dll"},
    {"pow",                      "ucrtbase.dll"},
    {"fabs",                     "ucrtbase.dll"},
    {"log",                      "ucrtbase.dll"},
    {"log10",                    "ucrtbase.dll"},
    {"exp",                      "ucrtbase.dll"},
    {"sin",                      "ucrtbase.dll"},
    {"cos",                      "ucrtbase.dll"},
    {"tan",                      "ucrtbase.dll"},

    {NULL, NULL}  /* sentinel */
};

static const char *pe_lookup_builtin_dll(const char *func_name) {
    const BuiltinImportEntry *e;
    for (e = builtin_imports; e->func_name != NULL; e++) {
        if (strcmp(e->func_name, func_name) == 0)
            return e->dll_name;
    }
    return NULL;
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
            /* Try built-in import table (for cross-compilation without .lib files) */
            const char *dll = pe_lookup_builtin_dll(l->symbols[i].name);
            if (dll) {
                pe_linker_add_import(l, l->symbols[i].name, dll, 0);
                l->imports[l->import_count - 1].sym_index = (uint32_t)i;
                found = 1;
            }
        }

        if (!found) {
            /* Check for __imp_ prefixed symbols and try builtin table
             * with the unprefixed name */
            if (strncmp(l->symbols[i].name, "__imp_", 6) == 0) {
                const char *unprefixed = l->symbols[i].name + 6;
                const char *dll = pe_lookup_builtin_dll(unprefixed);
                if (dll) {
                    /* Find or create the import entry */
                    size_t k;
                    int imp_found = 0;
                    for (k = 0; k < l->import_count; k++) {
                        if (strcmp(l->imports[k].func_name, unprefixed) == 0 &&
                            strcmp(l->imports[k].dll_name, dll) == 0) {
                            l->imports[k].imp_sym_index = (uint32_t)i;
                            imp_found = 1;
                            break;
                        }
                    }
                    if (!imp_found) {
                        pe_linker_add_import(l, unprefixed, dll, 0);
                        l->imports[l->import_count - 1].imp_sym_index = (uint32_t)i;
                    }
                    found = 1;
                }
            }
        }

        if (!found) {
            /* Still unknown — report error if referenced by a relocation */
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

static void pe_build_import_tables(PELinker *l,
                                   uint32_t rdata_rva_base,
                                   ImportTableInfo *info) {
    memset(info, 0, sizeof(ImportTableInfo));

    if (l->import_count == 0) return;

    size_t rdata_start = l->rdata.size;
    buffer_pad(&l->rdata, 8);
    rdata_start = l->rdata.size;
    info->rdata_start = rdata_start;

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

                /* NOTE: sym_index is NOT updated here — it points to the
                 * import thunk in .text (set during thunk generation).
                 * Only imp_sym_index (__imp_ symbol) gets the IAT entry,
                 * which is handled in the IAT fixup pass below. */
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
                /* Record IAT offset for thunk patching */
                l->imports[imp_idx].iat_rdata_offset = rdata_start + iat_pos;
                /* __imp_ symbols point directly to IAT entry */
                uint32_t isym = l->imports[imp_idx].imp_sym_index;
                if (isym != (uint32_t)-1 && isym < (uint32_t)l->sym_count) {
                    l->symbols[isym].section = SEC_RDATA;
                    l->symbols[isym].value = rdata_start + iat_pos;
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
    info->idt_rva  = (uint32_t)(rdata_rva_base + idt_off);
    info->idt_size = (uint32_t)idt_size;
    info->iat_rva  = (uint32_t)(rdata_rva_base + iat_off);
    info->iat_size = (uint32_t)iat_size;

    free(hnt_offsets);
    free(dllname_offsets);
    (void)orig_rdata_size;
}

/* ------------------------------------------------------------------ */
/*  pe_linker_link — the main linking driver                          */
/* ------------------------------------------------------------------ */

int pe_linker_link(PELinker *l, const char *output_path) {
    size_t i;

    /* ---- 0. Add default library search paths --------------------- */
    pe_add_default_lib_paths(l);

    /* Auto-add CRT dependencies when linking with MSVC CRT.
     * libcmt.lib depends on ucrt.lib (ucrtbase.dll) and
     * vcruntime.lib (vcruntime140.dll) but this isn't always
     * expressed in .drectve sections. */
    {
        int has_crt = 0;
        for (i = 0; i < l->lib_count; i++) {
            const char *nm = l->libraries[i];
            if (strcmp(nm, "libcmt.lib") == 0 || strcmp(nm, "LIBCMT") == 0 ||
                strcmp(nm, "libcmt") == 0 || strcmp(nm, "LIBCMT.lib") == 0 ||
                strcmp(nm, "libcmtd.lib") == 0 || strcmp(nm, "LIBCMTD") == 0 ||
                strcmp(nm, "msvcrt.lib") == 0 || strcmp(nm, "MSVCRT.lib") == 0) {
                has_crt = 1;
                break;
            }
        }
        if (has_crt) {
            pe_linker_add_library(l, "ucrt.lib");
            pe_linker_add_library(l, "vcruntime.lib");
        }
    }

    /* ---- Generate built-in entry stub ----------------------------- */
    /* Two modes depending on entry_name:
     *
     * "main" (default, no CRT):
     *   Minimal wrapper that calls main(0, NULL) then ExitProcess(result).
     *   No CRT library dependencies needed.
     *
     * "mainCRTStartup" (with CRT libs):
     *   Calls CRT init functions for argc/argv, then main, then exit.
     *   Requires ucrt.lib / vcruntime.lib / libcmt.lib and vcvars64 env.
     */
    if (l->entry_name && strcmp(l->entry_name, "mainCRTStartup") == 0 &&
        pe_find_global(l, l->entry_name) < 0) {

        buffer_pad(&l->text, 16);
        size_t stub_start = l->text.size;

        /* Define the entry symbol */
        pe_add_sym(l, "mainCRTStartup", stub_start, SEC_TEXT,
                   PE_SYM_CLASS_EXTERNAL, 0);

        /* Ensure callee symbols exist (undefined if not yet) */
        const char *callees[] = {
            "_configure_narrow_argv",
            "_initialize_narrow_environment",
            "__p___argc",
            "__p___argv",
            "main",
            "exit"
        };
        uint32_t callee_syms[6];
        {
            int ci;
            for (ci = 0; ci < 6; ci++) {
                int idx = pe_find_global(l, callees[ci]);
                if (idx >= 0) {
                    callee_syms[ci] = (uint32_t)idx;
                } else {
                    callee_syms[ci] = pe_add_sym(l, callees[ci], 0, SEC_UNDEF,
                                                 PE_SYM_CLASS_EXTERNAL, 0);
                }
            }
        }

        /* sub rsp, 56 */
        buffer_write_byte(&l->text, 0x48);
        buffer_write_byte(&l->text, 0x83);
        buffer_write_byte(&l->text, 0xEC);
        buffer_write_byte(&l->text, 0x38);

        /* xor ecx, ecx */
        buffer_write_byte(&l->text, 0x31);
        buffer_write_byte(&l->text, 0xC9);

        /* call _configure_narrow_argv */
        buffer_write_byte(&l->text, 0xE8);
        pe_add_reloc(l, l->text.size, SEC_TEXT,
                     callee_syms[0], IMAGE_REL_AMD64_REL32);
        buffer_write_dword(&l->text, 0);

        /* call _initialize_narrow_environment */
        buffer_write_byte(&l->text, 0xE8);
        pe_add_reloc(l, l->text.size, SEC_TEXT,
                     callee_syms[1], IMAGE_REL_AMD64_REL32);
        buffer_write_dword(&l->text, 0);

        /* call __p___argc */
        buffer_write_byte(&l->text, 0xE8);
        pe_add_reloc(l, l->text.size, SEC_TEXT,
                     callee_syms[2], IMAGE_REL_AMD64_REL32);
        buffer_write_dword(&l->text, 0);

        /* mov eax, [rax] */
        buffer_write_byte(&l->text, 0x8B);
        buffer_write_byte(&l->text, 0x00);

        /* mov [rsp+32], eax */
        buffer_write_byte(&l->text, 0x89);
        buffer_write_byte(&l->text, 0x44);
        buffer_write_byte(&l->text, 0x24);
        buffer_write_byte(&l->text, 0x20);

        /* call __p___argv */
        buffer_write_byte(&l->text, 0xE8);
        pe_add_reloc(l, l->text.size, SEC_TEXT,
                     callee_syms[3], IMAGE_REL_AMD64_REL32);
        buffer_write_dword(&l->text, 0);

        /* mov rax, [rax] */
        buffer_write_byte(&l->text, 0x48);
        buffer_write_byte(&l->text, 0x8B);
        buffer_write_byte(&l->text, 0x00);

        /* mov ecx, [rsp+32] — argc */
        buffer_write_byte(&l->text, 0x8B);
        buffer_write_byte(&l->text, 0x4C);
        buffer_write_byte(&l->text, 0x24);
        buffer_write_byte(&l->text, 0x20);

        /* mov rdx, rax — argv */
        buffer_write_byte(&l->text, 0x48);
        buffer_write_byte(&l->text, 0x89);
        buffer_write_byte(&l->text, 0xC2);

        /* call main */
        buffer_write_byte(&l->text, 0xE8);
        pe_add_reloc(l, l->text.size, SEC_TEXT,
                     callee_syms[4], IMAGE_REL_AMD64_REL32);
        buffer_write_dword(&l->text, 0);

        /* mov ecx, eax — exit code */
        buffer_write_byte(&l->text, 0x89);
        buffer_write_byte(&l->text, 0xC1);

        /* call exit */
        buffer_write_byte(&l->text, 0xE8);
        pe_add_reloc(l, l->text.size, SEC_TEXT,
                     callee_syms[5], IMAGE_REL_AMD64_REL32);
        buffer_write_dword(&l->text, 0);

        /* int3 */
        buffer_write_byte(&l->text, 0xCC);

        /* Also provide __ImageBase — this symbol equals the image base
         * address.  Define it as SEC_ABS with value 0; during symbol
         * finalization, SEC_ABS symbols are not adjusted by any section RVA.
         * For ADDR32NB relocs: S + addend = 0 (RVA of image base).
         * For ADDR64 relocs: S64 = 0 + image_base = image_base. */
        if (pe_find_global(l, "__ImageBase") < 0) {
            pe_add_sym(l, "__ImageBase", 0, SEC_ABS,
                       PE_SYM_CLASS_EXTERNAL, 0);
        }

    } else if (l->entry_name && strcmp(l->entry_name, "main") == 0) {
        /* ---- Minimal entry stub: main(0, NULL) + ExitProcess ---- */
        /* No CRT libraries needed — works without vcvars64.
         *   sub  rsp, 40     ; 48 83 EC 28  shadow(32) + align(8)
         *   xor  ecx, ecx   ; 31 C9        argc = 0
         *   xor  edx, edx   ; 31 D2        argv = NULL
         *   call main        ; E8 rel32
         *   mov  ecx, eax   ; 89 C1        exit code
         *   call ExitProcess ; E8 rel32
         *   int3             ; CC
         */
        buffer_pad(&l->text, 16);
        size_t stub_start = l->text.size;

        /* Create internal entry symbol; keep "main" as user symbol */
        pe_add_sym(l, "__pe_entry", stub_start, SEC_TEXT,
                   PE_SYM_CLASS_EXTERNAL, 0);
        l->entry_name = "__pe_entry";

        /* Locate or create callee symbols */
        int main_idx = pe_find_global(l, "main");
        uint32_t main_sym, exitp_sym;
        if (main_idx >= 0) {
            main_sym = (uint32_t)main_idx;
        } else {
            main_sym = pe_add_sym(l, "main", 0, SEC_UNDEF,
                                  PE_SYM_CLASS_EXTERNAL, 0);
        }
        {
            int idx = pe_find_global(l, "ExitProcess");
            if (idx >= 0) {
                exitp_sym = (uint32_t)idx;
            } else {
                exitp_sym = pe_add_sym(l, "ExitProcess", 0, SEC_UNDEF,
                                       PE_SYM_CLASS_EXTERNAL, 0);
            }
        }

        /* sub rsp, 40 */
        buffer_write_byte(&l->text, 0x48);
        buffer_write_byte(&l->text, 0x83);
        buffer_write_byte(&l->text, 0xEC);
        buffer_write_byte(&l->text, 0x28);

        /* xor ecx, ecx — argc = 0 */
        buffer_write_byte(&l->text, 0x31);
        buffer_write_byte(&l->text, 0xC9);

        /* xor edx, edx — argv = NULL */
        buffer_write_byte(&l->text, 0x31);
        buffer_write_byte(&l->text, 0xD2);

        /* call main */
        buffer_write_byte(&l->text, 0xE8);
        pe_add_reloc(l, l->text.size, SEC_TEXT,
                     main_sym, IMAGE_REL_AMD64_REL32);
        buffer_write_dword(&l->text, 0);

        /* mov ecx, eax — exit code */
        buffer_write_byte(&l->text, 0x89);
        buffer_write_byte(&l->text, 0xC1);

        /* call ExitProcess */
        buffer_write_byte(&l->text, 0xE8);
        pe_add_reloc(l, l->text.size, SEC_TEXT,
                     exitp_sym, IMAGE_REL_AMD64_REL32);
        buffer_write_dword(&l->text, 0);

        /* int3 */
        buffer_write_byte(&l->text, 0xCC);

        /* Also provide __ImageBase */
        if (pe_find_global(l, "__ImageBase") < 0) {
            pe_add_sym(l, "__ImageBase", 0, SEC_ABS,
                       PE_SYM_CLASS_EXTERNAL, 0);
        }

    } else if (l->entry_name && pe_find_global(l, l->entry_name) < 0) {
        /* Other entry: seed as undefined so archive resolves it */
        pe_add_sym(l, l->entry_name, 0, SEC_UNDEF,
                   PE_SYM_CLASS_EXTERNAL, 0);
    }

    /* ---- 1. Process libraries (iteratively — .drectve may add more) */
    {
        size_t lib_idx = 0;
        char **loaded_libs = NULL;
        size_t loaded_lib_count = 0;
        size_t k;

        while (lib_idx < l->lib_count) {
            /* Check if this library was already loaded */
            int already = 0;
            for (k = 0; k < loaded_lib_count; k++) {
                if (strcmp(loaded_libs[k], l->libraries[lib_idx]) == 0) {
                    already = 1;
                    break;
                }
            }
            if (!already) {
                const char *path = pe_find_lib_file(l, l->libraries[lib_idx]);
                if (path) {
                    pe_load_library(l, path);
                } else {
                    fprintf(stderr, "pe_linker: warning: cannot find library '%s'\n",
                            l->libraries[lib_idx]);
                }
                loaded_libs = realloc(loaded_libs,
                                      (loaded_lib_count + 1) * sizeof(char *));
                loaded_libs[loaded_lib_count++] = pe_strdup(l->libraries[lib_idx]);
            }
            lib_idx++;
        }

        for (k = 0; k < loaded_lib_count; k++) free(loaded_libs[k]);
        free(loaded_libs);
    }

    /* ---- 2. Add default imports ---------------------------------- */
    if (!l->no_default_imports)
        pe_add_default_imports(l);

    /* ---- 3. Resolve imports -------------------------------------- */
    pe_resolve_imports(l);

    /* ---- 4. Check for undefined symbols -------------------------- */
    {
        int has_errors = 0;
        for (i = 0; i < l->sym_count; i++) {
            if (l->symbols[i].section == SEC_UNDEF &&
                l->symbols[i].storage_class != PE_SYM_CLASS_STATIC &&
                l->symbols[i].name[0] != '\0') {
                size_t k;
                int referenced = 0;
                for (k = 0; k < l->reloc_count; k++) {
                    if (l->relocs[k].sym_index == (uint32_t)i) {
                        referenced = 1;
                        break;
                    }
                }
                int is_import = 0;
                size_t j;
                for (j = 0; j < l->import_count; j++) {
                    if (l->imports[j].sym_index == (uint32_t)i ||
                        l->imports[j].imp_sym_index == (uint32_t)i) {
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

    /* ---- 5. Find entry point ------------------------------------- */
    int entry_idx = pe_find_global(l, l->entry_name);
    if (entry_idx < 0) {
        fprintf(stderr, "pe_linker: undefined entry point: %s\n", l->entry_name);
        return 1;
    }

    /* ---- 6. Generate import thunks ------------------------------- */
    /*
     * For each import whose sym_index refers to a symbol used with a
     * direct CALL rel32 (not __imp_), generate a 6-byte thunk in .text:
     *   FF 25 xx xx xx xx   jmp [rip + disp32]
     * padded to 8 bytes.  The displacement is filled after layout.
     */
    if (l->import_count > 0) {
        buffer_pad(&l->text, 16);
        for (i = 0; i < l->import_count; i++) {
            uint32_t si = l->imports[i].sym_index;
            if (si == (uint32_t)-1) continue;
            if (si >= (uint32_t)l->sym_count) continue;
            /* Generate thunk */
            l->imports[i].thunk_text_offset = l->text.size;
            buffer_write_byte(&l->text, 0xFF);  /* jmp [rip+disp32] */
            buffer_write_byte(&l->text, 0x25);
            buffer_write_dword(&l->text, 0);     /* placeholder disp */
            buffer_write_byte(&l->text, 0xCC);   /* int3 padding */
            buffer_write_byte(&l->text, 0xCC);
            /* Point the symbol to this thunk (in .text) */
            l->symbols[si].section = SEC_TEXT;
            l->symbols[si].value = l->imports[i].thunk_text_offset;
        }
    }

    /* ---- 7. Layout: compute section RVAs and file offsets --------- */

    uint32_t dos_stub_size = sizeof(PE_DosHeader);
    uint32_t pe_sig_size = 4;
    uint32_t file_hdr_size = sizeof(PE_FileHeader);
    uint32_t opt_hdr_size = sizeof(PE_OptionalHeader64);

    int num_sections = 0;
    int text_sec_idx = -1, rdata_sec_idx = -1, data_sec_idx = -1;

    if (l->text.size > 0)  { text_sec_idx  = num_sections++; }
    if (1) { rdata_sec_idx = num_sections++; }
    if (l->data.size > 0 || l->bss_size > 0) { data_sec_idx  = num_sections++; }

    uint32_t section_hdrs_size = (uint32_t)num_sections * sizeof(PE_SectionHeader);
    uint32_t headers_raw_size = dos_stub_size + pe_sig_size + file_hdr_size +
                                opt_hdr_size + section_hdrs_size;
    uint32_t headers_size = pe_align_up32(headers_raw_size, PE_FILE_ALIGNMENT);

    uint32_t current_rva = pe_align_up32(headers_raw_size, PE_SECTION_ALIGNMENT);
    uint32_t current_foff = headers_size;

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
    ImportTableInfo import_info;
    pe_build_import_tables(l, rdata_rva, &import_info);

    uint32_t rdata_foff = current_foff;
    uint32_t rdata_vsize = (uint32_t)l->rdata.size;
    uint32_t rdata_rsize = pe_align_up32(rdata_vsize, PE_FILE_ALIGNMENT);
    current_rva  = pe_align_up32(rdata_rva + rdata_vsize, PE_SECTION_ALIGNMENT);
    current_foff += rdata_rsize;

    uint32_t data_rva = 0, data_foff = 0, data_vsize = 0, data_rsize = 0;
    if (data_sec_idx >= 0) {
        data_rva    = current_rva;
        data_foff   = current_foff;
        data_vsize  = (uint32_t)(l->data.size + l->bss_size);
        data_rsize  = pe_align_up32((uint32_t)l->data.size, PE_FILE_ALIGNMENT);
        if (data_rsize == 0 && l->bss_size > 0)
            data_rsize = 0;
        current_rva  = pe_align_up32(data_rva + data_vsize, PE_SECTION_ALIGNMENT);
        current_foff += data_rsize;
    }

    uint32_t image_size = current_rva;

    /* ---- 8. Patch import thunk displacements --------------------- */
    for (i = 0; i < l->import_count; i++) {
        uint32_t si = l->imports[i].sym_index;
        if (si == (uint32_t)-1 || si >= (uint32_t)l->sym_count) continue;
        size_t toff = l->imports[i].thunk_text_offset;
        size_t iat_off_in_rdata = l->imports[i].iat_rdata_offset;

        uint32_t thunk_rva = text_rva + (uint32_t)toff;
        uint32_t iat_rva = rdata_rva + (uint32_t)iat_off_in_rdata;
        /* disp = target - (rip_after_instruction)
         * instruction is 6 bytes: FF 25 xx xx xx xx
         * rip_after = thunk_rva + 6 */
        int32_t disp = (int32_t)(iat_rva - (thunk_rva + 6));
        memcpy(l->text.data + toff + 2, &disp, 4);
    }

    /* ---- 9. Compute final virtual addresses for symbols ---------- */
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
