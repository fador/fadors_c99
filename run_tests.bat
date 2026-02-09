@echo off
SETLOCAL
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if %errorlevel% neq 0 (
    echo [ERROR] Failed to set up vcvars64.
    exit /b %errorlevel%
)

echo [BUILD]
cmake --build build --config Debug
if %errorlevel% neq 0 (
    echo [ERROR] Build failed.
    exit /b %errorlevel%
)

echo [TEST 01_return]
.\build\Debug\fadors99.exe tests\01_return.c --obj
if %errorlevel% neq 0 (
    echo [ERROR] 01_return.c compilation failed.
    exit /b %errorlevel%
)

echo [DUMPBIN 01_return]
dumpbin /headers tests\01_return.obj
dumpbin /symbols tests\01_return.obj

echo [LINK 01_return]
link /nologo /entry:main /subsystem:console /out:tests\01_return.exe tests\01_return.obj kernel32.lib
if %errorlevel% neq 0 (
    echo [ERROR] Linking 01_return failed.
) else (
    .\tests\01_return.exe
    echo 01_return Exit Code: %errorlevel%
)

echo [TEST 04_if]
.\build\Debug\fadors99.exe tests\04_if.c --obj
if %errorlevel% neq 0 (
    echo [ERROR] 04_if.c compilation failed.
    exit /b %errorlevel%
)
link /nologo /entry:main /subsystem:console /out:tests\04_if.exe tests\04_if.obj kernel32.lib
if %errorlevel% neq 0 (
    echo [ERROR] Linking 04_if failed.
) else (
    .\tests\04_if.exe
    echo 04_if Exit Code: %errorlevel%
)

ENDLOCAL
