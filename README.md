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
- **Storage Classes / Qualifiers**: `static` (global and local), `extern`, `const`, `volatile`, `inline`, `restrict`, `auto`, `register`. GCC/MSVC extensions: `__inline`, `__inline__`, `__forceinline`, `__attribute__((always_inline))`, `__attribute__((noinline))`, `__declspec(noinline)`.
- **Variadic Functions**: `...` in declarations (ABI-compliant calling convention).

### Backends / Code Generation
- **Integrated Pipeline**: Full compilation to executable without external tools on Linux. Windows PE linker also built-in.
- **Built-in x86-64 Encoder**: Direct machine code generation — no external assembler needed. Supports all GPR registers (rax–r15), XMM0–XMM15, YMM0–YMM15, REX prefixes, VEX prefixes (2-byte and 3-byte), ModR/M, SIB encoding. Packed SSE/SSE2 instructions (movups, addps, mulps, movdqu, paddd, etc.) for 128-bit vectorized loops. AVX/AVX2 VEX-encoded instructions (vmovups, vaddps, vmovdqu, vpaddd, etc.) for 256-bit vectorized loops with `-mavx`/`-mavx2` flags.
- **Custom ELF Linker**: Built-in static linker for Linux that merges `.o` files and static archives (`.a`) into executables. Includes a `_start` stub (no CRT needed) and supports dynamic linking against `libc.so.6` via PLT/GOT generation. Generates DWARF 4 debug sections (`.debug_info`, `.debug_abbrev`, `.debug_line`, `.debug_str`, `.debug_aranges`) when `-g` is used.
- **Custom PE/COFF Linker**: Links COFF `.obj` files into PE executables with DLL import table generation (`kernel32.dll`), `.rdata`, `.data`, `.bss` sections.
- **Custom COFF Object Writer**: Direct machine code → COFF `.obj` generation on Windows (bypasses MASM).
- **Custom ELF Object Writer**: Direct machine code → ELF `.o` generation on Linux (bypasses GNU `as`).
- **x86_64 AT&T Assembly**: Default text assembly for Linux/Unix (generates `.s`).
- **x86_64 Intel/MASM Assembly**: Supported via `--masm` flag (generates `.asm` for Windows).
- **Dual ABI Support**: System V AMD64 (Linux, 6 int + 8 XMM register args) and Win64 (4 register args, shadow space).
- **Float/Double Codegen**: Full SSE scalar instruction support (addss/sd, subss/sd, mulss/sd, divss/sd, conversions).
- **Peephole Optimization**: Multi-state peephole optimizer with: push/pop→mov elimination, jcc-over-jmp inversion, setcc+movzbq+test+jcc→direct jcc fusion, add $0/imul $1/imul $0 elimination, self-move elimination, dead code after unconditional branches, and instruction scheduling (push/pop→register for binary expression operands).
- **Register Allocator**: AST pre-scan assigns up to 5 callee-saved registers (`%rbx`, `%r12`–`%r15`) to the most-used scalar integer locals and parameters. Eliminates stack loads/stores in hot loops. Activated at `-O2+`.
- **Stack Management**: Automatic local variable allocation and ABI-compliant register-based argument passing.

### Bundled Standard Library Headers
- `<stdio.h>`: `printf`, `fprintf`, `sprintf`, `snprintf`, `sscanf`, `fopen`/`fclose`/`fread`/`fwrite`/`fseek`/`ftell`, `fflush`, `puts`, `fputs`, `fgetc`, `fgets`, `FILE`/`stdin`/`stdout`/`stderr`.
- `<stdlib.h>`: `malloc`, `calloc`, `realloc`, `free`, `atoi`, `atof`, `strtol`, `strtoul`, `exit`, `abs`, `getenv`, `system`.
- `<string.h>`: `memcpy`, `memset`, `strlen`, `strcmp`, `strncmp`, `strcpy`, `strncpy`, `strchr`, `strrchr`, `strcat`, `strdup`.
- `<stddef.h>`: `size_t`, `ptrdiff_t`, `NULL`.
- `<stdint.h>`: `int8_t` through `uint64_t`.
- `<ctype.h>`: `isalpha`, `isdigit`, `isalnum`, `isspace`, `toupper`, `tolower`.
- `<time.h>`: `time_t`, `time()`.
- `<assert.h>`: `assert()` macro. When `NDEBUG` is defined, expands to no-op. Otherwise emits a runtime check (`ud2` trap on failure) and provides value range hints to the optimizer.

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
  --nostdlib             Don't auto-link libc
  -h, --help             Print usage

