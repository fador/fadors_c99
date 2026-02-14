#!/usr/bin/env bash
# =======================================================
# Analysis Passes Test Suite  (liveness + loop detection)
# =======================================================
set -euo pipefail
CC="${CC:-build_linux/fadors99}"
PASS=0; FAIL=0

pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
check() {
    local label="$1" pattern="$2" text="$3"
    if echo "$text" | grep -qE "$pattern"; then pass "$label"; else fail "$label"; fi
}
check_not() {
    local label="$1" pattern="$2" text="$3"
    if echo "$text" | grep -qE "$pattern"; then fail "$label"; else pass "$label"; fi
}
run_exit() {
    local label="$1" src="$2" expect="$3"
    local out
    out=$("$CC" -O2 "$src" -o "${src%.c}" 2>&1) || { fail "$label (compile)"; return; }
    local got; got=0; "${src%.c}" && got=$? || got=$?
    if [ "$got" -eq "$expect" ]; then pass "$label"; else fail "$label (got $got, expected $expect)"; fi
}

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

echo "=== Analysis Passes Tests ==="

# ------------------------------------------------------------------
# 1. Liveness: straight-line (no live across blocks)
# ------------------------------------------------------------------
cat > "$TMP/t1.c" << 'EOF'
int main() {
    int a = 5;
    int b = 3;
    int c = a + b;
    return c;
}
EOF
IR=$("$CC" -O2 --dump-ir "$TMP/t1.c" -o "$TMP/t1" 2>&1)
check "t1: single block, no loop" "1 blocks" "$IR"
check_not "t1: no loop annotation" "loop:" "$IR"
run_exit "t1: correctness" "$TMP/t1.c" 8

# ------------------------------------------------------------------
# 2. Liveness: if/else — variable live across both branches
# ------------------------------------------------------------------
cat > "$TMP/t2.c" << 'EOF'
int main() {
    int x = 10;
    int r;
    if (x > 5) {
        r = x + 1;
    } else {
        r = x - 1;
    }
    return r;
}
EOF
IR=$("$CC" -O2 --dump-ir "$TMP/t2.c" -o "$TMP/t2" 2>&1)
# x should be live into both then/else blocks
check "t2: multiple blocks" "[3-9] blocks" "$IR"
check_not "t2: no loop" "loop:" "$IR"
run_exit "t2: correctness" "$TMP/t2.c" 11

# ------------------------------------------------------------------
# 3. Liveness: while loop — loop variable live across back edge
# ------------------------------------------------------------------
cat > "$TMP/t3.c" << 'EOF'
int main() {
    int i = 0;
    while (i < 3) {
        i = i + 1;
    }
    return i;
}
EOF
IR=$("$CC" -O2 --dump-ir "$TMP/t3.c" -o "$TMP/t3" 2>&1)
check "t3: loop detected" "loop: depth=1" "$IR"
check "t3: liveness present" "live_in|live_out" "$IR"
run_exit "t3: correctness" "$TMP/t3.c" 3

# ------------------------------------------------------------------
# 4. Liveness: for loop — two vars live across loop
# ------------------------------------------------------------------
cat > "$TMP/t4.c" << 'EOF'
int main() {
    int sum = 0;
    for (int i = 0; i < 5; i = i + 1) {
        sum = sum + i;
    }
    return sum;
}
EOF
IR=$("$CC" -O2 --dump-ir "$TMP/t4.c" -o "$TMP/t4" 2>&1)
check "t4: for loop detected" "loop: depth=1" "$IR"
# for_body (or any loop block) should have live vars
check "t4: live_in has data" "live_in\([1-9]" "$IR"
run_exit "t4: correctness" "$TMP/t4.c" 10

# ------------------------------------------------------------------
# 5. Loop detection: no loops → no loop annotations
# ------------------------------------------------------------------
cat > "$TMP/t5.c" << 'EOF'
int main() {
    int a = 1;
    int b = 2;
    if (a < b) {
        return a;
    }
    return b;
}
EOF
IR=$("$CC" -O2 --dump-ir "$TMP/t5.c" -o "$TMP/t5" 2>&1)
check_not "t5: no loops in if/else" "loop:" "$IR"
run_exit "t5: correctness" "$TMP/t5.c" 1

