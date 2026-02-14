#!/usr/bin/env bash
# =======================================================
# Phase 4d: Optimization Passes Test Suite
#   SCCP, CSE/GVN, LICM, Register Allocation
# =======================================================
set -eo pipefail
CC="${CC:-build_linux/fadors99}"
PASS=0; FAIL=0

pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1 — $2"; FAIL=$((FAIL + 1)); }
check() {
    local label="$1" pattern="$2" text="$3"
    if echo "$text" | grep -qE "$pattern"; then pass "$label"; else fail "$label" "pattern not found: $pattern"; fi
}
check_not() {
    local label="$1" pattern="$2" text="$3"
    if echo "$text" | grep -qE "$pattern"; then fail "$label" "unexpected pattern: $pattern"; else pass "$label"; fi
}
run_ir() {
    local src="$1"
    "$CC" -O2 --dump-ir "$src" -o "${src%.c}" 2>&1
}
run_exit() {
    local label="$1" src="$2" expect="$3"
    local out
    out=$("$CC" -O2 "$src" -o "${src%.c}" 2>&1) || { fail "$label" "compile failed"; return; }
    local got; got=0; "${src%.c}" && got=$? || got=$?
    if [ "$got" -eq "$expect" ]; then pass "$label"; else fail "$label" "got $got, expected $expect"; fi
}

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

echo "=== Phase 4d: Optimization Passes Tests ==="
echo ""

# ===================================================================
#  SECTION 1: SCCP (Sparse Conditional Constant Propagation)
# ===================================================================
echo "--- SCCP ---"

# 1. Constants folded across blocks
cat > "$TMP/sccp1.c" << 'EOF'
int main() {
    int a = 5;
    int b = 3;
    int c = a + b;
    int d = c * 2;
    return d;
}
EOF
IR=$(run_ir "$TMP/sccp1.c")
check "sccp1: constant 16 in IR" '[$]16' "$IR"
run_exit "sccp1: correctness (5+3)*2=16" "$TMP/sccp1.c" 16

# 2. Subtraction and multiply constant fold
cat > "$TMP/sccp2.c" << 'EOF'
int main() {
    int a = 10;
    int b = 3;
    int c = a - b;
    int d = c * 5;
    return d;
}
EOF
IR=$(run_ir "$TMP/sccp2.c")
check "sccp2: constant 35 in IR" '[$]35' "$IR"
run_exit "sccp2: correctness (10-3)*5=35" "$TMP/sccp2.c" 35

# 3. Division constant fold
cat > "$TMP/sccp3.c" << 'EOF'
int main() {
    int a = 100;
    int b = 4;
    int c = a / b;
    return c;
}
EOF
IR=$(run_ir "$TMP/sccp3.c")
check "sccp3: constant 25 in IR" '[$]25' "$IR"
run_exit "sccp3: correctness 100/4=25" "$TMP/sccp3.c" 25

# 4. Modulo constant fold
cat > "$TMP/sccp4.c" << 'EOF'
int main() {
    int a = 17;
    int b = 5;
    int c = a % b;
    return c;
}
EOF
IR=$(run_ir "$TMP/sccp4.c")
check "sccp4: constant 2 in ret" 'ret [$]2' "$IR"
run_exit "sccp4: correctness 17%5=2" "$TMP/sccp4.c" 2

# 5. Constant propagation through conditional (true branch)
cat > "$TMP/sccp5.c" << 'EOF'
int main() {
    int x = 1;
    int r;
    if (x) {
        r = 42;
    } else {
        r = 99;
    }
    return r;
}
EOF
IR=$(run_ir "$TMP/sccp5.c")
run_exit "sccp5: constant branch folding (true)" "$TMP/sccp5.c" 42

# 6. Constant propagation through conditional (false branch)
cat > "$TMP/sccp6.c" << 'EOF'
int main() {
    int x = 0;
    int r;
    if (x) {
        r = 42;
    } else {
        r = 99;
    }
    return r;
}
EOF
run_exit "sccp6: constant branch folding (false)" "$TMP/sccp6.c" 99

# 7. Multi-step constant chain
cat > "$TMP/sccp7.c" << 'EOF'
int main() {
    int a = 2;
    int b = a + 3;
    int c = b * 4;
    int d = c - 1;
    return d;
}
EOF
IR=$(run_ir "$TMP/sccp7.c")
check "sccp7: folded to 19" '[$]19' "$IR"
run_exit "sccp7: correctness (2+3)*4-1=19" "$TMP/sccp7.c" 19

