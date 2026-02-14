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

/* Debug type encoding (mirrors DWARF base type encodings) */
typedef enum {
    DBG_TYPE_VOID = 0,
    DBG_TYPE_INT,
    DBG_TYPE_UINT,
    DBG_TYPE_CHAR,
    DBG_TYPE_UCHAR,
    DBG_TYPE_SHORT,
    DBG_TYPE_USHORT,
    DBG_TYPE_LONG,
    DBG_TYPE_ULONG,
    DBG_TYPE_LONGLONG,
    DBG_TYPE_ULONGLONG,
    DBG_TYPE_FLOAT,
    DBG_TYPE_DOUBLE,
    DBG_TYPE_PTR,
    DBG_TYPE_ARRAY,
    DBG_TYPE_STRUCT,
    DBG_TYPE_UNION,
    DBG_TYPE_ENUM
} DebugTypeKind;

/* Debug variable entry: a local variable or parameter in a function */
typedef struct {
    char    *name;          /* variable name */
    int32_t  rbp_offset;    /* offset from %rbp (negative = locals, positive = stack params) */
    uint8_t  is_param;      /* 1 = formal parameter, 0 = local variable */
    uint8_t  type_kind;     /* DebugTypeKind */
    int32_t  type_size;     /* size in bytes */
    char    *type_name;     /* type name (for struct/union/enum), NULL for basic types */
} DebugVarEntry;

/* Debug function entry: a subprogram with its variables */
typedef struct {
    char    *name;          /* function name */
    uint32_t start_addr;    /* .text offset of first instruction */
    uint32_t end_addr;      /* .text offset past last instruction */
    uint8_t  ret_type_kind; /* DebugTypeKind of return type */
    int32_t  ret_type_size; /* size of return type */
    DebugVarEntry *vars;    /* array of variables/parameters */
    size_t   var_count;
    size_t   var_capacity;
} DebugFuncEntry;

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

    /* Relocations for .debug$S section */
    RelocEntry *debugs_relocs;
    size_t debugs_relocs_count;
    size_t debugs_relocs_capacity;

    /* Debug info (populated when -g is active) */
    char *debug_source_file;         /* source filename for .debug_line */
    char *debug_comp_dir;            /* compilation directory */
    DebugLineEntry *debug_lines;
    size_t debug_line_count;
    size_t debug_line_capacity;
    DebugFuncEntry *debug_funcs;
    size_t debug_func_count;
    size_t debug_func_capacity;
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
void coff_writer_begin_debug_func(COFFWriter *w, const char *name, uint32_t start_addr,
                                   uint8_t ret_type_kind, int32_t ret_type_size);
void coff_writer_end_debug_func(COFFWriter *w, uint32_t end_addr);
void coff_writer_add_debug_var(COFFWriter *w, const char *name, int32_t rbp_offset,
                                uint8_t is_param, uint8_t type_kind, int32_t type_size,
                                const char *type_name);

#endif
