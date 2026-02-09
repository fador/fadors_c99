#include "codegen.h"
#include "arch_x86_64.h"

// Wrapper for the code generator.
// Currently hardcoded to x86_64, but can be expanded to support other architectures.

void codegen_init(FILE *output) {
    arch_x86_64_init(output);
}

void codegen_set_syntax(CodegenSyntax syntax) {
    arch_x86_64_set_syntax(syntax);
}

void codegen_set_writer(COFFWriter *writer) {
    arch_x86_64_set_writer(writer);
}

void codegen_generate(ASTNode *program) {
    arch_x86_64_generate(program);
}
