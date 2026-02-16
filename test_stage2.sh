#!/bin/bash
# test_stage2.sh — Build stage-2 compilers from stage-1, then test each by
#                  having it compile the compiler at -O3 ("stage 3") and
#                  running the test suite against the result.
#
# For each -O level variant (O0, O1, O2, O3, Os, Og), this script:
#   0. Builds the stage-2 binary by compiling all sources with stage-1 at -O{level}
#   1. Compiles all 17 source files to assembly using the stage-2 binary with -O3
#   2. Assembles with GNU as and links with gcc to produce a stage-3 binary
#   3. Runs the full test suite against the stage-3 binary (full pipeline: -o)
#   4. Reports PASS/FAIL per test, per stage-2 variant, with timing and size data
#
# Usage:
#   ./test_stage2.sh                           # Build & test all variants
#   ./test_stage2.sh O0 O2                     # Build & test only O0 and O2
#   STAGE1=path ./test_stage2.sh               # Custom stage-1 compiler
#   STAGE2_DIR=path ./test_stage2.sh           # Custom stage-2 output directory
#   SKIP_BUILD=1 ./test_stage2.sh              # Skip build, use existing binaries
#
# Prerequisites:
#   - Stage-1 compiler built (build_linux/fadors99_stage1)
#   - GNU as and gcc available on PATH
#
# Exit code:
#   0  All variants pass all tests
#   1  One or more failures
#
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

STAGE1="${STAGE1:-build_linux/fadors99_stage1}"
STAGE2_DIR="${STAGE2_DIR:-build_linux}"
SKIP_BUILD="${SKIP_BUILD:-0}"
STAGE3_OPT="${STAGE3_OPT:--O3}"

