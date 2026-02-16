#!/bin/bash
# test_stage3_stage4.sh — Bootstrap verification: build stage-3 and stage-4,
#                          compare their assembly outputs to verify fixed-point.
#
# For each working stage-2 variant (O0, O1, Og), this script:
#   1. Compiles all 17 source files with stage-2 at -O0 to get stage-3 assembly
#   2. Assembles + links stage-3 binary
#   3. Runs test suite against stage-3
#   4. Compiles all 17 source files with stage-3 at -O0 to get stage-4 assembly
#   5. Assembles + links stage-4 binary
#   6. Runs test suite against stage-4
#   7. Compares stage-3 and stage-4 assembly outputs (should be identical = fixed-point)
#
# A matching stage-3/stage-4 proves the compiler has reached a bootstrap fixed point.
#
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

STAGE2_DIR="${STAGE2_DIR:-build_linux}"

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

# Expected results
declare -A EXPECTED
EXPECTED=(
    [01_return]=42 [02_arithmetic]=7 [03_variables]=30 [04_if]=100
    [05_complex_expr]=30 [06_while]=10 [07_function]=123 [08_include]=57
    [09_struct_ptr]=10 [10_ptr]=100 [11_nested_struct]=10 [12_string]=72
    [14_params]=10 [15_nested_calls]=10 [16_void_func]=123 [17_for]=45
    [18_typedef]=42 [19_array]=100 [20_switch]=120 [21_enum]=6
    [22_union]=42 [23_pointer_math]=0 [24_coff_test]=42 [25_global]=52
    [28_minimal_float]=0 [27_float]=42 [29_float_unary_ret]=42
    [31_binary_ops_int]=42 [32_binary_ops_float]=42 [33_binary_ops_mixed]=42
    [34_precedence]=42 [36_ifdef]=42 [38_func_macros]=42 [39_builtin_macros]=42
    [40_string_cmp]=42 [41_inc_dec]=42 [42_cast]=42 [43_inc_dec_cond]=0
    [44_sizeof]=0 [45_const_static]=0 [46_system_includes]=0
    [47_compound_assign]=0 [48_for_init_decl]=0 [49_init_list]=0
    [50_hex_char_long]=0 [51_named_union_static_local]=0 [52_forward_struct]=0
    [53_pragma_pack]=0 [53_minimal_pack]=0 [54_minimal_string_escape]=0
    [54_string_escapes]=0 [55_headers_test]=0 [56_ptr_deref_inc]=0
    [57_ptr_deref_inc_array]=0 [58_ptr_write_inc]=0 [59_char_ptr_inc]=0
    [60_struct_layout]=0 [61_ptr_struct_field]=0 [62_union_in_struct]=4
    [63_array_index]=30 [64_struct_offsets]=0 [65_chained_ptr]=4
    [66_type_struct]=0 [67_chained_member]=0 [68_union_member_chain]=0
    [69_self_ref_struct]=0 [70_minimal_switch]=20 [71_2d_array]=0
    [72_global_init_list]=0 [73_int64_arith]=42 [74_reloc_arith]=42
    [75_reloc_test]=42 [test_ternary]=42
)

declare -A SKIP
SKIP=([13_extern]=1 [26_external]=1)

TMPDIR=$(mktemp -d)
trap "rm -rf $TMPDIR" EXIT

