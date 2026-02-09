@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" > nul
set "SOURCE=%~1"
set "BASE=%~n1"
set "OBJ=tests\\%BASE%.obj"
set "EXE=tests\%BASE%.exe"

echo [1/3] Compiling %SOURCE% to %OBJ%...
.\build\Release\fadors99.exe %SOURCE% --obj
if errorlevel 1 exit /b 1

echo [2/3] Linking %OBJ% to %EXE%...
link.exe /nologo /entry:main /subsystem:console /out:"%EXE%" "%OBJ%" kernel32.lib
if errorlevel 1 exit /b 1

echo [3/3] Running %EXE%...
"%EXE%"
set "RET=%errorlevel%"
echo Return Code: %RET%
exit /b %RET%
