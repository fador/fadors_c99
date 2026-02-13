cmake_minimum_required(VERSION 3.10)
project(FadorsC99 LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON) # Enable extensions for anon structs/unions

# Compiler warnings
if(MSVC)
    add_compile_options(/W4)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# Source files
set(SOURCES
    src/main.c
    src/lexer.c
    src/parser.c
    src/ast.c
    src/codegen.c
    src/arch_x86_64.c
    src/preprocessor.c
    src/types.c
    src/buffer.c
    src/encoder.c
    src/coff_writer.c
    src/elf_writer.c
    src/linker.c
    src/pe_linker.c
)

add_executable(fadors99 ${SOURCES})

# Output directory for binaries
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

if(MSVC)
    set_target_properties(fadors99 PROPERTIES LINK_FLAGS "/STACK:8000000")
endif()
