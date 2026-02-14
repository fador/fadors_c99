# Fador's C99 Compiler

A self-hosting C99-standard compliant compiler written in C99, targeting x86_64 Windows (COFF/PE) and Linux/Unix (ELF). Features a fully custom toolchain on Linux — including assembler, linker, and dynamic linking — with no external tools required. The compiler can compile itself (triple-tested bootstrapping).

## Features

### Preprocessor
- **Includes**: Recursive `#include "file.h"` (relative path resolution) and `#include <file>` (system include search path).
- **Object Macros**: Constant substitution via `#define NAME VALUE`, with `#undef`.
- **Function Macros**: `#define NAME(a,b) BODY` with parameter substitution and nested expansion.
- **Conditional Compilation**: `#ifdef`, `#ifndef`, `#elif`, `#else`, `#endif`.
- **Pragma Support**: `#pragma pack(push, N)`, `#pragma pack(pop)`, `#pragma pack(N)` for struct packing.
- **Multi-line Macros**: Backslash-newline continuation in `#define`.
- **Built-in Macros**: `__FILE__`, `__LINE__`, `_WIN32`, `_WIN64`, `__linux__`, `__unix__`.
- **Comment Stripping**: `//` and `/* */` comment removal.
- **Command-line Defines**: `-DNAME[=VALUE]` flag.

### Language Support
- **Types**: `int`, `short`, `long`, `long long`, `char`, `float`, `double`, `void`, `unsigned` variants, and multilevel pointers.
- **Data Structures**: `struct` (nested, forward-declared, self-referential, packed), `union`, `enum`, `typedef`.
- **Control Flow**: `if`/`else`, `while`, `do`...`while`, `for` (with C99 init declarations), `switch`/`case`/`default`, `break`, `continue`, `goto`/labels, `return`.
- **Expressions**: Full C operator precedence, ternary (`? :`), `sizeof`, type casts, function calls.
- **Operators**: All arithmetic, comparison, logical, bitwise, compound assignment (`+=`, `-=`, `*=`, `/=`, `%=`, `|=`, `&=`, `^=`, `<<=`, `>>=`), increment/decrement (pre/post).
- **Pointers**: Full pointer arithmetic (scaled by type size), pointer difference, address-of (`&`), dereference (`*`), chained pointer/member access.
- **Arrays**: 1D and 2D arrays, subscript operator, initializer lists (local and global).
- **Literals**: Integer (decimal, hex `0x`), float (with exponent, `f`/`F` suffix), integer suffixes (`U`, `L`, `UL`, `ULL`), string literals with full escape sequences (`\n`, `\t`, `\"`, `\\`, `\0`, `\'`), character literals.
- **Storage Classes / Qualifiers**: `static` (global and local), `extern`, `const`, `volatile`, `inline`, `restrict`, `auto`, `register`.
- **Variadic Functions**: `...` in declarations (ABI-compliant calling convention).

### Backends / Code Generation
- **Integrated Pipeline**: Full compilation to executable without external tools on Linux. Windows PE linker also built-in.
- **Built-in x86-64 Encoder**: Direct machine code generation — no external assembler needed. Supports all GPR registers (rax–r15), XMM0–XMM15, REX prefixes, ModR/M, SIB encoding.
- **Custom ELF Linker**: Built-in static linker for Linux that merges `.o` files and static archives (`.a`) into executables. Includes a `_start` stub (no CRT needed) and supports dynamic linking against `libc.so.6` via PLT/GOT generation.
- **Custom PE/COFF Linker**: Links COFF `.obj` files into PE executables with DLL import table generation (`kernel32.dll`), `.rdata`, `.data`, `.bss` sections.
- **Custom COFF Object Writer**: Direct machine code → COFF `.obj` generation on Windows (bypasses MASM).
- **Custom ELF Object Writer**: Direct machine code → ELF `.o` generation on Linux (bypasses GNU `as`).
- **x86_64 AT&T Assembly**: Default text assembly for Linux/Unix (generates `.s`).
- **x86_64 Intel/MASM Assembly**: Supported via `--masm` flag (generates `.asm` for Windows).
- **Dual ABI Support**: System V AMD64 (Linux, 6 int + 8 XMM register args) and Win64 (4 register args, shadow space).
- **Float/Double Codegen**: Full SSE scalar instruction support (addss/sd, subss/sd, mulss/sd, divss/sd, conversions).
- **Peephole Optimization**: Eliminates redundant jumps and dead code after unconditional branches.
- **Stack Management**: Automatic local variable allocation and ABI-compliant register-based argument passing.

