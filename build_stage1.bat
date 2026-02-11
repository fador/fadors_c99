cmake --build build --config Release; 
.\build\Release\fadors99.exe src\arch_x86_64.c --obj
.\build\Release\fadors99.exe src\ast.c --obj
.\build\Release\fadors99.exe src\buffer.c --obj
.\build\Release\fadors99.exe src\codegen.c --obj
.\build\Release\fadors99.exe src\coff_writer.c --obj
.\build\Release\fadors99.exe src\encoder.c --obj
.\build\Release\fadors99.exe src\lexer.c --obj
.\build\Release\fadors99.exe src\main.c --obj
.\build\Release\fadors99.exe src\parser.c --obj
.\build\Release\fadors99.exe src\preprocessor.c --obj
.\build\Release\fadors99.exe src\types.c --obj
.\link_stage1.bat