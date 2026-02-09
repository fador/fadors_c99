#include "coff_writer.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

void coff_writer_init(COFFWriter *w) {
    buffer_init(&w->text_section);
    buffer_init(&w->data_section);
    buffer_init(&w->string_table);
    
    // String table starts with 4-byte size field
    buffer_write_dword(&w->string_table, 4);
    
    w->symbols_count = 0;
    w->symbols_capacity = 16;
    w->symbols = malloc(w->symbols_capacity * sizeof(SymbolEntry));
    
    w->relocs_count = 0;
    w->relocs_capacity = 16;
    w->relocs = malloc(w->relocs_capacity * sizeof(RelocEntry));
}

void coff_writer_free(COFFWriter *w) {
    buffer_free(&w->text_section);
    buffer_free(&w->data_section);
    buffer_free(&w->string_table);
    
    if (w->symbols) {
        for (size_t i = 0; i < w->symbols_count; i++) {
            free(w->symbols[i].name);
        }
        free(w->symbols);
    }
    if (w->relocs) {
        free(w->relocs);
    }
}

int32_t coff_writer_find_symbol(COFFWriter *w, const char *name) {
    for (size_t i = 0; i < w->symbols_count; i++) {
        if (strcmp(w->symbols[i].name, name) == 0) {
            return (int32_t)i;
        }
    }
    return -1;
}

uint32_t coff_writer_add_symbol(COFFWriter *w, const char *name, uint32_t value, int16_t section, uint16_t type, uint8_t storage_class) {
    int32_t existing = coff_writer_find_symbol(w, name);
    if (existing >= 0) {
        // Update existing symbol if it was undefined or we are providing a definition
        if (w->symbols[existing].section == 0 && section != 0) {
            w->symbols[existing].value = value;
            w->symbols[existing].section = section;
            w->symbols[existing].type = type;
        }
        return (uint32_t)existing;
    }

    if (w->symbols_count >= w->symbols_capacity) {
        w->symbols_capacity *= 2;
        w->symbols = realloc(w->symbols, w->symbols_capacity * sizeof(SymbolEntry));
    }
    
    uint32_t index = (uint32_t)w->symbols_count++;
    w->symbols[index].name = _strdup(name);
    w->symbols[index].value = value;
    w->symbols[index].section = section;
    w->symbols[index].type = type;
    w->symbols[index].storage_class = storage_class;
    
    return index;
}

void coff_writer_add_reloc(COFFWriter *w, uint32_t virtual_address, uint32_t symbol_index, uint16_t type) {
    if (w->relocs_count >= w->relocs_capacity) {
        w->relocs_capacity *= 2;
        w->relocs = realloc(w->relocs, w->relocs_capacity * sizeof(RelocEntry));
    }
    
    w->relocs[w->relocs_count].virtual_address = virtual_address;
    w->relocs[w->relocs_count].symbol_index = symbol_index;
    w->relocs[w->relocs_count].type = type;
    w->relocs_count++;
}

void coff_writer_write(COFFWriter *w, const char *filename) {
    FILE *f = fopen(filename, "wb");
    if (!f) return;
    
    COFFHeader header = {0};
    header.Machine = IMAGE_FILE_MACHINE_AMD64;
    header.NumberOfSections = 0;
    if (w->text_section.size > 0) header.NumberOfSections++;
    if (w->data_section.size > 0) header.NumberOfSections++;
    
    header.TimeDateStamp = (uint32_t)time(NULL);
    header.SizeOfOptionalHeader = 0;
    header.Characteristics = 0;
    
    uint32_t section_headers_pos = sizeof(COFFHeader);
    uint32_t current_data_pos = section_headers_pos + sizeof(COFFSectionHeader) * header.NumberOfSections;
    
    uint32_t text_data_pos = 0;
    uint32_t text_relocs_pos = 0;
    uint32_t data_data_pos = 0;
    
    if (w->text_section.size > 0) {
        text_data_pos = current_data_pos;
        current_data_pos += (uint32_t)w->text_section.size;
        text_relocs_pos = current_data_pos;
        current_data_pos += (uint32_t)w->relocs_count * sizeof(COFFRelocation);
    }
    
    if (w->data_section.size > 0) {
        data_data_pos = current_data_pos;
        current_data_pos += (uint32_t)w->data_section.size;
    }
    
    header.PointerToSymbolTable = current_data_pos;
    header.NumberOfSymbols = (uint32_t)w->symbols_count;
    
    fwrite(&header, sizeof(header), 1, f);
    
    uint16_t section_idx = 1;
    if (w->text_section.size > 0) {
        COFFSectionHeader text_sh = {0};
        memcpy(text_sh.Name, ".text\0\0\0", 8);
        text_sh.SizeOfRawData = (uint32_t)w->text_section.size;
        text_sh.PointerToRawData = text_data_pos;
        text_sh.PointerToRelocations = text_relocs_pos;
        text_sh.NumberOfRelocations = (uint16_t)w->relocs_count;
        text_sh.Characteristics = IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ | IMAGE_SCN_ALIGN_16BYTES;
        fwrite(&text_sh, sizeof(text_sh), 1, f);
        section_idx++;
    }
    
    if (w->data_section.size > 0) {
        COFFSectionHeader data_sh = {0};
        memcpy(data_sh.Name, ".data\0\0\0", 8);
        data_sh.SizeOfRawData = (uint32_t)w->data_section.size;
        data_sh.PointerToRawData = data_data_pos;
        data_sh.Characteristics = IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE | IMAGE_SCN_ALIGN_4BYTES;
        fwrite(&data_sh, sizeof(data_sh), 1, f);
    }
    
    // Write Section Data
    if (w->text_section.size > 0) {
        fwrite(w->text_section.data, 1, w->text_section.size, f);
        for (size_t i = 0; i < w->relocs_count; i++) {
            COFFRelocation r;
            r.VirtualAddress = w->relocs[i].virtual_address;
            r.SymbolTableIndex = w->relocs[i].symbol_index;
            r.Type = w->relocs[i].type;
            fwrite(&r, sizeof(r), 1, f);
        }
    }
    
    if (w->data_section.size > 0) {
        fwrite(w->data_section.data, 1, w->data_section.size, f);
    }
    
    // Symbol Table
    for (size_t i = 0; i < w->symbols_count; i++) {
        COFFSymbol sym = {0};
        if (strlen(w->symbols[i].name) <= 8) {
            memcpy(sym.N.ShortName, w->symbols[i].name, strlen(w->symbols[i].name));
        } else {
            sym.N.Name.Zeroes = 0;
            sym.N.Name.Offset = (uint32_t)w->string_table.size;
            buffer_write_bytes(&w->string_table, w->symbols[i].name, strlen(w->symbols[i].name) + 1);
            // Update string table size at the beginning
            uint32_t total_size = (uint32_t)w->string_table.size;
            memcpy(w->string_table.data, &total_size, 4);
        }
        sym.Value = w->symbols[i].value;
        sym.SectionNumber = w->symbols[i].section;
        sym.Type = w->symbols[i].type;
        sym.StorageClass = w->symbols[i].storage_class;
        sym.NumberOfAuxSymbols = 0;
        fwrite(&sym, sizeof(sym), 1, f);
    }
    
    fwrite(w->string_table.data, 1, w->string_table.size, f);
    fclose(f);
}