# Variants to test — only those that produce working stage-2 binaries
if [ $# -gt 0 ]; then
    VARIANTS=("$@")
else
    VARIANTS=(O0 O1 Og)
fi

echo "============================================"
echo "  Stage-3 / Stage-4 Bootstrap Verification"
echo "============================================"
echo "Variants: ${VARIANTS[*]}"
echo ""

TOTAL_VARIANTS=0
PASSED_VARIANTS=0
FAILED_VARIANTS=0

# Helper: build a compiler binary from source using a given compiler
# Usage: build_stage COMPILER OPT_FLAG ASM_DIR BIN_PATH STAGE_NAME
build_stage() {
    local compiler="$1"
    local opt_flag="$2"
    local asm_dir="$3"
    local bin_path="$4"
    local stage_name="$5"

    mkdir -p "$asm_dir"
    local objects=""
    local fail=0

    for src in "${SOURCES[@]}"; do
        local name=$(basename "$src" .c)
        local asm_file="$asm_dir/${name}.s"
        local obj_file="$asm_dir/${name}.o"

        if ! "$compiler" "$src" -S $opt_flag 2>/dev/null; then
            echo "    FAIL compile $src ($stage_name)"
            fail=1
            continue
        fi

        local src_s="src/${name}.s"
        if [ -f "$src_s" ]; then
            mv "$src_s" "$asm_file"
        else
            echo "    FAIL no .s output for $src ($stage_name)"
            fail=1
            continue
        fi

        if ! as -o "$obj_file" "$asm_file" 2>/dev/null; then
            echo "    FAIL assemble $name.s ($stage_name)"
            fail=1
            continue
        fi

        objects="$objects $obj_file"
    done

    if [ "$fail" -ne 0 ]; then
        return 1
    fi

    if ! gcc -o "$bin_path" $objects -lc -no-pie 2>/dev/null; then
        echo "    FAIL link $stage_name"
        return 1
    fi

    return 0
}

# Helper: run test suite against a compiler binary
# Usage: run_tests COMPILER STAGE_NAME
# Returns: sets TEST_PASS, TEST_FAIL
run_tests() {
    local compiler="$1"
    local stage_name="$2"
    TEST_PASS=0
    TEST_FAIL=0
    local test_skip=0

    for testfile in tests/[0-9]*.c tests/test_*.c; do
        [ -f "$testfile" ] || continue
        local name=$(basename "$testfile" .c)

        [[ -n "${SKIP[$name]:-}" ]] && { test_skip=$((test_skip+1)); continue; }
        [[ -z "${EXPECTED[$name]:-}" ]] && { test_skip=$((test_skip+1)); continue; }

        local expected="${EXPECTED[$name]}"
        local binfile="$TMPDIR/test_${stage_name}_${name}"

        if ! "$compiler" "$testfile" -o "$binfile" >/dev/null 2>&1; then
            echo "    FAIL  $name  (compile/link error, expected=$expected) [$stage_name]"
            TEST_FAIL=$((TEST_FAIL+1))
            continue
        fi

        "$binfile" >/dev/null 2>&1
        local actual=$?

        if [ "$actual" -eq "$expected" ]; then
            TEST_PASS=$((TEST_PASS+1))
        else
            echo "    FAIL  $name  (exit=$actual, expected=$expected) [$stage_name]"
            TEST_FAIL=$((TEST_FAIL+1))
        fi
    done

    echo "  $stage_name: $TEST_PASS pass, $TEST_FAIL fail, $test_skip skip"
}

for VARIANT in "${VARIANTS[@]}"; do
    STAGE2_BIN="$STAGE2_DIR/fadors99_stage2_$VARIANT"

    if [ ! -x "$STAGE2_BIN" ]; then
        echo "--- $VARIANT: SKIP (stage-2 binary not found: $STAGE2_BIN) ---"
        echo ""
        continue
    fi

    TOTAL_VARIANTS=$((TOTAL_VARIANTS + 1))

    STAGE3_ASM="$TMPDIR/stage3_asm_$VARIANT"
    STAGE3_BIN="$TMPDIR/fadors99_stage3_$VARIANT"
    STAGE4_ASM="$TMPDIR/stage4_asm_$VARIANT"
    STAGE4_BIN="$TMPDIR/fadors99_stage4_$VARIANT"

    echo "============================================"
    echo "  Variant: -$VARIANT"
    echo "============================================"
    echo "  Stage-2: $STAGE2_BIN"

    # --- Build Stage-3 (stage-2 compiles at -O0) ---
    echo ""
    echo "  Building Stage-3 (stage-2 -$VARIANT compiles at -O0)..."
    S3_START=$(date +%s%N)
    if ! build_stage "$STAGE2_BIN" "-O0" "$STAGE3_ASM" "$STAGE3_BIN" "stage3-$VARIANT"; then
        echo "  Stage-3 build FAILED for -$VARIANT"
        FAILED_VARIANTS=$((FAILED_VARIANTS + 1))
        echo ""
        continue
    fi
    S3_END=$(date +%s%N)
    S3_MS=$(( (S3_END - S3_START) / 1000000 ))
    S3_SIZE=$(stat -c%s "$STAGE3_BIN" 2>/dev/null || echo 0)
    echo "  Stage-3 built: $(( (S3_SIZE+512)/1024 )) KB in $(echo "scale=3; $S3_MS/1000" | bc)s"

    # --- Test Stage-3 ---
    echo ""
    echo "  Testing Stage-3..."
    run_tests "$STAGE3_BIN" "stage3-$VARIANT"
    S3_PASS=$TEST_PASS
    S3_FAIL=$TEST_FAIL

    if [ "$S3_FAIL" -ne 0 ]; then
        echo "  Stage-3 from -$VARIANT has failures — skipping stage-4"
        FAILED_VARIANTS=$((FAILED_VARIANTS + 1))
        echo ""
        continue
    fi

    # --- Build Stage-4 (stage-3 compiles at -O0) ---
    echo ""
    echo "  Building Stage-4 (stage-3 compiles at -O0)..."
    S4_START=$(date +%s%N)
    if ! build_stage "$STAGE3_BIN" "-O0" "$STAGE4_ASM" "$STAGE4_BIN" "stage4-$VARIANT"; then
        echo "  Stage-4 build FAILED for -$VARIANT"
        FAILED_VARIANTS=$((FAILED_VARIANTS + 1))
        echo ""
        continue
    fi
    S4_END=$(date +%s%N)
    S4_MS=$(( (S4_END - S4_START) / 1000000 ))
    S4_SIZE=$(stat -c%s "$STAGE4_BIN" 2>/dev/null || echo 0)
    echo "  Stage-4 built: $(( (S4_SIZE+512)/1024 )) KB in $(echo "scale=3; $S4_MS/1000" | bc)s"

    # --- Test Stage-4 ---
    echo ""
    echo "  Testing Stage-4..."
    run_tests "$STAGE4_BIN" "stage4-$VARIANT"
    S4_PASS=$TEST_PASS
    S4_FAIL=$TEST_FAIL

    # --- Compare Stage-3 and Stage-4 assembly ---
    echo ""
    echo "  Comparing Stage-3 vs Stage-4 assembly..."
    DIFF_COUNT=0
    MATCH_COUNT=0
    for src in "${SOURCES[@]}"; do
        name=$(basename "$src" .c)
        s3_asm="$STAGE3_ASM/${name}.s"
        s4_asm="$STAGE4_ASM/${name}.s"

        if [ ! -f "$s3_asm" ] || [ ! -f "$s4_asm" ]; then
            echo "    MISSING: ${name}.s"
            DIFF_COUNT=$((DIFF_COUNT + 1))
            continue
        fi

        if diff -q "$s3_asm" "$s4_asm" >/dev/null 2>&1; then
            MATCH_COUNT=$((MATCH_COUNT + 1))
        else
            DIFF_COUNT=$((DIFF_COUNT + 1))
            echo "    DIFF: ${name}.s"
            # Show first few differences
            diff --unified=3 "$s3_asm" "$s4_asm" | head -20
            echo "    ..."
        fi
    done

    echo ""
    echo "  Assembly comparison: $MATCH_COUNT/${#SOURCES[@]} identical, $DIFF_COUNT different"

    # --- Compare binary sizes ---
    echo "  Binary sizes: stage-3=$(( (S3_SIZE+512)/1024 ))KB  stage-4=$(( (S4_SIZE+512)/1024 ))KB"
    if [ "$S3_SIZE" -eq "$S4_SIZE" ]; then
        echo "  Binary sizes: MATCH"
    else
        echo "  Binary sizes: DIFFER (delta=$((S4_SIZE - S3_SIZE)) bytes)"
    fi

    # --- Verdict ---
    echo ""
    if [ "$DIFF_COUNT" -eq 0 ] && [ "$S3_FAIL" -eq 0 ] && [ "$S4_FAIL" -eq 0 ]; then
        echo "  *** -$VARIANT: FIXED POINT REACHED — Stage-3 and Stage-4 are IDENTICAL ***"
        PASSED_VARIANTS=$((PASSED_VARIANTS + 1))
    elif [ "$S3_FAIL" -eq 0 ] && [ "$S4_FAIL" -eq 0 ] && [ "$DIFF_COUNT" -gt 0 ]; then
        echo "  *** -$VARIANT: Both stages pass tests but assembly DIFFERS ($DIFF_COUNT files) ***"
        FAILED_VARIANTS=$((FAILED_VARIANTS + 1))
    else
        echo "  *** -$VARIANT: FAILED ($S3_FAIL stage-3 failures, $S4_FAIL stage-4 failures, $DIFF_COUNT asm diffs) ***"
        FAILED_VARIANTS=$((FAILED_VARIANTS + 1))
    fi
    echo ""
done

echo "============================================"
echo "  Final Summary"
echo "============================================"
echo "  Variants tested:  $TOTAL_VARIANTS"
echo "  Fixed point:      $PASSED_VARIANTS"
echo "  Failed:           $FAILED_VARIANTS"
echo ""

if [ "$FAILED_VARIANTS" -eq 0 ] && [ "$TOTAL_VARIANTS" -gt 0 ]; then
    echo "All tested variants reached bootstrap fixed point!"
    echo "Stage-3 and Stage-4 produce identical assembly."
    exit 0
elif [ "$TOTAL_VARIANTS" -eq 0 ]; then
    echo "No stage-2 binaries found."
    exit 1
else
    echo "$FAILED_VARIANTS variant(s) did not reach fixed point."
    exit 1
fi
