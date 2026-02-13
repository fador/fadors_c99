/*
 * pe.h — PE/COFF format definitions for Windows x86-64 executables.
 *
 * Defines structures and constants for:
 *   - DOS stub header
 *   - PE signature
 *   - COFF file header (IMAGE_FILE_HEADER)
 *   - Optional header (IMAGE_OPTIONAL_HEADER64)
 *   - Section headers
 *   - Import directory tables
 *   - Data directory entries
 *
 * All structures use #pragma pack(push, 1) for exact binary layout.
 */
#ifndef PE_H
#define PE_H

#include <stdint.h>

#pragma pack(push, 1)

/* ------------------------------------------------------------------ */
/*  DOS Header (IMAGE_DOS_HEADER) — 64 bytes                         */
/* ------------------------------------------------------------------ */
typedef struct {
    uint16_t e_magic;     /* 0x5A4D = "MZ" */
    uint16_t e_cblp;
    uint16_t e_cp;
    uint16_t e_crlc;
    uint16_t e_cparhdr;
    uint16_t e_minalloc;
    uint16_t e_maxalloc;
    uint16_t e_ss;
    uint16_t e_sp;
    uint16_t e_csum;
    uint16_t e_ip;
    uint16_t e_cs;
    uint16_t e_lfarlc;
    uint16_t e_ovno;
    uint16_t e_res[4];
    uint16_t e_oemid;
    uint16_t e_oeminfo;
    uint16_t e_res2[10];
    uint32_t e_lfanew;    /* Offset to PE signature */
} PE_DosHeader;

#define PE_DOS_MAGIC  0x5A4D  /* "MZ" */

/* ------------------------------------------------------------------ */
/*  PE Signature                                                      */
/* ------------------------------------------------------------------ */
#define PE_SIGNATURE  0x00004550  /* "PE\0\0" */

/* ------------------------------------------------------------------ */
/*  COFF File Header (IMAGE_FILE_HEADER) — 20 bytes                  */
/* ------------------------------------------------------------------ */
typedef struct {
    uint16_t Machine;
    uint16_t NumberOfSections;
    uint32_t TimeDateStamp;
    uint32_t PointerToSymbolTable;
    uint32_t NumberOfSymbols;
    uint16_t SizeOfOptionalHeader;
    uint16_t Characteristics;
} PE_FileHeader;

#define PE_FILE_MACHINE_AMD64         0x8664

#define PE_FILE_RELOCS_STRIPPED       0x0001
#define PE_FILE_EXECUTABLE_IMAGE      0x0002
#define PE_FILE_LARGE_ADDRESS_AWARE   0x0020

/* ------------------------------------------------------------------ */
/*  Data Directory Entry — 8 bytes                                    */
/* ------------------------------------------------------------------ */
typedef struct {
    uint32_t VirtualAddress;
    uint32_t Size;
} PE_DataDirectory;

/* Data directory indices */
#define PE_DIR_EXPORT        0
#define PE_DIR_IMPORT        1
#define PE_DIR_RESOURCE      2
#define PE_DIR_EXCEPTION     3
#define PE_DIR_SECURITY      4
#define PE_DIR_BASERELOC     5
#define PE_DIR_DEBUG         6
#define PE_DIR_ARCHITECTURE  7
#define PE_DIR_GLOBALPTR     8
#define PE_DIR_TLS           9
#define PE_DIR_LOAD_CONFIG  10
#define PE_DIR_BOUND_IMPORT 11
#define PE_DIR_IAT          12
#define PE_DIR_DELAY_IMPORT 13
#define PE_DIR_CLR_RUNTIME  14
#define PE_DIR_RESERVED     15
#define PE_NUM_DATA_DIRS    16

