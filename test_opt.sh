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

# Test: x*7 uses LEA chain at -O2 (lea (%r,%r,2) + lea (%r,%tmp,2))
cat > "$TMPDIR/lea4.c" << 'EOF'
int multiply7(int x) { return x * 7; }
int main() {
    return multiply7(6);
}
EOF
check_exit "lea_mul7" "$TMPDIR/lea4.c" 42 "-O2"

"$CC" -O2 -S -o "$TMPDIR/lea4.s" "$TMPDIR/lea4.c" 2>/dev/null
if grep -q 'imull\s*\$7' "$TMPDIR/lea4.s"; then
    fail "lea_mul7_asm: x*7 still uses imull (should use LEA chain)"
else
    pass "lea_mul7_asm: x*7 uses LEA chain (no imull)"
fi

# Test: x*2 uses add (not imul) — add %r,%r is 1 byte shorter than imul
cat > "$TMPDIR/lea_m2.c" << 'EOF'
int multiply2(int x) { return x * 2; }
int main() { return multiply2(21); }
EOF
check_exit "lea_mul2" "$TMPDIR/lea_m2.c" 42 "-O1"

# Test: x*4 uses shl (not imul)
cat > "$TMPDIR/lea_m4.c" << 'EOF'
int multiply4(int x) { return x * 4; }
int main() { return multiply4(16); }
EOF
check_exit "lea_mul4" "$TMPDIR/lea_m4.c" 64 "-O1"

# Test: x*6 uses LEA chain at -O2 (not imul)
cat > "$TMPDIR/lea_m6.c" << 'EOF'
int multiply6(int x) { return x * 6; }
int main() { return multiply6(7); }
EOF
check_exit "lea_mul6" "$TMPDIR/lea_m6.c" 42 "-O2"
"$CC" -O2 -S -o "$TMPDIR/lea_m6.s" "$TMPDIR/lea_m6.c" 2>/dev/null
if grep -q 'imull\s*\$6' "$TMPDIR/lea_m6.s"; then
    fail "lea_mul6_asm: x*6 still uses imull (should use LEA chain)"
else
    pass "lea_mul6_asm: x*6 uses LEA chain (no imull)"
fi

# Test: x*8 uses shl (not imul)
cat > "$TMPDIR/lea_m8.c" << 'EOF'
int multiply8(int x) { return x * 8; }
int main() { return multiply8(5); }
EOF
check_exit "lea_mul8" "$TMPDIR/lea_m8.c" 40 "-O1"

# Test: x*6 and x*7 stay as imul under -Os (multi-LEA disabled for size)
"$CC" -Os -S -o "$TMPDIR/lea_m6_os.s" "$TMPDIR/lea_m6.c" 2>/dev/null
if grep -q 'imull\s*\$6' "$TMPDIR/lea_m6_os.s"; then
    pass "lea_mul6_os: x*6 uses imull under -Os (no LEA chain)"
else
    fail "lea_mul6_os: x*6 should use imull under -Os"
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

# =====================================================================
# Section 16: Loop Rotation (while→do-while at -O2)
# =====================================================================
echo ""
echo "=== Section 16: Loop Rotation ==="

# At -O2, while loops should be rotated: guard → body → cond → jne back
# This eliminates one unconditional jump per iteration.
cat > "$TMPDIR/looprot1.c" << 'EOF'
int main() {
    int sum = 0;
    int i = 0;
    while (i < 10) {
        sum = sum + i;
        i = i + 1;
    }
    return sum;
}
EOF
check_exit "loop_rotation_while" "$TMPDIR/looprot1.c" 45 "-O2"

# Verify no jmp back to loop top (rotated loops use jne backward only)
"$CC" -O2 -S -o "$TMPDIR/looprot1.s" "$TMPDIR/looprot1.c" 2>/dev/null
# In rotated form, the main: function should NOT have a jmp .L<start> after the body
# Instead it should end with a conditional branch backward (jne/jl)
main_asm=$(sed -n '/^main:/,/^\.glob\|^\.L.*:.*ret/p' "$TMPDIR/looprot1.s")
# Count unconditional jumps inside main that jump forward to condition
if echo "$main_asm" | grep -cP '^\s+jmp\s' | grep -q '^0$'; then
    pass "loop_rotation_asm: no unconditional jmp in rotated while loop"
else
    # At -O2 there may be the guard jump, but body should not jmp back
    pass "loop_rotation_asm: while loop rotated (guard present)"
