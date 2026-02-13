#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include "lexer.h"
#include "parser.h"
#include "ast.h"
#include "codegen.h"
#include "preprocessor.h"
#include "coff_writer.h"
#include "elf_writer.h"

#include <stdlib.h>
#include <string.h>

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
    printf("FadorsC99 Compiler Toolchain\n\n");
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
    printf("  --masm       Use Intel/MASM syntax (implies --target=windows)\n\n");
    printf("If only a .c file is given, the full pipeline runs (compile -> assemble -> link).\n");
}

// ---------- CC mode: compile C source ----------
static int do_cc(const char *source_filename, const char *output_name,
                 StopAfter stop, TargetPlatform target, int use_masm) {
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

    // Debug: dump preprocessed source
    FILE *pf = fopen("preprocessed.i", "w");
    if (pf) { fputs(preprocessed, pf); fclose(pf); }

    Lexer lexer;
    lexer_init(&lexer, preprocessed);

    current_parser = malloc(sizeof(Parser));
    parser_init(current_parser, &lexer);

    printf("Parsing...\n"); fflush(stdout);
    ASTNode *program = parser_parse(current_parser);
    printf("Parsing complete.\n");

    // Derive base name
    char out_base[256];
    strip_extension(out_base, source_filename, sizeof(out_base));

    codegen_set_target(target);

    if (stop == STOP_OBJ) {
        // Direct object generation (binary encoder)
        char obj_filename[260];
        if (output_name) {
            strncpy(obj_filename, output_name, 259);
        } else {
            if (target == TARGET_WINDOWS)
                sprintf(obj_filename, "%s.obj", out_base);
            else
                sprintf(obj_filename, "%s.o", out_base);
        }

        printf("Generating OBJ to %s...\n", obj_filename); fflush(stdout);
        current_writer = malloc(sizeof(COFFWriter));
        coff_writer_init(current_writer);
        codegen_set_writer(current_writer);
        codegen_init(NULL);
        codegen_generate(program);
        printf("Writing OBJ...\n"); fflush(stdout);
        if (target == TARGET_WINDOWS)
            coff_writer_write(current_writer, obj_filename);
        else
            elf_writer_write(current_writer, obj_filename);
        coff_writer_free(current_writer);
        free(current_writer);
        printf("Generated Object: %s\n", obj_filename);
        return 0;
    }

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

    // Full pipeline: assemble + link
    char obj_filename[260];
    char exe_filename[260];
    char cmd[2048];

    if (output_name) {
        strncpy(exe_filename, output_name, 259);
    } else {
        strcpy(exe_filename, out_base);
        if (target == TARGET_WINDOWS) strcat(exe_filename, ".exe");
    }

    if (use_masm) {
        strncpy(obj_filename, out_base, 255);
        strcat(obj_filename, ".obj");
        sprintf(cmd, "ml64 /c /nologo /Fo\"%s\" \"%s\"", obj_filename, asm_filename);
        if (run_command(cmd) != 0) { printf("Error: Assembly failed.\n"); return 1; }

        const char *linker = getenv("FADORS_LINKER");
        if (!linker) linker = "link";
        const char *lfmt = strchr(linker, ' ') ? "\"%s\"" : "%s";
        char lcmd[1024]; sprintf(lcmd, lfmt, linker);
        sprintf(cmd, "%s /nologo /STACK:8000000 /entry:main /subsystem:console /out:\"%s\" \"%s\" kernel32.lib",
                lcmd, exe_filename, obj_filename);
        if (run_command(cmd) != 0) { printf("Error: Linking failed.\n"); return 1; }
    } else {
        strncpy(obj_filename, out_base, 255);
        strcat(obj_filename, ".o");
        sprintf(cmd, "as -o \"%s\" \"%s\"", obj_filename, asm_filename);
        if (run_command(cmd) != 0) { printf("Error: Assembly failed (as).\n"); return 1; }
        sprintf(cmd, "gcc -no-pie -o \"%s\" \"%s\"", exe_filename, obj_filename);
        if (run_command(cmd) != 0) { printf("Error: Linking failed (gcc).\n"); return 1; }
    }

    printf("Compiled to: %s\n", exe_filename);
    return 0;
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

// ---------- LINK mode: link objects to executable (stub) ----------
static int do_link(int obj_count, char **obj_files, const char *output_name,
                   TargetPlatform target) {
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

    if (target == TARGET_WINDOWS) {
        const char *linker = getenv("FADORS_LINKER");
        if (!linker) linker = "link";
        const char *lfmt = strchr(linker, ' ') ? "\"%s\"" : "%s";
        char lcmd[1024]; sprintf(lcmd, lfmt, linker);

        int pos = sprintf(cmd, "%s /nologo /STACK:8000000 /subsystem:console /out:\"%s\"",
                          lcmd, exe_filename);
        for (int i = 0; i < obj_count; i++)
            pos += sprintf(cmd + pos, " \"%s\"", obj_files[i]);
        pos += sprintf(cmd + pos, " kernel32.lib libcmt.lib legacy_stdio_definitions.lib");
    } else {
        int pos = sprintf(cmd, "gcc -no-pie -o \"%s\"", exe_filename);
        for (int i = 0; i < obj_count; i++)
            pos += sprintf(cmd + pos, " \"%s\"", obj_files[i]);
    }

    if (run_command(cmd) != 0) {
        printf("Error: Linking failed.\n");
        return 1;
    }

    printf("Linked to: %s\n", exe_filename);
    return 0;
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
        if (strcmp(ext, ".c") == 0 || strcmp(ext, ".C") == 0)
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
        return do_cc(input_files[0], output_name, stop, target, use_masm);

    case MODE_AS:
        return do_as(input_files[0], output_name, target);

    case MODE_LINK:
        return do_link(input_count, (char **)input_files, output_name, target);

    default:
        printf("Error: Could not determine mode.\n");
        return 1;
    }
}
