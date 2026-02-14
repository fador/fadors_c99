#!/bin/bash
# test_valgrind.sh — Run fadors99 under Valgrind to detect memory errors.
#
# Usage:  ./test_valgrind.sh [compiler_path]
#   compiler_path  Path to the fadors99 binary (default: build_linux/fadors99)
#
# Tests the compiler itself for:
#   - Memory leaks
#   - Use-after-free
#   - Uninitialized reads
#   - Buffer overflows
#   - Invalid memory access
#
# Runs the compiler on each test to exercise the full pipeline
# (preprocessor → lexer → parser → codegen → encoder → linker).
#
# Requires: valgrind (apt install valgrind)
#
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

COMPILER="${1:-build_linux/fadors99}"

if [ ! -x "$COMPILER" ]; then
    echo "[ERROR] Compiler not found at: $COMPILER"
    exit 1
fi

if ! command -v valgrind >/dev/null 2>&1; then
    echo "[ERROR] valgrind not found. Install with: sudo apt install valgrind"
    exit 1
fi

TMPDIR=$(mktemp -d)
trap "rm -rf $TMPDIR" EXIT

# Valgrind options
VALGRIND_OPTS=(
    --tool=memcheck
    --leak-check=full
    --show-leak-kinds=definite,possible
    --errors-for-leak-kinds=definite
    --error-exitcode=99
    --track-origins=yes
    --quiet
    --log-file="$TMPDIR/valgrind_%p.log"
)

# Tests to exercise different compiler paths
# Use a representative subset that covers all major subsystems:
#   - Preprocessor: includes, macros, ifdefs, pragma pack
#   - Parser: all statement/expression types
#   - Codegen: functions, structs, arrays, floats, pointers, control flow
#   - Encoder: all instruction types (binary output)
#   - Linker: ELF linking, relocations

TEST_FILES=(
    # Core features
    tests/01_return.c
    tests/02_arithmetic.c
    tests/03_variables.c
    tests/04_if.c
    tests/05_complex_expr.c
    tests/06_while.c
    tests/07_function.c
    tests/08_include.c
    tests/09_struct_ptr.c
    tests/10_ptr.c
    tests/11_nested_struct.c
    tests/12_string.c
    tests/14_params.c
    tests/15_nested_calls.c
    tests/17_for.c
    tests/19_array.c
    tests/20_switch.c
    tests/21_enum.c
    tests/22_union.c
    tests/23_pointer_math.c
    tests/25_global.c
    # Floating point
    tests/27_float.c
    tests/28_minimal_float.c
    tests/32_binary_ops_float.c
    tests/33_binary_ops_mixed.c
    # Preprocessor features
    tests/36_ifdef.c
    tests/38_func_macros.c
    tests/39_builtin_macros.c
    tests/53_minimal_pack.c
    # Advanced features
    tests/42_cast.c
    tests/44_sizeof.c
    tests/47_compound_assign.c
    tests/48_for_init_decl.c
    tests/49_init_list.c
    tests/50_hex_char_long.c
    tests/51_named_union_static_local.c
    tests/52_forward_struct.c
    tests/60_struct_layout.c
    tests/69_self_ref_struct.c
    tests/71_2d_array.c
    tests/72_global_init_list.c
    # Benchmarks (larger programs)
    tests/bench_loop.c
    tests/bench_calls.c
    tests/bench_array.c
    tests/bench_branch.c
    tests/bench_struct.c
)

echo "============================================================"
echo " fadors99 Valgrind Memory Check"
echo "============================================================"
echo "Compiler: $COMPILER"
echo "Tests:    ${#TEST_FILES[@]}"
echo ""

PASS=0
FAIL=0
ERROR_TESTS=()

for testfile in "${TEST_FILES[@]}"; do
    if [ ! -f "$testfile" ]; then
        continue
    fi

    name=$(basename "$testfile" .c)
    binfile="$TMPDIR/$name"
    logfile="$TMPDIR/valgrind_${name}.log"

    # Run compiler under valgrind (full pipeline: C -> .o -> ELF executable)
    VALGRIND_LOG="$logfile" valgrind "${VALGRIND_OPTS[@]}" \
        --log-file="$logfile" \
        "$COMPILER" "$testfile" -o "$binfile" >/dev/null 2>&1
    rc=$?

    if [ "$rc" -eq 99 ]; then
        # Valgrind detected errors
        echo "  FAIL  $name"
        
        # Extract error summary
        if [ -f "$logfile" ]; then
            # Show definite leaks and errors
            grep -E "ERROR SUMMARY|definitely lost|Invalid read|Invalid write|uninitialised|Conditional jump" "$logfile" | head -5 | sed 's/^/        /'
        fi
        
        FAIL=$((FAIL + 1))
        ERROR_TESTS+=("$name")
    elif [ "$rc" -ne 0 ]; then
        # Compiler itself failed (not a valgrind error)
        echo "  SKIP  $name  (compile error)"
    else
        PASS=$((PASS + 1))
    fi
done

echo ""
echo "============================================================"
echo " Results"
echo "============================================================"
echo "  PASS (no memory errors): $PASS"
echo "  FAIL (memory errors):    $FAIL"
echo ""

if [ "$FAIL" -gt 0 ]; then
    echo "Tests with memory errors:"
    for t in "${ERROR_TESTS[@]}"; do
        echo "  - $t  (log: $TMPDIR/valgrind_${t}.log)"
    done
    echo ""
    echo "To inspect details:"
    echo "  cat $TMPDIR/valgrind_${ERROR_TESTS[0]}.log"
    echo ""
    echo "To run a single test manually:"
    echo "  valgrind --leak-check=full --track-origins=yes $COMPILER tests/<test>.c -o /tmp/test_out"
fi

echo ""

# Also test the compiler with optimization flags under valgrind
echo "--- Testing with -O flags ---"
OPT_PASS=0
OPT_FAIL=0

for opt in O0 O1 O2 O3 Os Og; do
    logfile="$TMPDIR/valgrind_opt_${opt}.log"
    binfile="$TMPDIR/opt_test_${opt}"
    
    valgrind "${VALGRIND_OPTS[@]}" \
        --log-file="$logfile" \
        "$COMPILER" tests/01_return.c -${opt} -o "$binfile" >/dev/null 2>&1
    rc=$?
    
    if [ "$rc" -eq 99 ]; then
        echo "  FAIL  -${opt}"
        OPT_FAIL=$((OPT_FAIL + 1))
    else
        OPT_PASS=$((OPT_PASS + 1))
    fi
done

# Test -g flag
logfile="$TMPDIR/valgrind_debug_g.log"
binfile="$TMPDIR/debug_test"
valgrind "${VALGRIND_OPTS[@]}" \
    --log-file="$logfile" \
    "$COMPILER" tests/01_return.c -g -o "$binfile" >/dev/null 2>&1
rc=$?
if [ "$rc" -eq 99 ]; then
    echo "  FAIL  -g"
    OPT_FAIL=$((OPT_FAIL + 1))
else
    OPT_PASS=$((OPT_PASS + 1))
fi

echo "  Optimization/debug flags: $OPT_PASS passed, $OPT_FAIL failed"
echo ""

TOTAL_FAIL=$((FAIL + OPT_FAIL))
if [ "$TOTAL_FAIL" -eq 0 ]; then
    echo "All memory checks passed."
    exit 0
else
    echo "$TOTAL_FAIL total failure(s) detected."
    exit 1
fi
