#!/bin/bash
# Phase 2c: Source-Level Debugging Verification
# Tests that -g produces correct DWARF for gdb and lldb.
#
# Usage: ./test_debug.sh [compiler_path]
#   Default compiler: build_linux/fadors99

CC="${1:-build_linux/fadors99}"
TMPDIR=$(mktemp -d)
PASS=0
FAIL=0
SKIP=0

cleanup() { rm -rf "$TMPDIR"; }
trap cleanup EXIT

pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
skip() { echo "  SKIP: $1"; SKIP=$((SKIP + 1)); }

echo "=== Phase 2c: Debug Symbol Verification ==="
echo "Compiler: $CC"
echo ""

# ---- Test program ----
cat > "$TMPDIR/test.c" << 'SRCEOF'
int add(int a, int b) {
    int result = a + b;
    return result;
}

int main() {
    int x = 10;
    int y = 20;
    int z = add(x, y);
    return z;
}
SRCEOF

# Compile with -g
if ! "$CC" -g "$TMPDIR/test.c" -o "$TMPDIR/test" 2>/dev/null; then
    echo "FATAL: Compilation with -g failed"
    exit 1
fi

# ---- 1. Verify DWARF sections exist ----
echo "--- DWARF Section Checks ---"

SECTIONS=$(readelf -S "$TMPDIR/test" 2>/dev/null)
for sect in .debug_info .debug_abbrev .debug_line .debug_str .debug_aranges; do
    if echo "$SECTIONS" | grep -q "$sect"; then
        pass "$sect section present"
    else
        fail "$sect section missing"
    fi
done

# ---- 2. Verify .debug_info content ----
echo ""
echo "--- .debug_info Checks ---"

DINFO=$(readelf --debug-dump=info "$TMPDIR/test" 2>/dev/null)

# Compile unit DIE
if echo "$DINFO" | grep -q "DW_TAG_compile_unit"; then
    pass "DW_TAG_compile_unit present"
else
    fail "DW_TAG_compile_unit missing"
fi

# Producer string (via strp)
if echo "$DINFO" | grep -q "fadors99"; then
    pass "DW_AT_producer = fadors99"
else
    fail "DW_AT_producer missing"
fi

# Subprogram DIEs
if echo "$DINFO" | grep -q "DW_TAG_subprogram"; then
    pass "DW_TAG_subprogram present"
else
    fail "DW_TAG_subprogram missing"
fi

# Function names
for fname in add main; do
    if echo "$DINFO" | grep -q "$fname"; then
        pass "Function '$fname' in debug info"
    else
        fail "Function '$fname' not found in debug info"
    fi
done

# Variable DIEs
if echo "$DINFO" | grep -q "DW_TAG_variable"; then
    pass "DW_TAG_variable present"
else
    fail "DW_TAG_variable missing"
fi

# Parameter DIEs
if echo "$DINFO" | grep -q "DW_TAG_formal_parameter"; then
    pass "DW_TAG_formal_parameter present"
else
    fail "DW_TAG_formal_parameter missing"
fi

# Base type DIE
if echo "$DINFO" | grep -q "DW_TAG_base_type"; then
    pass "DW_TAG_base_type present"
else
    fail "DW_TAG_base_type missing"
fi

# Location expressions
if echo "$DINFO" | grep -q "DW_OP_fbreg"; then
    pass "DW_OP_fbreg location expressions"
else
    fail "DW_OP_fbreg location expressions missing"
fi

# Frame base
if echo "$DINFO" | grep -q "DW_OP_reg6"; then
    pass "DW_AT_frame_base = DW_OP_reg6 (rbp)"
else
    fail "DW_AT_frame_base missing"
fi

# ---- 3. Verify .debug_str content ----
echo ""
echo "--- .debug_str Checks ---"

# Extract .debug_str strings (hex dump is line-wrapped, can split strings across lines)
DSTR=$(objcopy --dump-section .debug_str=/dev/stdout "$TMPDIR/test" 2>/dev/null | strings -n 1)
for s in fadors99 int add main result; do
    if echo "$DSTR" | grep -q "$s"; then
        pass "'$s' in .debug_str"
    else
        fail "'$s' not in .debug_str"
    fi
done

# Verify strp references work (no "unable to find" warnings)
WARNINGS=$(readelf --debug-dump=info "$TMPDIR/test" 2>&1 | grep -c "Warning" || true)
if [ "$WARNINGS" = "0" ]; then
    pass "No readelf warnings (strp refs valid)"
else
    fail "$WARNINGS readelf warnings (possible strp issues)"
fi

# ---- 4. Verify .debug_aranges ----
echo ""
echo "--- .debug_aranges Checks ---"

DARANGES=$(readelf --debug-dump=aranges "$TMPDIR/test" 2>/dev/null)
if echo "$DARANGES" | grep -q "Length:"; then
    pass ".debug_aranges has entries"
else
    fail ".debug_aranges is empty"
fi

# ---- 5. Verify .debug_line ----
echo ""
echo "--- .debug_line Checks ---"

DLINE=$(readelf --debug-dump=decodedline "$TMPDIR/test" 2>/dev/null)
LINE_COUNT=$(echo "$DLINE" | grep -c "test.c" || true)
if [ "$LINE_COUNT" -ge 5 ]; then
    pass ".debug_line has $LINE_COUNT line entries"
else
    fail ".debug_line has only $LINE_COUNT entries (expected >= 5)"
fi

# ---- 6. GDB tests ----
echo ""
echo "--- GDB Tests ---"

