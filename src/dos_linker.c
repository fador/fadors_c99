#include "dos_linker.h"
#include "dos_stub.h"
#include "coff.h"
#include "buffer.h" /* Buffer functions */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Section IDs */
#define SEC_UNDEF 0
#define SEC_TEXT  1
#define SEC_DATA  2
#define SEC_BSS   3
#define SEC_RDATA 4

/* Helper: Duplicate string */
static char *my_strdup(const char *s) {
    size_t len = strlen(s);
    char *p = malloc(len + 1);
    memcpy(p, s, len + 1);
    return p;
}

DosLinker *dos_linker_new(void) {
    DosLinker *l = calloc(1, sizeof(DosLinker));
    buffer_init(&l->text);
    buffer_init(&l->data);
    buffer_init(&l->rdata);
    buffer_init(&l->bss);
    l->entry_name = "main"; /* Default to main for now */
    l->image_base = 0;      /* Will be set to size of stub later? Or 0 relative to segment? */
    return l;
}

void dos_linker_free(DosLinker *l) {
    if (!l) return;
    buffer_free(&l->text);
    buffer_free(&l->data);
    buffer_free(&l->rdata);
    buffer_free(&l->bss);
    for (size_t i = 0; i < l->sym_count; i++) free(l->symbols[i].name);
    free(l->symbols);
    free(l->relocs);
    for (size_t i = 0; i < l->lib_path_count; i++) free(l->lib_paths[i]);
    free(l->lib_paths);
    for (size_t i = 0; i < l->lib_count; i++) free(l->libraries[i]);
    free(l->libraries);
    free(l);
}

void dos_linker_add_lib_path(DosLinker *l, const char *path) {
    l->lib_paths = realloc(l->lib_paths, (l->lib_path_count + 1) * sizeof(char *));
    l->lib_paths[l->lib_path_count++] = my_strdup(path);
}

void dos_linker_add_library(DosLinker *l, const char *name) {
    l->libraries = realloc(l->libraries, (l->lib_count + 1) * sizeof(char *));
    l->libraries[l->lib_count++] = my_strdup(name);
}

void dos_linker_set_entry(DosLinker *l, const char *name) {
    l->entry_name = name;
}

/* Symbol Management */
static int dos_find_global(DosLinker *l, const char *name) {
    for (size_t i = 0; i < l->sym_count; i++) {
        if (l->symbols[i].storage_class != 3 /* STATIC */ &&
            strcmp(l->symbols[i].name, name) == 0)
            return (int)i;
    }
    return -1;
}

static uint32_t dos_add_sym(DosLinker *l, const char *name, uint64_t value, int section, uint8_t sc, uint16_t type) {
    if (l->sym_count >= l->sym_cap) {
        l->sym_cap = l->sym_cap ? l->sym_cap * 2 : 256;
        l->symbols = realloc(l->symbols, l->sym_cap * sizeof(DosLinkSymbol));
    }
    DosLinkSymbol *s = &l->symbols[l->sym_count];
    s->name = my_strdup(name);
    s->value = value;
    s->section = section;
    s->storage_class = sc;
    s->type = type;
    return (uint32_t)l->sym_count++;
}

/* Relocation Management */
static void dos_add_reloc(DosLinker *l, uint64_t offset, int section, uint32_t sym_index, uint32_t type) {
    if (l->reloc_count >= l->reloc_cap) {
        l->reloc_cap = l->reloc_cap ? l->reloc_cap * 2 : 256;
        l->relocs = realloc(l->relocs, l->reloc_cap * sizeof(DosLinkReloc));
    }
    DosLinkReloc *r = &l->relocs[l->reloc_count++];
    r->offset = offset;
    r->section = section;
    r->sym_index = sym_index;
    r->type = type;
}

