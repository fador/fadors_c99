#ifndef COFF_H
#define COFF_H

#include <stdint.h>

#pragma pack(push, 1)

// IMAGE_FILE_HEADER
typedef struct {
    uint16_t Machine;
    uint16_t NumberOfSections;
    uint32_t TimeDateStamp;
    uint32_t PointerToSymbolTable;
    uint32_t NumberOfSymbols;
    uint16_t SizeOfOptionalHeader;
    uint16_t Characteristics;
} COFFHeader;

#define IMAGE_FILE_MACHINE_AMD64 0x8664
#define IMAGE_FILE_MACHINE_I386  0x014c

// IMAGE_SECTION_HEADER
typedef struct {
    uint8_t  Name[8];
    uint32_t VirtualSize;
    uint32_t VirtualAddress;
    uint32_t SizeOfRawData;
    uint32_t PointerToRawData;
    uint32_t PointerToRelocations;
    uint32_t PointerToLinenumbers;
    uint16_t NumberOfRelocations;
    uint16_t NumberOfLinenumbers;
    uint32_t Characteristics;
} COFFSectionHeader;

#define IMAGE_SCN_CNT_CODE               0x00000020
#define IMAGE_SCN_CNT_INITIALIZED_DATA   0x00000040
#define IMAGE_SCN_CNT_UNINITIALIZED_DATA 0x00000080
#define IMAGE_SCN_LNK_INFO               0x00000200
#define IMAGE_SCN_LNK_REMOVE             0x00000800
#define IMAGE_SCN_MEM_EXECUTE            0x20000000
#define IMAGE_SCN_MEM_READ               0x40000000
#define IMAGE_SCN_MEM_WRITE              0x80000000
#define IMAGE_SCN_ALIGN_4BYTES           0x00300000
#define IMAGE_SCN_ALIGN_16BYTES          0x00500000

// IMAGE_SYMBOL
typedef struct {
    union {
        uint8_t ShortName[8];
        struct {
            uint32_t Zeroes;
            uint32_t Offset;
        } Name;
    } N;
    uint32_t Value;
    int16_t  SectionNumber;
    uint16_t Type;
    uint8_t  StorageClass;
    uint8_t  NumberOfAuxSymbols;
} COFFSymbol;

#define IMAGE_SYM_CLASS_EXTERNAL 2
#define IMAGE_SYM_CLASS_STATIC   3
#define IMAGE_SYM_DTYPE_FUNCTION 0x20

// IMAGE_RELOCATION
typedef struct {
    uint32_t VirtualAddress;
    uint32_t SymbolTableIndex;
    uint16_t Type;
} COFFRelocation;

#define IMAGE_REL_AMD64_ADDR64  0x0001
#define IMAGE_REL_AMD64_ADDR32NB 0x0003
#define IMAGE_REL_AMD64_REL32  0x0004
#define IMAGE_REL_AMD64_SECTION 0x000A
#define IMAGE_REL_AMD64_SECREL  0x000B

/* Additional section characteristics */
#define IMAGE_SCN_MEM_DISCARDABLE 0x02000000
#define IMAGE_SCN_ALIGN_1BYTES    0x00100000

/* ===================================================================
 * CodeView Debug Format Constants
 * =================================================================== */

/* CodeView signature (used at start of .debug$S and .debug$T) */
#define CV_SIGNATURE_C13    4

/* --- Debug subsection types (in .debug$S) --- */
#define DEBUG_S_SYMBOLS      0xF1
#define DEBUG_S_LINES        0xF2
#define DEBUG_S_STRINGTABLE  0xF3
#define DEBUG_S_FILECHKSMS   0xF4

/* --- CodeView Symbol Record Types --- */
#define S_END           0x0006
#define S_OBJNAME       0x1101
#define S_LDATA32       0x110C
#define S_GDATA32       0x110D
#define S_LPROC32       0x110F
#define S_GPROC32       0x1110
#define S_REGREL32      0x1111
#define S_FRAMEPROC     0x1012
#define S_COMPILE3      0x113C
#define S_LOCAL         0x113E
#define S_DEFRANGE_REGISTER_REL 0x1145

/* --- CodeView Type Leaf Kinds --- */
#define LF_MODIFIER     0x1001
#define LF_POINTER      0x1002
#define LF_PROCEDURE    0x1008
#define LF_ARGLIST      0x1201
#define LF_FIELDLIST    0x1203
#define LF_STRUCTURE    0x1505
#define LF_UNION        0x1506
#define LF_ENUM         0x1507
#define LF_ARRAY        0x1503

/* --- CodeView Basic Type Indices --- */
#define T_NOTYPE        0x0000
#define T_VOID          0x0003
#define T_CHAR          0x0010
#define T_UCHAR         0x0020
#define T_SHORT         0x0011
#define T_USHORT        0x0021
#define T_LONG          0x0012
#define T_ULONG         0x0022
#define T_QUAD          0x0013  /* int64 */
#define T_UQUAD         0x0023
#define T_REAL32        0x0040  /* float */
#define T_REAL64        0x0041  /* double */
#define T_INT4          0x0074  /* 32-bit int */
#define T_UINT4         0x0075
#define T_INT8          0x0076  /* 64-bit int */
#define T_UINT8         0x0077

/* Near 64-bit pointer modifiers: add to base type */
#define T_64PTR_MODE    0x0600
#define T_64PVOID       0x0603

/* --- CodeView AMD64 Register Numbers --- */
#define CV_AMD64_RAX    17
#define CV_AMD64_RBX    20
#define CV_AMD64_RBP    334
#define CV_AMD64_RSP    335

/* --- CodeView Compile3 Machine Types --- */
#define CV_CFL_AMD64    0xD0

/* --- CodeView Compile3 Language --- */
#define CV_CFL_C        0x00

/* --- CodeView File Checksum Types --- */
#define CHKSUM_TYPE_NONE    0
#define CHKSUM_TYPE_MD5     1
#define CHKSUM_TYPE_SHA1    2
#define CHKSUM_TYPE_SHA256  3

/* --- CodeView Proc Flags --- */
#define CV_PFLAG_NONE   0x00

#pragma pack(pop)

#endif