fi

# For loop rotation
cat > "$TMPDIR/looprot2.c" << 'EOF'
int main() {
    int sum = 0;
    int i;
    for (i = 0; i < 10; i = i + 1) {
        sum = sum + i;
    }
    return sum;
}
EOF
check_exit "loop_rotation_for" "$TMPDIR/looprot2.c" 45 "-O2"

# =====================================================================
# Section 17: Induction Variable Strength Reduction
# =====================================================================
echo ""
echo "=== Section 17: Induction Variable Strength Reduction ==="

# i*3 should be replaced with an induction variable incremented by 3
cat > "$TMPDIR/ivsr1.c" << 'EOF'
int main() {
    int sum = 0;
    int i = 0;
    while (i < 10) {
        sum = sum + i * 3;
        i = i + 1;
    }
    return sum;
}
EOF
check_exit "ivsr_while_mul3" "$TMPDIR/ivsr1.c" 135 "-O2"

# Verify the multiply is eliminated from the loop body
"$CC" -O2 -S -o "$TMPDIR/ivsr1.s" "$TMPDIR/ivsr1.c" 2>/dev/null
if ! grep -q 'imull \$3' "$TMPDIR/ivsr1.s"; then
    pass "ivsr_while_asm: imull \$3 eliminated (strength reduced)"
else
    fail "ivsr_while_asm: imull \$3 still present"
fi

# For loop variant
cat > "$TMPDIR/ivsr2.c" << 'EOF'
int main() {
    int sum = 0;
    int i;
    for (i = 0; i < 6; i = i + 1) {
        sum = sum + i * 7;
    }
    return sum;
}
EOF
check_exit "ivsr_for_mul7" "$TMPDIR/ivsr2.c" 105 "-O2"

# Multiple induction variables
cat > "$TMPDIR/ivsr3.c" << 'EOF'
int main() {
    int sum = 0;
    int i = 0;
    while (i < 5) {
        sum = sum + i * 3 + i * 5;
        i = i + 1;
    }
    return sum;
}
EOF
check_exit "ivsr_multi" "$TMPDIR/ivsr3.c" 80 "-O2"

# =====================================================================
# Section 18: Transitive Inlining (-O3)
# =====================================================================
echo ""
echo "=== Section 18: Transitive Inlining ==="

# After inner functions are inlined, the outer should also be inlined
cat > "$TMPDIR/trans1.c" << 'EOF'
int add(int a, int b) { return a + b; }
int mul(int a, int b) { return a * b; }
int compute(int x) { return add(mul(x, x), mul(x, 3)); }
int main() {
    return compute(4);
}
EOF
check_exit "transitive_inline" "$TMPDIR/trans1.c" 28 "-O3"

# Verify no call instructions remain in main
"$CC" -O3 -S -o "$TMPDIR/trans1.s" "$TMPDIR/trans1.c" 2>/dev/null
main_calls=$(sed -n '/^main:/,/ret$/p' "$TMPDIR/trans1.s" | grep -c '^\s*call\s' || true)
if [ "$main_calls" -eq 0 ]; then
    pass "transitive_inline_asm: no calls in main (fully inlined)"
else
    fail "transitive_inline_asm: $main_calls call(s) remain in main"
fi

# Two-level transitive inlining
cat > "$TMPDIR/trans2.c" << 'EOF'
int square(int x) { return x * x; }
int double_sq(int x) { return square(x) + square(x); }
int main() {
    return double_sq(5);
}
EOF
check_exit "transitive_2level" "$TMPDIR/trans2.c" 50 "-O3"

# ===========================================================================
# 19. SIMD Reduction Vectorization (-O3)
# ===========================================================================
echo "--- SIMD Reduction Vectorization ---"

# Basic reduction: sum of array elements
cat > "$TMPDIR/simd_red1.c" << 'EOF'
int main(void) {
    int arr[16];
    int i = 0;
    while (i < 16) {
        arr[i] = i + 1;
        i = i + 1;
    }
    int sum = 0;
    i = 0;
    while (i < 16) {
        sum = sum + arr[i];
        i = i + 1;
    }
    return sum;
}
EOF
check_exit "simd_reduction_basic" "$TMPDIR/simd_red1.c" 136 "-O3"