Optimization:
  -O0                    No optimization (default)
  -O1                    Basic optimizations
  -O2                    Standard optimizations
  -O3                    Aggressive optimizations
  -Os                    Optimize for code size
  -Og                    Optimize for debugging experience
  -mavx                  Enable AVX (256-bit float vectorization)
  -mavx2                 Enable AVX2 (256-bit integer vectorization)

Debug:
  -g                     Emit debug symbols (DWARF on Linux, CodeView on Windows)
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
- `tests/`: Comprehensive automated test suite (97+ tests).
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

### Benchmark Suite
Measures output binary performance at each optimization level and compares against GCC.

```bash
./bench.sh                    # Run all benchmarks (default: 3 reps)
REPS=5 ./bench.sh             # More repetitions for stability
./bench.sh build_linux/fadors99_stage1  # Benchmark stage-1 compiler output
```

Benchmark programs in `tests/bench_*.c` exercise: tight loops, nested function calls, array traversal, branch-heavy code (Collatz), and struct field access.

### Valgrind Memory Check
Runs the compiler itself under Valgrind to detect memory errors (leaks, use-after-free, uninitialized reads, buffer overflows).

```bash
./test_valgrind.sh                          # Full check
./test_valgrind.sh build_linux/fadors99     # Specify compiler path

# Manual single-file check:
valgrind --leak-check=full --track-origins=yes ./build_linux/fadors99 tests/01_return.c -o /tmp/test
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

#### Phase 1: Type System Foundation
- [x] `const` qualifier: Parse and ignore. Used in every function signature.
- [x] `size_t` / `uint8_t` / `uint16_t` / `uint32_t` / `uint64_t` / `int16_t` / `int32_t`: Typedef aliases via `<stdint.h>` and `<stddef.h>`.
- [x] `static` functions: Internal linkage (used in 9/11 `.c` files).
- [x] `unsigned` type modifier.

#### Phase 2: Minimal Standard Library Headers
- [x] `<stddef.h>`: `size_t`, `NULL`, `ptrdiff_t`.
- [x] `<stdint.h>`: Fixed-width integer typedefs.
- [x] `<stdlib.h>`: `malloc`, `free`, `realloc`, `atoi`, `exit`.
- [x] `<string.h>`: `memcpy`, `memset`, `strlen`, `strcmp`, `strncmp`, `strcpy`, `strncpy`, `strchr`, `strcat`.
- [x] `<stdio.h>`: `FILE`, `printf`, `sprintf`, `fopen`, `fclose`, `fread`, `fwrite`, `fseek`, `ftell`.
- [x] `<ctype.h>`: `isalpha`, `isdigit`, `isalnum`, `isspace`.
- [x] `#include <...>` angle-bracket support with `include/` search path.

#### Phase 3: Language Feature Completion
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

- [x] **CLI parsing**: Add `-O0`, `-O1`, `-O2`, `-O3`, `-Og`, `-Os`, and `-g` flag handling in `main.c`.
- [x] **Compiler options struct**: Add `opt_level` (0–3) and `debug_info` (bool) fields to a global `CompilerOptions` struct in `codegen.h`.
- [x] **Pass options to codegen**: `g_compiler_options` is globally accessible from all pipeline stages via `codegen.h`.

### Phase 2: `-g` — Debug Symbol Generation (DWARF / CodeView)

**Goal**: Emit debug information so `gdb`/`lldb` (Linux) or Visual Studio/WinDbg (Windows) can map machine code back to source lines, variables, and types.

