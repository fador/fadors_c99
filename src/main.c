#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include "lexer.h"
#include "parser.h"
#include "ast.h"
#include "codegen.h"
#include "preprocessor.h"
#include "coff_writer.h"
#include "elf_writer.h"
#include "linker.h"
#include "pe_linker.h"
#include "types.h"

#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#define getcwd _getcwd
#else
#include <unistd.h>
#endif

// Manually declare needed Windows API to avoid header conflicts with coff.h
#ifdef _WIN32
#ifndef _MSC_VER
#define __declspec(x)
#define __stdcall
#endif
typedef void *HANDLE;
#define STD_OUTPUT_HANDLE ((unsigned long)-11)
#define STD_ERROR_HANDLE ((unsigned long)-12)
__declspec(dllimport) HANDLE __stdcall GetStdHandle(unsigned long nStdHandle);
__declspec(dllimport) int __stdcall WriteFile(HANDLE hFile, const void *lpBuffer, unsigned long nNumberOfBytesToWrite, unsigned long *lpNumberOfBytesWritten, void *lpOverlapped);
#endif

// ---------- Tool modes ----------
typedef enum {
    MODE_AUTO,      // decide from file extension
    MODE_CC,        // compile C source
    MODE_AS,        // assemble .s/.asm to object
    MODE_LINK       // link objects (stub/reservation)
} ToolMode;

// ---------- Pipeline stop point ----------
typedef enum {
    STOP_FULL,      // compile + assemble + link (default)
    STOP_ASM,       // -S : stop after generating assembly
    STOP_OBJ        // -c / --obj : stop after object file
} StopAfter;

// Large state moved to heap to avoid segment/initialization issues
static Parser *current_parser;
static COFFWriter *current_writer;

// Helper to execute a command
static int run_command(const char *cmd) {
    printf("[CMD] %s\n", cmd);
    return system(cmd);
}

// ---------- Helpers ----------
static const char *file_extension(const char *path) {
    const char *dot = strrchr(path, '.');
    return dot ? dot : "";
}

static void strip_extension(char *buf, const char *path, size_t bufsz) {
    strncpy(buf, path, bufsz - 1);
    buf[bufsz - 1] = '\0';
    char *dot = strrchr(buf, '.');
    if (dot) *dot = '\0';
}

static void print_usage(const char *progname) {
    printf("Fador's C99 Compiler Toolchain\n\n");
    printf("Usage: %s [mode] [options] <input-file> [-o <output>]\n\n", progname);
    printf("Modes (auto-detected from file extension if omitted):\n");
    printf("  cc           Compile C source (.c)\n");
    printf("  as           Assemble assembly (.s / .asm) to object file\n");
    printf("  link         Link object files to executable (reserved)\n\n");
    printf("Options:\n");
    printf("  -S           Stop after generating assembly\n");
    printf("  -c, --obj    Stop after generating object file\n");
    printf("  -o <file>    Output file name\n");
    printf("  --target=linux|windows   Target platform (default: host)\n");
    printf("  --masm       Use Intel/MASM syntax (implies --target=windows)\n");
    printf("  -l<name>     Link against lib<name>.a\n");
    printf("  -L<path>     Add library search directory\n");
    printf("  -D<name>[=<value>]  Preprocessor define\n");
    printf("  --nostdlib   Don't auto-link libc\n\n");
    printf("Optimization:\n");
    printf("  -O0          No optimization (default)\n");
    printf("  -O1          Basic optimizations\n");
    printf("  -O2          Standard optimizations\n");
    printf("  -O3          Aggressive optimizations\n");
    printf("  -Os          Optimize for code size\n");
    printf("  -Og          Optimize for debugging experience\n\n");
    printf("Debug:\n");
    printf("  -g           Emit debug symbols (DWARF on Linux, CodeView on Windows)\n\n");
    printf("If only a .c file is given, the full pipeline runs (compile -> assemble -> link).\n");
}