### Bundled Standard Library Headers
- `<stdio.h>`: `printf`, `fprintf`, `sprintf`, `snprintf`, `sscanf`, `fopen`/`fclose`/`fread`/`fwrite`/`fseek`/`ftell`, `fflush`, `puts`, `fputs`, `fgetc`, `fgets`, `FILE`/`stdin`/`stdout`/`stderr`.
- `<stdlib.h>`: `malloc`, `calloc`, `realloc`, `free`, `atoi`, `atof`, `strtol`, `strtoul`, `exit`, `abs`, `getenv`, `system`.
- `<string.h>`: `memcpy`, `memset`, `strlen`, `strcmp`, `strncmp`, `strcpy`, `strncpy`, `strchr`, `strrchr`, `strcat`, `strdup`.
- `<stddef.h>`: `size_t`, `ptrdiff_t`, `NULL`.
- `<stdint.h>`: `int8_t` through `uint64_t`.
- `<ctype.h>`: `isalpha`, `isdigit`, `isalnum`, `isspace`, `toupper`, `tolower`.
- `<time.h>`: `time_t`, `time()`.

## Building

Requires CMake and a C99-compliant compiler (Visual Studio MSVC, GCC, or Clang).

```bash
mkdir build
cd build
cmake ..
cmake --build .
```

## Usage

### CLI Reference

```
fadors99 [mode] <input> [options]

Modes (auto-detected from file extension):
  cc          Compile C source (.c)
  as          Assemble assembly (.s/.asm) to object file
  link        Link object files to executable

Options:
  -S                     Stop after generating assembly
  -c, --obj              Stop after generating object file
  -o <file>              Output file name
  --target=<t>           Target platform: linux, windows, win64
  --masm                 Use Intel/MASM syntax (implies Windows target)
  -l<name>               Link against lib<name>.a
  -L<path>               Add library search directory
  -D<name>[=<value>]     Preprocessor define
  -h, --help             Print usage
```

### Compile to Executable
Automatically detects platform tools. On Linux, the entire pipeline (compile → assemble → link) runs without any external tools.

```bash
# Windows (COFF binary path — generates .obj, links with built-in PE linker)
./fadors99 main.c --masm
# Output: main.exe

# Linux (fully self-contained — custom ELF assembler + linker)
./fadors99 main.c
# Output: main (static executable, no CRT required)

# Linux with dynamic linking (libc)
./fadors99 main.c -lc
# Output: main (dynamically linked against libc.so.6)
```

### Compile to Object File
```bash
./fadors99 main.c -c
# Output: main.o (Linux) or main.obj (Windows)
```

### Inspect Assembly
Use `-S` to stop after assembly generation.

```bash
./fadors99 main.c -S
# Output: main.s (AT&T syntax)

./fadors99 main.c -S --masm
# Output: main.asm (Intel syntax)
```

### Assemble and Link Separately
```bash
./fadors99 as main.s -o main.o
./fadors99 link main.o -o main -lc
```

## Project Structure

- `src/`: Compiler core (Lexer, Parser, AST, CodeGen, Preprocessor, Types, Encoder, Linker).
- `include/`: Bundled C standard library headers.
- `stage1/`: Self-compiled compiler assembly (for bootstrapping).
- `tests/`: Comprehensive automated test suite (75+ tests).
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

