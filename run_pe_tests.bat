@echo off
setlocal enabledelayedexpansion

:: run_pe_tests.bat — Run full test suite using the PE linker pipeline
::                    (C -> COFF .obj -> custom PE linker -> .exe)
::
:: No external tools required (no MSVC link.exe, no CRT).
:: Tests requiring CRT functions (printf, malloc, etc.) are skipped.

set "FADORS=%~1"
if "%FADORS%"=="" set "FADORS=.\build\Release\fadors99.exe"

if not exist "%FADORS%" (
    echo [ERROR] Compiler not found: %FADORS%
    exit /b 1
)

:: Format: test_name:expected_return_code
:: Note: 08_include expects 1337 (Windows preserves full 32-bit exit code, no 0xFF mask)
:: Note: 23_pointer_math expects 0 (test fixed for 4-byte int type)
:: Note: 13_extern and 26_external are Windows-only (call ExitProcess directly)
::
:: Skipped tests (require CRT functions not available with PE linker):
::   46_system_includes  — needs malloc, free, strlen, strcmp, isalpha
::   53_pragma_pack      — needs printf
::   54_string_escapes   — needs printf, strlen
::   55_headers_test     — needs malloc, strlen, strcmp, includes src/buffer.h
::   73_int64_arith      — needs printf
::   74_reloc_arith      — needs printf, malloc, memcpy
::   75_reloc_test       — needs printf, malloc, memcpy, calloc, strcmp, free
::
:: Skipped tests (PE linker limitations / Windows path differences):
::   13_extern           — calls ExitProcess directly; PE linker lacks JMP thunks
::   26_external         — calls ExitProcess directly; PE linker lacks JMP thunks
::   39_builtin_macros   — __FILE__ uses backslashes on Windows, test expects '/'

set TEST_LIST=^
 01_return:42^
 02_arithmetic:7^
 03_variables:30^
 04_if:100^
 05_complex_expr:30^
 06_while:10^
 07_function:123^
 08_include:1337^
 09_struct_ptr:10^
 10_ptr:100^
 11_nested_struct:10^
 12_string:72^
 14_params:10^
 15_nested_calls:10^
 16_void_func:123^
 17_for:45^
 18_typedef:42^
 19_array:100^
 20_switch:120^
 21_enum:6^
 22_union:42^
 23_pointer_math:0^
 24_coff_test:42^
 25_global:52^
 27_float:42^
 28_minimal_float:0^
 29_float_unary_ret:42^
 31_binary_ops_int:42^
 32_binary_ops_float:42^
 33_binary_ops_mixed:42^
 34_precedence:42^
 36_ifdef:42^
 38_func_macros:42^
 40_string_cmp:42^
 41_inc_dec:42^
 42_cast:42^
 43_inc_dec_cond:0^
 44_sizeof:0^
 45_const_static:0^
 47_compound_assign:0^
 48_for_init_decl:0^
 49_init_list:0^
 50_hex_char_long:0^
 51_named_union_static_local:0^
 52_forward_struct:0^
 53_minimal_pack:0^
 54_minimal_string_escape:0^
 56_ptr_deref_inc:0^
 57_ptr_deref_inc_array:0^
 58_ptr_write_inc:0^
 59_char_ptr_inc:0^
 60_struct_layout:0^
 61_ptr_struct_field:0^
 62_union_in_struct:4^
 63_array_index:30^
 64_struct_offsets:0^
 65_chained_ptr:4^
 66_type_struct:0^
 67_chained_member:0^
 68_union_member_chain:0^
 69_self_ref_struct:0^
 70_minimal_switch:20^
 71_2d_array:0^
 72_global_init_list:0^
 94_assert_basic:42^
 96_assert_range:20

:: Optimization tests (require -O1 flag)
set OPT_TEST_LIST=^
 95_assert_exact_val:40^
 97_assert_pow2:40

set PASS=0
set FAIL=0
set SKIP=0
set TOTAL=0

echo === PE Linker Test Suite ===
echo Compiler: %FADORS%
echo Pipeline: C -^> COFF .obj -^> custom PE linker (no external tools)
echo.

for %%T in (%TEST_LIST%) do (
    set /a TOTAL+=1
    for /f "tokens=1,2 delims=:" %%A in ("%%T") do (
        set "TNAME=%%A"
        set "EXPECTED=%%B"
    )

    %FADORS% tests\!TNAME!.c -o tests\!TNAME!.exe > nul 2>&1
    if errorlevel 1 (
        echo   FAIL  !TNAME!  ^(compile/link error, expected exit=!EXPECTED!^)
        set /a FAIL+=1
    ) else (
        tests\!TNAME!.exe > nul 2>&1
        set "RC=!errorlevel!"
        if "!RC!"=="!EXPECTED!" (
            set /a PASS+=1
        ) else (
            echo   FAIL  !TNAME!  ^(exit=!RC!, expected=!EXPECTED!^)
            set /a FAIL+=1
        )
    )
)

:: Run optimization tests (need -O1 flag)
for %%T in (%OPT_TEST_LIST%) do (
    set /a TOTAL+=1
    for /f "tokens=1,2 delims=:" %%A in ("%%T") do (
        set "TNAME=%%A"
        set "EXPECTED=%%B"
    )

    %FADORS% tests\!TNAME!.c -O1 -o tests\!TNAME!.exe > nul 2>&1
    if errorlevel 1 (
        echo   FAIL  !TNAME! -O1  ^(compile/link error, expected exit=!EXPECTED!^)
        set /a FAIL+=1
    ) else (
        tests\!TNAME!.exe > nul 2>&1
        set "RC=!errorlevel!"
        if "!RC!"=="!EXPECTED!" (
            set /a PASS+=1
        ) else (
            echo   FAIL  !TNAME! -O1  ^(exit=!RC!, expected=!EXPECTED!^)
            set /a FAIL+=1
        )
    )
)

echo.
echo === Results ===
echo   PASS:    !PASS!
echo   FAIL:    !FAIL!
echo   TOTAL:   !TOTAL!
echo.
if !FAIL! GTR 0 (
    echo !FAIL! test^(s^) FAILED.
    exit /b 1
) else (
    echo All PE linker tests passed!
    exit /b 0
)
