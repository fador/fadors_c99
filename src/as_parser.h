#ifndef AS_PARSER_H
#define AS_PARSER_H

#include "coff_writer.h"
#include "codegen.h"

/**
 * Assemble an assembly file to an object file using the built-in encoder.
 * 
 * @param input_file Path to the .s assembly file.
 * @param output_file Path to the .o/.obj output file.
 * @param target The target platform (e.g., TARGET_DOS).
 * @return 0 on success, non-zero on failure.
 */
int assemble_file(const char *input_file, const char *output_file, TargetPlatform target);

#endif
