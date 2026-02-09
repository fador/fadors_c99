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
.\tests\01_return.exe
echo 01_return result: %errorlevel%

echo [TEST 04_if]
.\build\Debug\fadors99.exe tests\04_if.c --obj
if %errorlevel% neq 0 (
    echo [ERROR] 04_if.c compilation failed.
    exit /b %errorlevel%
)
.\tests\04_if.exe
echo 04_if result: %errorlevel%

echo [TEST 07_function]
.\build\Debug\fadors99.exe tests\07_function.c --obj
if %errorlevel% neq 0 (
    echo [ERROR] 07_function.c compilation failed.
    exit /b %errorlevel%
)
.\tests\07_function.exe
echo 07_function result: %errorlevel%

ENDLOCAL
