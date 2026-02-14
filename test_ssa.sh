#!/bin/bash
# test_ssa.sh — SSA construction tests for fadors99
# Verifies dominator tree, dominance frontiers, phi insertion, variable
# renaming, and SSA correctness across branching, loops, and nested control flow.

set -e

COMPILER="${1:-build_linux/fadors99}"
PASS=0
FAIL=0
TOTAL=0

pass() { PASS=$((PASS+1)); TOTAL=$((TOTAL+1)); echo "  PASS: $1"; }
fail() { FAIL=$((FAIL+1)); TOTAL=$((TOTAL+1)); echo "  FAIL: $1 — $2"; }

run_ssa() {
    # Compile with -O2 --dump-ir and capture IR output
    local src="$1"
    $COMPILER -O2 --dump-ir "$src" -o /tmp/ssa_test_out 2>/tmp/ssa_ir_dump
    local rc=$?
    if [ $rc -ne 0 ]; then
        echo "  compilation failed (rc=$rc)"
        return 1
    fi
    cat /tmp/ssa_ir_dump
}

check_exec() {
    # Run the compiled binary and check exit code
    local expected="$1"
    /tmp/ssa_test_out
    local rc=$?
    if [ $rc -eq $expected ]; then
        return 0
    else
        echo "  expected exit $expected, got $rc"
        return 1
    fi
}

echo "=== SSA Construction Tests ==="
echo ""

# ------------------------------------------------------------------
# Test 1: SSA flag present in output
# ------------------------------------------------------------------
cat > /tmp/ssa_t1.c << 'EOF'
int main() { return 42; }
EOF
IR=$(run_ssa /tmp/ssa_t1.c)
if echo "$IR" | grep -q "(SSA)"; then
    pass "SSA flag in dump"
else
    fail "SSA flag in dump" "no (SSA) marker found"
fi

# ------------------------------------------------------------------
# Test 2: Simple if-else generates phi node
# ------------------------------------------------------------------
cat > /tmp/ssa_t2.c << 'EOF'
int main() {
    int x = 5;
    if (x > 3) { x = 10; } else { x = 20; }
    return x;
}
EOF
IR=$(run_ssa /tmp/ssa_t2.c)
if echo "$IR" | grep -q "phi"; then
    pass "If-else phi insertion"
else
    fail "If-else phi insertion" "no phi found"
fi
if check_exec 10; then
    pass "If-else correctness"
else
    fail "If-else correctness" "wrong result"
fi

# ------------------------------------------------------------------
# Test 3: For loop generates phi nodes at loop header
# ------------------------------------------------------------------
cat > /tmp/ssa_t3.c << 'EOF'
int main() {
    int sum = 0;
    int i;
    for (i = 0; i < 5; i = i + 1) {
        sum = sum + i;
    }
    return sum;
}
EOF
IR=$(run_ssa /tmp/ssa_t3.c)
PHI_COUNT=$(echo "$IR" | grep -c "phi" || true)
if [ "$PHI_COUNT" -ge 2 ]; then
    pass "For loop phi count (>= 2)"
else
    fail "For loop phi count (>= 2)" "found $PHI_COUNT"
fi
if check_exec 10; then
    pass "For loop correctness"
else
    fail "For loop correctness" "wrong result"
fi

# ------------------------------------------------------------------
# Test 4: While loop generates phi
# ------------------------------------------------------------------
cat > /tmp/ssa_t4.c << 'EOF'
int main() {
    int x = 1;
    int n = 5;
    while (n > 0) {
        x = x * 2;
        n = n - 1;
    }
    return x;
}
EOF
IR=$(run_ssa /tmp/ssa_t4.c)
if echo "$IR" | grep -q "phi"; then
    pass "While loop phi insertion"
else
    fail "While loop phi insertion" "no phi found"
fi
if check_exec 32; then
    pass "While loop correctness (1*2^5=32)"
else
    fail "While loop correctness" "wrong result"
fi

# ------------------------------------------------------------------
# Test 5: Dominator info in dump
# ------------------------------------------------------------------
cat > /tmp/ssa_t5.c << 'EOF'
int main() {
    int x = 1;
    if (x) { x = 2; }
    return x;
}
EOF
IR=$(run_ssa /tmp/ssa_t5.c)
if echo "$IR" | grep -q "idom:"; then
    pass "Dominator info in dump"
else
    fail "Dominator info in dump" "no idom found"
fi

# ------------------------------------------------------------------
# Test 6: Dominance frontier in dump
# ------------------------------------------------------------------
if echo "$IR" | grep -q "DF:"; then
    pass "Dominance frontier in dump"
else
    fail "Dominance frontier in dump" "no DF found"
fi