### Current Focus: Optimizations & Debug Symbols
- [x] Custom ELF Assembler (`.o` writer from machine code)
- [x] Custom COFF Assembler (`.obj` writer from machine code)
- [x] Custom ELF Static Linker (merges `.o` + `.a` → executable)
- [x] ELF Dynamic Linking (PLT/GOT, `.interp`, `.dynamic` section generation)
- [x] Built-in `_start` stub (no CRT needed for simple programs)
- [x] Custom Windows PE/COFF Linker (merge `.obj` → `.exe`, import tables for `kernel32.dll`)
- [x] Direct machine code generation (COFF) completion for verification without external tools.
- [x] Global variable initialization support.
- [x] Relocation handling for external symbols.
- [x] Floating Point Support (`float`, `double`, SSE/AVX).

### Language Features
- [x] **Arrays**: 1D and 2D arrays, subscript operator, initializer lists.
- [x] **Control Flow**: `for` loops (with C99 init declarations), `switch`/`case`/`default`, `do`...`while`, `break`, `continue`, `goto`/labels.
- [x] **Abstractions**: `typedef` for complex declarations.
- [x] **Data Types**: `enum`, `union`, nested/forward-declared/self-referential structs.
- [x] **Pointer Arithmetic**: Scaling based on type size, pointer difference.
- [x] **Operators**: All compound assignments, ternary, `sizeof`, type casts, pre/post increment/decrement.
- [x] **Floating Point**: Full `float`/`double` support with SSE scalar codegen.
- [x] **Storage Classes**: `static` (global and local), `extern`, `const`.
- [x] **Literals**: Hex integers, float with exponent, integer suffixes, full escape sequences.

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

---

## Optimization & Debug Symbols Plan

### Overview

This section outlines the implementation plan for compiler optimization flags (`-O1`, `-O2`, `-O3`) and debug symbol generation (`-g`). These are standard compiler CLI options that control the trade-off between compilation speed, output binary performance, and debuggability.

### Phase 1: Infrastructure — CLI Flags & Compiler State

**Goal**: Parse `-O0`/`-O1`/`-O2`/`-O3` and `-g` flags, propagate settings through the compilation pipeline.

- [ ] **CLI parsing**: Add `-O0`, `-O1`, `-O2`, `-O3`, `-Og`, `-Os`, and `-g` flag handling in `main.c`.
- [ ] **Compiler options struct**: Add `opt_level` (0–3) and `debug_info` (bool) fields to a global/passed-through options struct.
- [ ] **Pass options to codegen**: Thread optimization level and debug flag through `codegen_init()` or equivalent entry points.

### Phase 2: `-g` — Debug Symbol Generation (DWARF / CodeView)

**Goal**: Emit debug information so `gdb`/`lldb` (Linux) or Visual Studio/WinDbg (Windows) can map machine code back to source lines, variables, and types.

#### Phase 2a: ELF / DWARF (Linux)
- [ ] **Line number tracking**: Record source file + line number for each AST node during parsing; propagate to codegen.
- [ ] **`.debug_line` section**: Emit DWARF line number program (opcodes: `DW_LNS_advance_pc`, `DW_LNS_advance_line`, `DW_LNS_copy`, etc.) mapping instruction offsets → source lines.
- [ ] **`.debug_info` section**: Emit compilation unit DIE (`DW_TAG_compile_unit`) with producer, language, file reference.
- [ ] **`.debug_abbrev` section**: Define abbreviation table entries for compile unit, subprogram, variable, base type DIEs.
- [ ] **Subprogram DIEs**: `DW_TAG_subprogram` for each function — name, low_pc/high_pc, return type.
- [ ] **Variable DIEs**: `DW_TAG_variable` / `DW_TAG_formal_parameter` with location expressions (`DW_OP_fbreg + offset`) for locals and parameters.
- [ ] **Type DIEs**: `DW_TAG_base_type` for int/char/float/double, `DW_TAG_pointer_type`, `DW_TAG_structure_type`, `DW_TAG_array_type`, `DW_TAG_typedef`, `DW_TAG_enumeration_type`, `DW_TAG_union_type`.
- [ ] **`.debug_str` section**: Pooled string table for type/variable/function names.
- [ ] **`.debug_aranges` section**: Address range lookup table.
- [ ] **ELF writer integration**: Add `.debug_*` sections to ELF object writer and linker, including the necessary relocations.
- [ ] **DWARF version**: Target DWARF 4 (widely supported by gdb 7.5+, lldb).

