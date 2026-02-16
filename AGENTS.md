# AGENTS.md — Fador's C99 Compiler

## Project Overview

Fador's C99 Compiler (`fadors99`) is a self-hosting C99 compiler targeting x86_64 Linux (ELF) and Windows (COFF/PE). On Linux the entire pipeline — compile, assemble, link — runs without external tools. The compiler supports optimization levels `-O0` through `-O3`, `-Os`, `-Og`, and DWARF/CodeView debug symbols via `-g`.

## Building

### Prerequisites

- CMake 3.x+
- A C99-compliant host compiler (GCC, Clang, or MSVC)

### Linux Build

```bash
cd build_linux
cmake ..
cmake --build .
```

This produces `build_linux/fadors99` (stage-0 binary).

### Windows Build

```bash
mkdir build && cd build
cmake ..
cmake --build . --config Release
```

### Stage-1 Bootstrap (Self-Compiled)

Compile the compiler with itself using the stage-0 binary:

```bash
./build_stage1.sh [stage0_path]
```

- Default stage-0: `build_linux/fadors99`
- Compiles all 16 source files (`buffer.c`, `types.c`, `ast.c`, `lexer.c`, `preprocessor.c`, `parser.c`, `codegen.c`, `arch_x86_64.c`, `encoder.c`, `coff_writer.c`, `elf_writer.c`, `linker.c`, `pe_linker.c`, `optimizer.c`, `ir.c`, `main.c`) to assembly via the stage-0 compiler, then assembles with GNU `as` and links with `gcc`.
- Output: `build_linux/fadors99_stage1` and intermediate assembly in `stage1/*.s`
- Includes a smoke test: compiles `tests/01_return.c` with stage-1 and verifies exit code = 42.

## Test Suites

All test scripts live in the project root. They default to `build_linux/fadors99` and accept an optional compiler path as the first argument. All return exit code 0 on success, 1 on failure.

### Quick Reference

| Script | Tests | What It Verifies |
|--------|-------|-----------------|
| `test_obj.sh` | ~73 | Object file mode (`-c`) correctness |
| `test_opt.sh` | ~72 | Optimization passes (`-O1`, `-O2`) |
| `test_stage1.sh` | ~76 | Stage-1 self-compiled compiler correctness |
| `test_linker.sh` | ~72 | Built-in ELF linker mode |
| `test_debug.sh` | ~35 | DWARF debug symbols + GDB/LLDB |
| `test_ir.sh` | 12 | IR/CFG construction |
| `test_ssa.sh` | 24 | SSA construction |
| `test_analysis.sh` | 25 | Liveness analysis + loop detection |
| `test_phase4d.sh` | 48 | SCCP, CSE/GVN, LICM, register allocation |
| `test_valgrind.sh` | ~49 | Memory safety (Valgrind memcheck) |

### Run All Core Tests

```bash
./test_obj.sh && ./test_opt.sh && ./test_stage1.sh
```

### Run Full Suite

```bash
./test_obj.sh && \
./test_opt.sh && \
./test_stage1.sh && \
./test_linker.sh && \
./test_debug.sh && \
./test_ir.sh && \
./test_ssa.sh && \
./test_analysis.sh && \
./test_phase4d.sh && \
./test_valgrind.sh
```

---

### test_obj.sh — Object File Mode Tests

Compiles each `tests/*.c` to `.o` with the compiler (`-c` flag), links with `gcc -no-pie`, runs the binary, and checks the exit code against expected values.

```bash
./test_obj.sh [compiler_path]
```

- **Pipeline**: `fadors99 -c` → `.o` → `gcc -no-pie` → run → check exit code
- **Skips**: `13_extern`, `26_external` (require external linkage)
- **73 expected results** (e.g., `01_return` → 42, `02_arithmetic` → 7)

### test_opt.sh — Optimization Verification

Tests that `-O1` and `-O2` optimizations produce correct results and improve code quality. Creates inline test programs, compiles at various optimization levels, and inspects both exit codes and generated assembly for expected patterns.

```bash
./test_opt.sh [compiler_path]
```

**Sections tested** (15 total):
1. Constant Folding — compile-time evaluation
2. Strength Reduction — `*2^n` → `<<n`
3. Dead Code Elimination — unreachable removal
4. Algebraic Simplification — identity/annihilator rules
5. Code Size Comparison — `-O1` text smaller than `-O0`
6. `-O0` Correctness — baseline sanity
7. Zero-init Optimization — `xor %eax,%eax`
8. Immediate Operand Optimization — `OP $imm, %reg`
9. Branch Optimization — redundant jump elimination
10. Tail Call Optimization (`-O2`) — `call+ret` → `jmp`
11. Function Inlining (`-O2`) — small function inlining
12. Inline Hinting — `__forceinline`, `__attribute__((always_inline))`, `__attribute__((noinline))`, `__declspec(noinline)`
13. LEA Multiply-Add — `imul $3/5/9` → `lea` peephole
14. `test` vs `cmp $0` — `cmpl $0` → `testl` shorter encoding
15. Conditional Move — `cmov` for simple ternary expressions