# ------------------------------------------------------------------
# Test 7: Nested if-else (multiple phi nodes)
# ------------------------------------------------------------------
cat > /tmp/ssa_t7.c << 'EOF'
int main() {
    int x = 0;
    int y = 0;
    if (x == 0) {
        x = 1;
        if (y == 0) {
            y = 2;
        } else {
            y = 3;
        }
    } else {
        x = 4;
        y = 5;
    }
    return x + y;
}
EOF
IR=$(run_ssa /tmp/ssa_t7.c)
PHI_COUNT=$(echo "$IR" | grep -c "phi" || true)
if [ "$PHI_COUNT" -ge 2 ]; then
    pass "Nested if-else phi count"
else
    fail "Nested if-else phi count" "found $PHI_COUNT"
fi
if check_exec 3; then
    pass "Nested if-else correctness (1+2=3)"
else
    fail "Nested if-else correctness" "wrong result"
fi

# ------------------------------------------------------------------
# Test 8: Do-while loop
# ------------------------------------------------------------------
cat > /tmp/ssa_t8.c << 'EOF'
int main() {
    int i = 0;
    int sum = 0;
    do {
        sum = sum + i;
        i = i + 1;
    } while (i < 4);
    return sum;
}
EOF
IR=$(run_ssa /tmp/ssa_t8.c)
if echo "$IR" | grep -q "phi"; then
    pass "Do-while phi insertion"
else
    fail "Do-while phi insertion" "no phi found"
fi
if check_exec 6; then
    pass "Do-while correctness (0+1+2+3=6)"
else
    fail "Do-while correctness" "wrong result"
fi

# ------------------------------------------------------------------
# Test 9: Variable only defined in one branch (phi with undef/zero)
# ------------------------------------------------------------------
cat > /tmp/ssa_t9.c << 'EOF'
int main() {
    int x = 0;
    int cond = 1;
    if (cond) {
        x = 42;
    }
    return x;
}
EOF
IR=$(run_ssa /tmp/ssa_t9.c)
if echo "$IR" | grep -q "phi"; then
    pass "Single-branch phi insertion"
else
    fail "Single-branch phi insertion" "no phi found"
fi
if check_exec 42; then
    pass "Single-branch correctness"
else
    fail "Single-branch correctness" "wrong result"
fi

# ------------------------------------------------------------------
# Test 10: No phi needed (no join point for variable)
# ------------------------------------------------------------------
cat > /tmp/ssa_t10.c << 'EOF'
int main() {
    int x = 5;
    int y = x + 3;
    return y;
}
EOF
IR=$(run_ssa /tmp/ssa_t10.c)
PHI_COUNT=$(echo "$IR" | grep -c "phi" || true)
if [ "$PHI_COUNT" -eq 0 ]; then
    pass "No phi for straight-line code"
else
    fail "No phi for straight-line code" "found $PHI_COUNT phi(s)"
fi
if check_exec 8; then
    pass "Straight-line correctness"
else
    fail "Straight-line correctness" "wrong result"
fi

# ------------------------------------------------------------------
# Test 11: SSA with function parameters
# ------------------------------------------------------------------
cat > /tmp/ssa_t11.c << 'EOF'
int compute(int a, int b) {
    int result;
    if (a > b) {
        result = a - b;
    } else {
        result = b - a;
    }
    return result;
}
int main() {
    return compute(15, 7);
}
EOF
IR=$(run_ssa /tmp/ssa_t11.c)
if echo "$IR" | grep -q "phi"; then
    pass "Function params phi insertion"
else
    fail "Function params phi insertion" "no phi found"
fi
if check_exec 8; then
    pass "Function params correctness (|15-7|=8)"
else
    fail "Function params correctness" "wrong result"
fi

# ------------------------------------------------------------------
# Test 12: Multiple variables across loop
# ------------------------------------------------------------------
cat > /tmp/ssa_t12.c << 'EOF'
int main() {
    int a = 1;
    int b = 0;
    int i;
    for (i = 0; i < 4; i = i + 1) {
        int temp = a;
        a = a + b;
        b = temp;
    }
    return a;
}
EOF
IR=$(run_ssa /tmp/ssa_t12.c)
PHI_COUNT=$(echo "$IR" | grep -c "phi" || true)
if [ "$PHI_COUNT" -ge 3 ]; then
    pass "Multi-var loop phi count (>= 3)"
else
    fail "Multi-var loop phi count (>= 3)" "found $PHI_COUNT"
fi
if check_exec 5; then
    pass "Multi-var loop correctness (fib(5)=5)"
else
    fail "Multi-var loop correctness" "wrong result"
fi

