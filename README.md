# Fador's C99 Compiler

A lightweight C99-standard compliant compiler written in C99, targeting x86_64 Windows (COFF/PE) and Linux/Unix (ELF). Features a fully custom toolchain on Linux — including assembler, linker, and dynamic linking — with no external tools required.

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
- **Integrated Pipeline**: Full compilation to executable without external tools on Linux. Windows uses system linker (`link.exe`) for final linking.
- **Custom ELF Linker**: Built-in static linker for Linux that merges `.o` files and static archives (`.a`) into executables. Includes a `_start` stub (no CRT needed) and supports dynamic linking against `libc.so.6` via PLT/GOT generation.
- **Custom COFF Object Writer**: Direct machine code → COFF `.obj` generation on Windows (bypasses MASM).
- **Custom ELF Object Writer**: Direct machine code → ELF `.o` generation on Linux (bypasses GNU `as`).
- **x86_64 AT&T**: Default text assembly for Linux/Unix (generates `.s`).
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
Automatically detects platform tools. On Linux, the entire pipeline (compile → assemble → link) runs without any external tools.

```bash
# Windows (COFF binary path — generates .obj, links with system link.exe)
./fadors99 main.c --masm
# Output: main.exe

# Linux (fully self-contained — custom ELF assembler + linker)
./fadors99 main.c
# Output: main (static executable, no CRT required)

# Linux with dynamic linking (libc)
./fadors99 main.c -lc
# Output: main (dynamically linked against libc.so.6)
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

### Current Focus: Custom Windows PE Linker
- [x] Custom ELF Assembler (`.o` writer from machine code)
- [x] Custom COFF Assembler (`.obj` writer from machine code)
- [x] Custom ELF Static Linker (merges `.o` + `.a` → executable)
- [x] ELF Dynamic Linking (PLT/GOT, `.interp`, `.dynamic` section generation)
- [x] Built-in `_start` stub (no CRT needed for simple programs)
- [ ] Custom Windows PE/COFF Linker (merge `.obj` → `.exe`, import tables for `kernel32.dll`)
- [ ] PE Import Table generation (dynamic linking against Windows DLLs)
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

The goal is to compile the compiler using itself. Based on an audit of `src/`, these are the remaining blockers, ordered by priority:

#### Phase 1: Type System Foundation ✅
- [x] `const` qualifier: Parse and ignore. Used in every function signature.
- [x] `size_t` / `uint8_t` / `uint16_t` / `uint32_t` / `uint64_t` / `int16_t` / `int32_t`: Typedef aliases via `<stdint.h>` and `<stddef.h>`.
- [x] `static` functions: Internal linkage (used in 9/11 `.c` files).
- [x] `unsigned` type modifier.

#### Phase 2: Minimal Standard Library Headers ✅
- [x] `<stddef.h>`: `size_t`, `NULL`, `ptrdiff_t`.
- [x] `<stdint.h>`: Fixed-width integer typedefs.
- [x] `<stdlib.h>`: `malloc`, `free`, `realloc`, `atoi`, `exit`.
- [x] `<string.h>`: `memcpy`, `memset`, `strlen`, `strcmp`, `strncmp`, `strcpy`, `strncpy`, `strchr`, `strcat`.
- [x] `<stdio.h>`: `FILE`, `printf`, `sprintf`, `fopen`, `fclose`, `fread`, `fwrite`, `fseek`, `ftell`.
- [x] `<ctype.h>`: `isalpha`, `isdigit`, `isalnum`, `isspace`.
- [x] `#include <...>` angle-bracket support with `include/` search path.

#### Phase 3: Language Feature Completion ✅
- [x] Compound assignment operators: `+=`, `-=`, `*=`, `/=`, `%=`, `|=`, `&=`, `^=`, `<<=`, `>>=`.
- [x] Ternary operator: `a ? b : c`.
- [x] Variable declarations in `for` init: `for (int i = 0; ...)`.
- [x] Initializer lists: `{1, 2, 3}` for arrays and structs.

#### Phase 4: Critical Self-Hosting Blockers
Discovered by attempting to compile `types.c`, `buffer.c`, `lexer.c` with the compiler itself:
- [x] **`#include "file.h"` relative path resolution**: Resolves relative to the source file's directory, with CWD fallback.
- [x] **Forward struct declarations**: `struct Type;` (used in `types.h`).
- [x] **Named unions in structs**: `union { ... } data;` (used in `types.h`, `ast.h`).
- [x] **`static` local variables**: `static int x = 1;` (used in `preprocessor.c`).
- [x] **`long` type**: `long size = ftell(f);` (treated as `int`, both 8 bytes).
- [x] **Hex integer literals**: `0x8664`, `0x00000020` (lexer + `strtol`-based parsing).
- [x] **Character escape sequences in code**: `'\n'`, `'\0'`, `'\\'` (full escape table).
- [x] **`#pragma pack(push/pop)`**: Required by `coff.h` for struct packing.
- [x] **Multi-line `#define` with `\` continuation**: Backslash-newline joining in preprocessor.

#### Phase 5: Self-Hosting Verification
- [x] Module-by-module: Compile `buffer.c` → `types.c` → `lexer.c` → etc.
- [x] Full self-compilation: Compile entire compiler with itself.
- [x] Triple test: Self-compiled compiler compiles itself, output matches.
