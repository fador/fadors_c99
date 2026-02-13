@echo off
echo ===== Building Stage 2 using Stage 1 (multi-file) =====

call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"

echo Compiling and linking Stage 2...
.\fadors_stage1.exe ^
  src\buffer.c ^
  src\types.c ^
  src\ast.c ^
  src\lexer.c ^
  src\codegen.c ^
  src\preprocessor.c ^
  src\coff_writer.c ^
  src\pe_linker.c ^
  src\elf_writer.c ^
  src\linker.c ^
  src\encoder.c ^
  src\parser.c ^
  src\arch_x86_64.c ^
  src\main.c ^
  -o fadors_stage2.exe ^
  -llibcmt.lib -llegacy_stdio_definitions.lib -loldnames.lib

if %ERRORLEVEL% NEQ 0 (
    echo Stage 2 build failed!
    exit /b %ERRORLEVEL%
)

echo Stage 2 compiler built successfully: fadors_stage2.exe