#### Phase 2a: ELF / DWARF (Linux)
- [x] **Line number tracking**: Record source file + line number for each AST node during parsing; propagate to codegen.
- [x] **`.debug_line` section**: Emit DWARF 4 line number program (opcodes: `DW_LNS_advance_pc`, `DW_LNS_advance_line`, `DW_LNS_copy`, special opcodes) mapping instruction offsets → source lines.
- [x] **`.debug_info` section**: Emit compilation unit DIE (`DW_TAG_compile_unit`) with producer, language, file reference, low_pc/high_pc, stmt_list.
- [x] **`.debug_abbrev` section**: Define abbreviation table entries for compile unit DIE.
- [x] **Symbol table**: `.symtab` / `.strtab` sections with function symbols for debugger resolution.
- [x] **ELF writer integration**: Custom `.fadors_debug` section in `.o` files carries raw line entries; linker generates proper DWARF 4 sections (`.debug_abbrev`, `.debug_info`, `.debug_line`) in final executable.
- [x] **DWARF version**: DWARF 4 (compatible with gdb 7.5+, lldb).
- [x] **GDB verification**: `break main`, `step`, `list`, `info line` all work correctly with source-level mapping.
- [x] **Subprogram DIEs**: `DW_TAG_subprogram` for each function — name, low_pc/high_pc, frame_base, return type, external flag.
- [x] **Variable DIEs**: `DW_TAG_variable` / `DW_TAG_formal_parameter` with location expressions (`DW_OP_fbreg + SLEB128 offset`) for locals and parameters.
- [x] **Type DIEs**: `DW_TAG_base_type` for int/char/short/long/float/double and unsigned variants, `DW_TAG_pointer_type`, `DW_TAG_unspecified_type` (void). Type deduplication across all functions.
- [x] **`.debug_str` section**: Pooled string table for type/variable/function names, referenced via `DW_FORM_strp`.
- [x] **`.debug_aranges` section**: Address range lookup table mapping text segment to compilation unit.

#### Phase 2b: COFF / CodeView (Windows)
- [x] **`.debug$S` section**: Emit CodeView S_GPROC32 (function symbols), S_REGREL32 (stack-relative variable locations), S_FRAMEPROC (frame info), S_OBJNAME, S_COMPILE3 (compiler info), S_END.
- [x] **`.debug$T` section**: Emit CodeView type records (LF_PROCEDURE, LF_ARGLIST) with correct basic type indices (T_INT4, T_CHAR, T_REAL32, T_64PVOID, etc.).
- [x] **Line number records**: CodeView line-to-address mapping in `.debug$S` subsection (DEBUG_S_LINES) with SECREL/SECTION relocations.
- [x] **File checksum records**: DEBUG_S_FILECHKSMS for source file identification, DEBUG_S_STRINGTABLE for filename strings.
- [x] **PE linker integration**: Skip `.debug$S` and `.debug$T` sections during PE linking (PDB generation deferred). Section symbols with auxiliary records for SECREL/SECTION relocations.
- [x] **CodeView version**: CodeView C13 format (CV_SIGNATURE_C13), compatible with Visual Studio 2015+ and MSVC `link.exe /DEBUG`.

#### Phase 2c: Source-Level Debugging Verification
- [x] **Test `gdb` step-through**: Compile a test program with `-g`, verify `gdb` can `break main`, `step`, `list`, `info line`.
- [x] **Test `gdb` variable inspection**: Verify `print` and `info locals` work — `print result` shows 30, `info args` shows a=10 b=20, `info locals` shows x/y/z.
- [x] **Test `lldb` step-through**: Breakpoints resolve to source file:line, `frame variable` shows typed locals (e.g., `(int) x = 0`), step-over advances correctly, `source list` shows correct code with `->` arrow.
- [ ] **Test Visual Studio debugging**: Compile with `-g --masm`, verify variable inspection in VS debugger.
- [x] **Test source mapping accuracy**: Line-by-line stepping matches actual source code. Automated test suite (`test_debug.sh`) verifies 39 checks: DWARF sections, .debug_info content, .debug_str strings, .debug_aranges, .debug_line entries, GDB (breakpoints, locals, args, print, backtrace, stepping), LLDB (breakpoints, frame variable, step-over, source list).