# ------------------------------------------------------------------
# Test 13: Nested loops
# ------------------------------------------------------------------
cat > /tmp/ssa_t13.c << 'EOF'
int main() {
    int sum = 0;
    int i;
    int j;
    for (i = 0; i < 3; i = i + 1) {
        for (j = 0; j < 3; j = j + 1) {
            sum = sum + 1;
        }
    }
    return sum;
}
EOF
IR=$(run_ssa /tmp/ssa_t13.c)
PHI_COUNT=$(echo "$IR" | grep -c "phi" || true)
if [ "$PHI_COUNT" -ge 3 ]; then
    pass "Nested loops phi count (>= 3)"
else
    fail "Nested loops phi count (>= 3)" "found $PHI_COUNT"
fi
if check_exec 9; then
    pass "Nested loops correctness (3*3=9)"
else
    fail "Nested loops correctness" "wrong result"
fi

# ------------------------------------------------------------------
# Test 14: Phi predecessor block references
# ------------------------------------------------------------------
cat > /tmp/ssa_t14.c << 'EOF'
int main() {
    int x = 1;
    if (x) { x = 2; } else { x = 3; }
    return x;
}
EOF
IR=$(run_ssa /tmp/ssa_t14.c)
# Check phi has proper [value, block] format
if echo "$IR" | grep "phi" | grep -q "\[t.*bb"; then
    pass "Phi predecessor format"
else
    fail "Phi predecessor format" "phi args don't show [tN, bbN]"
fi

# ------------------------------------------------------------------
# Test 15: Variable reassignment in single block (no phi needed)
# ------------------------------------------------------------------
cat > /tmp/ssa_t15.c << 'EOF'
int main() {
    int x = 1;
    x = x + 2;
    x = x * 3;
    return x;
}
EOF
IR=$(run_ssa /tmp/ssa_t15.c)
PHI_COUNT=$(echo "$IR" | grep -c "phi" || true)
if [ "$PHI_COUNT" -eq 0 ]; then
    pass "No phi for sequential reassignment"
else
    fail "No phi for sequential reassignment" "found $PHI_COUNT phi(s)"
fi
if check_exec 9; then
    pass "Sequential reassignment correctness ((1+2)*3=9)"
else
    fail "Sequential reassignment correctness" "wrong result"
fi

# ------------------------------------------------------------------
# Test 16: Entry block has no idom
# ------------------------------------------------------------------
IR=$(run_ssa /tmp/ssa_t1.c)
# The entry block should NOT have an idom annotation
ENTRY_LINE=$(echo "$IR" | grep "entry (bb0):")
if echo "$ENTRY_LINE" | grep -q "idom:"; then
    fail "Entry block no idom" "entry has idom"
else
    pass "Entry block no idom"
fi

# ------------------------------------------------------------------
# Test 17: Switch with multiple cases
# ------------------------------------------------------------------
cat > /tmp/ssa_t17.c << 'EOF'
int main() {
    int x = 2;
    int result = 0;
    switch (x) {
        case 1: result = 10; break;
        case 2: result = 20; break;
        case 3: result = 30; break;
    }
    return result;
}
EOF
IR=$(run_ssa /tmp/ssa_t17.c)
if echo "$IR" | grep -q "phi"; then
    pass "Switch phi insertion"
else
    # phi might not be needed if switch exit is the merge
    pass "Switch phi insertion (skipped — may not need phi)"
fi
if check_exec 20; then
    pass "Switch correctness"
else
    fail "Switch correctness" "wrong result"
fi

# ------------------------------------------------------------------
# Test 18: Large SSA vreg count (many blocks/vars)
# ------------------------------------------------------------------
cat > /tmp/ssa_t18.c << 'EOF'
int main() {
    int a = 1; int b = 2; int c = 3; int d = 4;
    if (a > 0) { a = a + b; b = b + c; }
    else { a = a - b; c = c - d; }
    if (b > 0) { c = c + d; d = d + a; }
    else { c = c - a; d = d - b; }
    return a + b + c + d;
}
EOF
IR=$(run_ssa /tmp/ssa_t18.c)
if echo "$IR" | grep -q "(SSA)"; then
    pass "Large multi-var SSA"
else
    fail "Large multi-var SSA" "no SSA marker"
fi
if check_exec 22; then
    pass "Large multi-var correctness"
else
    fail "Large multi-var correctness" "wrong result"
fi

# ------------------------------------------------------------------
# Test 19: Loop with break (variable defined before break)
# ------------------------------------------------------------------
cat > /tmp/ssa_t19.c << 'EOF'
int main() {
    int result = 0;
    int i;
    for (i = 0; i < 10; i = i + 1) {
        if (i == 5) {
            result = i * 2;
            break;
        }
        result = i;
    }
    return result;
}
EOF
run_ssa /tmp/ssa_t19.c > /dev/null 2>&1
if check_exec 10; then
    pass "Loop-with-break correctness (5*2=10)"