# Source files (must match CMakeLists.txt / build_stage1.sh + pgo.c)
SOURCES=(
    src/buffer.c
    src/types.c
    src/ast.c
    src/lexer.c
    src/preprocessor.c
    src/parser.c
    src/codegen.c
    src/arch_x86_64.c
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

# All possible optimization variants
ALL_VARIANTS=(O0 O1 O2 O3 Os Og)

# Select variants: from command line args, or all
if [ $# -gt 0 ]; then
    VARIANTS=("$@")
else
    VARIANTS=("${ALL_VARIANTS[@]}")
fi

# Expected results (same as test_stage1.sh + test_linker.sh)
declare -A EXPECTED
EXPECTED=(
    [01_return]=42
    [02_arithmetic]=7
    [03_variables]=30
    [04_if]=100
    [05_complex_expr]=30
    [06_while]=10
    [07_function]=123
    [08_include]=57
    [09_struct_ptr]=10
    [10_ptr]=100
    [11_nested_struct]=10
    [12_string]=72
    [14_params]=10
    [15_nested_calls]=10
    [16_void_func]=123
    [17_for]=45
    [18_typedef]=42
    [19_array]=100
    [20_switch]=120
    [21_enum]=6
    [22_union]=42
    [23_pointer_math]=0
    [24_coff_test]=42
    [25_global]=52
    [28_minimal_float]=0
    [27_float]=42
    [29_float_unary_ret]=42
    [31_binary_ops_int]=42
    [32_binary_ops_float]=42
    [33_binary_ops_mixed]=42
    [34_precedence]=42
    [36_ifdef]=42
    [38_func_macros]=42
    [39_builtin_macros]=42
    [40_string_cmp]=42
    [41_inc_dec]=42
    [42_cast]=42
    [43_inc_dec_cond]=0
    [44_sizeof]=0
    [45_const_static]=0
    [46_system_includes]=0
    [47_compound_assign]=0
    [48_for_init_decl]=0
    [49_init_list]=0
    [50_hex_char_long]=0
    [51_named_union_static_local]=0
    [52_forward_struct]=0
    [53_pragma_pack]=0
    [53_minimal_pack]=0
    [54_minimal_string_escape]=0
    [54_string_escapes]=0
    [55_headers_test]=0
    [56_ptr_deref_inc]=0
    [57_ptr_deref_inc_array]=0
    [58_ptr_write_inc]=0
    [59_char_ptr_inc]=0
    [60_struct_layout]=0
    [61_ptr_struct_field]=0
    [62_union_in_struct]=4
    [63_array_index]=30
    [64_struct_offsets]=0
    [65_chained_ptr]=4
    [66_type_struct]=0
    [67_chained_member]=0
    [68_union_member_chain]=0
    [69_self_ref_struct]=0
    [70_minimal_switch]=20
    [71_2d_array]=0
    [72_global_init_list]=0
    [73_int64_arith]=42
    [74_reloc_arith]=42
    [75_reloc_test]=42
    [test_ternary]=42
)

# Tests to skip
declare -A SKIP
SKIP=(
    [13_extern]=1
    [26_external]=1
)

TMPDIR=$(mktemp -d)
trap "rm -rf $TMPDIR" EXIT

echo "============================================"
echo "  Stage-2 → Stage-3 Full Test Suite"
echo "============================================"
echo "Stage-1 compiler: $STAGE1"
echo "Stage-2 dir: $STAGE2_DIR"
echo "Stage-3 compile flags: $STAGE3_OPT"
echo "Variants: ${VARIANTS[*]}"
echo ""

# ============================================
# Phase 0: Build stage-2 binaries from stage-1
# ============================================
if [ "$SKIP_BUILD" -eq 0 ]; then
    if [ ! -x "$STAGE1" ]; then
        echo "[ERROR] Stage-1 compiler not found at: $STAGE1"
        echo "        Build it first with: ./build_stage1.sh"
        exit 1
    fi

    echo "============================================"
    echo "  Phase 0: Building Stage-2 Binaries"
    echo "============================================"
    echo ""

    for VARIANT in "${VARIANTS[@]}"; do
        # Map variant to compiler flag
        case "$VARIANT" in
            O0) OPT_FLAG="-O0" ;;
            O1) OPT_FLAG="-O1" ;;
            O2) OPT_FLAG="-O2" ;;
            O3) OPT_FLAG="-O3" ;;
            Os) OPT_FLAG="-Os" ;;
            Og) OPT_FLAG="-Og" ;;
            *)  echo "  Unknown variant: $VARIANT, skipping"; continue ;;
        esac

        STAGE2_BIN="$STAGE2_DIR/fadors99_stage2_$VARIANT"
        STAGE2_ASM_DIR="$TMPDIR/stage2_asm_$VARIANT"
        mkdir -p "$STAGE2_ASM_DIR"

        echo "--- Building stage-2 -$VARIANT (stage-1 $OPT_FLAG) ---"

        BUILD_START=$(date +%s%N)
        OBJECTS=""
        S2_BUILD_FAIL=0
        for src in "${SOURCES[@]}"; do
            name=$(basename "$src" .c)
            asm_file="$STAGE2_ASM_DIR/${name}.s"
            obj_file="$STAGE2_ASM_DIR/${name}.o"

            if ! "$STAGE1" "$src" -S $OPT_FLAG 2>/dev/null; then
                echo "  FAIL  compile $src (stage-1 $OPT_FLAG error)"
                S2_BUILD_FAIL=1
                continue
            fi

            src_s="src/${name}.s"
            if [ -f "$src_s" ]; then
                mv "$src_s" "$asm_file"
            else
                echo "  FAIL  compile $src (no .s output)"
                S2_BUILD_FAIL=1
                continue
            fi

            if ! as -o "$obj_file" "$asm_file" 2>/dev/null; then
                echo "  FAIL  assemble $name.s"
                S2_BUILD_FAIL=1
                continue
            fi

            OBJECTS="$OBJECTS $obj_file"
        done
        BUILD_END=$(date +%s%N)
        BUILD_MS=$(( (BUILD_END - BUILD_START) / 1000000 ))
        BUILD_S=$(echo "scale=3; $BUILD_MS / 1000" | bc)

        if [ "$S2_BUILD_FAIL" -ne 0 ]; then
            echo "  Stage-2 -$VARIANT build FAILED (compile/assemble errors)"
            echo ""
            continue
        fi

        if ! gcc -o "$STAGE2_BIN" $OBJECTS -lc -no-pie 2>/dev/null; then
            echo "  Stage-2 -$VARIANT build FAILED (link error)"
            echo ""
            continue
        fi

        S2_SIZE=$(stat -c%s "$STAGE2_BIN" 2>/dev/null || echo 0)
        S2_SIZE_KB=$(( (S2_SIZE + 512) / 1024 ))
        echo "  Built: $STAGE2_BIN (${S2_SIZE_KB} KB) in ${BUILD_S}s"
        echo ""
    done

    echo "============================================"
    echo "  Phase 0 Complete — Starting Tests"
    echo "============================================"
    echo ""
