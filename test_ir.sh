#!/bin/bash
# test_ir.sh — Verify IR/CFG construction
#
# Compiles test programs with -O2 --dump-ir and checks that:
#   1. The IR is generated without crashing
#   2. Basic blocks and CFG edges are present
#   3. Key IR instructions appear for various language constructs
#
# Usage: ./test_ir.sh [compiler_path]

CC="${1:-build_linux/fadors99}"
TMPDIR=$(mktemp -d)
PASS=0
FAIL=0

cleanup() { rm -rf "$TMPDIR"; }
trap cleanup EXIT

pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

echo "=== IR / CFG Construction Tests ==="
echo "Compiler: $CC"
echo ""

# ---- Helper: compile with -O2 --dump-ir and capture IR ----
dump_ir() {
    local name="$1" src="$2" flags="$3"
    "$CC" $flags -O2 --dump-ir "$src" -o "$TMPDIR/$name" 2>"$TMPDIR/${name}.ir" 1>/dev/null
    return $?
}

# ---- Test 1: Simple return ----
cat > "$TMPDIR/t1.c" << 'EOF'
int main() { return 42; }
EOF
dump_ir t1 "$TMPDIR/t1.c"
if grep -q "function @main" "$TMPDIR/t1.ir"; then
    pass "t1: function @main found in IR"
else
    fail "t1: function @main not found in IR"
fi
if grep -q "ret" "$TMPDIR/t1.ir"; then
    pass "t1: ret instruction present"
else
    fail "t1: ret instruction not present"
fi
if grep -q "const.*\\\$42" "$TMPDIR/t1.ir"; then
    pass "t1: const 42 present"
else
    fail "t1: const 42 not present"
fi

# ---- Test 2: Binary expression ----
cat > "$TMPDIR/t2.c" << 'EOF'
int main() { int a = 10; int b = 20; return a + b; }
EOF
dump_ir t2 "$TMPDIR/t2.c"
if grep -q "add" "$TMPDIR/t2.ir" || grep -q "const.*\\\$30" "$TMPDIR/t2.ir"; then
    pass "t2: add instruction or folded constant present"
else
    fail "t2: no add instruction or folded constant"
fi

# ---- Test 3: If/else (branches and multiple blocks) ----
cat > "$TMPDIR/t3.c" << 'EOF'
int main() {
    int x = 5;
    if (x > 3) {
        return 1;
    } else {
        return 0;
    }
}
EOF
dump_ir t3 "$TMPDIR/t3.c"
if grep -q "if_then" "$TMPDIR/t3.ir" || grep -q "branch" "$TMPDIR/t3.ir"; then
    pass "t3: branch/if_then blocks present"
else
    fail "t3: no branch/if_then blocks"
fi
if grep -q "preds:" "$TMPDIR/t3.ir" || grep -q "succs:" "$TMPDIR/t3.ir"; then
    pass "t3: CFG edges (preds/succs) present"
else
    fail "t3: no CFG edges found"
fi

# ---- Test 4: While loop ----
cat > "$TMPDIR/t4.c" << 'EOF'
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
dump_ir t4 "$TMPDIR/t4.c"
if grep -q "while_cond" "$TMPDIR/t4.ir"; then
    pass "t4: while_cond block present"
else
    fail "t4: while_cond block not found"
fi
if grep -q "while_body" "$TMPDIR/t4.ir"; then
    pass "t4: while_body block present"
else
    fail "t4: while_body block not found"
fi
if grep -q "while_exit" "$TMPDIR/t4.ir"; then
    pass "t4: while_exit block present"
else
    fail "t4: while_exit block not found"
fi

# ---- Test 5: For loop ----
cat > "$TMPDIR/t5.c" << 'EOF'
int main() {
    int sum = 0;
    for (int i = 0; i < 5; i = i + 1) {
        sum = sum + i;
    }
    return sum;
}
EOF
dump_ir t5 "$TMPDIR/t5.c"
if grep -q "for_cond" "$TMPDIR/t5.ir"; then
    pass "t5: for_cond block present"
else
    fail "t5: for_cond block not found"
fi
if grep -q "for_incr" "$TMPDIR/t5.ir"; then
    pass "t5: for_incr block present"
else
    fail "t5: for_incr block not found"
fi

