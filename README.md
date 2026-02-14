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

### Phase 1: Infrastructure — CLI Flags & Compiler State ✅

**Goal**: Parse `-O0`/`-O1`/`-O2`/`-O3` and `-g` flags, propagate settings through the compilation pipeline.

- [x] **CLI parsing**: Add `-O0`, `-O1`, `-O2`, `-O3`, `-Og`, `-Os`, and `-g` flag handling in `main.c`.
- [x] **Compiler options struct**: Add `opt_level` (0–3) and `debug_info` (bool) fields to a global `CompilerOptions` struct in `codegen.h`.
- [x] **Pass options to codegen**: `g_compiler_options` is globally accessible from all pipeline stages via `codegen.h`.

### Phase 2: `-g` — Debug Symbol Generation (DWARF / CodeView) 🔄

**Goal**: Emit debug information so `gdb`/`lldb` (Linux) or Visual Studio/WinDbg (Windows) can map machine code back to source lines, variables, and types.

#### Phase 2a: ELF / DWARF (Linux) ✅
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

#### Phase 4c: Analysis Passes ✅
- [x] **Liveness analysis**: Iterative backward dataflow on SSA-form IR. Computes def/use bitsets per block, propagates `live_in[B] = use[B] ∪ (live_out[B] − def[B])` and `live_out[B] = ∪ live_in[S]` to fixed point. PHI arguments modelled as uses in predecessor blocks. Parameter entry vregs implicitly defined. Live-in/live-out vreg sets shown in `--dump-ir` output.
- [x] **Reaching definitions**: Forward dataflow analysis tracking which `(block, vreg, instr_idx)` definitions reach each point. Per-vreg gen/kill sets with iterative convergence. Allocated on demand via `ir_compute_reaching_defs()` (caller frees).
- [x] **Dominator tree**: Compute dominance relationships and dominance frontiers. Used for SSA φ-function placement and loop detection.
- [x] **Loop detection**: Back-edge identification via dominator tree (`ir_dominates()` walk), natural loop body collection via backward DFS from back-edge source, loop depth and header annotations per block. Handles nested loops (sorted by body size so inner loops overwrite outer headers). Shown in `--dump-ir` output as `loop: depth=N hdr=bbM`.

#### Phase 4d: Advanced Optimization Passes (future — requires analysis)
- [ ] **Global constant propagation**: Propagate known-constant values across basic blocks (current implementation is within-block only).
- [ ] **Common subexpression elimination (CSE)**: Detect repeated computations (`a + b` computed twice) and reuse the first result.
- [ ] **Loop-invariant code motion (LICM)**: Move computations that don't change within a loop to the preheader.
- [ ] **Register allocation**: Replace naive stack-spill-everything with a graph-coloring or linear-scan register allocator. Use liveness analysis to minimize spills.

### Phase 5: `-O3` — Aggressive Optimizations

**Goal**: More aggressive, potentially code-size-increasing optimizations for maximum runtime performance.

- [x] **Aggressive inlining**: Multi-statement functions (up to 20 stmts) inlined at call sites with parameter substitution, local variable renaming, and statement injection. Self-recursion prevention.
- [x] **Loop unrolling**: Full unroll for constant-count loops with N ≤ 8; partial unroll with factor 2–4 for larger loops (9–256 iterations). Remainder iterations unrolled with constant substitution.
- [x] **Loop strength reduction**: Achieved through loop unrolling + constant folding — after unrolling, `a[i]` becomes `a[0]`, `a[1]`, ... which are constant-folded into direct indexed addressing.
- [x] **Interprocedural optimization**: IPA constant propagation (specialize parameters always passed as the same constant across all call sites), dead argument elimination (remove unused parameters from definitions and call sites), dead function elimination (remove functions with zero callers after inlining), and return value propagation (replace calls to always-constant-returning functions with the constant).
- [x] **Vectorization hints**: Auto-detect simple array loops (`a[i] = b[i] OP c[i]`) and emit packed instructions. SSE/SSE2 path (default): `movups`/`addps`/`subps`/`mulps`/`divps` for 4×float, `movdqu`/`paddd`/`psubd` for 4×int32 (128-bit, 4 elements per iteration). AVX/AVX2 path (`-mavx`/`-mavx2`): `vmovups`/`vaddps`/`vsubps`/`vmulps`/`vdivps` for 8×float, `vmovdqu`/`vpaddd`/`vpsubd` for 8×int32 (256-bit YMM registers, 8 elements per iteration, 3-operand VEX encoding). Automatic scalar remainder for non-aligned sizes and `vzeroupper` for AVX-to-SSE transition penalty avoidance.
- [ ] **Instruction scheduling**: Reorder independent instructions to reduce pipeline stalls and improve ILP (instruction-level parallelism). (future — requires instruction buffer + CFG)
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
