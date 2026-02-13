@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" > nul 2>&1

set "SOURCE=%~1"
if "%SOURCE%"=="" set "SOURCE=tests\01_return.c"
set "BASE=%~n1"
set "EXE=tests\%BASE%.exe"

echo [1/3] Compiling %SOURCE% to OBJ...
.\build\Release\fadors99.exe %SOURCE% --obj
if errorlevel 1 (
    echo Compilation failed.
    exit /b 1
)

echo [2/3] Linking tests\%BASE%.obj to %EXE%...
link.exe /nologo /entry:main /subsystem:console /out:"%EXE%" "tests\%BASE%.obj" kernel32.lib
if errorlevel 1 (
    echo Linking failed.
    exit /b 1
)

echo [3/3] Executing %EXE%...
%EXE%
set RET=%errorlevel%
echo Return Code: %RET%