else
    echo "(Skipping stage-2 build — SKIP_BUILD=1)"
    echo ""
fi

TOTAL_VARIANTS=0
PASSED_VARIANTS=0
FAILED_VARIANTS=0
SKIPPED_VARIANTS=0

# Arrays for the summary table
declare -a SUMMARY_VARIANT
declare -a SUMMARY_STAGE2_SIZE
declare -a SUMMARY_COMPILE_TIME
declare -a SUMMARY_ASM_SIZE
declare -a SUMMARY_STAGE3_SIZE
declare -a SUMMARY_TESTS
declare -a SUMMARY_STATUS
SUMMARY_IDX=0

for VARIANT in "${VARIANTS[@]}"; do
    # Map variant name to stage-2 binary path
    STAGE2_BIN="$STAGE2_DIR/fadors99_stage2_$VARIANT"

    if [ ! -x "$STAGE2_BIN" ]; then
        echo "--- Stage-2 -$VARIANT: SKIP (binary not found: $STAGE2_BIN) ---"
        echo ""
        SKIPPED_VARIANTS=$((SKIPPED_VARIANTS + 1))
        continue
    fi

    TOTAL_VARIANTS=$((TOTAL_VARIANTS + 1))

    STAGE3_DIR="$TMPDIR/stage3_$VARIANT"
    STAGE3_BIN="$TMPDIR/fadors99_stage3_$VARIANT"
    mkdir -p "$STAGE3_DIR"

    echo "--- Stage-2 -$VARIANT → Stage-3 (compile with $STAGE3_OPT) ---"
    echo "  Stage-2 binary: $STAGE2_BIN"

    # Measure stage-2 binary size
    STAGE2_SIZE=$(stat -c%s "$STAGE2_BIN" 2>/dev/null || echo 0)
    STAGE2_SIZE_KB=$(( (STAGE2_SIZE + 512) / 1024 ))
    echo "  Stage-2 binary size: ${STAGE2_SIZE_KB} KB ($STAGE2_SIZE bytes)"

    # Step 1: Compile each source file to assembly using stage-2 at -O3
    COMPILE_START=$(date +%s%N)
    OBJECTS=""
    BUILD_FAIL=0
    for src in "${SOURCES[@]}"; do
        name=$(basename "$src" .c)
        asm_file="$STAGE3_DIR/${name}.s"
        obj_file="$STAGE3_DIR/${name}.o"

        if ! "$STAGE2_BIN" "$src" -S $STAGE3_OPT 2>/dev/null; then
            echo "  FAIL  compile $src (stage-2 -$VARIANT compiler error)"
            BUILD_FAIL=1
            continue
        fi

        # The compiler outputs .s next to the source; move it
        src_s="src/${name}.s"
        if [ -f "$src_s" ]; then
            mv "$src_s" "$asm_file"
        else
            echo "  FAIL  compile $src (no .s output)"
            BUILD_FAIL=1
            continue
        fi

        if ! as -o "$obj_file" "$asm_file" 2>/dev/null; then
            echo "  FAIL  assemble $name.s (assembler error)"
            BUILD_FAIL=1
            continue
        fi

        OBJECTS="$OBJECTS $obj_file"
    done

    COMPILE_END=$(date +%s%N)

    if [ "$BUILD_FAIL" -ne 0 ]; then
        echo "  Stage-3 build FAILED for -$VARIANT (compile/assemble errors)"
        echo ""
        FAILED_VARIANTS=$((FAILED_VARIANTS + 1))
        SUMMARY_VARIANT[$SUMMARY_IDX]="$VARIANT"
        SUMMARY_STAGE2_SIZE[$SUMMARY_IDX]="$STAGE2_SIZE"
        SUMMARY_COMPILE_TIME[$SUMMARY_IDX]="-"
        SUMMARY_ASM_SIZE[$SUMMARY_IDX]="-"
        SUMMARY_STAGE3_SIZE[$SUMMARY_IDX]="-"
        SUMMARY_TESTS[$SUMMARY_IDX]="BUILD FAIL"
        SUMMARY_STATUS[$SUMMARY_IDX]="FAIL"
        SUMMARY_IDX=$((SUMMARY_IDX + 1))
        continue
    fi

    # Measure compile time (compilation + assembly of all sources)
    COMPILE_MS=$(( (COMPILE_END - COMPILE_START) / 1000000 ))
    COMPILE_S=$(echo "scale=3; $COMPILE_MS / 1000" | bc)
    echo "  Compile time:  ${COMPILE_S}s (${COMPILE_MS}ms for ${#SOURCES[@]} files)"

    # Measure total assembly output size
    TOTAL_ASM_SIZE=0
    for src in "${SOURCES[@]}"; do
        name=$(basename "$src" .c)
        asm_file="$STAGE3_DIR/${name}.s"
        if [ -f "$asm_file" ]; then
            sz=$(stat -c%s "$asm_file" 2>/dev/null || echo 0)
            TOTAL_ASM_SIZE=$((TOTAL_ASM_SIZE + sz))
        fi
    done
    TOTAL_ASM_KB=$(( (TOTAL_ASM_SIZE + 512) / 1024 ))
    echo "  Total asm size: ${TOTAL_ASM_KB} KB ($TOTAL_ASM_SIZE bytes)"

    # Measure compile speed in lines-per-second (source lines)
    TOTAL_SRC_LINES=0
    for src in "${SOURCES[@]}"; do
        lines=$(wc -l < "$src" 2>/dev/null || echo 0)
        TOTAL_SRC_LINES=$((TOTAL_SRC_LINES + lines))
    done
    if [ "$COMPILE_MS" -gt 0 ]; then
        LINES_PER_SEC=$(( TOTAL_SRC_LINES * 1000 / COMPILE_MS ))
        echo "  Compile speed:  $LINES_PER_SEC lines/s ($TOTAL_SRC_LINES source lines)"
    fi

    # Step 2: Link into stage-3 binary
    LINK_START=$(date +%s%N)
    if ! gcc -o "$STAGE3_BIN" $OBJECTS -lc -no-pie 2>/dev/null; then
        echo "  Stage-3 build FAILED for -$VARIANT (link error)"
        echo ""
        FAILED_VARIANTS=$((FAILED_VARIANTS + 1))
        SUMMARY_VARIANT[$SUMMARY_IDX]="$VARIANT"
        SUMMARY_STAGE2_SIZE[$SUMMARY_IDX]="$STAGE2_SIZE"
        SUMMARY_COMPILE_TIME[$SUMMARY_IDX]="${COMPILE_S}s"
        SUMMARY_ASM_SIZE[$SUMMARY_IDX]="$TOTAL_ASM_SIZE"
        SUMMARY_STAGE3_SIZE[$SUMMARY_IDX]="-"
        SUMMARY_TESTS[$SUMMARY_IDX]="LINK FAIL"
        SUMMARY_STATUS[$SUMMARY_IDX]="FAIL"
        SUMMARY_IDX=$((SUMMARY_IDX + 1))
        continue
    fi
    LINK_END=$(date +%s%N)
    LINK_MS=$(( (LINK_END - LINK_START) / 1000000 ))
    LINK_S=$(echo "scale=3; $LINK_MS / 1000" | bc)

    # Measure stage-3 binary size
    STAGE3_SIZE=$(stat -c%s "$STAGE3_BIN" 2>/dev/null || echo 0)
    STAGE3_SIZE_KB=$(( (STAGE3_SIZE + 512) / 1024 ))
    echo "  Link time:     ${LINK_S}s"
    echo "  Stage-3 binary size: ${STAGE3_SIZE_KB} KB ($STAGE3_SIZE bytes)"
    echo "  Stage-3 binary: $STAGE3_BIN"

    # Step 3: Smoke test
    SMOKE_BIN="$TMPDIR/smoke_$VARIANT"
    if ! "$STAGE3_BIN" tests/01_return.c -o "$SMOKE_BIN" >/dev/null 2>&1; then
        echo "  Smoke test FAILED (compile/link error)"
        FAILED_VARIANTS=$((FAILED_VARIANTS + 1))
        echo ""
        continue
    fi
    "$SMOKE_BIN" >/dev/null 2>&1
    smoke_rc=$?
    if [ "$smoke_rc" -ne 42 ]; then
        echo "  Smoke test FAILED (exit=$smoke_rc, expected 42)"
        FAILED_VARIANTS=$((FAILED_VARIANTS + 1))
        echo ""
        continue
    fi

    # Step 4: Run full test suite against stage-3 binary (using built-in linker: -o)
    PASS=0
    FAIL=0
    SKIP_COUNT=0
    TEST_TOTAL=0

    for testfile in tests/[0-9]*.c tests/test_*.c; do
        [ -f "$testfile" ] || continue
        name=$(basename "$testfile" .c)
        TEST_TOTAL=$((TEST_TOTAL + 1))

        # Skip known failures
        if [[ -n "${SKIP[$name]:-}" ]]; then
            SKIP_COUNT=$((SKIP_COUNT + 1))
            continue
        fi

        # Skip if no expected result
        if [[ -z "${EXPECTED[$name]:-}" ]]; then
            SKIP_COUNT=$((SKIP_COUNT + 1))
            continue
        fi

        expected="${EXPECTED[$name]}"
        binfile="$TMPDIR/test_${VARIANT}_${name}"

        # Full pipeline: compile + internal link
        if ! "$STAGE3_BIN" "$testfile" -o "$binfile" >/dev/null 2>&1; then
            echo "  FAIL  $name  (compile/link error, expected exit=$expected)"
            FAIL=$((FAIL + 1))
            continue
        fi

        if [ ! -f "$binfile" ]; then
            echo "  FAIL  $name  (no binary generated, expected exit=$expected)"
            FAIL=$((FAIL + 1))
            continue
        fi

        # Run
        "$binfile" >/dev/null 2>&1
        actual=$?

        if [ "$actual" -eq "$expected" ]; then
            PASS=$((PASS + 1))
        else
            echo "  FAIL  $name  (exit=$actual, expected=$expected)"
            FAIL=$((FAIL + 1))
        fi
    done

    echo "  Results: $PASS pass, $FAIL fail, $SKIP_COUNT skip (of $TEST_TOTAL)"

    # Record summary data
    SUMMARY_VARIANT[$SUMMARY_IDX]="$VARIANT"
    SUMMARY_STAGE2_SIZE[$SUMMARY_IDX]="$STAGE2_SIZE"
    SUMMARY_COMPILE_TIME[$SUMMARY_IDX]="${COMPILE_S}s"
    SUMMARY_ASM_SIZE[$SUMMARY_IDX]="$TOTAL_ASM_SIZE"
    SUMMARY_STAGE3_SIZE[$SUMMARY_IDX]="$STAGE3_SIZE"

    if [ "$FAIL" -eq 0 ]; then
        echo "  Stage-3 from -$VARIANT: ALL PASSED"
        PASSED_VARIANTS=$((PASSED_VARIANTS + 1))
        SUMMARY_TESTS[$SUMMARY_IDX]="$PASS/$((PASS+FAIL)) pass"
        SUMMARY_STATUS[$SUMMARY_IDX]="PASS"
    else
        echo "  Stage-3 from -$VARIANT: $FAIL FAILURE(S)"
        FAILED_VARIANTS=$((FAILED_VARIANTS + 1))
        SUMMARY_TESTS[$SUMMARY_IDX]="$PASS/$((PASS+FAIL)) pass"
        SUMMARY_STATUS[$SUMMARY_IDX]="FAIL"
    fi
    SUMMARY_IDX=$((SUMMARY_IDX + 1))
    echo ""
