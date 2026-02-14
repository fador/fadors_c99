#ifndef CODEGEN_H
#define CODEGEN_H

#include "ast.h"
#include <stdio.h>

#include "coff_writer.h"

typedef enum {
    SYNTAX_ATT,
    SYNTAX_INTEL
} CodegenSyntax;

typedef enum {
    TARGET_LINUX,
    TARGET_WINDOWS
} TargetPlatform;

// Optimization level
typedef enum {
    OPT_O0 = 0,   // No optimization (default)
    OPT_O1 = 1,   // Basic optimizations
    OPT_O2 = 2,   // Standard optimizations
    OPT_O3 = 3,   // Aggressive optimizations
    OPT_Os = 4,   // Optimize for size
    OPT_Og = 5    // Optimize for debugging
} OptLevel;

// Compiler options passed through the pipeline
typedef struct {
    OptLevel opt_level;     // Optimization level (default: OPT_O0)
    int debug_info;         // 1 if -g was specified (emit debug symbols)
} CompilerOptions;

// Global compiler options (set once from CLI, read by all pipeline stages)
extern CompilerOptions g_compiler_options;

void codegen_init(FILE *output);
void codegen_set_syntax(CodegenSyntax syntax);
void codegen_set_target(TargetPlatform target);
void codegen_set_writer(COFFWriter *writer);
void codegen_generate(ASTNode *program);

#endif // CODEGEN_H
