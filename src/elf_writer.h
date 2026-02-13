#ifndef ELF_WRITER_H
#define ELF_WRITER_H

#include "coff_writer.h"

/*
 * Write the contents of a COFFWriter as an ELF64 relocatable object file (.o).
 * The COFFWriter is used as a format-agnostic in-memory representation;
 * this function serializes it in ELF format (with Rela relocations).
 */
void elf_writer_write(COFFWriter *w, const char *filename);

#endif /* ELF_WRITER_H */