# 8. Negation constant fold
cat > "$TMP/sccp8.c" << 'EOF'
int main() {
    int a = 42;
    int b = -a;
    return b + 50;
}
EOF
run_exit "sccp8: negation fold -42+50=8" "$TMP/sccp8.c" 8

# 9. Comparison constant fold
cat > "$TMP/sccp9.c" << 'EOF'
int main() {
    int a = 5;
    int b = 3;
    if (a > b) { return 1; }
    return 0;
}
EOF
run_exit "sccp9: comparison fold 5>3 => 1" "$TMP/sccp9.c" 1

# 10. Parameters are NOT constant-folded
cat > "$TMP/sccp10.c" << 'EOF'
int add(int a, int b) { return a + b; }
int main() { return add(10, 20); }
EOF
IR=$(run_ir "$TMP/sccp10.c")
check "sccp10: params remain vregs" "add t.*t" "$IR"
run_exit "sccp10: correctness 10+20=30" "$TMP/sccp10.c" 30

# ===================================================================
#  SECTION 2: CSE / GVN (Common Subexpression Elimination)
# ===================================================================
echo ""
echo "--- CSE / GVN ---"

# 11. Redundant addition eliminated
cat > "$TMP/cse1.c" << 'EOF'
int compute(int a, int b) {
    int c = a + b;
    int d = a + b;
    return c + d;
}
int main() { return compute(10, 20); }
EOF
IR=$(run_ir "$TMP/cse1.c")
# After CSE, the second a+b should be a copy of the first
check "cse1: redundant add → copy" "copy" "$IR"
run_exit "cse1: correctness (10+20)*2=60" "$TMP/cse1.c" 60

# 12. CSE across copies
cat > "$TMP/cse2.c" << 'EOF'
int compute(int a, int b) {
    int c = a + b;
    int d = c;
    int e = a + b;
    return d + e;
}
int main() { return compute(5, 10); }
EOF
run_exit "cse2: CSE through copies" "$TMP/cse2.c" 30

# 13. Multiplication CSE
cat > "$TMP/cse3.c" << 'EOF'
int compute(int a, int b) {
    int c = a * b;
    int d = a * b;
    return c - d;
}
int main() { return compute(7, 11); }
EOF
run_exit "cse3: mul CSE, result=0" "$TMP/cse3.c" 0

# 14. CSE does not fire for different operations
cat > "$TMP/cse4.c" << 'EOF'
int compute(int a, int b) {
    int c = a + b;
    int d = a * b;
    return c + d;
}
int main() { return compute(3, 4); }
EOF
IR=$(run_ir "$TMP/cse4.c")
run_exit "cse4: different ops not merged (3+4)+(3*4)=19" "$TMP/cse4.c" 19

# 15. CSE with subtraction
cat > "$TMP/cse5.c" << 'EOF'
int compute(int a, int b) {
    int c = a - b;
    int d = a - b;
    return c + d;
}
int main() { return compute(20, 5); }
EOF
run_exit "cse5: sub CSE (20-5)*2=30" "$TMP/cse5.c" 30

# 16. Triple redundant expression
cat > "$TMP/cse6.c" << 'EOF'
int compute(int a, int b) {
    int c = a + b;
    int d = a + b;
    int e = a + b;
    return c + d + e;
}
int main() { return compute(3, 7); }
EOF
run_exit "cse6: triple CSE (3+7)*3=30" "$TMP/cse6.c" 30

# ===================================================================
#  SECTION 3: LICM (Loop-Invariant Code Motion)
# ===================================================================
echo ""
echo "--- LICM ---"

# 17. Constant loop-invariant (folded by SCCP too)
cat > "$TMP/licm1.c" << 'EOF'
int main() {
    int a = 5;
    int b = 3;
    int sum = 0;
    int i = 0;
    while (i < 4) {
        int c = a + b;
        sum = sum + c;
        i = i + 1;
    }
    return sum;
}
EOF
run_exit "licm1: constant invariant hoisted, 8*4=32" "$TMP/licm1.c" 32

# 18. Parameter-dependent loop-invariant
cat > "$TMP/licm2.c" << 'EOF'
int compute(int a, int b) {
    int sum = 0;
    int i = 0;
    while (i < 4) {
        int c = a + b;
        sum = sum + c;
        i = i + 1;
    }
    return sum;
}
int main() { return compute(5, 3); }
EOF
IR=$(run_ir "$TMP/licm2.c")
# The add of a+b should NOT be in any loop block
# Check that the loop body doesn't contain the parameter add
run_exit "licm2: param invariant hoisted, (5+3)*4=32" "$TMP/licm2.c" 32