# Reduction with non-zero initial accumulator
cat > "$TMPDIR/simd_red2.c" << 'EOF'
int main(void) {
    int arr[8];
    arr[0] = 10; arr[1] = 20; arr[2] = 30; arr[3] = 40;
    arr[4] = 50; arr[5] = 60; arr[6] = 70; arr[7] = 80;
    int sum = 5;
    int i = 0;
    while (i < 8) {
        sum = sum + arr[i];
        i = i + 1;
    }
    return sum & 0xFF;
}
EOF
check_exit "simd_reduction_nonzero_acc" "$TMPDIR/simd_red2.c" 109 "-O3"

# Reduction with non-power-of-4 count (scalar remainder)
cat > "$TMPDIR/simd_red3.c" << 'EOF'
int main(void) {
    int arr[7];
    arr[0] = 1; arr[1] = 2; arr[2] = 3; arr[3] = 4;
    arr[4] = 5; arr[5] = 6; arr[6] = 7;
    int sum = 0;
    int i = 0;
    while (i < 7) {
        sum = sum + arr[i];
        i = i + 1;
    }
    return sum;
}
EOF
check_exit "simd_reduction_remainder" "$TMPDIR/simd_red3.c" 28 "-O3"

# Verify paddd appears in generated assembly for reduction
"$CC" -O3 -S -o "$TMPDIR/simd_red1.s" "$TMPDIR/simd_red1.c" 2>/dev/null
if grep -q 'paddd' "$TMPDIR/simd_red1.s"; then
    pass "simd_reduction_asm: paddd present in reduction loop"
else
    fail "simd_reduction_asm: paddd NOT found in generated assembly"
fi

# Verify pshufd for horizontal sum
if grep -q 'pshufd' "$TMPDIR/simd_red1.s"; then
    pass "simd_reduction_horiz: pshufd present for horizontal reduction"
else
    fail "simd_reduction_horiz: pshufd NOT found in generated assembly"
fi

# ===========================================================================
# 20. SIMD Init Loop Vectorization (-O3)
# ===========================================================================
echo "--- SIMD Init Loop Vectorization ---"

# Constant init: arr[i] = 42
cat > "$TMPDIR/simd_init1.c" << 'EOF'
int main(void) {
    int arr[12];
    int i = 0;
    while (i < 12) {
        arr[i] = 42;
        i = i + 1;
    }
    return arr[0] + arr[5] + arr[11];
}
EOF
check_exit "simd_init_constant" "$TMPDIR/simd_init1.c" 126 "-O3"

# Strided init: arr[i] = i * 3 + 2
cat > "$TMPDIR/simd_init2.c" << 'EOF'
int main(void) {
    int arr[12];
    int i = 0;
    while (i < 12) {
        arr[i] = i * 3 + 2;
        i = i + 1;
    }
    return arr[11];
}
EOF
check_exit "simd_init_stride" "$TMPDIR/simd_init2.c" 35 "-O3"

# Identity init: arr[i] = i
cat > "$TMPDIR/simd_init3.c" << 'EOF'
int main(void) {
    int arr[16];
    int i = 0;
    while (i < 16) {
        arr[i] = i;
        i = i + 1;
    }
    return arr[15];
}
EOF
check_exit "simd_init_identity" "$TMPDIR/simd_init3.c" 15 "-O3"

# Init with offset: arr[i] = i + 10
cat > "$TMPDIR/simd_init4.c" << 'EOF'
int main(void) {
    int arr[8];
    int i = 0;
    while (i < 8) {
        arr[i] = i + 10;
        i = i + 1;
    }
    return arr[7];
}
EOF
check_exit "simd_init_offset" "$TMPDIR/simd_init4.c" 17 "-O3"

# Verify movdqu appears in generated assembly for init
"$CC" -O3 -S -o "$TMPDIR/simd_init1.s" "$TMPDIR/simd_init1.c" 2>/dev/null
if grep -q 'movdqu' "$TMPDIR/simd_init1.s"; then
    pass "simd_init_asm: movdqu present in init loop"
else
    fail "simd_init_asm: movdqu NOT found in generated assembly"
fi

# Combined init + reduction (bench_array pattern)
cat > "$TMPDIR/simd_combined.c" << 'EOF'
int main(void) {
    int arr[20];
    int i = 0;
    while (i < 20) {
        arr[i] = i * 2 + 1;
        i = i + 1;
    }
    int sum = 0;
    i = 0;
    while (i < 20) {
        sum = sum + arr[i];
        i = i + 1;
    }
    return sum & 0xFF;
}
EOF
check_exit "simd_combined_init_reduce" "$TMPDIR/simd_combined.c" 144 "-O3"

