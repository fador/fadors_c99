# Fador's C99 Compiler

A lightweight C99-standard compliant compiler written in C99, targeting x86_64 Windows (MASM/COFF) and Linux/Unix (AT&T/ELF).

## Features

### Preprocessor
- **Includes**: Recursive `#include "file.h"` support.
- **Macros**: Constant substitution via `#define NAME VALUE`.
- **Conditional Compilation**: Basic header guard support with `#ifndef`.

### Language Support
- **Types**: `int`, `void`, `char`, and pointers.
- **Data Structures**: Struct definition and member access (`.`, `->`).
- **Control Flow**: `if`/`else`, `while`, function calls.
- **Expressions**: Full operator precedence, assignments, and logic.

### Backends
- **x86_64 AT&T**: Default for Linux/Unix (generates `.s`).
- **x86_64 Intel/MASM**: Supported via `--masm` flag (generates `.asm` for Windows).

### Compilation Driver
- **Integrated Pipeline**: Automatically invokes system assemblers (`as`, `ml/ml64`) and linkers (`gcc`, `link.exe`) to produce executables.
- **Customizable**: Flags to stop at assembly (`-S`) or define output filenames (`-o`).

## Building

Requires CMake and a C99-compliant compiler (Visual Studio MSVC, GCC, or Clang).

```bash
mkdir build
cd build
cmake ..
cmake --build .
```

## Usage

### Compile to Executable
Automatically detects platform tools (e.g., `ml64` on Windows).

```bash
# Windows (MASM)
./fadors99 main.c --masm
# Output: main.exe

# Linux (AT&T)
./fadors99 main.c
# Output: a.out
```

### Inspect Assembly
Use `-S` to stop after assembly generation.

```bash
./fadors99 main.c -S --masm
# Output: main.asm
```

## Code Structure

- `src/`: Compiler core (Lexer, Parser, Codegen, Driver).
- `tests/`: Automated test suite (`test_asm_execution.py` runs compiled binaries).

## Development Roadmap
- [x] Preprocessor & Macros
- [x] Structs & Memory Operations
- [x] Multi-Syntax Support (AT&T / MASM)
- [x] Compiler Driver (Invoke `ml`/`link` automatically)
- [ ] **Custom Assembler**: Generate machine code (COFF/ELF) directly to verify self-hosting without external tools.
- [ ] Standard Library: minimal `libc` implementation.
- [ ] Self-Hosting: Compile the compiler using itself.

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