### test_stage1.sh — Stage-1 Compiler Tests

Runs the full test suite through the self-compiled (stage-1) compiler using the built-in linker. Full pipeline with no external tools.

```bash
./test_stage1.sh [stage1_path]
# Default: build_linux/fadors99_stage1
# Also accepts COMPILER env var
```

- **Pipeline**: `fadors99_stage1 -o` → run → check exit code
- **76 expectations** (includes `72_global_init_list` through `75_reloc_test`)
- **Prerequisite**: Run `build_stage1.sh` first

### test_linker.sh — Built-in Linker Tests

Tests the compiler's built-in ELF linker. Same test set as `test_obj.sh` but uses the full compile+link pipeline (`-o` flag) with no external tools.

```bash
./test_linker.sh [compiler_path]
```

- **Pipeline**: `fadors99 -o` (compile + internal link) → run → check exit code
- **72 expected results**

### test_debug.sh — DWARF Debug Symbol Verification

Verifies that `-g` produces correct DWARF debug information for GDB and LLDB.

```bash
./test_debug.sh [compiler_path]
```

**Sections** (7):
1. **DWARF Sections** (5 checks): `.debug_info`, `.debug_abbrev`, `.debug_line`, `.debug_str`, `.debug_aranges` exist via `readelf -S`
2. **`.debug_info`** (9 checks): `DW_TAG_compile_unit`, `DW_AT_producer=fadors99`, `DW_TAG_subprogram`, function names, `DW_TAG_variable`, `DW_TAG_formal_parameter`, `DW_TAG_base_type`, `DW_OP_fbreg`, `DW_OP_reg6`
3. **`.debug_str`** (6 checks): Strings for types, functions, variables
4. **`.debug_aranges`** (1 check): Address range entries
5. **`.debug_line`** (1 check): ≥5 line entries
6. **GDB** (8 checks): breakpoints, locals, args, print, backtrace, stepping *(skipped if gdb not installed)*
7. **LLDB** (5 checks): breakpoints, frame variable, step-over, source list *(skipped if lldb not installed)*

### test_ir.sh — IR/CFG Construction Tests

Verifies IR/CFG construction by compiling with `-O2 --dump-ir` and checking IR output.

```bash
./test_ir.sh [compiler_path]
```

- 12 tests: function structure, basic blocks, control flow (if/else, while, for, do-while), function calls, variable tracking, global variables, block/vreg counts.

### test_ssa.sh — SSA Construction Tests

Verifies SSA form: dominator tree, dominance frontiers, phi insertion, variable renaming.

```bash
./test_ssa.sh [compiler_path]
```

- 24 tests: phi nodes for branches/loops, dominator info, nested control flow, switch, parameter reassignment, multi-function SSA, single-definition property.
- Uses `set -e` — exits on first failure.

### test_analysis.sh — Analysis Passes

Tests liveness analysis and loop detection passes.

```bash
CC=build_linux/fadors99 ./test_analysis.sh
```

- 25 tests covering:
  - **Liveness**: live-in/live-out annotations across blocks, loops, branches, phi nodes
  - **Loop detection**: depth annotations, nested loops, header identification, do-while, break/continue

### test_phase4d.sh — Advanced Optimization Passes

Tests SCCP, CSE/GVN, LICM, and register allocation.

```bash
CC=build_linux/fadors99 ./test_phase4d.sh
```

- **48 tests** across 6 sections:
  1. **SCCP** (10): Constant folding across blocks, branches, multi-step chains
  2. **CSE/GVN** (6): Redundant expression elimination
  3. **LICM** (7): Loop-invariant code motion to preheader
  4. **Register Allocation** (5): Regalloc summary, spills, parameters
  5. **Combined Passes** (10): Multi-pass interaction
  6. **Edge Cases** (10): Empty functions, many parameters, deeply nested arithmetic

### test_valgrind.sh — Memory Safety

Runs the compiler itself under Valgrind memcheck to detect memory errors.

```bash
./test_valgrind.sh [compiler_path]
```

- **Requires**: `valgrind` installed
- **Main suite**: 42 representative test files run under Valgrind with `--leak-check=full --show-leak-kinds=definite,possible --errors-for-leak-kinds=definite --error-exitcode=99 --track-origins=yes`
- **Optimization flags**: Tests compiling with `-O0`, `-O1`, `-O2`, `-O3`, `-Os`, `-Og`, and `-g` under Valgrind (7 additional checks)
- Exit code 99 from Valgrind = memory errors detected → FAIL

## Benchmarking

### bench.sh — Performance Benchmarks

Compiles and runs benchmark programs at each optimization level and compares against GCC.

```bash
./bench.sh [compiler_path]         # Default: build_linux/fadors99
REPS=5 ./bench.sh                  # More repetitions for stability
./bench.sh build_linux/fadors99_stage1  # Benchmark stage-1 output
```