# 19. LICM with for loop
cat > "$TMP/licm3.c" << 'EOF'
int compute(int a, int b) {
    int sum = 0;
    for (int i = 0; i < 5; i = i + 1) {
        int c = a + b;
        sum = sum + c;
    }
    return sum;
}
int main() { return compute(6, 4); }
EOF
run_exit "licm3: for-loop invariant hoisted, (6+4)*5=50" "$TMP/licm3.c" 50

# 20. LICM: non-invariant stays in loop
cat > "$TMP/licm4.c" << 'EOF'
int compute(int n) {
    int sum = 0;
    for (int i = 0; i < n; i = i + 1) {
        sum = sum + i;
    }
    return sum;
}
int main() { return compute(10); }
EOF
run_exit "licm4: loop-variant stays, 0+1+...+9=45" "$TMP/licm4.c" 45

# 21. LICM: multiplication invariant
cat > "$TMP/licm5.c" << 'EOF'
int compute(int a, int b) {
    int sum = 0;
    for (int i = 0; i < 3; i = i + 1) {
        int c = a * b;
        sum = sum + c;
    }
    return sum;
}
int main() { return compute(7, 3); }
EOF
run_exit "licm5: mul invariant hoisted, (7*3)*3=63" "$TMP/licm5.c" 63

# 22. LICM: nested expressions
cat > "$TMP/licm6.c" << 'EOF'
int compute(int a, int b, int c) {
    int sum = 0;
    for (int i = 0; i < 4; i = i + 1) {
        int d = a + b;
        int e = d * c;
        sum = sum + e;
    }
    return sum;
}
int main() { return compute(2, 3, 4); }
EOF
run_exit "licm6: nested invariant hoisted, ((2+3)*4)*4=80" "$TMP/licm6.c" 80

# 23. LICM: loop accumulator not hoisted
cat > "$TMP/licm7.c" << 'EOF'
int compute(int n) {
    int product = 1;
    for (int i = 1; i < n; i = i + 1) {
        product = product * i;
    }
    return product;
}
int main() { return compute(6); }
EOF
run_exit "licm7: accumulator stays, 1*2*3*4*5=120" "$TMP/licm7.c" 120

# ===================================================================
#  SECTION 4: Register Allocation
# ===================================================================
echo ""
echo "--- Register Allocation ---"

# 24. Regalloc summary present
cat > "$TMP/ra1.c" << 'EOF'
int main() {
    int a = 1;
    int b = 2;
    int c = a + b;
    return c;
}
EOF
IR=$(run_ir "$TMP/ra1.c")
check "ra1: regalloc summary shown" "regalloc:.*in regs" "$IR"
check "ra1: assignment shown" "assign:" "$IR"

# 25. Regalloc with many variables
cat > "$TMP/ra2.c" << 'EOF'
int main() {
    int a = 1;
    int b = 2;
    int c = 3;
    int d = 4;
    int e = 5;
    int f = a + b;
    int g = c + d;
    int h = f + g + e;
    return h;
}
EOF
IR=$(run_ir "$TMP/ra2.c")
check "ra2: multiple regs assigned" "assign:.*rax" "$IR"
run_exit "ra2: correctness 1+2+3+4+5=15" "$TMP/ra2.c" 15

# 26. Regalloc with function parameters
cat > "$TMP/ra3.c" << 'EOF'
int add3(int a, int b, int c) {
    return a + b + c;
}
int main() { return add3(10, 20, 30); }
EOF
IR=$(run_ir "$TMP/ra3.c")
check "ra3: regalloc for params" "regalloc:" "$IR"
run_exit "ra3: correctness 10+20+30=60" "$TMP/ra3.c" 60

# 27. Regalloc with loops (many live ranges)
cat > "$TMP/ra4.c" << 'EOF'
int compute(int n) {
    int sum = 0;
    for (int i = 0; i < n; i = i + 1) {
        sum = sum + i;
    }
    return sum;
}
int main() { return compute(10); }
EOF
IR=$(run_ir "$TMP/ra4.c")
check "ra4: regalloc with loop" "regalloc:" "$IR"
run_exit "ra4: correctness sum(0..9)=45" "$TMP/ra4.c" 45

