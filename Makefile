# bake -- multi-backend build
CC      = gcc
CFLAGS  = -Wall -Wextra -std=c11 -O2

# x86-64 backend (default)
bake: bake_frontend.o codegen_x86.o
	$(CC) $(CFLAGS) -o $@ $^

# WebAssembly WAT backend
bake_wasm: bake_frontend.o codegen_wasm.o
	$(CC) $(CFLAGS) -o $@ $^

bake_frontend.o: bake_frontend.c bake.h
	$(CC) $(CFLAGS) -c -o $@ $<

codegen_x86.o: codegen_x86.c bake.h
	$(CC) $(CFLAGS) -c -o $@ $<

codegen_wasm.o: codegen_wasm.c bake.h
	$(CC) $(CFLAGS) -c -o $@ $<

# ---------------------------------------------------------------
# Usage:
#   ./bake foo.c -o foo.s          # x86-64 assembly
#   ./bake_wasm -target wasm foo.c -o foo.wat  # WebAssembly text
#   wat2wasm foo.wat -o foo.wasm   # assemble to binary (needs wabt)
# ---------------------------------------------------------------

clean:
	rm -f *.o bake bake_wasm