**Benchmark programs** (in `tests/`):

| File | Focus |
|------|-------|
| `bench_loop.c` | Tight loop performance |
| `bench_calls.c` | Nested function call overhead |
| `bench_array.c` | Array traversal patterns |
| `bench_branch.c` | Branch-heavy code (Collatz) |
| `bench_struct.c` | Struct field access |

**Output**: Table with columns: Benchmark, `-O0`, `-O1`, `-O2`, `-O3`, `gcc -O0`, `gcc -O2`

**Configuration**:
- `REPS` env var: number of repetitions per test (default: 3, takes best)
- Timing via bash `time` builtin

### Reference Results

| Benchmark | -O0 | -O1 | -O2 | -O3 | gcc -O2 |
|-----------|-----|-----|-----|-----|---------|
| bench_array | 0.083s | 0.077s | 0.028s | 0.028s | 0.003s |
| bench_branch | 0.039s | 0.026s | 0.024s | 0.025s | 0.020s |
| bench_calls | 0.067s | 0.060s | 0.014s | 0.014s | 0.005s |
| bench_loop | 0.019s | 0.010s | 0.008s | 0.008s | 0.004s |
| bench_struct | 0.139s | 0.109s | 0.078s | 0.069s | 0.005s |

## Test File Conventions

- All test files are in `tests/`, numbered `01` through `95` plus 5 `bench_*.c` files (106 total).
- Test correctness is verified via process exit code (0–255).
- Scripts create a tmpdir and clean up via `trap`.
- All report PASS/FAIL/SKIP/TOTAL counts.
- Skipped tests across suites: `13_extern` and `26_external` (require external linkage / Windows-only).

## Source Structure

```
src/
  main.c             # CLI, driver
  lexer.c/h          # Tokenizer
  preprocessor.c/h   # #include, #define, #ifdef, #pragma
  parser.c/h         # Recursive descent parser
  ast.c/h            # AST node types and utilities
  types.c/h          # Type system
  optimizer.c/h      # AST optimization passes (-O1+)
  ir.c/h             # IR/CFG/SSA construction, analysis, IR-level optimization
  codegen.c/h        # Codegen driver, CompilerOptions
  arch_x86_64.c/h    # x86_64 code generator (assembly + peephole)
  encoder.c/h        # x86_64 machine code encoder
  elf_writer.c/h     # ELF .o writer
  coff_writer.c/h    # COFF .obj writer
  linker.c/h         # ELF static/dynamic linker
  pe_linker.c/h      # PE/COFF linker
  buffer.c/h         # Dynamic buffer utilities
  pgo.c/h            # Profile-guided optimization
include/             # Bundled standard library headers
stage1/              # Stage-1 intermediate assembly
stage2/              # Stage-2 intermediate assembly
```

## Compiler Usage Quick Reference

```bash
# Compile to executable (Linux, fully self-contained)
./build_linux/fadors99 main.c -o main

# Compile with optimization
./build_linux/fadors99 main.c -o main -O2

# Compile with debug symbols
./build_linux/fadors99 main.c -o main -g

# Compile to object file only
./build_linux/fadors99 main.c -c

# Compile to assembly only
./build_linux/fadors99 main.c -S

# Dump IR (for debugging optimizer/analysis)
./build_linux/fadors99 main.c -O2 --dump-ir -S

# Link against libc
./build_linux/fadors99 main.c -o main -lc

# Windows COFF/PE target
./build_linux/fadors99 main.c --masm -o main.exe
```

## Environment Variables

| Variable | Used By | Purpose |
|----------|---------|---------|
| `CC` | `test_analysis.sh`, `test_phase4d.sh` | Compiler path |
| `COMPILER` | `test_stage1.sh` | Alternative to positional arg |
| `REPS` | `bench.sh` | Benchmark repetition count (default: 3) |

## Adding a New Test

1. Create `tests/NN_name.c` with a `main()` that returns the expected exit code (0–255).
2. Add the expected value to the `EXPECTED` associative array in `test_obj.sh`, `test_stage1.sh`, and `test_linker.sh`.
3. Run `./test_obj.sh` to verify.

## Debugging Failures

```bash
# Compile a single failing test manually
./build_linux/fadors99 tests/42_struct_access.c -o /tmp/test && /tmp/test; echo $?

# Inspect generated assembly
./build_linux/fadors99 tests/42_struct_access.c -S && cat tests/42_struct_access.s

# Inspect IR
./build_linux/fadors99 tests/42_struct_access.c -O2 --dump-ir -S 2>&1 | head -100

# Run under Valgrind
valgrind --leak-check=full --track-origins=yes ./build_linux/fadors99 tests/42_struct_access.c -o /tmp/test

# Debug with GDB (compile with -g)
./build_linux/fadors99 tests/42_struct_access.c -g -o /tmp/test
gdb /tmp/test
```