done

echo "============================================"
echo "  Summary"
echo "============================================"
echo "  Variants tested:  $TOTAL_VARIANTS"
echo "  Variants passed:  $PASSED_VARIANTS"
echo "  Variants failed:  $FAILED_VARIANTS"
echo "  Variants skipped: $SKIPPED_VARIANTS"
echo ""

# Print comparison table
if [ "$SUMMARY_IDX" -gt 0 ]; then
    echo "============================================"
    echo "  Performance & Size Comparison"
    echo "============================================"
    printf "  %-8s  %10s  %10s  %10s  %10s  %14s  %6s\n" \
           "Variant" "Stage2" "CompTime" "Asm Size" "Stage3" "Tests" "Status"
    printf "  %-8s  %10s  %10s  %10s  %10s  %14s  %6s\n" \
           "-------" "----------" "----------" "----------" "----------" "--------------" "------"
    for ((i=0; i<SUMMARY_IDX; i++)); do
        # Format sizes
        s2="${SUMMARY_STAGE2_SIZE[$i]}"
        s3="${SUMMARY_STAGE3_SIZE[$i]}"
        asm="${SUMMARY_ASM_SIZE[$i]}"
        if [ "$s2" != "-" ] && [ "$s2" -gt 0 ] 2>/dev/null; then
            s2_fmt="$(( (s2 + 512) / 1024 )) KB"
        else
            s2_fmt="$s2"
        fi
        if [ "$s3" != "-" ] && [ "$s3" -gt 0 ] 2>/dev/null; then
            s3_fmt="$(( (s3 + 512) / 1024 )) KB"
        else
            s3_fmt="$s3"
        fi
        if [ "$asm" != "-" ] && [ "$asm" -gt 0 ] 2>/dev/null; then
            asm_fmt="$(( (asm + 512) / 1024 )) KB"
        else
            asm_fmt="$asm"
        fi
        printf "  %-8s  %10s  %10s  %10s  %10s  %14s  %6s\n" \
               "-${SUMMARY_VARIANT[$i]}" \
               "$s2_fmt" \
               "${SUMMARY_COMPILE_TIME[$i]}" \
               "$asm_fmt" \
               "$s3_fmt" \
               "${SUMMARY_TESTS[$i]}" \
               "${SUMMARY_STATUS[$i]}"
    done
    echo ""
fi

if [ "$FAILED_VARIANTS" -eq 0 ] && [ "$TOTAL_VARIANTS" -gt 0 ]; then
    echo "All stage-2 variants produce correct stage-3 compilers."
    exit 0
elif [ "$TOTAL_VARIANTS" -eq 0 ]; then
    echo "No stage-2 binaries found. Build them first."
    exit 1
else
    echo "$FAILED_VARIANTS variant(s) FAILED."
    exit 1
fi
