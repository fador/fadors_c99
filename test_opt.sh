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

# ---- 10. Tail Call Optimization (-O2) ----
echo ""
echo "--- Tail Call Optimization ---"

# Test: simple tail call to another function
# (multi-statement body so it won't be inlined)
cat > "$TMPDIR/tco1.c" << 'EOF'
int helper(int x) {
    int y = x + 1;
    return y;
}
int tail_caller(int a) {
    return helper(a + 5);
}
int main() {
    return tail_caller(3);
}
EOF
check_exit "tco_simple" "$TMPDIR/tco1.c" 9 "-O2"

# Verify jmp is used instead of call at -O2
"$CC" -O2 -S -o "$TMPDIR/tco1.s" "$TMPDIR/tco1.c" 2>/dev/null
if grep -q "jmp helper" "$TMPDIR/tco1.s"; then
    pass "tco_simple_asm: jmp helper found"
else
    fail "tco_simple_asm: expected jmp helper in output"
fi

# Verify call is used at -O1 (no TCO)
"$CC" -O1 -S -o "$TMPDIR/tco1_o1.s" "$TMPDIR/tco1.c" 2>/dev/null
if grep -q "call helper" "$TMPDIR/tco1_o1.s"; then
    pass "tco_no_tco_O1: call helper at -O1"
else
    fail "tco_no_tco_O1: expected call helper at -O1"
fi

# Test: self-recursive tail call (factorial with accumulator)
cat > "$TMPDIR/tco2.c" << 'EOF'
int factorial(int n, int acc) {
    if (n <= 1) return acc;
    return factorial(n - 1, n * acc);
}
int main() {
    return factorial(5, 1);
}
EOF
check_exit "tco_recursive" "$TMPDIR/tco2.c" 120 "-O2"

# Verify self-recursive jmp
"$CC" -O2 -S -o "$TMPDIR/tco2.s" "$TMPDIR/tco2.c" 2>/dev/null
if grep -q "jmp factorial" "$TMPDIR/tco2.s"; then
    pass "tco_recursive_asm: jmp factorial found"
else
    fail "tco_recursive_asm: expected jmp factorial in output"
fi

# Test: tail call with multiple args
cat > "$TMPDIR/tco3.c" << 'EOF'
int add3(int a, int b, int c) {
    return a + b + c;
}
int wrapper(int x) {
    return add3(x, x + 1, x + 2);
}
int main() {
    return wrapper(10);
}
EOF
check_exit "tco_multi_args" "$TMPDIR/tco3.c" 33 "-O2"

# Test: non-tail call (expression after call) should NOT be TCO'd
# (multi-statement body so it won't be inlined)
cat > "$TMPDIR/tco4.c" << 'EOF'
int square(int x) {
    int y = x * x;
    return y;
}
int not_tail(int a) {
    return square(a) + 1;
}
int main() {
    return not_tail(5);
}
EOF
check_exit "tco_non_tail" "$TMPDIR/tco4.c" 26 "-O2"

# Verify non-tail call still uses "call" not "jmp"
"$CC" -O2 -S -o "$TMPDIR/tco4.s" "$TMPDIR/tco4.c" 2>/dev/null
if grep -q "call square" "$TMPDIR/tco4.s"; then
    pass "tco_non_tail_asm: call square (not optimized)"
else
    fail "tco_non_tail_asm: expected call square"
fi

# Test: chain of tail calls
cat > "$TMPDIR/tco5.c" << 'EOF'
int step3(int x) { return x; }
int step2(int x) { return step3(x + 10); }
int step1(int x) { return step2(x + 20); }
int main() { return step1(5); }
EOF
check_exit "tco_chain" "$TMPDIR/tco5.c" 35 "-O2"

# Test: O2 constant propagation + tail call combined
cat > "$TMPDIR/tco6.c" << 'EOF'
int identity(int x) { return x; }
int main() {
    int a = 42;
    return identity(a);
}
EOF
check_exit "tco_with_constprop" "$TMPDIR/tco6.c" 42 "-O2"