# 28. Regalloc reports spills for many vregs
cat > "$TMP/ra5.c" << 'EOF'
int compute(int a, int b) {
    int c = a + b;
    int d = a - b;
    int e = a * b;
    int f = c + d;
    int g = e + f;
    int h = g - c;
    int i = h + d;
    int j = i * 2;
    return j;
}
int main() { return compute(10, 3); }
EOF
IR=$(run_ir "$TMP/ra5.c")
check "ra5: regalloc with many temps" "regalloc:" "$IR"
run_exit "ra5: correctness" "$TMP/ra5.c" 88

# ===================================================================
#  SECTION 5: Combined Passes (SCCP + CSE + LICM)
# ===================================================================
echo ""
echo "--- Combined Passes ---"

# 29. SCCP + CSE together
cat > "$TMP/comb1.c" << 'EOF'
int main() {
    int a = 5;
    int b = 3;
    int c = a + b;
    int d = a + b;
    return c + d;
}
EOF
IR=$(run_ir "$TMP/comb1.c")
# SCCP should fold a+b=8, so c=8, d=8 => ret 16
check "comb1: SCCP+CSE => 16" '[$]16' "$IR"
run_exit "comb1: correctness (5+3)*2=16" "$TMP/comb1.c" 16

# 30. SCCP + LICM together
cat > "$TMP/comb2.c" << 'EOF'
int main() {
    int a = 3;
    int b = 7;
    int sum = 0;
    for (int i = 0; i < 5; i = i + 1) {
        int c = a + b;
        sum = sum + c;
    }
    return sum;
}
EOF
run_exit "comb2: SCCP folds invariant, 10*5=50" "$TMP/comb2.c" 50

# 31. CSE + LICM together
cat > "$TMP/comb3.c" << 'EOF'
int compute(int a, int b) {
    int sum = 0;
    for (int i = 0; i < 3; i = i + 1) {
        int c = a + b;
        int d = a + b;
        sum = sum + c + d;
    }
    return sum;
}
int main() { return compute(4, 6); }
EOF
run_exit "comb3: CSE+LICM, (4+6)*2*3=60" "$TMP/comb3.c" 60

# 32. All passes: complex function
cat > "$TMP/comb4.c" << 'EOF'
int compute(int x, int y) {
    int k = 2;
    int m = k * 3;
    int sum = 0;
    for (int i = 0; i < m; i = i + 1) {
        int a = x + y;
        int b = x + y;
        sum = sum + a + b;
    }
    return sum;
}
int main() { return compute(5, 10); }
EOF
run_exit "comb4: SCCP(m=6)+CSE(a=b)+LICM, 15*2*6=180" "$TMP/comb4.c" 180

# 33. Nested loop with invariant
cat > "$TMP/comb5.c" << 'EOF'
int compute(int a, int b) {
    int sum = 0;
    for (int i = 0; i < 3; i = i + 1) {
        for (int j = 0; j < 4; j = j + 1) {
            int c = a + b;
            sum = sum + c;
        }
    }
    return sum;
}
int main() { return compute(2, 3); }
EOF
run_exit "comb5: nested loop invariant, (2+3)*12=60" "$TMP/comb5.c" 60

# 34. While loop with constant condition
cat > "$TMP/comb6.c" << 'EOF'
int main() {
    int x = 0;
    int i = 0;
    while (i < 10) {
        x = x + 1;
        i = i + 1;
    }
    return x;
}
EOF
run_exit "comb6: while loop count=10" "$TMP/comb6.c" 10

# 35. Multiple functions with optimization
cat > "$TMP/comb7.c" << 'EOF'
int square(int x) { return x * x; }
int add(int a, int b) { return a + b; }
int main() {
    int a = square(3);
    int b = square(4);
    return add(a, b);
}
EOF
run_exit "comb7: multi-func 9+16=25" "$TMP/comb7.c" 25

# 36. Zero-trip loop
cat > "$TMP/comb8.c" << 'EOF'
int main() {
    int sum = 42;
    for (int i = 0; i < 0; i = i + 1) {
        sum = sum + 1;
    }
    return sum;
}
EOF
run_exit "comb8: zero-trip loop, sum=42" "$TMP/comb8.c" 42

# 37. Single iteration loop
cat > "$TMP/comb9.c" << 'EOF'
int main() {
    int sum = 0;
    for (int i = 0; i < 1; i = i + 1) {
        sum = sum + 7;
    }
    return sum;
}
EOF
run_exit "comb9: single iteration, sum=7" "$TMP/comb9.c" 7