### Phase 3: `-O1` — Basic Optimizations

**Goal**: Low-cost optimizations that improve performance without significantly increasing compile time. These operate primarily on the AST or during codegen emission. Implemented as a new AST optimization pass (`optimizer.c`) run between parsing and codegen, plus codegen-level improvements in `arch_x86_64.c`.

- [x] **Constant folding**: Evaluate constant expressions at compile time (e.g., `3 + 4` → `7`, `(2+3)*(4-1)` → `15`). Handles all binary operators, unary `-`, `!`, `~`, and nested expressions. Also folds comparisons (`10 > 5` → `1`) and logical operators (`&&`, `||`). Reduces code size ~36% on constant-heavy code (222→142 bytes text).
- [x] **Dead code elimination (basic)**: Remove statements after unconditional `return`, `break`, `continue`, `goto` in blocks. Remove unreachable branches when `if`/`while`/`for` condition is a compile-time constant (`if(0){...}` → eliminated, `while(0){...}` → eliminated, `if(1){...} else{...}` → then-branch only).
- [x] **Immediate operand optimization**: When a binary operation's right operand is a compile-time integer constant, emit `OP $imm, %reg` directly instead of `mov $imm, %rax; push; pop %rcx; OP %rcx, %rax`. Covers `+`, `-`, `*`, `&`, `|`, `^`, `<<`, `>>`, and all 6 comparison operators. Handles pointer arithmetic scaling. Excludes `/` and `%` (require `idiv` with register operands). Reduces code size and eliminates unnecessary stack traffic.
- [x] **Strength reduction (simple)**: Replace `x * 2^n` with `x << n`, `x / 2^n` with `x >> n`, `x % 2^n` with `x & (2^n-1)`. Works for any power-of-two constant on either side of multiplication.
- [x] **Branch optimization**: Extend existing peephole — remove redundant jumps, convert `jcc` over `jmp` to inverted `jcc`. Buffers conditional jumps and detects `jcc L1; jmp L2; L1:` patterns, emitting `j!cc L2` (inverted condition) instead. Only fires when L1 is confirmed as the immediately following label. Also eliminates `jcc L; L:` (conditional jump to next instruction). Fixed dead-code elimination to preserve switch `case`/`default` labels after `break` statements.
- [x] **Zero-initialization optimization**: Use `xor %eax, %eax` instead of `mov $0, %rax` for integer zero-init at `-O1` and above. Also applied to pre-call register zeroing.
- [x] **Boolean simplification**: All condition tests (`if`, `while`, `do-while`, `for`, ternary) use `test %rax, %rax` instead of `cmp $0, %rax` (shorter encoding, 2 bytes vs 7). Applied unconditionally (always an improvement).
- [x] **Algebraic simplification**: Identity (`x+0→x`, `x*1→x`, `x/1→x`, `x|0→x`, `x^0→x`, `x<<0→x`) and annihilator (`x*0→0`, `x&0→0`) rules. Also double-negation removal (`-(-x)→x`, `~~x→x`). Automated test suite: `test_opt.sh` (30 tests).
- [x] **Assert-based value range analysis**: Extract value ranges from `assert()` conditions to guide optimization. Supports `assert(x == CONST)` (exact value substitution enabling constant folding + strength reduction), `assert(x >= lo && x <= hi)` (range narrowing), `assert((x & (x-1)) == 0)` (power-of-2 detection), and `&&` chains for combining multiple constraints. For example, `assert(x == 8); return y * x;` is optimized to `return y << 3;`.

### Phase 4: `-O2` — Standard Optimizations

**Goal**: Medium-cost optimizations that require analysis passes. Implemented as within-block analysis in the AST optimizer (`optimizer.c`) and codegen-level transforms in `arch_x86_64.c`. More advanced optimizations (CSE, LICM, register allocation) require IR/CFG infrastructure.