# ------------------------------------------------------------------
# 6. Loop detection: nested loops → inner gets depth=2
# ------------------------------------------------------------------
cat > "$TMP/t6.c" << 'EOF'
int main() {
    int total = 0;
    for (int i = 0; i < 3; i = i + 1) {
        for (int j = 0; j < 2; j = j + 1) {
            total = total + 1;
        }
    }
    return total;
}
EOF
IR=$("$CC" -O2 --dump-ir "$TMP/t6.c" -o "$TMP/t6" 2>&1)
check "t6: outer loop depth=1" "depth=1" "$IR"
check "t6: inner loop depth=2" "depth=2" "$IR"
run_exit "t6: correctness" "$TMP/t6.c" 6

# ------------------------------------------------------------------
# 7. Loop detection: do-while loop
# ------------------------------------------------------------------
cat > "$TMP/t7.c" << 'EOF'
int main() {
    int n = 5;
    int r = 1;
    do {
        r = r * n;
        n = n - 1;
    } while (n > 0);
    return r;
}
EOF
IR=$("$CC" -O2 --dump-ir "$TMP/t7.c" -o "$TMP/t7" 2>&1)
check "t7: do-while loop detected" "loop: depth=1" "$IR"
run_exit "t7: correctness" "$TMP/t7.c" 120

# ------------------------------------------------------------------
# 8. Loop detection: multiple disjoint loops
# ------------------------------------------------------------------
cat > "$TMP/t8.c" << 'EOF'
int main() {
    int a = 0;
    int i = 0;
    while (i < 3) {
        a = a + 1;
        i = i + 1;
    }
    int b = 0;
    int j = 0;
    while (j < 4) {
        b = b + 2;
        j = j + 1;
    }
    return a + b;
}
EOF
IR=$("$CC" -O2 --dump-ir "$TMP/t8.c" -o "$TMP/t8" 2>&1)
# Two separate loops, both depth=1
LOOP_COUNT=$(echo "$IR" | grep -c "loop: depth=1" || true)
if [ "$LOOP_COUNT" -ge 4 ]; then pass "t8: two disjoint loops found"; else fail "t8: two disjoint loops (found $LOOP_COUNT depth=1 blocks)"; fi
run_exit "t8: correctness" "$TMP/t8.c" 11

# ------------------------------------------------------------------
# 9. Liveness: variable dead after last use
# ------------------------------------------------------------------
cat > "$TMP/t9.c" << 'EOF'
int main() {
    int x = 10;
    int y = x + 5;
    int z = 42;
    return z;
}
EOF
IR=$("$CC" -O2 --dump-ir "$TMP/t9.c" -o "$TMP/t9" 2>&1)
# Single block, no values live across blocks
check_not "t9: no inter-block liveness" "live_in\([1-9]" "$IR"
run_exit "t9: correctness" "$TMP/t9.c" 42

# ------------------------------------------------------------------
# 10. Loop: variable live only inside loop
# ------------------------------------------------------------------
cat > "$TMP/t10.c" << 'EOF'
int main() {
    int result = 0;
    for (int i = 0; i < 4; i = i + 1) {
        int temp = i * 2;
        result = result + temp;
    }
    return result;
}
EOF
IR=$("$CC" -O2 --dump-ir "$TMP/t10.c" -o "$TMP/t10" 2>&1)
check "t10: loop present" "loop: depth=1" "$IR"
# Exit block should have live_in for result-related vreg
check "t10: exit has live_in" "live_in\(1\)" "$IR"
run_exit "t10: correctness" "$TMP/t10.c" 12

# ------------------------------------------------------------------
# 11. Void parameters: handled correctly (was a crash before)
# ------------------------------------------------------------------
cat > "$TMP/t11.c" << 'EOF'
int main(void) {
    int a = 1;
    int b = 2;
    return a + b;
}
EOF
IR=$("$CC" -O2 --dump-ir "$TMP/t11.c" -o "$TMP/t11" 2>&1)
check "t11: void param compiles" "function @main" "$IR"
run_exit "t11: correctness" "$TMP/t11.c" 3

# ------------------------------------------------------------------
# 12. Loop with break: partial loop body
# ------------------------------------------------------------------
cat > "$TMP/t12.c" << 'EOF'
int main() {
    int sum = 0;
    int i = 0;
    while (i < 100) {
        if (i > 4) {
            break;
        }
        sum = sum + i;
        i = i + 1;
    }
    return sum;
}
EOF
IR=$("$CC" -O2 --dump-ir "$TMP/t12.c" -o "$TMP/t12" 2>&1)
check "t12: loop with break detected" "loop: depth=1" "$IR"
run_exit "t12: correctness" "$TMP/t12.c" 10