# ---- Test 6: Function calls ----
cat > "$TMPDIR/t6.c" << 'EOF'
int add(int a, int b) { return a + b; }
int main() { return add(3, 4); }
EOF
dump_ir t6 "$TMPDIR/t6.c"
if grep -q "function @add" "$TMPDIR/t6.ir"; then
    pass "t6: function @add found"
else
    fail "t6: function @add not found"
fi
if grep -q "call" "$TMPDIR/t6.ir" || grep -q "param" "$TMPDIR/t6.ir"; then
    pass "t6: call/param instructions present"
else
    fail "t6: no call/param instructions"
fi

# ---- Test 7: Multiple blocks / complex CFG ----
cat > "$TMPDIR/t7.c" << 'EOF'
int main() {
    int x = 10;
    int result = 0;
    if (x > 5) {
        int y = x * 2;
        if (y > 15) {
            result = 1;
        } else {
            result = 2;
        }
    } else {
        result = 3;
    }
    return result;
}
EOF
dump_ir t7 "$TMPDIR/t7.c"
BLOCKS=$(grep -c "bb[0-9]*)" "$TMPDIR/t7.ir" || true)
if [ "$BLOCKS" -ge 3 ]; then
    pass "t7: multiple basic blocks found ($BLOCKS blocks)"
else
    fail "t7: too few basic blocks ($BLOCKS blocks, expected >= 3)"
fi

# ---- Test 8: Do-while loop ----
cat > "$TMPDIR/t8.c" << 'EOF'
int main() {
    int i = 0;
    do {
        i = i + 1;
    } while (i < 5);
    return i;
}
EOF
dump_ir t8 "$TMPDIR/t8.c"
if grep -q "do_body" "$TMPDIR/t8.ir"; then
    pass "t8: do_body block present"
else
    fail "t8: do_body block not found"
fi
if grep -q "do_cond" "$TMPDIR/t8.ir"; then
    pass "t8: do_cond block present"
else
    fail "t8: do_cond block not found"
fi

# ---- Test 9: Variable tracking ----
cat > "$TMPDIR/t9.c" << 'EOF'
int main() {
    int a = 1;
    int b = 2;
    int c = a + b;
    return c;
}
EOF
dump_ir t9 "$TMPDIR/t9.c"
if grep -q "; vars:" "$TMPDIR/t9.ir"; then
    pass "t9: variable table present"
else
    fail "t9: variable table not found"
fi
# Check vars are tracked
if grep -q "a=t" "$TMPDIR/t9.ir" && grep -q "b=t" "$TMPDIR/t9.ir"; then
    pass "t9: variables a and b tracked"
else
    fail "t9: variables not tracked properly"
fi

# ---- Test 10: Verify compiled output still correct ----
cat > "$TMPDIR/t10.c" << 'EOF'
int main() {
    int sum = 0;
    for (int i = 1; i <= 10; i = i + 1) {
        if (i % 2 == 0) {
            sum = sum + i;
        }
    }
    return sum;
}
EOF
dump_ir t10 "$TMPDIR/t10.c"
"$TMPDIR/t10"
actual=$?
if [ "$actual" -eq 30 ]; then
    pass "t10: program output correct (exit=30)"
else
    fail "t10: program output wrong (exit=$actual, expected 30)"
fi

# ---- Test 11: IR block count and vreg count printed ----
if grep -q "blocks" "$TMPDIR/t7.ir" && grep -q "vregs" "$TMPDIR/t7.ir"; then
    pass "t11: block and vreg counts in IR dump"
else
    fail "t11: block/vreg counts not found"
fi

# ---- Test 12: Global variables ----
cat > "$TMPDIR/t12.c" << 'EOF'
int g = 42;
int main() { return g; }
EOF
dump_ir t12 "$TMPDIR/t12.c"
if grep -q "@g" "$TMPDIR/t12.ir"; then
    pass "t12: global variable @g in IR"
else
    fail "t12: global variable @g not found"
fi

echo ""
echo "=== Results ==="
echo "  PASS:    $PASS"
echo "  FAIL:    $FAIL"
echo "  TOTAL:   $((PASS + FAIL))"
echo ""
if [ "$FAIL" -eq 0 ]; then
    echo "All IR/CFG tests passed!"
else
    echo "Some tests failed."
    exit 1
fi
