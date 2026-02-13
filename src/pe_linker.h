/*
 * pe_linker.h — Windows PE/COFF linker for x86-64
 *
 * Links one or more COFF relocatable objects (.obj) into a PE executable.
 * Supports dynamic linking against Windows DLLs (e.g., kernel32.dll)
 * via import table generation.
 *
 * Public API mirrors the ELF linker (linker.h) for consistency.
 */
#ifndef PE_LINKER_H
#define PE_LINKER_H

#include "buffer.h"
#include <stdint.h>
#include <stddef.h>

/* Section IDs used inside the PE linker */
#define PE_LINK_SEC_UNDEF  0
#define PE_LINK_SEC_TEXT   1
#define PE_LINK_SEC_DATA   2
#define PE_LINK_SEC_BSS    3
#define PE_LINK_SEC_RDATA  4

/* Symbol storage class (COFF) */
#define PE_SYM_CLASS_EXTERNAL  2
#define PE_SYM_CLASS_STATIC    3

typedef struct {
    char    *name;
    uint64_t value;        /* offset within section */
    int      section;      /* PE_LINK_SEC_* */
    uint8_t  storage_class;
    uint16_t type;
    uint64_t size;
} PELinkSymbol;

typedef struct {
    uint64_t offset;       /* offset within section buffer */
    int      section;      /* which section the reloc is in */
    uint32_t sym_index;    /* index into pe_linker symbols */
    uint32_t type;         /* COFF relocation type */
} PELinkReloc;

/* Import entry: a function imported from a DLL */
typedef struct {
    char    *func_name;    /* function name (e.g., "ExitProcess") */
    char    *dll_name;     /* DLL name (e.g., "kernel32.dll") */
    uint32_t sym_index;    /* corresponding symbol index */
    uint16_t hint;         /* ordinal hint (0 if unknown) */
} PEImportEntry;

/* Grouped by DLL for import table generation */
typedef struct {
    char    *dll_name;
    size_t  *import_indices;  /* indices into PELinker.imports */
    size_t   count;
    size_t   cap;
} PEImportDll;

typedef struct {
    Buffer   text;
    Buffer   data;
    Buffer   rdata;
    size_t   bss_size;

    PELinkSymbol *symbols;
    size_t        sym_count;
    size_t        sym_cap;

    PELinkReloc  *relocs;
    size_t        reloc_count;
    size_t        reloc_cap;

    PEImportEntry *imports;
    size_t         import_count;
    size_t         import_cap;

    PEImportDll   *import_dlls;
    size_t         dll_count;
    size_t         dll_cap;

    /* Default DLL imports (e.g., kernel32.dll:ExitProcess) */
    int            no_default_imports;

    /* Subsystem */
    uint16_t       subsystem;   /* PE_SUBSYSTEM_CONSOLE by default */
    uint64_t       stack_reserve;
    uint64_t       image_base;
    const char    *entry_name;  /* default: "main" */
} PELinker;

/* Create / destroy */
PELinker *pe_linker_new(void);
void      pe_linker_free(PELinker *l);

/* Add a COFF .obj file.  Returns 0 on success. */
int  pe_linker_add_object_file(PELinker *l, const char *path);

/* Declare an imported function from a DLL.
 * If sym_index is (uint32_t)-1, will be looked up by name. */
void pe_linker_add_import(PELinker *l, const char *func_name,
                          const char *dll_name, uint16_t hint);

/* Perform linking.  Returns 0 on success. */
int  pe_linker_link(PELinker *l, const char *output_path);

#endif /* PE_LINKER_H */
