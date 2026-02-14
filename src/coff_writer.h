#ifndef COFF_WRITER_H
#define COFF_WRITER_H

#include "buffer.h"
#include "coff.h"

typedef struct {
    char *name;
    uint32_t value;
    int16_t section;
    uint16_t type;
    uint8_t storage_class;
} SymbolEntry;

typedef struct {
    uint32_t virtual_address;
    uint32_t symbol_index;
    uint16_t type;
} RelocEntry;

/* Debug line number entry: maps a .text offset to a source line */
typedef struct {
    uint32_t address;   /* offset within .text */
    uint32_t line;      /* 1-based source line number */
    uint8_t  is_stmt;   /* 1 = start of statement */
    uint8_t  end_seq;   /* 1 = end of sequence marker */
} DebugLineEntry;

typedef struct {
    Buffer text_section;
    Buffer data_section;
    Buffer string_table;
    
    SymbolEntry *symbols;
    size_t symbols_count;
    size_t symbols_capacity;
    
    RelocEntry *text_relocs;
    size_t text_relocs_count;
    size_t text_relocs_capacity;
    
    RelocEntry *data_relocs;
    size_t data_relocs_count;
    size_t data_relocs_capacity;

    /* Debug info (populated when -g is active) */
    char *debug_source_file;         /* source filename for .debug_line */
    char *debug_comp_dir;            /* compilation directory */
    DebugLineEntry *debug_lines;
    size_t debug_line_count;
    size_t debug_line_capacity;
} COFFWriter;

void coff_writer_init(COFFWriter *w);
void coff_writer_free(COFFWriter *w);
int32_t coff_writer_find_symbol(COFFWriter *w, const char *name);
uint32_t coff_writer_add_symbol(COFFWriter *w, const char *name, uint32_t value, int16_t section, uint16_t type, uint8_t storage_class);
void coff_writer_add_reloc(COFFWriter *w, uint32_t virtual_address, uint32_t symbol_index, uint16_t type, int section);
void coff_writer_write(COFFWriter *w, const char *filename);

/* Debug info helpers */
void coff_writer_set_debug_source(COFFWriter *w, const char *filename, const char *comp_dir);
void coff_writer_add_debug_line(COFFWriter *w, uint32_t address, uint32_t line, uint8_t is_stmt);

#endif