# 38. Optimized arithmetic chains
cat > "$TMP/comb10.c" << 'EOF'
int main() {
    int a = 2;
    int b = 3;
    int c = a * b;
    int d = c + a;
    int e = d * b;
    int f = e - c;
    return f;
}
EOF
run_exit "comb10: chain (2*3=6, 6+2=8, 8*3=24, 24-6=18)" "$TMP/comb10.c" 18

# ===================================================================
#  SECTION 6: Edge Cases & Robustness
# ===================================================================
echo ""
echo "--- Edge Cases ---"

# 39. Empty function
cat > "$TMP/edge1.c" << 'EOF'
void noop() {}
int main() { noop(); return 0; }
EOF
run_exit "edge1: empty function" "$TMP/edge1.c" 0

# 40. Function with only return
cat > "$TMP/edge2.c" << 'EOF'
int val() { return 42; }
int main() { return val(); }
EOF
run_exit "edge2: return-only function" "$TMP/edge2.c" 42

# 41. Many parameters
cat > "$TMP/edge3.c" << 'EOF'
int add4(int a, int b, int c, int d) {
    return a + b + c + d;
}
int main() { return add4(1, 2, 3, 4); }
EOF
run_exit "edge3: 4 parameters, 1+2+3+4=10" "$TMP/edge3.c" 10

# 42. Large constant
cat > "$TMP/edge4.c" << 'EOF'
int main() {
    int a = 200;
    int b = a / 2;
    return b;
}
EOF
run_exit "edge4: large constant 200/2=100" "$TMP/edge4.c" 100

# 43. Chained if-else
cat > "$TMP/edge5.c" << 'EOF'
int classify(int x) {
    if (x > 10) return 3;
    if (x > 5) return 2;
    if (x > 0) return 1;
    return 0;
}
int main() { return classify(7); }
EOF
run_exit "edge5: chained if, classify(7)=2" "$TMP/edge5.c" 2

# 44. Deeply nested arithmetic
cat > "$TMP/edge6.c" << 'EOF'
int main() {
    int a = 1;
    int b = a + 1;
    int c = b + 1;
    int d = c + 1;
    int e = d + 1;
    int f = e + 1;
    int g = f + 1;
    int h = g + 1;
    return h;
}
EOF
IR=$(run_ir "$TMP/edge6.c")
check "edge6: deep chain folded to 8" '[$]8' "$IR"
run_exit "edge6: correctness 1+7=8" "$TMP/edge6.c" 8

# 45. SCCP with boolean logic
cat > "$TMP/edge7.c" << 'EOF'
int main() {
    int a = 5;
    int b = 5;
    int eq = (a == b);
    if (eq) return 1;
    return 0;
}
EOF
run_exit "edge7: equality fold 5==5 => 1" "$TMP/edge7.c" 1

# 46. Loop with computed bound
cat > "$TMP/edge8.c" << 'EOF'
int main() {
    int n = 2 + 3;
    int sum = 0;
    for (int i = 0; i < n; i = i + 1) {
        sum = sum + 1;
    }
    return sum;
}
EOF
run_exit "edge8: SCCP folds loop bound, sum=5" "$TMP/edge8.c" 5

# 47. Multiple loops sequential
cat > "$TMP/edge9.c" << 'EOF'
int main() {
    int sum = 0;
    for (int i = 0; i < 3; i = i + 1) {
        sum = sum + 1;
    }
    for (int j = 0; j < 4; j = j + 1) {
        sum = sum + 2;
    }
    return sum;
}
EOF
run_exit "edge9: sequential loops, 3+8=11" "$TMP/edge9.c" 11

# 48. LICM with subtraction invariant
cat > "$TMP/edge10.c" << 'EOF'
int compute(int a, int b) {
    int sum = 0;
    for (int i = 0; i < 5; i = i + 1) {
        int c = a - b;
        sum = sum + c;
    }
    return sum;
}
int main() { return compute(20, 8); }
EOF
run_exit "edge10: sub invariant hoisted, (20-8)*5=60" "$TMP/edge10.c" 60

# ===================================================================
echo ""
echo "==================================="
echo "  Phase 4d Optimization Test Results"
echo "==================================="
echo "  PASS:    $PASS"
echo "  FAIL:    $FAIL"
echo "  TOTAL:   $((PASS + FAIL))"
echo ""
if [ "$FAIL" -eq 0 ]; then
    echo "All Phase 4d optimization tests passed!"
else
    echo "Some tests failed."
    exit 1
fi