#### Phase 4a: Within-Block Optimization (implemented)
- [x] **Constant propagation**: Track `var = const` assignments within a basic block. Substitute known constants into subsequent uses, enabling further constant folding. Handles chained propagation (`x = 5; y = x + 3 → y = 8; z = y * 2 → z = 16`). Correctly avoids propagating into loop conditions to prevent infinite loops.
- [x] **Copy propagation**: Track `var = othervar` assignments. Replace uses of the copy with the original variable when the source hasn't been invalidated. Conservative invalidation on calls, pointer writes, and control flow.
- [x] **Dead store elimination**: Detect assignments to variables that are overwritten before being read. Remove the dead store (converted to no-op). Preserves stores with side effects. Only eliminates assignment statements, not variable declarations (which are still needed for stack allocation).
- [x] **Tail call optimization**: Detect `return f(args)` pattern and convert `call f; leave; ret` to `leave; jmp f` when all args fit in registers and return types are register-compatible. Eliminates stack frame overhead for tail-position calls. Works for both self-recursive and cross-function tail calls.
- [x] **Function inlining** (small functions): Inline single-return-expression functions at call sites when arguments have no side effects. Replaces `AST_CALL` with clone of callee's return expression, substituting parameters with actual arguments. Combined with constant folding, enables full compile-time evaluation (e.g., `add(square(3), square(4))` → `25`).
- [x] **Inline hinting**: GCC and MSVC style inline hints control inlining behavior across optimization levels. `__forceinline` and `__attribute__((always_inline))` force inlining even at `-O0`. `inline`, `__inline`, `__inline__` enable inlining at `-O1+`. `__attribute__((noinline))` and `__declspec(noinline)` suppress inlining at all levels. Supports both pre-return-type and post-parameter-list `__attribute__` syntax.

#### Phase 4b: IR / CFG Construction
- [x] **Basic block identification**: Split function bodies into basic blocks at branch targets and after jumps/returns.
- [x] **Control Flow Graph (CFG)**: Build directed graph of basic blocks with predecessor/successor edges. Three-address code IR with 40+ opcodes; expression/statement lowering for all AST node types; `--dump-ir` flag for debug output. Activated at `-O2+`.
- [x] **SSA construction**: Convert variables to Static Single Assignment form with φ-functions. Implements Cooper-Harvey-Kennedy iterative dominator algorithm, dominance frontier computation, φ-function insertion at iterated dominance frontiers, and DFS variable renaming on the dominator tree. Parameter SSA entry vregs, SSA validation (single-definition check), dominator/DF info in `--dump-ir` output.

#### Phase 4c: Analysis Passes
- [x] **Liveness analysis**: Iterative backward dataflow on SSA-form IR. Computes def/use bitsets per block, propagates `live_in[B] = use[B] ∪ (live_out[B] − def[B])` and `live_out[B] = ∪ live_in[S]` to fixed point. PHI arguments modelled as uses in predecessor blocks. Parameter entry vregs implicitly defined. Live-in/live-out vreg sets shown in `--dump-ir` output.
- [x] **Reaching definitions**: Forward dataflow analysis tracking which `(block, vreg, instr_idx)` definitions reach each point. Per-vreg gen/kill sets with iterative convergence. Allocated on demand via `ir_compute_reaching_defs()` (caller frees).
- [x] **Dominator tree**: Compute dominance relationships and dominance frontiers. Used for SSA φ-function placement and loop detection.
- [x] **Loop detection**: Back-edge identification via dominator tree (`ir_dominates()` walk), natural loop body collection via backward DFS from back-edge source, loop depth and header annotations per block. Handles nested loops (sorted by body size so inner loops overwrite outer headers). Shown in `--dump-ir` output as `loop: depth=N hdr=bbM`.