# ---- 11. Function Inlining (-O2) ----
echo ""
echo "--- Function Inlining ---"

# Test: basic inlining of single-return function
cat > "$TMPDIR/inl1.c" << 'EOF'
int square(int x) { return x * x; }
int main() {
    return square(5);
}
EOF
check_exit "inline_basic" "$TMPDIR/inl1.c" 25 "-O2"

# Verify no call to square at -O2 (inlined away)
"$CC" -O2 -S -o "$TMPDIR/inl1.s" "$TMPDIR/inl1.c" 2>/dev/null
if ! grep -q "call square" "$TMPDIR/inl1.s"; then
    pass "inline_basic_asm: square inlined (no call)"
else
    fail "inline_basic_asm: call square still present"
fi

# Test: nested inline (add(square(3), square(4)))
cat > "$TMPDIR/inl2.c" << 'EOF'
int square(int x) { return x * x; }
int add(int a, int b) { return a + b; }
int main() {
    return add(square(3), square(4));
}
EOF
check_exit "inline_nested" "$TMPDIR/inl2.c" 25 "-O2"

# Test: inlining + constant folding
cat > "$TMPDIR/inl3.c" << 'EOF'
int double_it(int x) { return x + x; }
int main() {
    return double_it(21);
}
EOF
check_exit "inline_constfold" "$TMPDIR/inl3.c" 42 "-O2"

# Verify constant folded to immediate
"$CC" -O2 -S -o "$TMPDIR/inl3.s" "$TMPDIR/inl3.c" 2>/dev/null
if grep -q 'mov \$42' "$TMPDIR/inl3.s"; then
    pass "inline_constfold_asm: folded to mov \$42"
else
    fail "inline_constfold_asm: expected mov \$42"
fi

# Test: function with side-effect argument should NOT be inlined
# (multi-use param, side-effect arg)
cat > "$TMPDIR/inl4.c" << 'EOF'
int double_val(int x) { return x + x; }
int g;
int bump() { g = g + 1; return g; }
int main() {
    g = 10;
    int r = double_val(bump());
    return r;
}
EOF
check_exit "inline_sideeffect" "$TMPDIR/inl4.c" 22 "-O2"

# Test: inlining should NOT happen for multi-statement functions
cat > "$TMPDIR/inl5.c" << 'EOF'
int compute(int x) {
    int y = x * 2;
    return y + 1;
}
int main() {
    return compute(10);
}
EOF
check_exit "inline_no_multi" "$TMPDIR/inl5.c" 21 "-O2"

# Verify multi-statement function is NOT inlined
"$CC" -O2 -S -o "$TMPDIR/inl5.s" "$TMPDIR/inl5.c" 2>/dev/null
if grep -q "call compute" "$TMPDIR/inl5.s" || grep -q "jmp compute" "$TMPDIR/inl5.s"; then
    pass "inline_no_multi_asm: compute not inlined"
else
    fail "inline_no_multi_asm: compute was unexpectedly inlined"
fi

# ---- 12. Inline Hinting (GCC/MSVC style) ----
echo ""
echo "--- Inline Hinting ---"

# Test: __forceinline at -O0 should inline
cat > "$TMPDIR/hint1.c" << 'EOF'
__forceinline int square(int x) { return x * x; }
int main() {
    return square(7);
}
EOF
check_exit "forceinline_O0" "$TMPDIR/hint1.c" 49 "-O0"

# Verify __forceinline eliminates call at -O0
"$CC" -O0 -S -o "$TMPDIR/hint1.s" "$TMPDIR/hint1.c" 2>/dev/null
if ! grep -q "call square" "$TMPDIR/hint1.s"; then
    pass "forceinline_O0_asm: square inlined at -O0"
else
    fail "forceinline_O0_asm: square not inlined at -O0"
fi

# Test: __attribute__((always_inline)) at -O0
cat > "$TMPDIR/hint2.c" << 'EOF'
__attribute__((always_inline)) int cube(int x) { return x * x * x; }
int main() {
    return cube(3);
}
EOF
check_exit "always_inline_O0" "$TMPDIR/hint2.c" 27 "-O0"

