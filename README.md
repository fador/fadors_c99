# Fador's C99 Compiler

A lightweight C99-standard compliant compiler written in C99, targeting x86_64 Linux/Unix assembly (AT&T syntax).

## Features

### Preprocessor
- **Includes**: Recursive `#include "file.h"` support.
- **Macros**: Constant substitution via `#define NAME VALUE`.
- **Conditional Compilation**: Basic header guard support with `#ifndef`.

### Language Support
- **Types**: `int`, `void`, and basic `char` support.
- **Pointers**: Full support for pointer depth, address-of (`&`), and dereference (`*`) operators.
- **Structs**: Definition and member access using both `.` and `->` operators.
- **Control Flow**: 
  - `if` / `else` statements.
  - `while` loops.
  - Function definitions and calls (up to 6 arguments via registers).
- **Expressions**: 
  - Full operator precedence (Arithmetic, Relational, Equality).
  - Parentheses for grouping.
  - L-value assignments (e.g., `*ptr = 10;`, `var.member = 5;`).

### Assembly Generation
- **Target**: x86_64 (AT&T syntax).
- **Stack Management**: Automatic local variable allocation and offset tracking.
- **ABI Compliance**: Simple function prologue/epilogue and register-based argument passing.

## Building

Requires CMake and a C99-compliant compiler (GCC, Clang, or MSVC).

```bash
mkdir build
cd build
cmake ..
cmake --build .
```

## Usage

Generate assembly from a C source file:

```bash
./fadors99 example.c
```

This will produce `example.s`, which can be assembled and linked using `gcc`:

```bash
gcc example.s -o example
./example
```

## Project Structure

- `src/`: Compiler source code (Lexer, Parser, AST, CodeGen, Preprocessor, Types).
- `tests/`: Comprehensive test suite for language features.
- `CMakeLists.txt`: Build configuration.

## Development Goals
- [x] Preprocessor
- [x] Structs and Pointers
- [ ] Arrays and pointer arithmetic
- [ ] Standard Library headers
- [ ] Self-hosting (Compiling its own source)
