# bake toolchain Makefile
# ---------------------------------------------------------------
# Binaries:
#   bake      -- C compiler + assembler (x86-64 → ELF via xasm)
#   bake_wasm -- C compiler → WebAssembly Text Format (.wat)
#   xasm      -- standalone assembler: .asm/.s → ELF
#
# Integrated pipeline examples:
#   ./bake foo.c -o foo.s -dyn    → compiles + assembles → ./foo
#   ./bake foo.c -o foo.s         → assembly only → foo.s
#   ./xasm foo.s foo              → assemble separately
# ---------------------------------------------------------------

CC     = gcc
CFLAGS = -Wall -std=c11 -O2 -D_GNU_SOURCE

all: bake bake_wasm xasm

# ---------------------------------------------------------------
# bake: C compiler with integrated xasm pipeline
# ---------------------------------------------------------------

bake: bake_frontend.o codegen_x86.o
	$(CC) $(CFLAGS) -o $@ $^

bake_wasm: bake_frontend.o codegen_wasm.o
	$(CC) $(CFLAGS) -o $@ $^

xasm: xasm.c
	$(CC) $(CFLAGS) -o $@ $<

# ---------------------------------------------------------------
# Object files
# ---------------------------------------------------------------

bake_frontend.o: bake_frontend.c bake.h
	$(CC) $(CFLAGS) -c -o $@ $<

codegen_x86.o: codegen_x86.c bake.h
	$(CC) $(CFLAGS) -c -o $@ $<

codegen_wasm.o: codegen_wasm.c bake.h
	$(CC) $(CFLAGS) -c -o $@ $<

# ---------------------------------------------------------------
# Compile a .c file all the way to a native ELF in one step:
#   make foo          (dynamically linked, default)
#   make foo MODE=exe (statically linked)
#   make foo MODE=obj (relocatable object)
#
# Requires both bake and xasm to be built first.
# ---------------------------------------------------------------

MODE ?= dyn

%: %.c bake xasm
	./bake $< -o $<.s -$(MODE)
	@rm -f $<.s   # bake already invoked xasm; .s is intermediate

# ---------------------------------------------------------------
# WebAssembly
# ---------------------------------------------------------------

%.wat: %.c bake_wasm
	./bake_wasm -target wasm $< -o $@

%.wasm: %.wat
	wat2wasm $< -o $@

# ---------------------------------------------------------------

clean:
	rm -f *.o bake bake_wasm xasm *.s *.wat *.wasm

.PHONY: all clean
