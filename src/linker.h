/*
 * linker.h — ELF static linker for x86-64 Linux
 *
 * Links one or more ELF relocatable objects (.o) and static archives (.a)
 * into a static ELF executable.  Provides a built-in _start stub so that
 * no external CRT is required for simple programs.
 */
#ifndef LINKER_H
#define LINKER_H

#include "buffer.h"
#include <stdint.h>
#include <stddef.h>

/* Section IDs used inside the linker */
#define LINK_SEC_UNDEF  0
#define LINK_SEC_TEXT   1
#define LINK_SEC_DATA   2
#define LINK_SEC_BSS    3

typedef struct {
    char    *name;
    uint64_t value;
    int      section;
    uint8_t  binding;
    uint8_t  type;
    uint64_t size;
} LinkSymbol;

typedef struct {
    uint64_t offset;
    int      section;
    uint32_t sym_index;
    uint32_t type;
    int64_t  addend;
} LinkReloc;

/* Debug line entries (read from .fadors_debug sections) */
typedef struct {
    uint32_t address;   /* offset within merged .text */
    uint32_t line;      /* 1-based source line number */
    uint8_t  is_stmt;
    uint8_t  end_seq;
} LinkDebugLine;

typedef struct {
    Buffer   text;
    Buffer   data;
    size_t   bss_size;

    LinkSymbol *symbols;
    size_t      sym_count;
    size_t      sym_cap;

    LinkReloc  *relocs;
    size_t      reloc_count;
    size_t      reloc_cap;

    char      **lib_paths;
    size_t      lib_path_count;

    char      **libraries;
    size_t      lib_count;

    /* Debug info */
    char           *debug_source_file;
    char           *debug_comp_dir;
    LinkDebugLine  *debug_lines;
    size_t          debug_line_count;
    size_t          debug_line_cap;
} Linker;

/* Create / destroy */
Linker *linker_new(void);
void    linker_free(Linker *l);

/* Add an ELF .o file.  Returns 0 on success. */
int  linker_add_object_file(Linker *l, const char *path);

/* Add a library search directory (-L<path>). */
void linker_add_lib_path(Linker *l, const char *path);

/* Add a library to link against (-l<name>  =>  lib<name>.a). */
void linker_add_library(Linker *l, const char *name);

/* Perform linking.  Returns 0 on success. */
int  linker_link(Linker *l, const char *output_path);

#endif /* LINKER_H */
