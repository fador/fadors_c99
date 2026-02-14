#!/bin/bash
# Phase 3: -O1 Optimization Verification
# Tests that optimizations produce correct results and actually improve code.
#
# Usage: ./test_opt.sh [compiler_path]

CC="${1:-build_linux/fadors99}"
TMPDIR=$(mktemp -d)
PASS=0
FAIL=0

cleanup() { rm -rf "$TMPDIR"; }
trap cleanup EXIT

pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

echo "=== Phase 3: -O1 Optimization Tests ==="
echo "Compiler: $CC"
echo ""

# ---- Helper: compile, run, check exit code ----
check_exit() {
    local name="$1" src="$2" expected="$3" flags="$4"
    "$CC" $flags "$src" -o "$TMPDIR/$name" 2>/dev/null
    "$TMPDIR/$name"
    local actual=$?
    if [ "$actual" -eq "$expected" ]; then
        pass "$name: exit=$actual (expected $expected)"
    else
        fail "$name: exit=$actual (expected $expected)"
    fi
}

# ---- 1. Constant Folding ----
echo "--- Constant Folding ---"

cat > "$TMPDIR/cf1.c" << 'EOF'
int main() {
    int x = 3 + 4;
    int y = 10 * 5 - 2;
    int z = 100 / 4;
    int w = 17 % 5;
    return x + y + z + w;
}
EOF
check_exit "cf_basic" "$TMPDIR/cf1.c" 82 "-O1"

cat > "$TMPDIR/cf2.c" << 'EOF'
int main() {
    int x = (2 + 3) * (4 - 1);
    return x;
}
EOF
check_exit "cf_nested" "$TMPDIR/cf2.c" 15 "-O1"

cat > "$TMPDIR/cf3.c" << 'EOF'
int main() {
    int a = 10 > 5;
    int b = 3 == 3;
    int c = 7 != 7;
    int d = 4 <= 4;
    return a + b + c + d;
}
EOF
check_exit "cf_comparisons" "$TMPDIR/cf3.c" 3 "-O1"

cat > "$TMPDIR/cf4.c" << 'EOF'
int main() {
    int x = -(-5);
    int y = !0;
    int z = ~(~7);
    return x + y + z;
}
EOF
check_exit "cf_unary" "$TMPDIR/cf4.c" 13 "-O1"

cat > "$TMPDIR/cf5.c" << 'EOF'
int main() {
    int x = (0xFF & 0x0F) | 0x30;
    int y = 1 << 3;
    int z = 64 >> 2;
    return x + y + z;
}
EOF
check_exit "cf_bitwise" "$TMPDIR/cf5.c" 87 "-O1"

# ---- 2. Strength Reduction ----
echo ""
echo "--- Strength Reduction ---"

cat > "$TMPDIR/sr1.c" << 'EOF'
int main() {
    int a = 5;
    int b = a * 4;
    int c = a * 8;
    int d = a * 2;
    return b + c + d;
}
EOF
check_exit "sr_mul_pow2" "$TMPDIR/sr1.c" 70 "-O1"

cat > "$TMPDIR/sr2.c" << 'EOF'
int main() {
    int a = 100;
    int b = a % 16;
    int c = a % 8;
    int d = a % 4;
    return b + c + d;
}
EOF
check_exit "sr_mod_pow2" "$TMPDIR/sr2.c" 8 "-O1"

# ---- 3. Dead Code Elimination ----
echo ""
echo "--- Dead Code Elimination ---"

cat > "$TMPDIR/dc1.c" << 'EOF'
int main() {
    int x = 10;
    int y = 20;
    return x + y;
    int z = 99;
    return z;
}
EOF
check_exit "dc_after_return" "$TMPDIR/dc1.c" 30 "-O1"

cat > "$TMPDIR/dc2.c" << 'EOF'
int main() {
    if (0) {
        return 99;
    }
    return 42;
}
EOF
check_exit "dc_if_false" "$TMPDIR/dc2.c" 42 "-O1"

cat > "$TMPDIR/dc3.c" << 'EOF'
int main() {
    if (1) {
        return 10;
    } else {
        return 99;
    }
}
EOF
check_exit "dc_if_true" "$TMPDIR/dc3.c" 10 "-O1"

cat > "$TMPDIR/dc4.c" << 'EOF'
int main() {
    while (0) {
        return 99;
    }
    return 7;
}
EOF
check_exit "dc_while_false" "$TMPDIR/dc4.c" 7 "-O1"

