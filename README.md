# Fador's C99 Compiler

A lightweight C99-standard compliant compiler written in C99, targeting x86_64 Windows (MASM/COFF) and Linux/Unix (AT&T/ELF).

## Features

### Preprocessor
- **Includes**: Recursive `#include "file.h"` support.
- **Macros**: Constant substitution via `#define NAME VALUE`.
- **Conditional Compilation**: Basic header guard support with `#ifndef`.

- **Language Support**:
  - **Types**: `int`, `void`, `char`, `float`, `double`, and pointers.
  - **Data Structures**: Struct definition and member access (`.`, `->`).
  - **Control Flow**: `if`/`else`, `while`, function calls.
  - **Expressions**: Full C operator precedence, assignments, and logic (including short-circuiting `&&` and `||`).
  - **Binary Operators**: Full support for `%`, `&`, `|`, `^`, `<<`, `>>`, and logical operators.
  - **Pointers**: Full support for pointer depth, address-of (`&`), and dereference (`*`) operators.

### Backends / Assembly Generation
- **Integrated Pipeline**: Automatically invokes system assemblers (`as`, `ml/ml64`) and linkers (`gcc`, `link.exe`) to produce executables.
- **x86_64 AT&T**: Default for Linux/Unix (generates `.s`).
- **x86_64 Intel/MASM**: Supported via `--masm` flag (generates `.asm` for Windows).
- **Stack Management**: Automatic local variable allocation and ABI-compliant register-based argument passing.

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

- `src/`: Compiler core (Lexer, Parser, AST, CodeGen, Preprocessor, Types).
- `tests/`: Comprehensive automated test suite.
- `CMakeLists.txt`: Build configuration.

## Testing Procedure

### Automated Test Suite
The main test suite is a Python script that compiles, assembles, links, and executes all tests in the `tests/` directory, verifying their return codes.

```bash
# General assembly-based verification
python tests/test_asm_execution.py
```

### Direct COFF Generation Testing
When working on the direct COFF backend (bypassing external assemblers), use the following specialized scripts:

#### Full COFF Suite
Runs all tests using only the internal COFF generator and the system linker.
```bash
# Windows
./run_coff_tests.bat
```

#### Single Test Verification
Compiles a single file with `--obj`, links it, and runs it to capture the return code.
```bash
./run_single_test.bat tests/01_return.c
```

#### Object File Inspection
Assembles a file with `fadors99`, links it manually, and executes it. Useful for standard MASM verification.
```bash
./verify_obj.bat tests/01_return.c
```

## Development Roadmap

### Current Focus: Direct Code Generation
- [x] Custom Assembler (COFF/ELF writer foundation)
- [x] Direct machine code generation (COFF) completion for verification without external tools.
- [x] Global variable initialization support.
- [x] Relocation handling for external symbols.
- [x] Floating Point Support (`float`, `double`, SSE/AVX).

### Language Features
- [x] **Arrays**: Parser support for `T name[size]` and `expr[index]`. <!-- id: 25 -->
- [x] **Control Flow**: Implement `for` loops and `switch` statements.
- [x] **Abstractions**: Implement `typedef` to simplify complex declarations.
- [x] **Data Types**: Add `enum` and `union` support.
- [x] **Pointer Arithmetic**: Scaling based on type size.

### Self-Hosting Path
- [ ] Environment Macros: Emit platform macros (e.g., `_WIN32`, `_LINUX_`) for build environment detection.
- [ ] Standard Library: Minimal `libc` implementation.
- [ ] Self-Hosting: Compile the compiler using itself.
