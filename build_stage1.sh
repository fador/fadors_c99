#!/bin/bash
# build_stage1.sh — Bootstrap build: compile the compiler with itself (stage1)
#
# Usage:  ./build_stage1.sh [stage0_path]
#   stage0_path  Path to the stage-0 (GCC-built) compiler binary.
#                Defaults to build_linux/fadors99
#
# Outputs:
#   build_linux/fadors99_stage1   — the self-compiled compiler binary
#   stage1/*.s                    — intermediate assembly files (for inspection)
#
# Prerequisites:
#   - Stage-0 compiler already built (cmake + make in build_linux/)
#   - GNU as and gcc available on PATH
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

STAGE0="${1:-build_linux/fadors99}"

if [ ! -x "$STAGE0" ]; then
    echo "[ERROR] Stage-0 compiler not found at: $STAGE0"
    echo "        Build it first:  mkdir -p build_linux && cd build_linux && cmake .. && make"
    exit 1
fi

STAGE1_DIR="stage1"
STAGE1_BIN="build_linux/fadors99_stage1"

SOURCES=(
    src/buffer.c
    src/types.c
    src/ast.c
    src/lexer.c
    src/preprocessor.c
    src/parser.c
    src/codegen.c
    src/arch_x86_64.c
    src/arch_x86.c
    src/dos_linker.c
    src/encoder.c
    src/coff_writer.c
    src/elf_writer.c
    src/linker.c
    src/pe_linker.c
    src/optimizer.c
    src/ir.c
    src/pgo.c
    src/main.c
)

mkdir -p "$STAGE1_DIR"

echo "=== Stage 1 Bootstrap Build ==="
echo "Stage-0 compiler: $STAGE0"
echo ""

# Step 1: Compile each source file to assembly using stage-0
OBJECTS=""
FAIL=0
for src in "${SOURCES[@]}"; do
    name=$(basename "$src" .c)
    asm_file="$STAGE1_DIR/${name}.s"
    obj_file="$STAGE1_DIR/${name}.o"

    echo -n "  Compiling $src ... "
    if ! "$STAGE0" "$src" -S; then
        echo "FAIL (compiler error)"
        FAIL=1
        continue
    fi

    # The compiler outputs .s next to the source; move it
    src_s="src/${name}.s"
    if [ -f "$src_s" ]; then
        mv "$src_s" "$asm_file"
    else
        echo "FAIL (no .s output)"
        FAIL=1
        continue
    fi

    echo -n "assembling ... "
    if ! as -o "$obj_file" "$asm_file"; then
        echo "FAIL (assembler error)"
        FAIL=1
        continue
    fi

    echo "OK"
    OBJECTS="$OBJECTS $obj_file"
done

if [ "$FAIL" -ne 0 ]; then
    echo ""
    echo "[ERROR] Some files failed to compile/assemble. Aborting link."
    exit 1
fi

# Step 2: Link all objects into the stage-1 binary
echo ""
echo -n "  Linking $STAGE1_BIN ... "
if gcc -o "$STAGE1_BIN" $OBJECTS -lc 2>/dev/null; then
    echo "OK"
else
    echo "FAIL"
    exit 1
fi

echo ""
echo "=== Stage 1 build complete ==="
echo "Binary: $STAGE1_BIN"
echo ""

# Assemble DOS library
echo -n "Assembling DOS library (src/dos_lib.s) ... "
if gcc -c -m32 -masm=intel -o build_linux/dos_lib.o src/dos_lib.s; then
    objcopy -O pe-i386 build_linux/dos_lib.o build_linux/dos_lib.o
    # Compile dos_libc.c using stage1 (stage0 segfaults on this file)
    echo "Building dos_libc.o..."
    "$STAGE1_BIN" src/dos_libc.c -c -o build_linux/dos_libc.o --target=dos
    echo "OK"
else
    echo "FAIL"
    exit 1
fi
echo ""

# Quick smoke test
echo -n "Smoke test (01_return.c) ... "
if "$STAGE1_BIN" tests/01_return.c -S 2>/dev/null && \
   as -o /tmp/_stage1_smoke.o tests/01_return.s 2>/dev/null && \
   gcc -o /tmp/_stage1_smoke /tmp/_stage1_smoke.o 2>/dev/null; then
    /tmp/_stage1_smoke && rc=$? || rc=$?
    if [ "$rc" -eq 42 ]; then
        echo "PASS (exit=42)"
    else
        echo "FAIL (exit=$rc, expected 42)"
        exit 1
    fi
else
    echo "FAIL (compile/link error)"
    exit 1
fi

echo ""
echo "Stage-1 compiler is functional."