#### Phase 2b: COFF / CodeView (Windows)
- [ ] **`.debug$S` section**: Emit CodeView S_GPROC32 / S_LPROC32 (function symbols), S_LOCAL (local variables), S_REGREL32 (stack-relative locations).
- [ ] **`.debug$T` section**: Emit CodeView type records (LF_PROCEDURE, LF_POINTER, LF_STRUCTURE, LF_ARRAY, LF_ENUM, LF_UNION, LF_ARGLIST, LF_MODIFIER).
- [ ] **Line number records**: CodeView line-to-address mapping in `.debug$S` subsection (DEBUG_S_LINES).
- [ ] **File checksum records**: DEBUG_S_FILECHKSMS for source file identification.
- [ ] **PE linker integration**: Merge `.debug$S` and `.debug$T` from multiple `.obj` files; optionally generate `/DEBUG` PDB-compatible output.
- [ ] **CodeView version**: Target CodeView 8 (compatible with Visual Studio 2015+).

#### Phase 2c: Source-Level Debugging Verification
- [ ] **Test `gdb` step-through**: Compile a test program with `-g`, verify `gdb` can `break main`, `step`, `print` variables.
- [ ] **Test `lldb` step-through**: Same with `lldb` on Linux.
- [ ] **Test Visual Studio debugging**: Compile with `-g --masm`, verify variable inspection in VS debugger.
- [ ] **Test `info locals`**: Verify local variables are visible and correct in debuggers.
- [ ] **Test source mapping accuracy**: Line-by-line stepping matches actual source code.

### Phase 3: `-O1` — Basic Optimizations

**Goal**: Low-cost optimizations that improve performance without significantly increasing compile time. These operate primarily on the AST or during codegen emission.

- [ ] **Constant folding**: Evaluate constant expressions at compile time (e.g., `3 + 4` → `7`, `sizeof(int) * 2` → `8`). Extend to constant propagation through known-constant variables.
- [ ] **Dead code elimination (basic)**: Remove code after unconditional `return`, `break`, `continue`, `goto`. Remove unreachable `else` branches when condition is a compile-time constant.
- [ ] **Redundant load/store elimination**: Track register contents; skip reload if value already in register from prior instruction.
- [ ] **Strength reduction (simple)**: Replace `x * 2` with `x << 1`, `x * 4` with `x << 2`, `x / 2` with `x >> 1` (for unsigned), `x % 2` with `x & 1`.
- [ ] **Branch optimization**: Extend existing peephole — remove redundant jumps, convert `jcc` over `jmp` to inverted `jcc`.
- [ ] **Zero-initialization optimization**: Use `xor reg, reg` instead of `mov reg, 0` (already partially done, systematize).
- [ ] **Boolean simplification**: `x == 0` → `test x, x` + `setz`, `x != 0` → `test x, x` + `setnz`.

### Phase 4: `-O2` — Standard Optimizations

**Goal**: Medium-cost optimizations that require analysis passes. This is where most of the performance improvement comes from for typical code. Requires an intermediate representation (IR) or enhanced AST annotations.

#### Phase 4a: IR / CFG Construction
- [ ] **Basic block identification**: Split function bodies into basic blocks at branch targets and after jumps/returns.
- [ ] **Control Flow Graph (CFG)**: Build directed graph of basic blocks with predecessor/successor edges.
- [ ] **SSA construction** (optional, for later): Convert variables to Static Single Assignment form with φ-functions for more powerful analyses.

