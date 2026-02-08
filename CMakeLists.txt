cmake_minimum_required(VERSION 3.10)
project(FadorsC99 LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF) # Ensure strict C99 compliance

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
    src/preprocessor.c
    src/types.c
)

add_executable(fadors99 ${SOURCES})

# Output directory for binaries
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