/* ------------------------------------------------------------------ */
/*  Optional Header (IMAGE_OPTIONAL_HEADER64) — 240 bytes            */
/* ------------------------------------------------------------------ */
typedef struct {
    uint16_t Magic;                     /* 0x020B = PE32+ */
    uint8_t  MajorLinkerVersion;
    uint8_t  MinorLinkerVersion;
    uint32_t SizeOfCode;
    uint32_t SizeOfInitializedData;
    uint32_t SizeOfUninitializedData;
    uint32_t AddressOfEntryPoint;
    uint32_t BaseOfCode;
    uint64_t ImageBase;
    uint32_t SectionAlignment;
    uint32_t FileAlignment;
    uint16_t MajorOperatingSystemVersion;
    uint16_t MinorOperatingSystemVersion;
    uint16_t MajorImageVersion;
    uint16_t MinorImageVersion;
    uint16_t MajorSubsystemVersion;
    uint16_t MinorSubsystemVersion;
    uint32_t Win32VersionValue;
    uint32_t SizeOfImage;
    uint32_t SizeOfHeaders;
    uint32_t CheckSum;
    uint16_t Subsystem;
    uint16_t DllCharacteristics;
    uint64_t SizeOfStackReserve;
    uint64_t SizeOfStackCommit;
    uint64_t SizeOfHeapReserve;
    uint64_t SizeOfHeapCommit;
    uint32_t LoaderFlags;
    uint32_t NumberOfRvaAndSizes;
    PE_DataDirectory DataDirectory[PE_NUM_DATA_DIRS];
} PE_OptionalHeader64;

#define PE_OPT_MAGIC_PE32PLUS  0x020B

/* Subsystem values */
#define PE_SUBSYSTEM_CONSOLE   3
#define PE_SUBSYSTEM_WINDOWS   2

/* DllCharacteristics flags */
#define PE_DLLCHAR_HIGH_ENTROPY_VA    0x0020
#define PE_DLLCHAR_DYNAMIC_BASE       0x0040
#define PE_DLLCHAR_NX_COMPAT          0x0100
#define PE_DLLCHAR_TERMINAL_SERVER     0x8000

/* ------------------------------------------------------------------ */
/*  Section Header (IMAGE_SECTION_HEADER) — 40 bytes                 */
/* ------------------------------------------------------------------ */
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
} PE_SectionHeader;

/* Section characteristic flags */
#define PE_SCN_CNT_CODE               0x00000020
#define PE_SCN_CNT_INITIALIZED_DATA   0x00000040
#define PE_SCN_CNT_UNINITIALIZED_DATA 0x00000080
#define PE_SCN_MEM_EXECUTE            0x20000000
#define PE_SCN_MEM_READ               0x40000000
#define PE_SCN_MEM_WRITE              0x80000000

/* ------------------------------------------------------------------ */
/*  Import Directory Table Entry — 20 bytes                          */
/* ------------------------------------------------------------------ */
typedef struct {
    uint32_t OriginalFirstThunk;  /* RVA to Import Lookup Table (ILT) */
    uint32_t TimeDateStamp;
    uint32_t ForwarderChain;
    uint32_t Name;                /* RVA to DLL name string */
    uint32_t FirstThunk;          /* RVA to Import Address Table (IAT) */
} PE_ImportDescriptor;

/* ------------------------------------------------------------------ */
/*  Import Lookup Table Entry (64-bit)                               */
/* ------------------------------------------------------------------ */
/* Bit 63 = 0: import by name (bits 30:0 = hint/name RVA)
 * Bit 63 = 1: import by ordinal (bits 15:0 = ordinal)
 */
#define PE_ILT_ORDINAL_FLAG64  0x8000000000000000ULL

/* Import Hint/Name Table Entry (variable size) */
typedef struct {
    uint16_t Hint;
    /* char   Name[]; — null-terminated ASCII string follows */
} PE_ImportHintName;

/* ------------------------------------------------------------------ */
/*  COFF relocation types for AMD64 (same as coff.h, repeated for    */
/*  self-containment)                                                 */
/* ------------------------------------------------------------------ */
#define PE_REL_AMD64_ADDR64   0x0001
#define PE_REL_AMD64_ADDR32NB 0x0003
#define PE_REL_AMD64_REL32    0x0004

#pragma pack(pop)

#endif /* PE_H */