# ---- 4. Algebraic Simplification ----
echo ""
echo "--- Algebraic Simplification ---"

cat > "$TMPDIR/alg1.c" << 'EOF'
int main() {
    int a = 7;
    int b = a + 0;
    int c = a * 1;
    int d = 0 + a;
    int e = a - 0;
    int f = a / 1;
    int g = a | 0;
    int h = a ^ 0;
    return b + c + d + e + f + g + h;
}
EOF
check_exit "alg_identity" "$TMPDIR/alg1.c" 49 "-O1"

cat > "$TMPDIR/alg2.c" << 'EOF'
int main() {
    int a = 7;
    int b = a * 0;
    int c = 0 * a;
    int d = a & 0;
    return b + c + d;
}
EOF
check_exit "alg_annihilator" "$TMPDIR/alg2.c" 0 "-O1"

# ---- 5. Code Size Reduction ----
echo ""
echo "--- Code Size Check ---"

# Constant folding should reduce text size
"$CC" "$TMPDIR/cf1.c" -o "$TMPDIR/cf1_o0" 2>&1 | grep -oP 'text=\K[0-9]+' > "$TMPDIR/sz0"
"$CC" -O1 "$TMPDIR/cf1.c" -o "$TMPDIR/cf1_o1" 2>&1 | grep -oP 'text=\K[0-9]+' > "$TMPDIR/sz1"
SIZE_O0=$(cat "$TMPDIR/sz0")
SIZE_O1=$(cat "$TMPDIR/sz1")
if [ "$SIZE_O1" -lt "$SIZE_O0" ]; then
    pass "O1 text smaller than O0 ($SIZE_O1 < $SIZE_O0)"
else
    fail "O1 text not smaller ($SIZE_O1 >= $SIZE_O0)"
fi

# Dead code should reduce text size
"$CC" "$TMPDIR/dc1.c" -o "$TMPDIR/dc1_o0" 2>&1 | grep -oP 'text=\K[0-9]+' > "$TMPDIR/sz0"
"$CC" -O1 "$TMPDIR/dc1.c" -o "$TMPDIR/dc1_o1" 2>&1 | grep -oP 'text=\K[0-9]+' > "$TMPDIR/sz1"
SIZE_O0=$(cat "$TMPDIR/sz0")
SIZE_O1=$(cat "$TMPDIR/sz1")
if [ "$SIZE_O1" -le "$SIZE_O0" ]; then
    pass "Dead code O1 text <= O0 ($SIZE_O1 <= $SIZE_O0)"
else
    fail "Dead code O1 text > O0 ($SIZE_O1 > $SIZE_O0)"
fi

# ---- 6. O0 still works (no regressions) ----
echo ""
echo "--- O0 Correctness (no regressions) ---"

check_exit "cf_basic_o0" "$TMPDIR/cf1.c" 82 ""
check_exit "sr_mul_o0" "$TMPDIR/sr1.c" 70 ""
check_exit "dc_return_o0" "$TMPDIR/dc1.c" 30 ""
check_exit "alg_ident_o0" "$TMPDIR/alg1.c" 49 ""

# ---- 7. Verify xor-zero optimization in generated code ----
echo ""
echo "--- Zero-Init Optimization ---"

cat > "$TMPDIR/zero.c" << 'EOF'
int main() {
    int x = 0;
    return x;
}
EOF
"$CC" -O1 "$TMPDIR/zero.c" -o "$TMPDIR/zero_o1" 2>/dev/null
# Search for xor eax,eax (31 c0) byte pattern in the binary
if xxd "$TMPDIR/zero_o1" | grep -q "31 c0"; then
    pass "xor eax,eax (31 c0) found for zero-init"
else
    fail "xor not found for zero-init"
fi

# ---- 8. Immediate Operand Optimization ----
echo ""
echo "--- Immediate Operand Optimization ---"

# Test: add with immediate — should use add $imm,%eax instead of push/pop
cat > "$TMPDIR/imm1.c" << 'EOF'
int main() {
    int x = 10;
    int y = x + 5;
    int z = x - 3;
    int w = x * 2;
    return y + z + w;
}
EOF
check_exit "imm_arith" "$TMPDIR/imm1.c" 42 "-O1"