# ------------------------------------------------------------------
# 13. Loop with continue
# ------------------------------------------------------------------
cat > "$TMP/t13.c" << 'EOF'
int main() {
    int sum = 0;
    for (int i = 0; i < 5; i = i + 1) {
        if (i == 2) {
            continue;
        }
        sum = sum + i;
    }
    return sum;
}
EOF
IR=$("$CC" -O2 --dump-ir "$TMP/t13.c" -o "$TMP/t13" 2>&1)
check "t13: loop with continue detected" "loop: depth=1" "$IR"
run_exit "t13: correctness" "$TMP/t13.c" 8

# ------------------------------------------------------------------
# 14. Multiple functions with loops
# ------------------------------------------------------------------
cat > "$TMP/t14.c" << 'EOF'
int sum_n(int n) {
    int s = 0;
    for (int i = 0; i < n; i = i + 1)
        s = s + i;
    return s;
}
int main() {
    return sum_n(5);
}
EOF
IR=$("$CC" -O2 --dump-ir "$TMP/t14.c" -o "$TMP/t14" 2>&1)
check "t14: two functions in IR" "2 functions" "$IR"
check "t14: loop in sum_n" "loop: depth=1" "$IR"
run_exit "t14: correctness" "$TMP/t14.c" 10

# ------------------------------------------------------------------
# 15. Liveness: parameter live across blocks
# ------------------------------------------------------------------
cat > "$TMP/t15.c" << 'EOF'
int abs_val(int x) {
    if (x < 0)
        return 0 - x;
    return x;
}
int main() {
    return abs_val(0 - 7);
}
EOF
IR=$("$CC" -O2 --dump-ir "$TMP/t15.c" -o "$TMP/t15" 2>&1)
# Parameter x (t0) should be live into both branches
check "t15: function with param" "function @abs_val" "$IR"
check "t15: multiple blocks" "[3-9] blocks" "$IR"
run_exit "t15: correctness" "$TMP/t15.c" 7

# ------------------------------------------------------------------
# 16. SSA phi nodes and liveness
# ------------------------------------------------------------------
cat > "$TMP/t16.c" << 'EOF'
int main() {
    int x;
    if (1) {
        x = 10;
    } else {
        x = 20;
    }
    return x;
}
EOF
IR=$("$CC" -O2 --dump-ir "$TMP/t16.c" -o "$TMP/t16" 2>&1)
check "t16: phi present for merge" "phi" "$IR"
run_exit "t16: correctness" "$TMP/t16.c" 10

# ------------------------------------------------------------------
# 17. Liveness: loop with accumulator pattern
# ------------------------------------------------------------------
cat > "$TMP/t17.c" << 'EOF'
int main() {
    int prod = 1;
    for (int i = 1; i < 6; i = i + 1) {
        prod = prod * i;
    }
    return prod;
}
EOF
IR=$("$CC" -O2 --dump-ir "$TMP/t17.c" -o "$TMP/t17" 2>&1)
check "t17: loop with accumulator" "loop: depth=1" "$IR"
# live_out should show accumulator and counter vregs
check "t17: live_out in loop" "live_out\([1-9]" "$IR"
run_exit "t17: correctness" "$TMP/t17.c" 120

# ------------------------------------------------------------------
# 18. Loop header identification
# ------------------------------------------------------------------
cat > "$TMP/t18.c" << 'EOF'
int main() {
    int s = 0;
    int i = 0;
    while (i < 10) {
        s = s + i;
        i = i + 1;
    }
    return s;
}
EOF
IR=$("$CC" -O2 --dump-ir "$TMP/t18.c" -o "$TMP/t18" 2>&1)
# Loop header should be the while_cond block
check "t18: loop header annotation" "hdr=bb" "$IR"
# The while_cond block should be the loop header pointing to itself
check "t18: while_cond is header" "while_cond.*loop: depth=1 hdr=bb" "$IR"
run_exit "t18: correctness" "$TMP/t18.c" 45

# ------------------------------------------------------------------
# 19. Triple-nested loops
# ------------------------------------------------------------------
cat > "$TMP/t19.c" << 'EOF'
int main() {
    int count = 0;
    for (int i = 0; i < 2; i = i + 1)
        for (int j = 0; j < 3; j = j + 1)
            for (int k = 0; k < 4; k = k + 1)
                count = count + 1;
    return count;
}
EOF
IR=$("$CC" -O2 --dump-ir "$TMP/t19.c" -o "$TMP/t19" 2>&1)
check "t19: depth=1 exists" "depth=1" "$IR"
check "t19: depth=2 exists" "depth=2" "$IR"
check "t19: depth=3 exists" "depth=3" "$IR"
run_exit "t19: correctness" "$TMP/t19.c" 24