// ---------- Helper: compile a single .c file to .obj/.o ----------
static int compile_c_to_obj(const char *source_filename, const char *obj_filename,
                            TargetPlatform target,
                            int define_count, const char **define_names,
                            const char **define_values) {
    int i;

    /* Log optimization and debug settings */
    if (g_compiler_options.opt_level != OPT_O0) {
        const char *opt_names[] = {"O0", "O1", "O2", "O3", "Os", "Og"};
        printf("  [opt] -%s enabled\n", opt_names[g_compiler_options.opt_level]);
    }
    if (g_compiler_options.debug_info) {
        printf("  [dbg] Debug symbols enabled (-g)\n");
    }

    /* Reset preprocessor for this compilation unit */
    preprocess_reset();
    if (target == TARGET_WINDOWS) {
        preprocess_define("_WIN32", "1");
        preprocess_define("_WIN64", "1");
    } else {
        preprocess_define("__linux__", "1");
        preprocess_define("__unix__", "1");
    }
    for (i = 0; i < define_count; i++)
        preprocess_define(define_names[i], define_values[i]);

    FILE *f = fopen(source_filename, "rb");
    if (!f) {
        printf("Could not open file %s\n", source_filename);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *source = malloc(size + 1);
    fread(source, 1, size, f);
    source[size] = '\0';
    fclose(f);
    char *preprocessed = preprocess(source, source_filename);
    free(source);

    Lexer lexer;
    lexer_init(&lexer, preprocessed);
    current_parser = malloc(sizeof(Parser));
    parser_init(current_parser, &lexer);
    types_set_target(target == TARGET_WINDOWS);
    ASTNode *program = parser_parse(current_parser);

    codegen_set_target(target);
    current_writer = malloc(sizeof(COFFWriter));
    coff_writer_init(current_writer);

    /* Set debug source info when -g is active */
    if (g_compiler_options.debug_info) {
        char cwd[512];
        if (getcwd(cwd, sizeof(cwd)) == NULL) cwd[0] = '\0';
        coff_writer_set_debug_source(current_writer, source_filename, cwd);
    }

    codegen_set_writer(current_writer);
    codegen_init(NULL);
    codegen_generate(program);

    if (target == TARGET_WINDOWS)
        coff_writer_write(current_writer, obj_filename);
    else
        elf_writer_write(current_writer, obj_filename);

    coff_writer_free(current_writer);
    free(current_writer);
    printf("  %s -> %s\n", source_filename, obj_filename);
    return 0;
}

// ---------- CC mode: compile C source ----------
static int do_cc(int input_count, const char **input_files,
                 const char *output_name,
                 StopAfter stop, TargetPlatform target, int use_masm,
                 int lib_count, const char **libraries,
                 int libpath_count, const char **libpaths,
                 int define_count, const char **define_names,
                 const char **define_values) {
    int i;

    /*
     * Binary encoder path: .c -> .obj (no external assembler).
     * Supports multiple input files.
     * Always used for STOP_OBJ and STOP_FULL.
     */
    if (stop == STOP_OBJ || stop == STOP_FULL) {
        char *obj_paths[64];
        int obj_count = 0;

        for (i = 0; i < input_count; i++) {
            char out_base[256];
            strip_extension(out_base, input_files[i], sizeof(out_base));

            char obj_filename[260];
            if (stop == STOP_OBJ && input_count == 1 && output_name) {
                strncpy(obj_filename, output_name, 259);
                obj_filename[259] = '\0';
            } else {
                strncpy(obj_filename, out_base, 255);
                obj_filename[255] = '\0';
                if (target == TARGET_WINDOWS)
                    strcat(obj_filename, ".obj");
                else
                    strcat(obj_filename, ".o");
            }

            if (compile_c_to_obj(input_files[i], obj_filename, target,
                                 define_count, define_names, define_values) != 0)
                return 1;

            obj_paths[obj_count++] = strdup(obj_filename);
        }

        if (stop == STOP_OBJ) {
            for (i = 0; i < obj_count; i++) free(obj_paths[i]);
            return 0;
        }

        /* STOP_FULL: link all .obj files */
        char exe_filename[260];
        if (output_name) {
            strncpy(exe_filename, output_name, 259);
            exe_filename[259] = '\0';
        } else {
            char out_base[256];
            strip_extension(out_base, input_files[0], sizeof(out_base));
            strcpy(exe_filename, out_base);
            if (target == TARGET_WINDOWS) strcat(exe_filename, ".exe");
        }

        int rc = 0;
        if (target == TARGET_LINUX) {
            Linker *lnk = linker_new();
            for (i = 0; i < obj_count; i++)
                linker_add_object_file(lnk, obj_paths[i]);
            for (i = 0; i < libpath_count; i++)
                linker_add_lib_path(lnk, libpaths[i]);
            for (i = 0; i < lib_count; i++)
                linker_add_library(lnk, libraries[i]);
            rc = linker_link(lnk, exe_filename);
            linker_free(lnk);
            if (rc != 0) printf("Error: ELF linking failed.\n");
        } else {
            /* Windows: use custom PE linker (handles libraries too) */
            PELinker *plnk = pe_linker_new();
            for (i = 0; i < obj_count; i++)
                pe_linker_add_object_file(plnk, obj_paths[i]);
            for (i = 0; i < libpath_count; i++)
                pe_linker_add_lib_path(plnk, libpaths[i]);
            for (i = 0; i < lib_count; i++)
                pe_linker_add_library(plnk, libraries[i]);
            if (lib_count > 0)
                pe_linker_set_entry(plnk, "mainCRTStartup");
            rc = pe_linker_link(plnk, exe_filename);
            pe_linker_free(plnk);
            if (rc != 0) printf("Error: PE linking failed.\n");
        }

        for (i = 0; i < obj_count; i++) free(obj_paths[i]);
        if (rc != 0) return 1;
        printf("Linked: %s\n", exe_filename);
        return 0;
    }

    /*
     * Assembly text path (single file only).
     * Also handles MASM assemble + system link.
     */
    if (input_count > 1) {
        printf("Error: Multiple input files only supported with binary object output.\n");
        return 1;
    }

    const char *source_filename = input_files[0];

    /* Reset preprocessor for this file */
    preprocess_reset();
    if (target == TARGET_WINDOWS) {
        preprocess_define("_WIN32", "1");
        preprocess_define("_WIN64", "1");
    } else {
        preprocess_define("__linux__", "1");
        preprocess_define("__unix__", "1");
    }
    for (i = 0; i < define_count; i++)
        preprocess_define(define_names[i], define_values[i]);

    FILE *f = fopen(source_filename, "rb");
    if (!f) {
        printf("Could not open file %s\n", source_filename);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *source = malloc(size + 1);
    fread(source, 1, size, f);
    source[size] = '\0';
    fclose(f);
    char *preprocessed = preprocess(source, source_filename);
    free(source);

    Lexer lexer;
    lexer_init(&lexer, preprocessed);

    current_parser = malloc(sizeof(Parser));
    parser_init(current_parser, &lexer);

    types_set_target(target == TARGET_WINDOWS);
    printf("Parsing...\n"); fflush(stdout);
    ASTNode *program = parser_parse(current_parser);
    printf("Parsing complete.\n");

    // Derive base name
    char out_base[256];
    strip_extension(out_base, source_filename, sizeof(out_base));

    codegen_set_target(target);

    // Assembly text generation
    if (use_masm)
        codegen_set_syntax(SYNTAX_INTEL);
    else
        codegen_set_syntax(SYNTAX_ATT);

    char asm_filename[260];
    if (stop == STOP_ASM && output_name) {
        strncpy(asm_filename, output_name, 259);
    } else {
        strncpy(asm_filename, out_base, 255);
        if (use_masm)
            strcat(asm_filename, ".asm");
        else
            strcat(asm_filename, ".s");
    }

#ifdef _WIN32
    for (char *p = asm_filename; *p; p++) if (*p == '/') *p = '\\';
#endif

    FILE *asm_out = fopen(asm_filename, "w");
    if (!asm_out) {
        printf("Could not create output file %s\n", asm_filename);
        return 1;
    }

    codegen_init(asm_out);
    codegen_generate(program);
    fclose(asm_out);
    printf("Generated: %s\n", asm_filename);

    if (stop == STOP_ASM)
        return 0;

    /* -S was not specified but we ended up here: redirect through binary
     * encoder path (re-invoke do_cc without use_masm so the binary path
     * is taken).  This removes the need for external assemblers. */
    printf("Note: full pipeline now uses built-in binary encoder.\n");
    return do_cc(input_count, input_files, output_name, STOP_FULL, target,
                 0 /* use_masm=0 forces binary path */,
                 lib_count, libraries, libpath_count, libpaths,
                 define_count, define_names, define_values);
}

// ---------- AS mode: assemble .s/.asm to object ----------
static int do_as(const char *input_file, const char *output_name,
                 TargetPlatform target) {
    const char *ext = file_extension(input_file);
    int is_masm = (strcmp(ext, ".asm") == 0 || strcmp(ext, ".ASM") == 0);

    char out_base[256];
    strip_extension(out_base, input_file, sizeof(out_base));

    char obj_filename[260];
    if (output_name) {
        strncpy(obj_filename, output_name, 259);
    } else {
        if (target == TARGET_WINDOWS || is_masm)
            sprintf(obj_filename, "%s.obj", out_base);
        else
            sprintf(obj_filename, "%s.o", out_base);
    }

    char cmd[2048];
    if (is_masm) {
        sprintf(cmd, "ml64 /c /nologo /Fo\"%s\" \"%s\"", obj_filename, input_file);
    } else {
        sprintf(cmd, "as -o \"%s\" \"%s\"", obj_filename, input_file);
    }

    if (run_command(cmd) != 0) {
        printf("Error: Assembly failed.\n");
        return 1;
    }

    printf("Assembled: %s\n", obj_filename);
    return 0;
}

// ---------- LINK mode: link objects to executable ----------
static int do_link(int obj_count, const char **obj_files, const char *output_name,
                   TargetPlatform target,
                   int lib_count, const char **libraries,
                   int libpath_count, const char **libpaths) {
    char cmd[4096];
    char exe_filename[260];

    if (output_name) {
        strncpy(exe_filename, output_name, 259);
    } else {
        if (target == TARGET_WINDOWS)
            strcpy(exe_filename, "a.exe");
        else
            strcpy(exe_filename, "a.out");
    }

    if (target == TARGET_LINUX) {
        /* Use our custom ELF linker */
        Linker *lnk = linker_new();
        int i;
        for (i = 0; i < obj_count; i++)
            linker_add_object_file(lnk, obj_files[i]);
        for (i = 0; i < libpath_count; i++)
            linker_add_lib_path(lnk, libpaths[i]);
        for (i = 0; i < lib_count; i++)
            linker_add_library(lnk, libraries[i]);
        int rc = linker_link(lnk, exe_filename);
        linker_free(lnk);
        return rc;
    }

    /* Windows target: use custom PE linker */
    {
        PELinker *plnk = pe_linker_new();
        int i;
        for (i = 0; i < obj_count; i++)
            pe_linker_add_object_file(plnk, obj_files[i]);
        for (i = 0; i < libpath_count; i++)
            pe_linker_add_lib_path(plnk, libpaths[i]);
        for (i = 0; i < lib_count; i++)
            pe_linker_add_library(plnk, libraries[i]);
        if (lib_count > 0)
            pe_linker_set_entry(plnk, "mainCRTStartup");
        int rc = pe_linker_link(plnk, exe_filename);
        pe_linker_free(plnk);
        return rc;
    }
}

// ---------- main ----------
int main(int argc, char **argv) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    ToolMode mode = MODE_AUTO;
    StopAfter stop = STOP_FULL;
    TargetPlatform target =
#ifdef _WIN32
        TARGET_WINDOWS;
#else
        TARGET_LINUX;
#endif
    int use_masm = 0;
    const char *output_name = NULL;

    // Collect positional (non-option) arguments
    const char *input_files[64];
    int input_count = 0;

    // Libraries and library paths
    const char *libraries[64];
    int lib_count = 0;
    const char *libpaths[64];
    int libpath_count = 0;

    // Preprocessor defines stored for per-file application
    const char *define_names[256];
    const char *define_values[256];
    int define_count = 0;

    for (int i = 1; i < argc; i++) {
        // Mode selectors
        if (strcmp(argv[i], "cc") == 0 && i == 1) {
            mode = MODE_CC; continue;
        }
        if (strcmp(argv[i], "as") == 0 && i == 1) {
            mode = MODE_AS; continue;
        }
        if (strcmp(argv[i], "link") == 0 && i == 1) {
            mode = MODE_LINK; continue;
        }
        // Options
        if (strcmp(argv[i], "-S") == 0) {
            stop = STOP_ASM; continue;
        }
        if (strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--obj") == 0) {
            stop = STOP_OBJ; continue;
        }
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            output_name = argv[++i]; continue;
        }
        if (strcmp(argv[i], "--masm") == 0) {
            use_masm = 1;
            target = TARGET_WINDOWS;
            continue;
        }
        if (strncmp(argv[i], "--target=", 9) == 0) {
            const char *t = argv[i] + 9;
            if (strcmp(t, "linux") == 0)
                target = TARGET_LINUX;
            else if (strcmp(t, "windows") == 0 || strcmp(t, "win64") == 0 || strcmp(t, "win") == 0)
                target = TARGET_WINDOWS;
            else {
                printf("Unknown target: %s\n", t);
                return 1;
            }
            continue;
        }
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        }
        // Optimization flags
        if (strcmp(argv[i], "-O0") == 0) {
            g_compiler_options.opt_level = OPT_O0; continue;
        }
        if (strcmp(argv[i], "-O1") == 0) {
            g_compiler_options.opt_level = OPT_O1; continue;
        }
        if (strcmp(argv[i], "-O2") == 0) {
            g_compiler_options.opt_level = OPT_O2; continue;
        }
        if (strcmp(argv[i], "-O3") == 0) {
            g_compiler_options.opt_level = OPT_O3; continue;
        }
        if (strcmp(argv[i], "-Os") == 0) {
            g_compiler_options.opt_level = OPT_Os; continue;
        }
        if (strcmp(argv[i], "-Og") == 0) {
            g_compiler_options.opt_level = OPT_Og; continue;
        }
        // Debug symbols flag
        if (strcmp(argv[i], "-g") == 0) {
            g_compiler_options.debug_info = 1; continue;
        }
        // -l<name>  library
        if (strncmp(argv[i], "-l", 2) == 0 && argv[i][2] != '\0') {
            if (lib_count < 64)
                libraries[lib_count++] = argv[i] + 2;
            continue;
        }
        // -L<path>  library search path
        if (strncmp(argv[i], "-L", 2) == 0 && argv[i][2] != '\0') {
            if (libpath_count < 64)
                libpaths[libpath_count++] = argv[i] + 2;
            continue;
        }
        // -D<name>[=<value>]  preprocessor define (stored for per-file application)
        if (strncmp(argv[i], "-D", 2) == 0 && argv[i][2] != '\0') {
            if (define_count < 256) {
                const char *def = argv[i] + 2;
                const char *eq = strchr(def, '=');
                if (eq) {
                    size_t nlen = (size_t)(eq - def);
                    char *nm = malloc(nlen + 1);
                    if (nlen > 255) nlen = 255;
                    strncpy(nm, def, nlen);
                    nm[nlen] = '\0';
                    define_names[define_count] = nm;
                    define_values[define_count] = eq + 1;
                } else {
                    define_names[define_count] = def;
                    define_values[define_count] = "1";
                }
                define_count++;
            }
            continue;
        }
        // Positional argument (input file)
        if (input_count < 64)
            input_files[input_count++] = argv[i];
    }

    if (input_count == 0) {
        printf("Error: No input file specified.\n");
        return 1;
    }

    // Auto-detect mode from first input file extension
    if (mode == MODE_AUTO) {
        const char *ext = file_extension(input_files[0]);
        if (strcmp(ext, ".c") == 0 || strcmp(ext, ".C") == 0 || strcmp(ext, ".cc") == 0 || strcmp(ext, ".CC") == 0)
            mode = MODE_CC;
        else if (strcmp(ext, ".s") == 0 || strcmp(ext, ".S") == 0 ||
                 strcmp(ext, ".asm") == 0 || strcmp(ext, ".ASM") == 0)
            mode = MODE_AS;
        else if (strcmp(ext, ".o") == 0 || strcmp(ext, ".obj") == 0 ||
                 strcmp(ext, ".O") == 0 || strcmp(ext, ".OBJ") == 0)
            mode = MODE_LINK;
        else
            mode = MODE_CC;  // default
    }

    // Intel/MASM implies Windows target & Intel syntax
    if (use_masm) {
        target = TARGET_WINDOWS;
    }

    switch (mode) {
    case MODE_CC:
        return do_cc(input_count, input_files, output_name, stop, target, use_masm,
                     lib_count, libraries, libpath_count, libpaths,
                     define_count, define_names, define_values);

    case MODE_AS:
        return do_as(input_files[0], output_name, target);

    case MODE_LINK:
        return do_link(input_count, input_files, output_name, target,
                       lib_count, libraries, libpath_count, libpaths);

    default:
        printf("Error: Could not determine mode.\n");
        return 1;
    }
}