/* COFF Reader (Simplified from pe_linker.c) */
static int dos_read_coff_object(DosLinker *l, const unsigned char *data, size_t file_size, const char *filename) {
    if (file_size < sizeof(COFFHeader)) return -1;
    const COFFHeader *hdr = (const COFFHeader *)data;
    
    /* Support i386 */
    if (hdr->Machine != 0x014C && hdr->Machine != 0x8664) {
        fprintf(stderr, "dos_linker: %s: unsupported machine type 0x%04X\n", filename, hdr->Machine);
        return -1;
    }
    
    int num_sec = hdr->NumberOfSections;
    const COFFSectionHeader *shdrs = (const COFFSectionHeader *)(data + sizeof(COFFHeader));
    
    /* Symbols */
    const COFFSymbol *symtab = NULL;
    const char *strtab = NULL;
    size_t strtab_size = 0;
    int sym_count = 0;
    
    if (hdr->PointerToSymbolTable != 0) {
        symtab = (const COFFSymbol *)(data + hdr->PointerToSymbolTable);
        sym_count = (int)hdr->NumberOfSymbols;
        size_t sym_end = hdr->PointerToSymbolTable + sym_count * sizeof(COFFSymbol);
        if (sym_end + 4 <= file_size) {
            strtab = (const char *)(data + sym_end);
            uint32_t st_sz;
            memcpy(&st_sz, strtab, 4);
            strtab_size = st_sz;
        }
    }
    
    int *sec_id = calloc(num_sec, sizeof(int));
    size_t *sec_base = calloc(num_sec, sizeof(size_t));
    
    /* Process sections */
    for (int i = 0; i < num_sec; i++) {
        char sec_name[9];
        memset(sec_name, 0, 9);
        if (shdrs[i].Name[0] == '/') {
            int off = atoi((const char *)shdrs[i].Name + 1);
            if (strtab && off >= 0) strncpy(sec_name, strtab + off, 8);
        } else {
            memcpy(sec_name, shdrs[i].Name, 8);
        }
        
        if (strcmp(sec_name, ".text") == 0) {
            sec_base[i] = l->text.size;
            sec_id[i] = SEC_TEXT;
            buffer_write_bytes(&l->text, data + shdrs[i].PointerToRawData, shdrs[i].SizeOfRawData);
        } else if (strcmp(sec_name, ".data") == 0) {
            sec_base[i] = l->data.size;
            sec_id[i] = SEC_DATA;
            buffer_write_bytes(&l->data, data + shdrs[i].PointerToRawData, shdrs[i].SizeOfRawData);
        } else if (strcmp(sec_name, ".rdata") == 0) {
            sec_base[i] = l->rdata.size;
            sec_id[i] = SEC_RDATA;
            buffer_write_bytes(&l->rdata, data + shdrs[i].PointerToRawData, shdrs[i].SizeOfRawData);
        } else if (strcmp(sec_name, ".bss") == 0) {
            sec_base[i] = l->bss_size;
            sec_id[i] = SEC_BSS;
            l->bss_size += shdrs[i].SizeOfRawData; /* Use RawDataSize for BSS in COFF usually? Or VirtualSize? */
        } else {
            sec_id[i] = SEC_UNDEF;
        }
    }
    
    /* Process symbols */
    uint32_t *sym_map = calloc(sym_count, sizeof(uint32_t));
    for (int i = 0; i < sym_count; i++) {
        const COFFSymbol *cs = &symtab[i];
        char name_buf[256];
        if (cs->N.Name.Zeroes == 0) {
            if (strtab) strncpy(name_buf, strtab + cs->N.Name.Offset, 255);
        } else {
            memcpy(name_buf, cs->N.ShortName, 8);
            name_buf[8] = '\0';
        }
        
        if (cs->StorageClass == 0x67 /* FILE */) { sym_map[i] = (uint32_t)-1; i += cs->NumberOfAuxSymbols; continue; }
        
        int section = SEC_UNDEF;
        uint64_t value = cs->Value;
        if (cs->SectionNumber > 0 && cs->SectionNumber <= num_sec) {
            int idx = cs->SectionNumber - 1;
            section = sec_id[idx];
            value += sec_base[idx];
        }
        
        if (cs->StorageClass == 2 /* EXTERNAL */) {
             int existing = dos_find_global(l, name_buf);
             if (existing >= 0) {
                 if (section != SEC_UNDEF && l->symbols[existing].section == SEC_UNDEF) {
                     l->symbols[existing].section = section;
                     l->symbols[existing].value = value;
                 }
                 sym_map[i] = existing;
             } else {
                 sym_map[i] = dos_add_sym(l, name_buf, value, section, 2, cs->Type);
             }
        } else {
            sym_map[i] = dos_add_sym(l, name_buf, value, section, cs->StorageClass, cs->Type);
        }
        i += cs->NumberOfAuxSymbols;
    }
    
    /* Process relocations */
    for (int i = 0; i < num_sec; i++) {
        if (sec_id[i] == SEC_UNDEF) continue;
        const COFFRelocation *relocs = (const COFFRelocation *)(data + shdrs[i].PointerToRelocations);
        for (int r = 0; r < shdrs[i].NumberOfRelocations; r++) {
             uint32_t new_sym = sym_map[relocs[r].SymbolTableIndex];
             if (new_sym != (uint32_t)-1) {
                 dos_add_reloc(l, relocs[r].VirtualAddress + sec_base[i], sec_id[i], new_sym, relocs[r].Type);
             }
        }
    }
    
    free(sym_map);
    free(sec_id);
    free(sec_base);
    return 0;
}

