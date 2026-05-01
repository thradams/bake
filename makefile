# bake -- split build
CC      = gcc
CFLAGS  = -Wall -Wextra -std=c11 -O2

OBJS = bake.o codegen_x86.o

bake: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

bake_frontend.o: bake.c bake.h
	$(CC) $(CFLAGS) -c -o $@ $<

codegen_x86.o: codegen_x86.c bake.h
	$(CC) $(CFLAGS) -c -o $@ $<

# ---------------------------------------------------------------
# To add a new backend (e.g. WebAssembly):
#   1. cp codegen_x86.c codegen_wasm.c
#   2. Implement gen_func(), gen_gvar(), emit() for wasm
#   3. Add a rule below and link bake_wasm
# ---------------------------------------------------------------
bake_wasm: bake.o codegen_wasm.o
	$(CC) $(CFLAGS) -o $@ $^

codegen_wasm.o: codegen_wasm.c bake.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f *.o bake bake_wasm
