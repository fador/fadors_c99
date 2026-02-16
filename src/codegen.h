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

/* Map -Os/-Og to their effective numeric optimization tier.
 * -Os behaves like -O2 (with size-preferring overrides applied elsewhere).
 * -Og behaves like -O1 (with debug-preserving overrides applied elsewhere).
 * Use this for all >= comparisons instead of raw enum values. */
static inline int opt_effective_level(OptLevel o) {
    if (o == OPT_Os) return OPT_O2;
    if (o == OPT_Og) return OPT_O1;
    return (int)o;
}

/* Convenience: does the current optimization level enable at least tier N? */
#define OPT_AT_LEAST(n) (opt_effective_level(g_compiler_options.opt_level) >= (n))

/* Convenience: is -Os (optimize-for-size) mode active? */
#define OPT_SIZE_MODE (g_compiler_options.opt_level == OPT_Os)

/* Convenience: is -Og (optimize-for-debug) mode active? */
#define OPT_DEBUG_MODE (g_compiler_options.opt_level == OPT_Og)

// Compiler options passed through the pipeline
typedef struct {
    OptLevel opt_level;     // Optimization level (default: OPT_O0)
    int debug_info;         // 1 if -g was specified (emit debug symbols)
    int avx_level;          // 0 = SSE only, 1 = -mavx (AVX), 2 = -mavx2 (AVX2)
    int pgo_generate;       // 1 if -fprofile-generate was specified
    char pgo_use_file[256]; // path from -fprofile-use=FILE (empty = none)
} CompilerOptions;

// Global compiler options (set once from CLI, read by all pipeline stages)
extern CompilerOptions g_compiler_options;

void codegen_init(FILE *output);
void codegen_set_syntax(CodegenSyntax syntax);
void codegen_set_target(TargetPlatform target);
void codegen_set_writer(COFFWriter *writer);
void codegen_generate(ASTNode *program);

#endif // CODEGEN_H