#### Phase 4b: Analysis Passes
- [ ] **Liveness analysis**: Compute live variable sets at each basic block entry/exit. Identify dead stores.
- [ ] **Reaching definitions**: Track which assignments reach each use of a variable.
- [ ] **Dominator tree**: Compute dominance relationships for loop detection and placement of φ-functions.
- [ ] **Loop detection**: Identify natural loops (back edges in CFG) for loop-focused optimizations.

#### Phase 4c: Optimization Passes
- [ ] **Global constant propagation**: Propagate known-constant values across basic blocks.
- [ ] **Dead store elimination**: Remove writes to variables that are never subsequently read.
- [ ] **Common subexpression elimination (CSE)**: Detect repeated computations (`a + b` computed twice) and reuse the first result.
- [ ] **Loop-invariant code motion (LICM)**: Move computations that don't change within a loop to the preheader.
- [ ] **Register allocation**: Replace naive stack-spill-everything with a graph-coloring or linear-scan register allocator. Use liveness analysis to minimize spills.
- [ ] **Function inlining** (small functions): Inline functions with small body size (threshold: ~20 instructions or compiler heuristic).
- [ ] **Tail call optimization**: Convert `return f(args)` to a jump when stack frame can be reused.
- [ ] **Copy propagation**: Replace `a = b; ... use a` with `... use b` when `b` is unchanged.
- [ ] **Algebraic simplification**: `x + 0` → `x`, `x * 1` → `x`, `x & 0xFF..FF` → `x`, `x | 0` → `x`, double negation removal.

### Phase 5: `-O3` — Aggressive Optimizations

**Goal**: More aggressive, potentially code-size-increasing optimizations for maximum runtime performance.

- [ ] **Aggressive inlining**: Inline larger functions and recursive-descent inline up to a depth limit.
- [ ] **Loop unrolling**: Unroll small loops with known iteration counts (full unroll for N ≤ 8, partial unroll with factor 2–4 for larger loops).
- [ ] **Loop strength reduction**: Replace array indexing in loops (`a[i]` → pointer increment pattern).
- [ ] **Vectorization hints**: Where possible, use wider SSE/AVX instructions for array operations (e.g., `addps` for float arrays).
- [ ] **Instruction scheduling**: Reorder independent instructions to reduce pipeline stalls and improve ILP (instruction-level parallelism).
- [ ] **Interprocedural optimization**: Constant propagation and dead argument elimination across function boundaries (requires whole-program analysis).
- [ ] **Profile-guided optimization (PGO) support** (future): Instrument code for profiling, then use profile data to guide inlining and branch prediction hints.

### Phase 6: `-Os` / `-Og` — Size & Debug Optimizations

- [ ] **`-Os`**: Apply `-O2` optimizations but prefer smaller code size — disable loop unrolling, prefer shorter instruction encodings, avoid aggressive inlining.
- [ ] **`-Og`**: Apply only optimizations that don't interfere with debugging — constant folding, dead code elimination, but no inlining, no register allocation changes that hide variables.

### Implementation Priority & Dependencies

```
Phase 1 (CLI flags)      ─── prerequisite for all others
    │
    ├── Phase 2 (-g)     ─── independent, high value for usability
    │
    └── Phase 3 (-O1)    ─── foundation for O2/O3
            │
            └── Phase 4a (IR/CFG)  ─── required for O2
                    │
                    ├── Phase 4b (Analysis)
                    │       │
                    │       └── Phase 4c (-O2 optimizations)
                    │               │
                    │               └── Phase 5 (-O3)
                    │
                    └── Phase 6 (-Os/-Og)
```

**Suggested order**: Phase 1 → Phase 2a (DWARF) → Phase 3 (-O1) → Phase 4 (-O2) → Phase 2b (CodeView) → Phase 5 (-O3) → Phase 6.
