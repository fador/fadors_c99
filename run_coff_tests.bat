@echo off
setlocal enabledelayedexpansion
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" > nul

set "FADORS=.\build\Release\fadors99.exe"

set TEST_LIST=01_return 02_arithmetic 03_variables 04_if 06_while 07_function 12_string 14_params 15_nested_calls 19_array 20_switch 21_enum 22_union 23_pointer_math

echo Running COFF tests...
echo ===================

for %%T in (%TEST_LIST%) do (
    echo [TEST] %%T
    %FADORS% tests\%%T.c --obj
    if errorlevel 1 (
        echo [FAIL] Compilation/Linking failed for %%T
    ) else (
        tests\%%T.exe
        echo [DONE] Return Code: !errorlevel!
    )
    echo -------------------
)

echo All COFF tests completed.
