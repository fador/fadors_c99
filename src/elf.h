#ifndef ELF_H
#define ELF_H

#include <stdint.h>

/* ELF identification bytes */
#define ELF_ELFMAG0       0x7f
#define ELF_ELFMAG1       'E'
#define ELF_ELFMAG2       'L'
#define ELF_ELFMAG3       'F'

#define ELF_ELFCLASS64    2
#define ELF_ELFDATA2LSB   1
#define ELF_EV_CURRENT    1
#define ELF_ELFOSABI_NONE 0

/* ELF object file types */
#define ELF_ET_REL        1   /* Relocatable object file */
#define ELF_ET_EXEC       2   /* Executable file */

/* Machine types */
#define ELF_EM_X86_64     62  /* AMD x86-64 */

/* Section header types */
#define ELF_SHT_NULL      0
#define ELF_SHT_PROGBITS  1
#define ELF_SHT_SYMTAB    2
#define ELF_SHT_STRTAB    3
#define ELF_SHT_RELA      4

/* Section header flags */
#define ELF_SHF_WRITE     0x1
#define ELF_SHF_ALLOC     0x2
#define ELF_SHF_EXECINSTR 0x4
#define ELF_SHF_INFO_LINK 0x40

/* Special section indices */
#define ELF_SHN_UNDEF     0

/* Symbol binding (upper 4 bits of st_info) */
#define ELF_STB_LOCAL     0
#define ELF_STB_GLOBAL    1

/* Symbol type (lower 4 bits of st_info) */
#define ELF_STT_NOTYPE    0
#define ELF_STT_FUNC      2

/* Symbol visibility */
#define ELF_STV_DEFAULT   0

/* ELF object file types */
#define ELF_ET_EXEC       2   /* Executable file */

/* Section header types (additions) */
#define ELF_SHT_NOBITS    8   /* .bss — occupies no file space */

/* Program header types */
#define ELF_PT_NULL       0
#define ELF_PT_LOAD       1

/* Segment permission flags */
#define ELF_PF_X          1   /* Execute */
#define ELF_PF_W          2   /* Write */
#define ELF_PF_R          4   /* Read */

/* Special section indices (additions) */
#define ELF_SHN_ABS       0xFFF1
#define ELF_SHN_COMMON    0xFFF2

/* Symbol type additions */
#define ELF_STT_OBJECT    1
#define ELF_STT_SECTION   3

/* Symbol binding additions */
#define ELF_STB_WEAK      2

/* x86-64 relocation types */
#define ELF_R_X86_64_64     1   /* S + A     (absolute 64-bit) */
#define ELF_R_X86_64_PC32   2   /* S + A - P (PC-relative 32-bit) */
#define ELF_R_X86_64_PLT32  4   /* L + A - P (PLT-relative 32-bit) */
#define ELF_R_X86_64_32    10   /* S + A     (absolute 32-bit, zero-ext) */
#define ELF_R_X86_64_32S   11   /* S + A     (absolute 32-bit, sign-ext) */

/* Macros */
#define ELF64_ST_INFO(bind, type)  (((bind) << 4) | ((type) & 0xf))
#define ELF64_ST_BIND(info)        ((info) >> 4)
#define ELF64_ST_TYPE(info)        ((info) & 0xf)
#define ELF64_R_INFO(sym, type)    (((uint64_t)(sym) << 32) | ((uint64_t)(type) & 0xffffffffULL))
#define ELF64_R_SYM(info)          ((uint32_t)((info) >> 32))
#define ELF64_R_TYPE(info)         ((uint32_t)((info) & 0xffffffffULL))

/* ELF64 file header (64 bytes) */
typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} Elf64_Ehdr;

/* ELF64 section header (64 bytes) */
typedef struct {
    uint32_t sh_name;
    uint32_t sh_type;
    uint64_t sh_flags;
    uint64_t sh_addr;
    uint64_t sh_offset;
    uint64_t sh_size;
    uint32_t sh_link;
    uint32_t sh_info;
    uint64_t sh_addralign;
    uint64_t sh_entsize;
} Elf64_Shdr;

/* ELF64 symbol table entry (24 bytes) */
typedef struct {
    uint32_t st_name;
    uint8_t  st_info;
    uint8_t  st_other;
    uint16_t st_shndx;
    uint64_t st_value;
    uint64_t st_size;
} Elf64_Sym;

/* ELF64 relocation entry with explicit addend (24 bytes) */
typedef struct {
    uint64_t r_offset;
    uint64_t r_info;
    int64_t  r_addend;
} Elf64_Rela;

/* ELF64 program header (56 bytes) */
typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} Elf64_Phdr;

#endif /* ELF_H */