#### Phase 4d: Advanced Optimization Passes
- [x] **Global constant propagation (SCCP)**: Sparse Conditional Constant Propagation — lattice-based (TOP/CONST/BOTTOM) iterative propagation on SSA virtual registers. Evaluates binary/unary ops on constant operands at compile time. Phase 2 rewrites constant vreg uses to immediates. Phase 3 folds constant branches to unconditional jumps, eliminating dead CFG paths. Handles addition, subtraction, multiplication, division, modulo, comparisons, and negation.
- [x] **Common subexpression elimination (CSE/GVN)**: Global Value Numbering with hash-based dominator-tree preorder walk. Value numbers assigned per SSA vreg with copy propagation (COPY instructions propagate VN from source). Pure computations with matching `(opcode, VN(src1), VN(src2))` replaced with copy from earlier result. Redundant `a + b` computed twice becomes `copy` of first result.
- [x] **Loop-invariant code motion (LICM)**: Uses `IRLoopInfo` from loop detection. Iteratively marks instructions as invariant if all sources are constants, defined outside the loop, or already marked invariant. Function parameters (no def block) are treated as invariant. Creates preheader block when needed, redirects CFG edges, updates PHI predecessors, and migrates invariant instructions before loop entry.
- [x] **Register allocation (linear scan)**: Linear scan over liveness intervals. 14 allocatable x86_64 GPRs (rax, rcx, rdx, rbx, rsi, rdi, r8–r15, excluding rsp/rbp). Spills longest-lived active interval when registers exhausted. Summary shown in `--dump-ir` output: `regalloc: N in regs, M spilled (S slots)` with per-vreg register assignments.

### Phase 5: `-O3` — Aggressive Optimizations

**Goal**: More aggressive, potentially code-size-increasing optimizations for maximum runtime performance.

- [x] **Aggressive inlining**: Multi-statement functions (up to 8 stmts) inlined at call sites with parameter substitution, local variable renaming, and statement injection. Recursively searches expression trees (`find_call_in_expr`) to find calls nested inside binary expressions, casts, and unary operators (e.g., `sum = sum + f(x)` where the call is inside a `+` node). Self-recursion prevention via per-call function name check.
- [x] **Loop unrolling**: Full unroll for constant-count loops with N ≤ 4. Remainder iterations unrolled with constant substitution.
- [x] **Loop strength reduction**: Achieved through loop unrolling + constant folding — after unrolling, `a[i]` becomes `a[0]`, `a[1]`, ... which are constant-folded into direct indexed addressing.
- [x] **Interprocedural optimization**: IPA constant propagation (specialize parameters always passed as the same constant across all call sites), dead argument elimination (remove unused parameters from definitions and call sites), dead function elimination (remove functions with zero callers after inlining), and return value propagation (replace calls to always-constant-returning functions with the constant).
- [x] **Vectorization hints**: Auto-detect simple array loops (`a[i] = b[i] OP c[i]`) and emit packed instructions. SSE/SSE2 path (default): `movups`/`addps`/`subps`/`mulps`/`divps` for 4×float, `movdqu`/`paddd`/`psubd` for 4×int32 (128-bit, 4 elements per iteration). AVX/AVX2 path (`-mavx`/`-mavx2`): `vmovups`/`vaddps`/`vsubps`/`vmulps`/`vdivps` for 8×float, `vmovdqu`/`vpaddd`/`vpsubd` for 8×int32 (256-bit YMM registers, 8 elements per iteration, 3-operand VEX encoding). Automatic scalar remainder for non-aligned sizes and `vzeroupper` for AVX-to-SSE transition penalty avoidance.
- [x] **Instruction scheduling**: Replace `pushq/popq` operand save/restore with register-to-register `mov` when the left operand of a binary expression is a simple load (`gen_expr_is_rax_only` check). Eliminates 2 memory operations per qualifying binary expression and schedules independent loads closer together for better ILP. Activated at `-O2+` (lowered from `-O3` with register allocator). Combined with aggressive inlining, yields 46% runtime improvement on struct-heavy benchmarks (0.167s → 0.090s).
- [x] **Profile-guided optimization (PGO)**: Two-phase workflow: (1) `-fprofile-generate` instruments function entries and branch points with `incq` counters, emits `__pgo_dump` function using Linux syscalls (open/write/close) to flush binary profile (`PGO1` magic + 64-byte name + 8-byte counter entries) at program exit; (2) `-fprofile-use=FILE` loads profile at optimization time, classifies functions as hot (≥10% of max count) or cold (≤1% or zero), raises aggressive inline threshold from 8→20 statements for hot functions, skips cold functions from inlining, and increases O2 expression node limit 4× for hot functions. Demonstrates **1.8× speedup** on profile-guided test cases by inlining 10-statement hot-loop functions that exceed normal thresholds.

