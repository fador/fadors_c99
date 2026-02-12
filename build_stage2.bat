@echo off
echo ===== Building Stage 2 using Stage 1 =====

echo Compiling Stage 2 buffer.c...
.\fadors_stage1.exe src\buffer.c --obj -S
if %ERRORLEVEL% NEQ 0 (
    echo FAILED: buffer.c
    goto error
)

echo Compiling Stage 2 types.c...
.\fadors_stage1.exe src\types.c --obj -S
if %ERRORLEVEL% NEQ 0 (
    echo FAILED: types.c
    goto error
)

echo Compiling Stage 2 ast.c...
.\fadors_stage1.exe src\ast.c --obj -S
if %ERRORLEVEL% NEQ 0 (
    echo FAILED: ast.c
    goto error
)

echo Compiling Stage 2 lexer.c...
.\fadors_stage1.exe src\lexer.c --obj -S
if %ERRORLEVEL% NEQ 0 (
    echo FAILED: lexer.c
    goto error
)

echo Compiling Stage 2 codegen.c...
.\fadors_stage1.exe src\codegen.c --obj -S
if %ERRORLEVEL% NEQ 0 (
    echo FAILED: codegen.c
    goto error
)

echo Compiling Stage 2 preprocessor.c...
.\fadors_stage1.exe src\preprocessor.c --obj -S
if %ERRORLEVEL% NEQ 0 (
    echo FAILED: preprocessor.c
    goto error
)

echo Compiling Stage 2 coff_writer.c...
.\fadors_stage1.exe src\coff_writer.c --obj -S
if %ERRORLEVEL% NEQ 0 (
    echo FAILED: coff_writer.c
    goto error
)

echo Compiling Stage 2 encoder.c...
.\fadors_stage1.exe src\encoder.c --obj -S
if %ERRORLEVEL% NEQ 0 (
    echo FAILED: encoder.c
    goto error
)

echo Compiling Stage 2 parser.c...
.\fadors_stage1.exe src\parser.c --obj -S
if %ERRORLEVEL% NEQ 0 (
    echo FAILED: parser.c
    goto error
)

echo Compiling Stage 2 arch_x86_64.c...
.\fadors_stage1.exe src\arch_x86_64.c --obj -S
if %ERRORLEVEL% NEQ 0 (
    echo FAILED: arch_x86_64.c
    goto error
)

echo Compiling Stage 2 main.c...
.\fadors_stage1.exe src\main.c --obj -S
if %ERRORLEVEL% NEQ 0 (
    echo FAILED: main.c
    goto error
)

echo All Stage 2 files compiled successfully!

call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
echo Linking Stage 2 compiler...
link /nologo /STACK:8000000 /subsystem:console /entry:mainCRTStartup /out:fadors_stage2.exe src\arch_x86_64.obj src\ast.obj src\buffer.obj src\codegen.obj src\coff_writer.obj src\encoder.obj src\lexer.obj src\main.obj src\parser.obj src\preprocessor.obj src\types.obj libcmt.lib legacy_stdio_definitions.lib
if %ERRORLEVEL% NEQ 0 (
    echo Linking Stage 2 failed!
    goto error
)

echo Stage 2 compiler linked successfully: fadors_stage2.exe
goto :eof

:error
echo Build failed with error %ERRORLEVEL%
exit /b %ERRORLEVEL%