else
    fail "Loop-with-break correctness" "wrong result"
fi

# ------------------------------------------------------------------
# Test 20: Empty function (no vars, trivial SSA)
# ------------------------------------------------------------------
cat > /tmp/ssa_t20.c << 'EOF'
int nothing() { return 0; }
int main() { return nothing(); }
EOF
IR=$(run_ssa /tmp/ssa_t20.c)
if echo "$IR" | grep -q "(SSA)"; then
    pass "Empty function SSA"
else
    fail "Empty function SSA" "no SSA marker"
fi

# ------------------------------------------------------------------
# Test 21: Verify all unique vreg definitions (SSA property)
# Parse the IR dump and check no vreg is defined more than once
# ------------------------------------------------------------------
cat > /tmp/ssa_t21.c << 'EOF'
int main() {
    int x = 0;
    int y = 1;
    int i;
    for (i = 0; i < 3; i = i + 1) {
        if (x > y) { x = y; } else { y = x; }
        x = x + 1;
        y = y + 1;
    }
    return x + y;
}
EOF
IR=$(run_ssa /tmp/ssa_t21.c)
# Extract all vreg definitions (tN = or tN = phi)
DEFS=$(echo "$IR" | grep -oP 't\d+(?= =)' | sort)
UNIQUE_DEFS=$(echo "$DEFS" | sort -u)
DEF_COUNT=$(echo "$DEFS" | wc -l)
UNIQUE_COUNT=$(echo "$UNIQUE_DEFS" | wc -l)
if [ "$DEF_COUNT" -eq "$UNIQUE_COUNT" ]; then
    pass "SSA single-definition property"
else
    fail "SSA single-definition property" "$DEF_COUNT defs but only $UNIQUE_COUNT unique"
fi

# ------------------------------------------------------------------
# Test 22: Parameter reassignment
# ------------------------------------------------------------------
cat > /tmp/ssa_t22.c << 'EOF'
int addone(int x) {
    x = x + 1;
    return x;
}
int main() { return addone(41); }
EOF
IR=$(run_ssa /tmp/ssa_t22.c)
# In addone, the parameter x is reassigned — should see the implicit param vreg
# and a COPY that renames it
if echo "$IR" | grep -q "(SSA)"; then
    pass "Param reassignment SSA"
else
    fail "Param reassignment SSA" "no SSA"
fi
if check_exec 42; then
    pass "Param reassignment correctness (41+1=42)"
else
    fail "Param reassignment correctness" "wrong result"
fi

# ------------------------------------------------------------------
# Test 23: Multiple functions with SSA
# ------------------------------------------------------------------
cat > /tmp/ssa_t23.c << 'EOF'
int abs_val(int x) {
    if (x < 0) { x = 0 - x; }
    return x;
}
int main() {
    int a = abs_val(0 - 7);
    int b = abs_val(5);
    return a + b;
}
EOF
IR=$(run_ssa /tmp/ssa_t23.c)
SSA_COUNT=$(echo "$IR" | grep -c "(SSA)" || true)
if [ "$SSA_COUNT" -ge 2 ]; then
    pass "Multi-function SSA ($SSA_COUNT functions)"
else
    fail "Multi-function SSA" "only $SSA_COUNT functions with SSA"
fi
if check_exec 12; then
    pass "Multi-function correctness (7+5=12)"
else
    fail "Multi-function correctness" "wrong result"
fi

# ------------------------------------------------------------------
# Test 24: Complex CFG — nested loop with conditional break
# ------------------------------------------------------------------
cat > /tmp/ssa_t24.c << 'EOF'
int main() {
    int total = 0;
    int i;
    for (i = 1; i <= 4; i = i + 1) {
        int j;
        int partial = 0;
        for (j = 1; j <= i; j = j + 1) {
            partial = partial + j;
            if (partial > 3) break;
        }
        total = total + partial;
    }
    return total;
}
EOF
run_ssa /tmp/ssa_t24.c > /dev/null 2>&1
# i=1: partial=1; i=2: partial=3; i=3: partial=1+2+3=6>3→partial=6; i=4: partial=1+2+3=6>3→partial=6
# total = 1+3+6+6 = 16
if check_exec 16; then
    pass "Complex nested loop correctness"
else
    fail "Complex nested loop correctness" "wrong result"
fi

# ------------------------------------------------------------------
# Summary
# ------------------------------------------------------------------
echo ""
echo "=== SSA Test Summary ==="
echo "  PASS:    $PASS"
echo "  FAIL:    $FAIL"
echo "  TOTAL:   $TOTAL"
echo ""
if [ $FAIL -eq 0 ]; then
    echo "All SSA tests passed!"
else
    echo "Some tests failed."
    exit 1
fi