# Test: __attribute__((noinline)) at -O2 should prevent inlining
cat > "$TMPDIR/hint3.c" << 'EOF'
__attribute__((noinline)) int add_one(int x) { return x + 1; }
int main() {
    return add_one(41);
}
EOF
check_exit "noinline_O2" "$TMPDIR/hint3.c" 42 "-O2"

# Verify noinline keeps call/jmp
"$CC" -O2 -S -o "$TMPDIR/hint3.s" "$TMPDIR/hint3.c" 2>/dev/null
if grep -q "call add_one\|jmp add_one" "$TMPDIR/hint3.s"; then
    pass "noinline_O2_asm: add_one not inlined"
else
    fail "noinline_O2_asm: add_one was inlined despite noinline"
fi

# Test: __declspec(noinline) at -O2
cat > "$TMPDIR/hint4.c" << 'EOF'
__declspec(noinline) int double_val(int x) { return x + x; }
int main() {
    return double_val(21);
}
EOF
check_exit "declspec_noinline" "$TMPDIR/hint4.c" 42 "-O2"

# Verify __declspec(noinline) keeps call/jmp
"$CC" -O2 -S -o "$TMPDIR/hint4.s" "$TMPDIR/hint4.c" 2>/dev/null
if grep -q "call double_val\|jmp double_val" "$TMPDIR/hint4.s"; then
    pass "declspec_noinline_asm: double_val not inlined"
else
    fail "declspec_noinline_asm: double_val inlined despite __declspec(noinline)"
fi

# Test: __inline at -O1 should inline (hint=1)
cat > "$TMPDIR/hint5.c" << 'EOF'
__inline int triple(int x) { return x + x + x; }
int plain(int x) { return x + 1; }
int main() {
    return triple(5) + plain(3);
}
EOF
check_exit "inline_hint_O1" "$TMPDIR/hint5.c" 19 "-O1"

# Verify __inline inlined but plain not at -O1
"$CC" -O1 -S -o "$TMPDIR/hint5.s" "$TMPDIR/hint5.c" 2>/dev/null
if grep -q "call plain" "$TMPDIR/hint5.s" && ! grep -q "call triple" "$TMPDIR/hint5.s"; then
    pass "inline_hint_O1_asm: triple inlined, plain called"
else
    fail "inline_hint_O1_asm: unexpected inline behavior"
fi

# Test: __inline__ (GCC alternate) at -O1
cat > "$TMPDIR/hint6.c" << 'EOF'
__inline__ int negate(int x) { return 0 - x; }
int main() {
    return negate(0 - 42);
}
EOF
check_exit "gcc_inline_O1" "$TMPDIR/hint6.c" 42 "-O1"

# Test: post-parameter __attribute__((always_inline)) on definition
cat > "$TMPDIR/hint7.c" << 'EOF'
int square(int x) __attribute__((always_inline)) { return x * x; }
int main() {
    return square(8);
}
EOF
check_exit "postattr_always_inline" "$TMPDIR/hint7.c" 64 "-O0"

# ---- LEA for multiply-add patterns ----
echo "--- LEA Multiply-Add ---"

# Test: x*3 uses LEA instead of imul
cat > "$TMPDIR/lea1.c" << 'EOF'
int multiply3(int x) { return x * 3; }
int main() {
    return multiply3(14);
}
EOF
check_exit "lea_mul3" "$TMPDIR/lea1.c" 42 "-O2"

"$CC" -O2 -S -o "$TMPDIR/lea1.s" "$TMPDIR/lea1.c" 2>/dev/null
if grep -q 'lea' "$TMPDIR/lea1.s" && ! grep -q 'imull\s*\$3' "$TMPDIR/lea1.s"; then
    pass "lea_mul3_asm: uses LEA instead of imul $3"
else
    fail "lea_mul3_asm: expected LEA for x*3"
fi

