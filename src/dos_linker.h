#ifndef DOS_LINKER_H
#define DOS_LINKER_H

#include "buffer.h"
#include <stdint.h>
#include <stddef.h>

/* DOS Linker: merges COFF objects into a flat binary appended to an MZ stub */

typedef struct {
    char    *name;
    uint64_t value;        /* offset within section */
    int      section;      /* Section ID */
    uint8_t  storage_class;
    uint16_t type;
} DosLinkSymbol;

typedef struct {
    uint64_t offset;       /* offset within section buffer */
    int      section;      /* which section the reloc is in */
    uint32_t sym_index;    /* index into symbols */
    uint32_t type;         /* COFF relocation type */
} DosLinkReloc;

typedef struct {
    Buffer   text;
    Buffer   data;
    Buffer   rdata;
    Buffer   bss;          /* Note: BSS is not written to file, but tracked for address */
    size_t   bss_size;

    DosLinkSymbol *symbols;
    size_t         sym_count;
    size_t         sym_cap;

    DosLinkReloc  *relocs;
    size_t         reloc_count;
    size_t         reloc_cap;

    /* Library support */
    char      **lib_paths;
    size_t      lib_path_count;
    char      **libraries;
    size_t      lib_count;

    uint64_t       image_base; /* Base address of the flat code (e.g. 0 or stub size) */
    const char    *entry_name; /* Entry point symbol (e.g. "_start" or "main") */
} DosLinker;

DosLinker *dos_linker_new(void);
void       dos_linker_free(DosLinker *l);

int  dos_linker_add_object_file(DosLinker *l, const char *path);
void dos_linker_add_lib_path(DosLinker *l, const char *path);
void dos_linker_add_library(DosLinker *l, const char *name);
void dos_linker_set_entry(DosLinker *l, const char *name);

int  dos_linker_link(DosLinker *l, const char *output_path);

#endif