int dos_linker_add_object_file(DosLinker *l, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *buf = malloc(sz);
    fread(buf, 1, sz, f);
    fclose(f);
    int rc = dos_read_coff_object(l, buf, sz, path);
    free(buf);
    return rc;
}

int dos_linker_link(DosLinker *l, const char *output_path) {
    /* Layout: [Stub] [Text] [RData] [Data] */
    uint64_t stub_size = sizeof(dos_stub);
    uint64_t text_base = stub_size;
    
    /* Align sections? Flat binary usually 16-byte aligned or more */
    /* Let's align text to 16 bytes */
    if (text_base % 16 != 0) text_base = (text_base + 15) & ~15;
    
    uint64_t rdata_base = text_base + l->text.size;
    if (rdata_base % 16 != 0) rdata_base = (rdata_base + 15) & ~15;
    
    uint64_t data_base = rdata_base + l->rdata.size;
    if (data_base % 16 != 0) data_base = (data_base + 15) & ~15;
    
    uint64_t bss_base = data_base + l->data.size;
    
    /* Resolve symbols */
    for (size_t i = 0; i < l->sym_count; i++) {
        DosLinkSymbol *s = &l->symbols[i];
        if (s->section == SEC_TEXT) s->value += text_base;
        else if (s->section == SEC_RDATA) s->value += rdata_base;
        else if (s->section == SEC_DATA) s->value += data_base;
        else if (s->section == SEC_BSS) s->value += bss_base;
    }
    
    /* Apply relocations */
    for (size_t i = 0; i < l->reloc_count; i++) {
        DosLinkReloc *r = &l->relocs[i];
        uint64_t base_off = 0;
        unsigned char *buf = NULL;
        
        if (r->section == SEC_TEXT) { base_off = text_base; buf = l->text.data; }
        else if (r->section == SEC_RDATA) { base_off = rdata_base; buf = l->rdata.data; }
        else if (r->section == SEC_DATA) { base_off = data_base; buf = l->data.data; }
        
        if (!buf) continue;
        
        /* Symbol address */
        uint64_t sym_val = l->symbols[r->sym_index].value;
        uint64_t patch_offset = r->offset; /* Offset within segment buffer */
        
        uint32_t val32 = 0;
        memcpy(&val32, buf + patch_offset, 4);
        
        /* IMAGE_REL_I386_DIR32 (0x0006) */
        if (r->type == 0x0006) {
            /* Absolute: S + A */
            val32 += (uint32_t)sym_val;
            memcpy(buf + patch_offset, &val32, 4);
        }
        /* IMAGE_REL_I386_REL32 (0x0014) */
        else if (r->type == 0x0014) {
            /* Relative: S + A - P */
            /* P = virtual address of reloc itself */
            uint64_t P = base_off + patch_offset;
            val32 += (uint32_t)(sym_val - (P + 4)); /* +4 for relative correction? standard x86 is relative to END of instruction */
            memcpy(buf + patch_offset, &val32, 4);
        }
    }
    
    /* Write Output */
    FILE *f = fopen(output_path, "wb");
    if (!f) return -1;
    
    /* Write MZ Stub */
    fwrite(dos_stub, 1, sizeof(dos_stub), f);
    
    /* Pad to text_base */
    uint64_t current_pos = sizeof(dos_stub);
    while (current_pos < text_base) { fputc(0, f); current_pos++; }
    
    /* Write Sections */
    fwrite(l->text.data, 1, l->text.size, f);
    current_pos += l->text.size;
    
    while (current_pos < rdata_base) { fputc(0, f); current_pos++; }
    fwrite(l->rdata.data, 1, l->rdata.size, f);
    current_pos += l->rdata.size;
    
    while (current_pos < data_base) { fputc(0, f); current_pos++; }
    fwrite(l->data.data, 1, l->data.size, f);
    
    fclose(f);
    return 0;
}
