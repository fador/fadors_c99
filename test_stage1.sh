#!/bin/bash
# test_stage1.sh — Run the test suite through the stage-1 (self-compiled) compiler
#                  using the built-in linker (full pipeline: C -> binary .o -> internal ELF linker).
#
# Usage:  ./test_stage1.sh [stage1_path]
#   stage1_path  Path to the stage-1 compiler binary.
#                Defaults to build_linux/fadors99_stage1
#
# Exit code:
#   0  All tests match expected results
#   1  One or more regressions detected
#
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

COMPILER="${COMPILER:-${1:-build_linux/fadors99_stage1}}"

if [ ! -x "$COMPILER" ]; then
    echo "[ERROR] Stage-1 compiler not found at: $COMPILER"
    echo "        Run build_stage1.sh first."
    exit 1
fi

# Expected results from stage-0 compiler (test_name:expected_exit_code)
# Tests marked SKIP are known pre-existing failures or require external linkage.
# "COMPILE_ERR", "LINK_ERR" and specific exit codes are accepted.
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

# Tests skipped (Windows-only or require features not yet supported)
declare -A SKIP
SKIP=(
    [13_extern]=1
    [26_external]=1
)

TMPDIR=$(mktemp -d)
trap "rm -rf $TMPDIR" EXIT

echo "=== Stage 1 Test Suite (built-in linker) ==="
echo "Compiler: $COMPILER"
echo "Pipeline: C -> binary .o -> internal ELF linker (no external tools)"
echo ""

PASS=0
FAIL=0
SKIP_COUNT=0
TOTAL=0

for testfile in tests/*.c; do
    name=$(basename "$testfile" .c)
    TOTAL=$((TOTAL + 1))

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
    binfile="$TMPDIR/$name"

    # Full pipeline: compile + internal link
    if ! "$COMPILER" "$testfile" -o "$binfile" >/dev/null 2>&1; then
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

echo ""
echo "=== Results ==="
echo "  PASS:    $PASS"
echo "  FAIL:    $FAIL"
echo "  SKIP:    $SKIP_COUNT"
echo "  TOTAL:   $TOTAL"
echo ""

if [ "$FAIL" -eq 0 ]; then
    echo "All tests passed (excluding known skips)."
    exit 0
else
    echo "$FAIL test(s) FAILED."
    exit 1
fi