if command -v gdb >/dev/null 2>&1; then
    # 6a. break main resolves
    GDB_BREAK=$(gdb -batch -ex "set debuginfod enabled off" \
                -ex "break main" "$TMPDIR/test" 2>&1)
    if echo "$GDB_BREAK" | grep -q "test.c"; then
        pass "gdb: break main resolves to source file"
    else
        fail "gdb: break main does not resolve"
    fi

    # 6b. info locals shows variables
    GDB_LOCALS=$(gdb -batch -ex "set debuginfod enabled off" \
                 -ex "break main" -ex "run" -ex "info locals" \
                 "$TMPDIR/test" 2>&1)
    if echo "$GDB_LOCALS" | grep -q "x = "; then
        pass "gdb: info locals shows 'x'"
    else
        fail "gdb: info locals missing 'x'"
    fi
    if echo "$GDB_LOCALS" | grep -q "y = "; then
        pass "gdb: info locals shows 'y'"
    else
        fail "gdb: info locals missing 'y'"
    fi
    if echo "$GDB_LOCALS" | grep -q "z = "; then
        pass "gdb: info locals shows 'z'"
    else
        fail "gdb: info locals missing 'z'"
    fi

    # 6c. info args in called function
    GDB_ARGS=$(gdb -batch -ex "set debuginfod enabled off" \
               -ex "break add" -ex "run" -ex "info args" \
               "$TMPDIR/test" 2>&1)
    if echo "$GDB_ARGS" | grep -q "a = 10"; then
        pass "gdb: info args shows a=10"
    else
        fail "gdb: info args missing a=10"
    fi
    if echo "$GDB_ARGS" | grep -q "b = 20"; then
        pass "gdb: info args shows b=20"
    else
        fail "gdb: info args missing b=20"
    fi

    # 6d. print variable after computation
    GDB_PRINT=$(gdb -batch -ex "set debuginfod enabled off" \
                -ex "break add" -ex "run" -ex "next" \
                -ex "print result" "$TMPDIR/test" 2>&1)
    if echo "$GDB_PRINT" | grep -q "= 30"; then
        pass "gdb: print result = 30"
    else
        fail "gdb: print result != 30"
    fi

    # 6e. backtrace shows function names
    GDB_BT=$(gdb -batch -ex "set debuginfod enabled off" \
             -ex "break add" -ex "run" -ex "bt" \
             "$TMPDIR/test" 2>&1)
    if echo "$GDB_BT" | grep -q "add"; then
        pass "gdb: backtrace shows 'add'"
    else
        fail "gdb: backtrace missing 'add'"
    fi
    if echo "$GDB_BT" | grep -q "main"; then
        pass "gdb: backtrace shows 'main'"
    else
        fail "gdb: backtrace missing 'main'"
    fi

    # 6f. stepping follows source lines
    GDB_STEP=$(gdb -batch -ex "set debuginfod enabled off" \
               -ex "break main" -ex "run" \
               -ex "next" -ex "next" -ex "next" \
               -ex "print z" "$TMPDIR/test" 2>&1)
    if echo "$GDB_STEP" | grep -q "= 30"; then
        pass "gdb: stepping computes z=30"
    else
        fail "gdb: stepping z!=30"
    fi
else
    skip "gdb: not installed"
fi

# ---- 7. LLDB tests ----
echo ""
echo "--- LLDB Tests ---"

if command -v lldb >/dev/null 2>&1; then
    # 7a. break main resolves
    LLDB_BREAK=$(lldb --batch -o "breakpoint set --name main" \
                 "$TMPDIR/test" 2>&1)
    if echo "$LLDB_BREAK" | grep -q "test.c"; then
        pass "lldb: break main resolves to source file"
    else
        fail "lldb: break main does not resolve"
    fi

    # 7b. frame variable shows locals
    LLDB_VARS=$(lldb --batch \
                -o "breakpoint set --name main" -o "run" \
                -o "frame variable" "$TMPDIR/test" 2>&1)
    if echo "$LLDB_VARS" | grep -q "(int) x ="; then
        pass "lldb: frame variable shows typed 'x'"
    else
        fail "lldb: frame variable missing 'x'"
    fi

    # 7c. args in called function
    LLDB_ARGS=$(lldb --batch \
                -o "breakpoint set --name add" -o "run" \
                -o "frame variable" "$TMPDIR/test" 2>&1)
    if echo "$LLDB_ARGS" | grep -q "a = 10"; then
        pass "lldb: frame variable shows a=10"
    else
        fail "lldb: frame variable missing a=10"
    fi
    if echo "$LLDB_ARGS" | grep -q "b = 20"; then
        pass "lldb: frame variable shows b=20"
    else
        fail "lldb: frame variable missing b=20"
    fi

    # 7d. step-over and verify result
    LLDB_STEP=$(lldb --batch \
                -o "breakpoint set --name add" -o "run" \
                -o "thread step-over" -o "frame variable" \
                "$TMPDIR/test" 2>&1)
    if echo "$LLDB_STEP" | grep -q "result = 30"; then
        pass "lldb: step-over shows result=30"
    else
        fail "lldb: step-over result!=30"
    fi

    # 7e. source listing at breakpoint
    LLDB_SRC=$(lldb --batch \
               -o "breakpoint set --name add" -o "run" \
               -o "source list" "$TMPDIR/test" 2>&1)
    if echo "$LLDB_SRC" | grep -q "int result"; then
        pass "lldb: source list shows correct code"
    else
        fail "lldb: source list incorrect"
    fi
else
    skip "lldb: not installed"
fi

# ---- Summary ----
echo ""
echo "=== Results ==="
echo "  PASS:    $PASS"
echo "  FAIL:    $FAIL"
echo "  SKIP:    $SKIP"
echo "  TOTAL:   $((PASS + FAIL + SKIP))"
echo ""

if [ $FAIL -eq 0 ]; then
    echo "All debug tests passed!"
    exit 0
else
    echo "Some tests failed."
    exit 1
fi
