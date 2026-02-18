#include "codegen.h"
#include "arch_x86_64.h"
#include "arch_x86.h"

// Global compiler options definition
CompilerOptions g_compiler_options = {.opt_level = OPT_O0, .debug_info = 0};

// Wrapper for the code generator.
// Dispatches to appropriate architecture backend.

void codegen_init(FILE *output) {
    if (g_target == TARGET_DOS) {
        arch_x86_init(output);
    } else {
        arch_x86_64_init(output);
    }
}

void codegen_set_syntax(CodegenSyntax syntax) {
    if (g_target == TARGET_DOS) {
        arch_x86_set_syntax(syntax);
    } else {
        arch_x86_64_set_syntax(syntax); // x86_64 also has set_syntax?
    }
}

void codegen_set_target(TargetPlatform target) {
    // This function sets the target for the codegen module itself
    // But since we use g_target directly, maybe we don't need to pass it down?
    // However, arch backends might need to know too.
    if (target == TARGET_DOS) {
        arch_x86_set_target(target);
    } else {
        arch_x86_64_set_target(target);
    }
}

void codegen_set_writer(COFFWriter *writer) {
    if (g_target == TARGET_DOS) {
        arch_x86_set_writer(writer);
    } else {
        arch_x86_64_set_writer(writer);
    }
}

void codegen_generate(ASTNode *program) {
    if (g_target == TARGET_DOS) {
        arch_x86_generate(program);
    } else {
        arch_x86_64_generate(program);
    }
}
