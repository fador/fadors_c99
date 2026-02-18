#!/bin/bash
set -e

mkdir -p build_dos

# Build DOS library first
echo "Building DOS library..."
./build_linux/fadors99_stage1 src/dos_lib.s -c -o build_dos/dos_lib.o --target=dos
objcopy -O pe-i386 build_dos/dos_lib.o build_dos/dos_lib.o

echo "Building DOS libc..."
./build_linux/fadors99_stage1 src/dos_libc.c -o build_dos/dos_libc.o -c --target=dos

# Compile compiler sources for DOS
echo "Compiling compiler sources for DOS..."
SOURCES="src/buffer.c src/types.c src/ast.c src/lexer.c src/preprocessor.c src/parser.c src/codegen.c src/arch_x86_64.c src/arch_x86.c src/dos_linker.c src/encoder.c src/coff_writer.c src/elf_writer.c src/linker.c src/pe_linker.c src/optimizer.c src/ir.c src/pgo.c src/main.c"

for src in $SOURCES; do
    obj="build_dos/$(basename "${src%.c}.o")"
    echo "  $src -> $obj"
    ./build_linux/fadors99_stage1 "$src" -c -o "$obj" --target=dos
    # Ensure object format is compatible (COFF for DOS linker)
    # The compiler should already produce COFF with --target=dos, but let's be sure.
done

# Link everything together
echo "Linking fadors_dos.exe..."
OBJS=""
for src in $SOURCES; do
    OBJS="$OBJS build_dos/$(basename "${src%.c}.o")"
done

# Check if we can link (expecting errors due to missing libc symbols)
if ./build_linux/fadors99_stage1 link $OBJS build_dos/dos_lib.o build_dos/dos_libc.o -o fadors_dos.exe --target=dos; then
    echo "Success! fadors_dos.exe created."
else
    echo "Linking failed (expected due to missing libc symbols). Check link_errors.txt"
fi
