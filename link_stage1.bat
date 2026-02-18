@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
echo Linking Stage 1 compiler...
link /nologo /STACK:8000000 /subsystem:console /out:fadors_stage1.exe src\arch_x86_64.obj src\arch_x86.obj src\ast.obj src\buffer.obj src\codegen.obj src\coff_writer.obj src\dos_linker.obj src\encoder.obj src\lexer.obj src\main.obj src\parser.obj src\preprocessor.obj src\types.obj src\optimizer.obj src\ir.obj src\pgo.obj src\pe_linker.obj src\elf_writer.obj src\linker.obj kernel32.lib libcmt.lib legacy_stdio_definitions.lib /entry:mainCRTStartup
if %ERRORLEVEL% EQU 0 (
    echo Stage 1 compiler linked successfully: fadors_stage1.exe
) else (
    echo Linking failed with error %ERRORLEVEL%.
    exit /b %ERRORLEVEL%
)