# Test: x*5 uses LEA
cat > "$TMPDIR/lea2.c" << 'EOF'
int multiply5(int x) { return x * 5; }
int main() {
    return multiply5(9);
}
EOF
check_exit "lea_mul5" "$TMPDIR/lea2.c" 45 "-O2"

"$CC" -O2 -S -o "$TMPDIR/lea2.s" "$TMPDIR/lea2.c" 2>/dev/null
if grep -q 'lea' "$TMPDIR/lea2.s" && ! grep -q 'imull\s*\$5' "$TMPDIR/lea2.s"; then
    pass "lea_mul5_asm: uses LEA instead of imul $5"
else
    fail "lea_mul5_asm: expected LEA for x*5"
fi

# Test: x*9 uses LEA
cat > "$TMPDIR/lea3.c" << 'EOF'
int multiply9(int x) { return x * 9; }
int main() {
    return multiply9(5);
}
EOF
check_exit "lea_mul9" "$TMPDIR/lea3.c" 45 "-O2"

"$CC" -O2 -S -o "$TMPDIR/lea3.s" "$TMPDIR/lea3.c" 2>/dev/null
if grep -q 'lea' "$TMPDIR/lea3.s" && ! grep -q 'imull\s*\$9' "$TMPDIR/lea3.s"; then
    pass "lea_mul9_asm: uses LEA instead of imul $9"
else
    fail "lea_mul9_asm: expected LEA for x*9"
fi

# Test: x*7 still uses imul (not a single-LEA pattern)
cat > "$TMPDIR/lea4.c" << 'EOF'
int multiply7(int x) { return x * 7; }
int main() {
    return multiply7(6);
}
EOF
check_exit "lea_mul7" "$TMPDIR/lea4.c" 42 "-O2"

"$CC" -O2 -S -o "$TMPDIR/lea4.s" "$TMPDIR/lea4.c" 2>/dev/null
if grep -q 'imull\s*\$7' "$TMPDIR/lea4.s"; then
    pass "lea_mul7_asm: x*7 correctly uses imul (not single-LEA)"
else
    fail "lea_mul7_asm: x*7 should use imul"
fi

# ---- test instead of cmp $0 ----
echo "--- test vs cmp \$0 ---"

cat > "$TMPDIR/test1.c" << 'EOF'
int check(int x) {
    if (x == 0) return 1;
    return 2;
}
int main() {
    return check(0) + check(5) * 10;
}
EOF
check_exit "test_cmp0" "$TMPDIR/test1.c" 21 "-O2"

"$CC" -O2 -S -o "$TMPDIR/test1.s" "$TMPDIR/test1.c" 2>/dev/null
if grep -q 'testl' "$TMPDIR/test1.s" && ! grep -q 'cmpl\s*\$0' "$TMPDIR/test1.s"; then
    pass "test_cmp0_asm: uses testl instead of cmpl $0"
else
    fail "test_cmp0_asm: expected testl instead of cmpl $0"
fi

# ---- Conditional move (cmov) ----
echo "--- Conditional Move ---"

cat > "$TMPDIR/cmov1.c" << 'EOF'
int main() {
    int a = 10;
    int b = 20;
    int x = (a > 5) ? 42 : 99;
    return x;
}
EOF
check_exit "cmov_ternary" "$TMPDIR/cmov1.c" 42 "-O2"

"$CC" -O2 -S -o "$TMPDIR/cmov1.s" "$TMPDIR/cmov1.c" 2>/dev/null
if grep -q 'cmov' "$TMPDIR/cmov1.s"; then
    pass "cmov_ternary_asm: uses cmov for simple ternary"
else
    fail "cmov_ternary_asm: expected cmov for simple ternary"
fi

# Test: cmov with variable condition
cat > "$TMPDIR/cmov2.c" << 'EOF'
int select(int cond, int a, int b) {
    return cond ? a : b;
}
int main() {
    return select(1, 42, 99);
}
EOF
check_exit "cmov_var" "$TMPDIR/cmov2.c" 42 "-O2"

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
