#!/bin/bash
# bench.sh — Benchmark runner for fadors99 optimization testing.
#
# Usage:  ./bench.sh [compiler_path]
#   compiler_path  Path to the fadors99 binary (default: build_linux/fadors99)
#
# Compiles and runs each tests/bench_*.c at each optimization level (-O0 through -O3)
# to measure the impact of optimizations on output binary performance.
#
# Also compares against GCC at -O0 and -O2 for reference.
#
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

COMPILER="${1:-build_linux/fadors99}"

if [ ! -x "$COMPILER" ]; then
    echo "[ERROR] Compiler not found at: $COMPILER"
    exit 1
fi

TMPDIR=$(mktemp -d)
trap "rm -rf $TMPDIR" EXIT

BENCH_FILES=(tests/bench_*.c)

if [ ${#BENCH_FILES[@]} -eq 0 ]; then
    echo "[ERROR] No benchmark files found (tests/bench_*.c)"
    exit 1
fi

# Number of repetitions for timing stability
REPS=${REPS:-3}

# Time a single binary execution, output time in seconds (best of $REPS runs)
time_binary() {
    local binary="$1"
    local best=""
    for ((r = 0; r < REPS; r++)); do
        # Use /usr/bin/time for portability (GNU time gives wall-clock)
        local t
        t=$( { time "$binary" >/dev/null 2>&1; } 2>&1 | grep real | sed 's/real\t//' )
        # Convert NmN.NNNs to seconds
        local mins secs total
        mins=$(echo "$t" | sed 's/m.*//')
        secs=$(echo "$t" | sed 's/.*m//' | sed 's/s$//')
        total=$(echo "$mins * 60 + $secs" | bc -l 2>/dev/null || echo "0")
        if [ -z "$best" ] || [ "$(echo "$total < $best" | bc -l 2>/dev/null)" = "1" ]; then
            best="$total"
        fi
    done
    echo "$best"
}

SEP="$(printf '%.0s-' {1..80})"

echo "============================================================"
echo " fadors99 Benchmark Suite"
echo "============================================================"
echo "Compiler:    $COMPILER"
echo "Repetitions: $REPS (best of)"
echo "Benchmarks:  ${#BENCH_FILES[@]} files"
echo ""

# Header
printf "%-25s %10s %10s %10s %10s | %10s %10s\n" \
       "Benchmark" "-O0" "-O1" "-O2" "-O3" "gcc -O0" "gcc -O2"
echo "$SEP"

TOTAL_PASS=0
TOTAL_FAIL=0

for bench in "${BENCH_FILES[@]}"; do
    name=$(basename "$bench" .c)
    
    # Compile with fadors99 at each optimization level
    declare -A times
    fadors_ok=1
    
    for opt in O0 O1 O2 O3; do
        binfile="$TMPDIR/${name}_${opt}"
        if "$COMPILER" "$bench" -${opt} -o "$binfile" >/dev/null 2>&1 && [ -f "$binfile" ]; then
            times[$opt]=$(time_binary "$binfile")
        else
            times[$opt]="FAIL"
            fadors_ok=0
        fi
    done
    
    # GCC reference (if available)
    gcc_o0_time="N/A"
    gcc_o2_time="N/A"
    if command -v gcc >/dev/null 2>&1; then
        gcc_bin_o0="$TMPDIR/${name}_gcc_O0"
        gcc_bin_o2="$TMPDIR/${name}_gcc_O2"
        if gcc -O0 -o "$gcc_bin_o0" "$bench" 2>/dev/null; then
            gcc_o0_time=$(time_binary "$gcc_bin_o0")
        fi
        if gcc -O2 -o "$gcc_bin_o2" "$bench" 2>/dev/null; then
            gcc_o2_time=$(time_binary "$gcc_bin_o2")
        fi
    fi
    
    # Print results
    printf "%-25s %10s %10s %10s %10s | %10s %10s\n" \
           "$name" \
           "${times[O0]}s" "${times[O1]}s" "${times[O2]}s" "${times[O3]}s" \
           "${gcc_o0_time}s" "${gcc_o2_time}s"
    
    if [ "$fadors_ok" -eq 1 ]; then
        TOTAL_PASS=$((TOTAL_PASS + 1))
    else
        TOTAL_FAIL=$((TOTAL_FAIL + 1))
    fi
    
    unset times
done

echo "$SEP"
echo ""
echo "Compiled: $TOTAL_PASS passed, $TOTAL_FAIL failed"
echo ""

# Summary note
echo "Note: -O1 through -O3 are not yet implemented — all levels currently"
echo "      produce identical output. This baseline allows measuring future"
echo "      optimization improvements."
echo ""
echo "To compare, also compile with GCC/Clang and time the output:"
echo "  gcc -O0 -o bench_loop_gcc tests/bench_loop.c && time ./bench_loop_gcc"
echo "  gcc -O2 -o bench_loop_gcc tests/bench_loop.c && time ./bench_loop_gcc"

exit $TOTAL_FAIL
