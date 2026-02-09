@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" > nul 2>&1

set "SOURCE=%~1"
if "%SOURCE%"=="" set "SOURCE=tests\01_return.c"
set "BASE=%~n1"
set "EXE=tests\%BASE%.exe"

echo [1] Compiling %SOURCE%...
.\build\Release\fadors99.exe %SOURCE% --obj
if errorlevel 1 (
    echo Compilation failed.
    exit /b 1
)

echo [2] Checking if %EXE% exists...
if not exist %EXE% (
    echo EXE was not created.
    exit /b 1
)

echo [3] Executing...
%EXE%
set RET=%errorlevel%
echo Return Code: %RET%
