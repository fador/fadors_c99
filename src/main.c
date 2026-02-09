#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include "lexer.h"
#include "parser.h"
#include "ast.h"
#include "codegen.h"
#include "preprocessor.h"
#include "coff_writer.h"

#include <stdlib.h>
#include <string.h>

// Helper to execute a command
static int run_command(const char *cmd) {
    printf("[CMD] %s\n", cmd);
    return system(cmd);
}

static void compile_and_link(const char *asm_file, const char *exe_file, int use_masm) {
    char cmd[1024];
    char obj_file[260];
    
    // Replace .asm/.s with .obj
    strncpy(obj_file, asm_file, 255);
    char *dot = strrchr(obj_file, '.');
    if (dot) *dot = '\0';
    strcat(obj_file, ".obj");

    const char *linker = getenv("FADORS_LINKER");
    if (!linker) linker = "link";

    if (use_masm) {
        // Assemble: ml64 /c /nologo /Fo<obj> <asm>
        sprintf(cmd, "ml64 /c /nologo /Fo\"%s\" \"%s\"", obj_file, asm_file);
        if (run_command(cmd) != 0) {
            printf("Error: Assembly failed.\n");
            exit(1);
        }

        // Link: <linker> /nologo /entry:main /subsystem:console /out:<exe> <obj>
        const char *linker_fmt = (strchr(linker, ' ')) ? "\"%s\"" : "%s";
        char linker_cmd[1024];
        sprintf(linker_cmd, linker_fmt, linker);

        sprintf(cmd, "%s /nologo /entry:main /subsystem:console /out:\"%s\" \"%s\" kernel32.lib", linker_cmd, exe_file, obj_file);
        if (run_command(cmd) != 0) {
            printf("Error: Linking failed.\n");
            exit(1);
        }
    } else {
        // GAS/GCC path
        sprintf(cmd, "as -o \"%s\" \"%s\"", obj_file, asm_file);
        if (run_command(cmd) != 0) {
             printf("Error: Assembly failed (as).\n");
             exit(1);
        }
        
        sprintf(cmd, "gcc -o \"%s\" \"%s\"", exe_file, obj_file);
        if (run_command(cmd) != 0) {
            printf("Error: Linking failed (gcc).\n");
            exit(1);
        }
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: %s <source.c> [--masm] [--obj] [-S]\n", argv[0]);
        return 1;
    }
    
    const char *source_filename = NULL;
    int use_masm = 0;
    int stop_at_asm = 0;
    int use_obj = 0;
    
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--masm") == 0) {
            use_masm = 1;
        } else if (strcmp(argv[i], "-S") == 0) {
            stop_at_asm = 1;
        } else if (strcmp(argv[i], "--obj") == 0) {
            use_obj = 1;
        } else {
            source_filename = argv[i];
        }
    }
    
    if (!source_filename) {
        printf("Error: No source file specified.\n");
        return 1;
    }
    
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
    
    Parser parser;
    parser_init(&parser, &lexer);
    
    ASTNode *program = parser_parse(&parser);
    
    
    char out_base[256];
    strncpy(out_base, source_filename, 250);
    char *dot = strrchr(out_base, '.');
    if (dot) *dot = '\0';
    
    if (use_obj) {
        char obj_filename[260];
        sprintf(obj_filename, "%s.obj", out_base);
        // Normalize paths for Windows
        for (char *p = obj_filename; *p; p++) if (*p == '/') *p = '\\';

        COFFWriter writer;
        coff_writer_init(&writer);
        codegen_set_writer(&writer);
        codegen_init(NULL);
        codegen_generate(program);
        coff_writer_write(&writer, obj_filename);
        coff_writer_free(&writer);
        printf("Generated Object: %s\n", obj_filename);

        if (!stop_at_asm) {
            char exe_filename[260];
            sprintf(exe_filename, "%s.exe", out_base);
            for (char *p = exe_filename; *p; p++) if (*p == '/') *p = '\\';
            
            char cmd[1024];
            const char *linker = getenv("FADORS_LINKER");
            if (!linker) linker = "link";
            
            // Quote the linker if it contains spaces
            const char *linker_fmt = (strchr(linker, ' ')) ? "\"%s\"" : "%s";
            char linker_cmd[1024];
            sprintf(linker_cmd, linker_fmt, linker);

            sprintf(cmd, "%s /nologo /entry:main /subsystem:console /out:\"%s\" \"%s\" kernel32.lib", linker_cmd, exe_filename, obj_filename);
            
            if (run_command(cmd) != 0) {
                printf("Error: Linking failed.\n");
                return 1;
            }
            printf("Linked to: %s\n", exe_filename);
        }
    } else {
        char asm_filename[260];
        strncpy(asm_filename, out_base, 255);
        
        if (use_masm) {
            strcat(asm_filename, ".asm");
            codegen_set_syntax(SYNTAX_INTEL);
        } else {
            strcat(asm_filename, ".s");
        }
        
        // Normalize paths for Windows
        for (char *p = asm_filename; *p; p++) if (*p == '/') *p = '\\';
        
        FILE *asm_out = fopen(asm_filename, "w");
        if (!asm_out) {
            printf("Could not create output file %s\n", asm_filename);
            return 1;
        }
        
        codegen_init(asm_out);
        codegen_generate(program);
        fclose(asm_out);
        
        printf("Generated: %s\n", asm_filename);
        
        if (!stop_at_asm) {
            char exe_filename[260];
            strcpy(exe_filename, out_base);
            strcat(exe_filename, ".exe");
            compile_and_link(asm_filename, exe_filename, use_masm);
            printf("Compiled to: %s\n", exe_filename);
        }
    }
    
    return 0;
}