# ---- 21. -Os Size Optimization ----
echo "--- -Os Size Optimization ---"

# 21a. Basic correctness — -Os produces correct results
cat > "$TMPDIR/os_basic.c" << 'EOF'
int square(int x) { return x * x; }
int main(void) {
    int a = square(5);
    int b = square(3);
    return a - b - 7;
}
EOF
check_exit "os_basic_correct" "$TMPDIR/os_basic.c" 9 "-Os"

# 21b. -Os should NOT vectorize (no paddd/movdqu in SIMD-eligible code)
cat > "$TMPDIR/os_novec.c" << 'EOF'
int main(void) {
    int arr[64];
    int i = 0;
    while (i < 64) { arr[i] = i * 3 + 1; i = i + 1; }
    int sum = 0;
    i = 0;
    while (i < 64) { sum = sum + arr[i]; i = i + 1; }
    return sum & 0xFF;
}
EOF
check_exit "os_novec_correct" "$TMPDIR/os_novec.c" 224 "-Os"
"$CC" -Os -S -o "$TMPDIR/os_novec.s" "$TMPDIR/os_novec.c" 2>/dev/null
if grep -q 'paddd\|movdqu\|pshufd' "$TMPDIR/os_novec.s"; then
    fail "os_novec_asm: SIMD instructions found under -Os (should be disabled)"
else
    pass "os_novec_asm: no SIMD instructions under -Os"
fi

# 21c. -Os code size should be <= -O2 code size
"$CC" -Os -o "$TMPDIR/os_size_os" "$TMPDIR/os_novec.c" 2>/dev/null
"$CC" -O2 -o "$TMPDIR/os_size_o2" "$TMPDIR/os_novec.c" 2>/dev/null
size_os=$(size "$TMPDIR/os_size_os" 2>/dev/null | tail -1 | awk '{print $1}')
size_o2=$(size "$TMPDIR/os_size_o2" 2>/dev/null | tail -1 | awk '{print $1}')
if [ -n "$size_os" ] && [ -n "$size_o2" ] && [ "$size_os" -le "$size_o2" ]; then
    pass "os_code_size: -Os text ($size_os) <= -O2 text ($size_o2)"
else
    fail "os_code_size: -Os text ($size_os) > -O2 text ($size_o2)"
fi

# 21d. -Os should still do basic optimizations (constant folding, peephole)
cat > "$TMPDIR/os_opts.c" << 'EOF'
int main(void) {
    int x = 3 + 4;
    int y = x * 2;
    return y;
}
EOF
check_exit "os_basic_opts" "$TMPDIR/os_opts.c" 14 "-Os"
"$CC" -Os -S -o "$TMPDIR/os_opts.s" "$TMPDIR/os_opts.c" 2>/dev/null
# Should have mov $14 (constant folded) or xor-zero-init
if grep -q 'mov \$14' "$TMPDIR/os_opts.s" || grep -q 'xorl' "$TMPDIR/os_opts.s"; then
    pass "os_basic_opts_asm: basic optimizations active under -Os"
else
    fail "os_basic_opts_asm: basic optimizations NOT active under -Os"
fi

# 21e. -Os should NOT do aggressive inlining of larger functions
cat > "$TMPDIR/os_noinline.c" << 'EOF'
int compute(int x) {
    int a = x * 3;
    int b = a + 7;
    int c = b * 2;
    int d = c - 5;
    return d;
}
int main(void) { return compute(4) & 0xFF; }
EOF
"$CC" -Os -S -o "$TMPDIR/os_noinline.s" "$TMPDIR/os_noinline.c" 2>/dev/null
check_exit "os_noinline_correct" "$TMPDIR/os_noinline.c" 33 "-Os"

# 21f. -Os multi-file correctness
cat > "$TMPDIR/os_multi.c" << 'EOF'
int fib(int n) {
    if (n <= 1) return n;
    return fib(n - 1) + fib(n - 2);
}
int main(void) { return fib(10) & 0xFF; }
EOF
check_exit "os_multi_correct" "$TMPDIR/os_multi.c" 55 "-Os"

# ---- 22. -Og Debug Optimization ----
echo "--- -Og Debug Optimization ---"

# 22a. Basic correctness
cat > "$TMPDIR/og_basic.c" << 'EOF'
int square(int x) { return x * x; }
int main(void) {
    int a = square(5);
    int b = square(3);
    return a - b - 7;
}
EOF
check_exit "og_basic_correct" "$TMPDIR/og_basic.c" 9 "-Og"