#### Benchmark Results (best of 3 runs)

| Benchmark | -O0 | -O1 | -O2 | -O3 | Speedup (O0→O3) |
|-----------|-----|-----|-----|-----|----------|
| bench_array | 0.084s | 0.079s | 0.029s | 0.028s | **67%** |
| bench_struct | 0.168s | 0.134s | 0.089s | 0.072s | **57%** |
| bench_calls | 0.069s | 0.060s | 0.014s | 0.014s | **80%** |
| bench_loop | 0.019s | 0.010s | 0.008s | 0.008s | **58%** |
| bench_branch | 0.038s | 0.029s | 0.025s | 0.025s | **34%** |

**-O2 register allocator impact** (vs previous -O1 baseline):

| Benchmark | -O1 (before) | -O2 (regalloc) | Speedup |
|-----------|-------------|----------------|----------|
| bench_array | 0.079s | 0.029s | **2.7×** |
| bench_calls | 0.060s | 0.014s | **4.3×** |
| bench_struct | 0.134s | 0.089s | **1.5×** |
| bench_branch | 0.029s | 0.025s | **1.2×** |
| bench_loop | 0.010s | 0.008s | **1.3×** |

### Phase 6: `-Os` / `-Og` — Size & Debug Optimizations

- [x] **`-Os`**: Applies `-O2` effective optimization level but avoids code-size-increasing transforms — no loop unrolling, no SIMD vectorization, no aggressive O3 inlining. Uses `opt_effective_level()` mapping (`Os→O2`) with `OPT_SIZE_MODE` guards. Code size ≤ `-O2` on all test cases.
- [x] **`-Og`**: Applies only `-O1` effective optimization level with additional debug-friendly restrictions — no function inlining (except `always_inline`), no register allocation (variables stay on stack for debugger access), no cmov (preserves branch structure), no tail call optimization (preserves call stack). Uses `opt_effective_level()` mapping (`Og→O1`) with `OPT_DEBUG_MODE` guards.

### Phase 7: Codegen — Closing the GCC Gap

**Goal**: Address the fundamental code quality gap between fadors99 and GCC -O2. Analysis showed the root cause was that all local variables lived on the stack, causing 2+ memory round-trips per variable per loop iteration. The register allocator (Phase 7a) addressed this, reducing the average gap from 10× to 5.9×.

#### Phase 7a: Register Allocator (highest impact — affects ALL benchmarks) 
- [x] **Register allocator for local variables**: AST pre-scan with use-count priority assignment. Assigns up to 5 callee-saved registers (`%rbx`, `%r12`–`%r15`) to the most-used eligible scalar integer locals (no address-taken, no arrays/structs/floats). Two-phase design: `regalloc_analyze()` scans the AST and determines assignments before codegen, `regalloc_emit_saves()` pushes callee-saved registers in the prologue. Variables in registers skip stack allocation entirely — loads become `mov %reg, %rax`, stores become `mov %rax, %reg`, increments directly modify the register. Callee-saved registers restored from `rbp`-relative slots before epilogue. Activated at `-O2+`.
- [x] **Function parameter register allocation**: Parameters passed in ABI argument registers (`%rdi`, `%rsi`, etc.) are moved directly to callee-saved registers instead of being spilled to the stack. Combined with local variable register allocation, this eliminates all memory traffic for hot scalar variables in functions like `collatz_steps(int n)` where `n` stays in `%rbx` throughout.
- [x] **Eliminate redundant loads/stores**: Track which value is already in a register and skip reloads from stack. Simple "last-value cache" per register — even without full regalloc this eliminates back-to-back `movl %eax, N(%rbp); movl N(%rbp), %eax`.
- [ ] **Frame pointer elimination (`-fomit-frame-pointer`)**: Free up `%rbp` as a GPR. Use `%rsp`-relative addressing. Requires tracking stack depth at each point.