# ------------------------------------------------------------------
# 20. Liveness across function with multiple exits
# ------------------------------------------------------------------
cat > "$TMP/t20.c" << 'EOF'
int clamp(int val, int lo, int hi) {
    if (val < lo) return lo;
    if (val > hi) return hi;
    return val;
}
int main() {
    return clamp(15, 0, 10);
}
EOF
IR=$("$CC" -O2 --dump-ir "$TMP/t20.c" -o "$TMP/t20" 2>&1)
check "t20: function @clamp" "function @clamp" "$IR"
check "t20: multiple blocks in clamp" "[4-9] blocks" "$IR"
run_exit "t20: correctness" "$TMP/t20.c" 10

# ------------------------------------------------------------------
# 21. Dominator and DF annotations present
# ------------------------------------------------------------------
cat > "$TMP/t21.c" << 'EOF'
int main() {
    int x = 1;
    while (x < 8) {
        x = x * 2;
    }
    return x;
}
EOF
IR=$("$CC" -O2 --dump-ir "$TMP/t21.c" -o "$TMP/t21" 2>&1)
check "t21: dominator info present" "idom:" "$IR"
check "t21: dominance frontier for loop" "DF:" "$IR"
check "t21: loop detected" "loop: depth=1" "$IR"
run_exit "t21: correctness" "$TMP/t21.c" 8

# ------------------------------------------------------------------
# 22. For loop with void param (combined regression)
# ------------------------------------------------------------------
cat > "$TMP/t22.c" << 'EOF'
int main(void) {
    int s = 0;
    for (int i = 0; i < 5; i = i + 1)
        s = s + i;
    return s;
}
EOF
IR=$("$CC" -O2 --dump-ir "$TMP/t22.c" -o "$TMP/t22" 2>&1)
check "t22: void param + loop" "loop: depth=1" "$IR"
run_exit "t22: correctness" "$TMP/t22.c" 10

# ------------------------------------------------------------------
# 23. Loop with if inside — blocks inside loop get depth=1
# ------------------------------------------------------------------
cat > "$TMP/t23.c" << 'EOF'
int main() {
    int even = 0;
    int odd = 0;
    for (int i = 0; i < 6; i = i + 1) {
        if (i - (i / 2) * 2 == 0) {
            even = even + 1;
        } else {
            odd = odd + 1;
        }
    }
    return even + odd;
}
EOF
IR=$("$CC" -O2 --dump-ir "$TMP/t23.c" -o "$TMP/t23" 2>&1)
check "t23: loop with if/else" "loop: depth=1" "$IR"
# Should have at least 2 blocks with depth=1 (cond + body blocks)
LOOP_BLOCKS=$(echo "$IR" | grep -c "depth=1" || true)
if [ "$LOOP_BLOCKS" -ge 2 ]; then pass "t23: multiple blocks in loop"; else fail "t23: multiple blocks in loop ($LOOP_BLOCKS)"; fi
run_exit "t23: correctness" "$TMP/t23.c" 6

# ------------------------------------------------------------------
# 24. Liveness: value used only on one branch
# ------------------------------------------------------------------
cat > "$TMP/t24.c" << 'EOF'
int main() {
    int x = 5;
    int y = 10;
    if (x > 3) {
        return y;
    }
    return 0;
}
EOF
IR=$("$CC" -O2 --dump-ir "$TMP/t24.c" -o "$TMP/t24" 2>&1)
check "t24: if with selective use" "function @main" "$IR"
run_exit "t24: correctness" "$TMP/t24.c" 10

# ------------------------------------------------------------------
# 25. Empty loop body
# ------------------------------------------------------------------
cat > "$TMP/t25.c" << 'EOF'
int main() {
    int i = 0;
    while (i < 5)
        i = i + 1;
    return i;
}
EOF
IR=$("$CC" -O2 --dump-ir "$TMP/t25.c" -o "$TMP/t25" 2>&1)
check "t25: minimal while loop" "loop: depth=1" "$IR"
run_exit "t25: correctness" "$TMP/t25.c" 5

# ------------------------------------------------------------------
# Summary
# ------------------------------------------------------------------
echo ""
echo "=== Analysis Test Summary ==="
echo "  PASS:    $PASS"
echo "  FAIL:    $FAIL"
echo "  TOTAL:   $((PASS + FAIL))"
echo ""
if [ "$FAIL" -eq 0 ]; then
    echo "All analysis tests passed!"
    exit 0
else
    echo "Some tests FAILED."
    exit 1
fi
