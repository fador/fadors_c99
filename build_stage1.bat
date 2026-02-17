@echo off
cmake --build build --config Release
if %ERRORLEVEL% NEQ 0 goto error

echo Compiling Stage 1 arch_x86_64.c...
.\build\Release\fadors99.exe src\arch_x86_64.c --obj
if %ERRORLEVEL% NEQ 0 goto error
echo Compiling Stage 1 arch_x86.c...
.\build\Release\fadors99.exe src\arch_x86.c --obj
if %ERRORLEVEL% NEQ 0 goto error
echo Compiling Stage 1 ast.c...
.\build\Release\fadors99.exe src\ast.c --obj
if %ERRORLEVEL% NEQ 0 goto error
echo Compiling Stage 1 buffer.c...
.\build\Release\fadors99.exe src\buffer.c --obj
if %ERRORLEVEL% NEQ 0 goto error
echo Compiling Stage 1 codegen.c...
.\build\Release\fadors99.exe src\codegen.c --obj
if %ERRORLEVEL% NEQ 0 goto error
echo Compiling Stage 1 coff_writer.c...
.\build\Release\fadors99.exe src\coff_writer.c --obj
if %ERRORLEVEL% NEQ 0 goto error
echo Compiling Stage 1 encoder.c...
.\build\Release\fadors99.exe src\encoder.c --obj
if %ERRORLEVEL% NEQ 0 goto error
echo Compiling Stage 1 linker.c...
.\build\Release\fadors99.exe src\linker.c --obj
if %ERRORLEVEL% NEQ 0 goto error
echo Compiling Stage 1 dos_linker.c...
.\build\Release\fadors99.exe src\dos_linker.c --obj
if %ERRORLEVEL% NEQ 0 goto error
echo Compiling Stage 1 lexer.c...
.\build\Release\fadors99.exe src\lexer.c --obj
if %ERRORLEVEL% NEQ 0 goto error
echo Compiling Stage 1 pe_linker.c...
.\build\Release\fadors99.exe src\pe_linker.c --obj
if %ERRORLEVEL% NEQ 0 goto error
echo Compiling Stage 1 elf_writer.c...
.\build\Release\fadors99.exe src\elf_writer.c --obj
if %ERRORLEVEL% NEQ 0 goto error
echo Compiling Stage 1 main.c...
.\build\Release\fadors99.exe src\main.c --obj
if %ERRORLEVEL% NEQ 0 goto error
echo Compiling Stage 1 parser.c...
.\build\Release\fadors99.exe src\parser.c --obj
if %ERRORLEVEL% NEQ 0 goto error
echo Compiling Stage 1 preprocessor.c...
.\build\Release\fadors99.exe src\preprocessor.c --obj
if %ERRORLEVEL% NEQ 0 goto error
echo Compiling Stage 1 types.c...
.\build\Release\fadors99.exe src\types.c --obj
if %ERRORLEVEL% NEQ 0 goto error
echo Compiling Stage 1 optimizer.c...
.\build\Release\fadors99.exe src\optimizer.c --obj
if %ERRORLEVEL% NEQ 0 goto error
echo Compiling Stage 1 ir.c...
.\build\Release\fadors99.exe src\ir.c --obj
if %ERRORLEVEL% NEQ 0 goto error
echo Compiling Stage 1 pgo.c...
.\build\Release\fadors99.exe src\pgo.c --obj
if %ERRORLEVEL% NEQ 0 goto error

call .\link_stage1.bat
goto :eof

:error
echo Build failed with error %ERRORLEVEL%
exit /b %ERRORLEVEL%