#### Phase 7b: Instruction Selection (easy wins)
- [x] **LEA for multiply-add patterns**: `x*3` → `lea (%rax,%rax,2)`, `x*5` → `lea (%rax,%rax,4)`, `a+b*4` → `lea (%rax,%rcx,4)`. Peephole on `imull $const` sequences. 1-cycle latency vs 3-cycle `imull`.
- [ ] **Strength reduction (imul→lea/add)**: Replace `imull $const, %reg` with LEA chains for constants 2–9. `x*7` → `lea (%rax,%rax,2), %rcx; lea (%rax,%rcx,2)`.
- [x] **Peephole: pushq/popq→mov reg**: Replace `pushq %rax; ...; popq %rcx` with `mov %rax, %r11; ...; mov %r11, %rcx` using scratch registers. Eliminates 2 memory ops per pair.
- [x] **`test` instead of `cmp $0`**: `cmpl $0, %eax` → `testl %eax, %eax` (shorter encoding, same semantics).
- [x] **Conditional move (cmov)**: Branch-free `if (cond) x = a; else x = b;` → `cmov`. Avoids branch misprediction on data-dependent branches.

#### Phase 7c: Loop Optimizations
- [x] **Loop induction variable strength reduction**: Replace `i*3` inside loop with induction variable `j += 3` that tracks the product. Eliminates multiply per iteration.
- [x] **Loop rotation (while→do-while)**: Transform `while(cond) { body }` → `if(cond) do { body } while(cond)`. Backward branch is predicted taken, eliminating one branch per iteration.
- [x] **Deeper transitive inlining**: Inline chains: if `compute` calls already-inlined `add`/`mul`, re-check stmt count post-substitution. Functions that shrink after inlining their callees become eligible.

#### Phase 7d: Vectorization
- [x] **Auto-vectorize reduction loops (SIMD)**: Detect `sum += a[i]` pattern → `paddd` accumulator with horizontal reduction at loop exit. Current vectorizer only handles `a[i] = b[i] OP c[i]`, not reductions.
- [x] **SIMD init loop vectorization**: Initialize arrays 4/8 elements at a time with packed integer ops (`pslld`/`paddd`).

#### GCC -O2 vs fadors99 -O3 Gap Analysis

| Benchmark | fadors99 -O3 | gcc -O2 | Gap | Remaining Root Causes |
|-----------|-------------|---------|-----|-------------|
| bench_array | 0.005s | 0.003s | 1.7× | SIMD reduction + init active; remaining gap is alignment & unrolling |
| bench_struct | 0.069s | 0.005s | 14× | pushq/popq temps in `distance_sq` (5-reg limit), no SIMD accumulator |
| bench_calls | 0.007s | 0.005s | 1.4× | Transitive inlining eliminates all calls; LEA mul-add active |
| bench_loop | 0.010s | 0.004s | 2.5× | IV SR active; remaining gap is register pressure |
| bench_branch | 0.025s | 0.019s | 1.3× | Loop rotation active; `call collatz_steps` not inlined |

*Phase 7d: SIMD vectorization cut bench_array -O3 from 0.026s to 0.005s (5.2× faster). Reduction uses paddd+pshufd horizontal sum; init uses movdqu stride stores. Average gap is ~4.2× across all benchmarks.*

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
                    ├── Phase 6 (-Os/-Og)
                    │
                    └── Phase 7 (Codegen quality)
```

**Suggested order**: Phase 1 → Phase 2a (DWARF) → Phase 3 (-O1) → Phase 4 (-O2) → Phase 2b (CodeView) → Phase 5 (-O3) → Phase 7a (regalloc) → Phase 7b (insn select) → Phase 7c (loops) → Phase 7d (vectorize) → Phase 6.
