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

echo [TEST 12_string]
.\build\Debug\fadors99.exe tests\12_string.c --obj
if %errorlevel% neq 0 (
    echo [ERROR] 12_string.c compilation failed.
    exit /b %errorlevel%
)
.\tests\12_string.exe
echo 12_string result: %errorlevel%

echo [TEST 13_extern]
.\build\Debug\fadors99.exe tests\13_extern.c --obj
if %errorlevel% neq 0 (
    echo [ERROR] 13_extern.c compilation failed.
    exit /b %errorlevel%
)
.\tests\13_extern.exe
echo 13_extern result: %errorlevel%

echo [TEST 14_params]
.\build\Debug\fadors99.exe tests\14_params.c --obj
if %errorlevel% neq 0 (
    echo [ERROR] 14_params.c compilation failed.
    exit /b %errorlevel%
)
.\tests\14_params.exe
echo 14_params result: %errorlevel%

echo [TEST 15_nested_calls]
.\build\Debug\fadors99.exe tests\15_nested_calls.c --obj
if %errorlevel% neq 0 (
    echo [ERROR] 15_nested_calls.c compilation failed.
    exit /b %errorlevel%
)
.\tests\15_nested_calls.exe
echo 15_nested_calls result: %errorlevel%

echo [TEST 16_void_func]
.\build\Debug\fadors99.exe tests\16_void_func.c --obj
if %errorlevel% neq 0 (
    echo [ERROR] 16_void_func.c compilation failed.
    exit /b %errorlevel%
)
.\tests\16_void_func.exe
echo 16_void_func result: %errorlevel%

ENDLOCAL