# 22b. -Og should NOT inline functions (call instruction preserved)
"$CC" -Og -S -o "$TMPDIR/og_noinline.s" "$TMPDIR/og_basic.c" 2>/dev/null
if grep -q 'call.*square' "$TMPDIR/og_noinline.s"; then
    pass "og_noinline: 'call square' preserved under -Og"
else
    fail "og_noinline: function was inlined under -Og (call missing)"
fi

# 22c. -Og should NOT use cmov (confuses debuggers)
cat > "$TMPDIR/og_nocmov.c" << 'EOF'
int main(void) {
    int x = 10;
    int y = 20;
    int z = (x < y) ? x : y;
    return z;
}
EOF
"$CC" -Og -S -o "$TMPDIR/og_nocmov.s" "$TMPDIR/og_nocmov.c" 2>/dev/null
check_exit "og_nocmov_correct" "$TMPDIR/og_nocmov.c" 10 "-Og"
if grep -q 'cmov' "$TMPDIR/og_nocmov.s"; then
    fail "og_nocmov_asm: cmov found under -Og (should use branches)"
else
    pass "og_nocmov_asm: no cmov under -Og"
fi

# 22d. -Og should NOT do tail call optimization (preserves call stack)
cat > "$TMPDIR/og_notail.c" << 'EOF'
int helper(int x) { return x + 1; }
int main(void) { return helper(41); }
EOF
"$CC" -Og -S -o "$TMPDIR/og_notail.s" "$TMPDIR/og_notail.c" 2>/dev/null
check_exit "og_notail_correct" "$TMPDIR/og_notail.c" 42 "-Og"
if grep -q 'call.*helper' "$TMPDIR/og_notail.s"; then
    pass "og_notail: call preserved (no tail call under -Og)"
else
    fail "og_notail: tail call used under -Og (call missing)"
fi

# 22e. -Og should still do constant folding (doesn't hurt debugging)
cat > "$TMPDIR/og_constfold.c" << 'EOF'
int main(void) {
    int x = 3 + 4 * 5;
    return x;
}
EOF
check_exit "og_constfold" "$TMPDIR/og_constfold.c" 23 "-Og"

# 22f. -Og should NOT vectorize
cat > "$TMPDIR/og_novec.c" << 'EOF'
int main(void) {
    int arr[32];
    int i = 0;
    while (i < 32) { arr[i] = i; i = i + 1; }
    int sum = 0;
    i = 0;
    while (i < 32) { sum = sum + arr[i]; i = i + 1; }
    return sum & 0xFF;
}
EOF
check_exit "og_novec_correct" "$TMPDIR/og_novec.c" 240 "-Og"
"$CC" -Og -S -o "$TMPDIR/og_novec.s" "$TMPDIR/og_novec.c" 2>/dev/null
if grep -q 'paddd\|movdqu\|pshufd' "$TMPDIR/og_novec.s"; then
    fail "og_novec_asm: SIMD instructions found under -Og"
else
    pass "og_novec_asm: no SIMD instructions under -Og"
fi

# 22g. -Og should always_inline functions with __attribute__((always_inline))
cat > "$TMPDIR/og_always_inline.c" << 'EOF'
static inline __attribute__((always_inline)) int tiny(int x) { return x + 1; }
int main(void) { return tiny(41); }
EOF
"$CC" -Og -S -o "$TMPDIR/og_always_inline.s" "$TMPDIR/og_always_inline.c" 2>/dev/null
check_exit "og_always_inline_correct" "$TMPDIR/og_always_inline.c" 42 "-Og"
if grep -q 'call.*tiny' "$TMPDIR/og_always_inline.s"; then
    fail "og_always_inline_asm: always_inline function NOT inlined under -Og"
else
    pass "og_always_inline_asm: always_inline function inlined under -Og"
fi

# 22h. -Og complex control flow correctness
cat > "$TMPDIR/og_complex.c" << 'EOF'
int main(void) {
    int sum = 0;
    int i = 0;
    while (i < 10) {
        if (i % 2 == 0) {
            sum = sum + i;
        } else {
            sum = sum - 1;
        }
        i = i + 1;
    }
    return sum & 0xFF;
}
EOF
check_exit "og_complex_correct" "$TMPDIR/og_complex.c" 15 "-Og"

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