# Check that O1 generates smaller code (no push/pop for constants)
"$CC" "$TMPDIR/imm1.c" -o "$TMPDIR/imm1_o0" 2>&1 | grep -oP 'text=\K[0-9]+' > "$TMPDIR/isz0"
"$CC" -O1 "$TMPDIR/imm1.c" -o "$TMPDIR/imm1_o1" 2>&1 | grep -oP 'text=\K[0-9]+' > "$TMPDIR/isz1"
IMM_O0=$(cat "$TMPDIR/isz0")
IMM_O1=$(cat "$TMPDIR/isz1")
if [ "$IMM_O1" -lt "$IMM_O0" ]; then
    pass "Immediate opt smaller code ($IMM_O1 < $IMM_O0)"
else
    fail "Immediate opt not smaller ($IMM_O1 >= $IMM_O0)"
fi

# Test: comparison with immediate
cat > "$TMPDIR/imm2.c" << 'EOF'
int main() {
    int x = 7;
    int a = (x > 3);
    int b = (x == 7);
    int c = (x < 10);
    int d = (x != 5);
    return a + b + c + d;
}
EOF
check_exit "imm_compare" "$TMPDIR/imm2.c" 4 "-O1"

# Test: bitwise with immediate
cat > "$TMPDIR/imm3.c" << 'EOF'
int main() {
    int x = 0xFF;
    int a = x & 0x0F;
    int b = x | 0x100;
    int c = x ^ 0x55;
    int d = x << 2;
    int e = x >> 4;
    return a + (b & 0xFF) + c + d + e;
}
EOF
check_exit "imm_bitwise" "$TMPDIR/imm3.c" 195 "-O1"

# Test: verify assembly uses immediate operands (no pushq/popq around constant ops)
cat > "$TMPDIR/imm4.c" << 'EOF'
int main() {
    int x = 10;
    return x + 5;
}
EOF
"$CC" -O1 -S "$TMPDIR/imm4.c" -o "$TMPDIR/imm4.s" 2>/dev/null
if grep -qE 'add[l]? \$5' "$TMPDIR/imm4.s" 2>/dev/null; then
    pass "Assembly uses add \$5 immediate"
else
    fail "Assembly missing immediate operand"
fi

# ---- 9. Branch Optimization ----
echo ""
echo "--- Branch Optimization ---"

# Test: if-else correctness with -O1
cat > "$TMPDIR/br1.c" << 'EOF'
int main() {
    int x = 5;
    int r;
    if (x > 3) {
        r = 10;
    } else {
        r = 20;
    }
    if (x < 2) {
        r = r + 100;
    }
    return r;
}
EOF
check_exit "br_if_else" "$TMPDIR/br1.c" 10 "-O1"

# Test: switch correctness with -O1
cat > "$TMPDIR/br2.c" << 'EOF'
int main() {
    int x = 3;
    int r = 0;
    switch (x) {
        case 1: r = 10; break;
        case 2: r = 20; break;
        case 3: r = 30; break;
        case 4: r = 40; break;
        default: r = 50; break;
    }
    return r;
}
EOF
check_exit "br_switch" "$TMPDIR/br2.c" 30 "-O1"

# Test: nested if-else with -O1
cat > "$TMPDIR/br3.c" << 'EOF'
int main() {
    int x = 7;
    int r = 0;
    if (x > 10) {
        r = 1;
    } else if (x > 5) {
        r = 2;
    } else if (x > 0) {
        r = 3;
    } else {
        r = 4;
    }
    return r;
}
EOF
check_exit "br_nested_if" "$TMPDIR/br3.c" 2 "-O1"

# Test: while loop with -O1
cat > "$TMPDIR/br4.c" << 'EOF'
int main() {
    int i = 0;
    int sum = 0;
    while (i < 10) {
        sum = sum + i;
        i = i + 1;
    }
    return sum;
}
EOF
check_exit "br_while_loop" "$TMPDIR/br4.c" 45 "-O1"

# Test: for loop with -O1
cat > "$TMPDIR/br5.c" << 'EOF'
int main() {
    int sum = 0;
    for (int i = 1; i <= 5; i = i + 1) {
        sum = sum + i * i;
    }
    return sum;
}
EOF
check_exit "br_for_loop" "$TMPDIR/br5.c" 55 "-O1"

# ---- Summary ----
echo ""
echo "=== Results ==="
echo "  PASS:    $PASS"
echo "  FAIL:    $FAIL"
echo "  TOTAL:   $((PASS + FAIL))"
echo ""

if [ $FAIL -eq 0 ]; then
    echo "All optimization tests passed!"
    exit 0
else
    echo "Some tests failed."
    exit 1
fi
