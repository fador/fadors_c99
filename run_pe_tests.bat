@echo off
setlocal enabledelayedexpansion

set "FADORS=.\build\Release\fadors99.exe"

:: Format: test_name:expected_return_code
set TEST_LIST=01_return:42 02_arithmetic:7 03_variables:30 04_if:100 06_while:10 07_function:123 12_string:72 14_params:10 15_nested_calls:10 19_array:100 20_switch:120 21_enum:6 22_union:42 23_pointer_math:0 50_hex_char_long:0 51_named_union_static_local:0 52_forward_struct:0

set PASS=0
set FAIL=0
set TOTAL=0

echo Running PE linker tests (custom linker, no external tools)...
echo ============================================================

for %%T in (%TEST_LIST%) do (
    set /a TOTAL+=1
    for /f "tokens=1,2 delims=:" %%A in ("%%T") do (
        set "TNAME=%%A"
        set "EXPECTED=%%B"
    )
    echo [TEST] !TNAME! ^(expected: !EXPECTED!^)
    %FADORS% tests\!TNAME!.c -o tests\!TNAME!.exe > nul 2>&1
    if errorlevel 1 (
        echo [FAIL] Compilation/PE-linking failed for !TNAME!
        set /a FAIL+=1
    ) else (
        tests\!TNAME!.exe
        set "RC=!errorlevel!"
        if "!RC!"=="!EXPECTED!" (
            echo [PASS] !TNAME! returned !RC!
            set /a PASS+=1
        ) else (
            echo [FAIL] !TNAME! returned !RC!, expected !EXPECTED!
            set /a FAIL+=1
        )
    )
    echo -------------------
)

echo.
echo ====================
echo RESULTS: !PASS!/!TOTAL! passed, !FAIL! failed
echo ====================
if !FAIL! GTR 0 (
    echo SOME TESTS FAILED.
    exit /b 1
) else (
    echo ALL PE LINKER TESTS PASSED!
    exit /b 0
)
