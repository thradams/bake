# bake — A Small C Compiler

A single-pass C compiler that supports multiple backends. The frontend parses and resolves a substantial subset of C; backends emit x86-64 AT&T assembly or WebAssembly Text Format (WAT).

---

## File Structure

| File | Role |
|---|---|
| `bake.h` | Shared header — all enums, structs, and cross-boundary declarations |
| `bake_frontend.c` | Lexer, parser, type system, symbol table, resolver, `main()` |
| `codegen_x86.c` | x86-64 AT&T assembly backend |
| `codegen_wasm.c` | WebAssembly Text Format (WAT) backend |

---

## Building

```sh
# x86-64 backend
make bake

# WebAssembly backend
make bake_wasm

# Both
make bake bake_wasm
```

Manual build:

```sh
gcc -Wall -std=c11 -c bake_frontend.c -o bake_frontend.o
gcc -Wall -std=c11 -c codegen_x86.c   -o codegen_x86.o
gcc -Wall -std=c11 -c codegen_wasm.c  -o codegen_wasm.o

gcc bake_frontend.o codegen_x86.o  -o bake
gcc bake_frontend.o codegen_wasm.o -o bake_wasm
```

---

## Usage

```sh
# Compile to x86-64 assembly
./bake input.c -o output.s
gcc output.s -o program

# Compile to WebAssembly Text Format
./bake_wasm -target wasm input.c -o output.wat
wat2wasm output.wat -o output.wasm   # requires wabt
```

### Flags

| Flag | Description |
|---|---|
| `-o <file>` | Write output to `<file>` (default: stdout) |
| `-target x86` | Select x86-64 backend (default for `bake`) |
| `-target wasm` | Select WebAssembly backend (default for `bake_wasm`) |

---

## Supported C Features

### Types

- `int`, `char`, `short`, `long`, `long long`
- `unsigned` variants
- `float`, `double`
- Pointers and pointer arithmetic
- Arrays (fixed-size)
- `struct` and `union` (with designated member access)
- Function pointers (partial)
- `void`

### Expressions

- All arithmetic and bitwise operators: `+ - * / % & | ^ ~ << >>`
- All comparison operators: `== != < > <= >=`
- Logical `&&`, `||`, `!` with short-circuit evaluation
- Assignment and compound assignment: `= += -= *= /= %= &= |= ^= <<= >>=`
- Pre/post increment and decrement: `++ --`
- Address-of `&`, dereference `*`
- Member access `.` and `->`
- Array subscript `[]`
- Ternary `?:`
- Explicit casts `(type)expr`
- Function calls

### Statements

- `if` / `else`
- `while`, `do`/`while`, `for`
- `break`, `continue`
- `return`
- Block scope `{}`
- Local variable declarations with initializers

### Top-level

- Function definitions and prototypes
- Global variable declarations
- `struct` / `union` definitions
- `extern` declarations (via prototype table)
- Variadic functions (declared with `...`, called from x86 backend)

---

## Static Initializers

bake supports C99-style initializer lists for both global and local variables.

### Syntax

```c
// Sequential array initializer
int primes[5] = {2, 3, 5, 7, 11};

// Designated array initializer (gaps are zero-filled)
int sparse[8] = {[0]=1, [3]=99, [7]=42};

// Empty initializer (all zeros)
int zeros[4] = {};

// Sequential struct initializer
struct Point { int x; int y; };
struct Point p1 = {10, 20};

// Designated struct initializer (order doesn't matter)
struct Point p2 = {.y=5, .x=3};

// Nested
struct Rect { struct Point a; struct Point b; };
struct Rect r = {{0,0}, {100,100}};

// Local variable initializers follow the same rules
void foo(void) {
    int a[4] = {1, 2, 3, 4};
    struct Point p = {.x=7, .y=8};
}
```

### How it works

**Parsing** — `parse_initializer(type)` is called instead of `parse_expr()` at every `=` site. It detects `{` for brace mode and handles:
- Sequential elements (index tracked by `seq_idx`)
- Designated field initializers: `.field = value`
- Designated index initializers: `[N] = value`
- Trailing commas
- Nested brace lists (recursively)

Falls through to `parse_expr()` for plain scalar initializers — fully backward-compatible.

**AST** — Brace initializers produce `ND_INIT_LIST` nodes. Each element node carries:
- `stmts` — linked list of element nodes (chained via `->next`)
- `lhs` — the element value (scalar expr or nested `ND_INIT_LIST`)
- `name` — designated field name for `.field=`, or `NULL`
- `ival` — element index (sequential or from `[N]=`)

**Resolution** — `resolve_init(n, type)` walks the `ND_INIT_LIST` tree, resolving each element expression against the correct sub-type (array element type, or struct member type found by name or position).

**x86 codegen (globals)** — `emit_init()` dispatches to:
- `emit_array_init()` — builds an index-addressed element table, emits `.long`/`.quad` for each slot, `.zero N` for gaps
- `emit_struct_init()` — walks members in declaration order, matches by designator name or sequential position, inserts padding bytes between members
- `emit_scalar_init()` — handles `ND_INT`, `ND_FLOAT`, and `ND_STR` (string pointer → `.quad label` + `.rodata` entry)

**x86 codegen (locals)** — `emit_local_init_list()` generates runtime stores:
1. Zeroes the entire stack slot with `rep stosb`
2. Stores only the explicitly provided elements at their `rbp`-relative offsets

**wasm codegen** — `wasm_serial_init()` serialises the initializer tree into a flat little-endian byte buffer, emitted as a WAT `(data (i32.const addr) "...")` segment.

---

## Architecture

### Frontend (`bake_frontend.c`)

The frontend is a single-pass recursive descent compiler. All stages share the arena allocator — there is no separate free.

```
Source text
    │
    ▼
Lexer (lex_all)
    │  Token stream in tokens[]
    ▼
Parser (parse_program)
    │  AST: linked list of Node*
    ▼
Resolver (resolve_func)
    │  Annotates AST with types, offsets, scopes
    ▼
Codegen (gen_func / gen_gvar)
    │  Called by main() via backend
    ▼
Output file
```

**Arena allocator** — All AST nodes, types, strings, and symbol table entries are allocated from a single 32 MB arena. There is no garbage collection; the arena is reset between compilation units.

**Symbol table** — Scoped via a linked list of `Scope` structs, each holding a `Sym` list. `push_scope()` / `pop_scope()` manage function and block scopes. A flat `ProtoEntry` list tracks function prototypes globally.

**Type system** — Types are heap-allocated `Type` structs. Pointer types are constructed on demand by `ptr_to(base)`. Struct types are registered in a tag table for forward references.

**Resolver** — `resolve_func()` walks the AST for each function:
- Assigns `rbp`-relative offsets to local variables
- Annotates `ND_IDENT` nodes with `is_local`, `offset`, and `type`
- Computes total frame size (stored in `fn->ival`) for the codegen prologue

### x86-64 Backend (`codegen_x86.c`)

Follows the System V AMD64 ABI.

- Expressions are evaluated onto a **push/pop stack** in `rax`; floats use `xmm0`
- Function arguments are passed in `rdi rsi rdx rcx r8 r9` (integer) and `xmm0..xmm7` (float) per ABI
- Large structs (> 16 bytes) use a hidden first pointer argument (`sret`)
- Small structs (≤ 16 bytes) are passed/returned in register pairs `rax:rdx`
- String literals and float constants are emitted into `.rodata` as `.LC_N:` labels
- Global variables go into `.data` (initialised) or `.bss` (zero)

### WebAssembly Backend (`codegen_wasm.c`)

Emits a self-contained `.wat` file that `wat2wasm` can assemble.

- All integers use `i64`; floats use `f32` or `f64`; pointers use `i32` (wasm32 model)
- Local variables map to indexed wasm locals (params first, then `$__sp_saved`, then others)
- A **software stack** (`$__sp` global, 1 MB) handles struct-by-value and frame allocation
- Global variables are laid out in linear memory starting at address 4096
- String literals and static data are placed above the software stack in linear memory
- Struct copies use the `memory.copy` instruction
- Control flow maps directly: `if/else` → `if/else/end`, loops → `block/loop/br_if`
- Imports are emitted for any prototype that has no function body (e.g. `malloc`, `free`, `printf`)

---

## Known Limitations

| Area | Limitation |
|---|---|
| Preprocessor | Not supported — `#include`, `#define`, etc. will error |
| `goto` / labels | Not supported in wasm backend |
| Variadic calls | Not supported in wasm backend |
| Compound assigns to globals | Wasm backend supports `=` only (not `+=` etc.) for global memory slots |
| `sizeof` | Not implemented |
| `typedef` | Not implemented |
| Initializers for local structs | From function-call return values only; brace init supported for scalars and arrays |

---

## Adding a New Backend

Any file that implements these three functions (declared in `bake.h`) can be linked with `bake_frontend.o` to form a new compiler target:

```c
// Called once per function definition (after resolve_func)
void gen_func(struct Node *fn);

// Called once per global variable
void gen_gvar(struct Node *gv);

// Line-oriented output — write one line to the `out` FILE*
void emit(const char *fmt, ...);
```

Optionally, implement the wasm-style module open/close hooks for formats that need a wrapper:

```c
void wasm_emit_module_open(struct Node *program);
void wasm_emit_module_close(struct Node *program);
```

Add a rule to the `Makefile`:

```makefile
bake_mybackend: bake_frontend.o codegen_mybackend.o
	$(CC) $(CFLAGS) -o $@ $^
```

---

## Example

```c
// hello.c
int printf(char *fmt, ...);

struct Point { int x; int y; };

int coords[3] = {[0]=10, [1]=20, [2]=30};

int dot(struct Point a, struct Point b) {
    return a.x * b.x + a.y * b.y;
}

int main(void) {
    struct Point p = {.x=3, .y=4};
    struct Point q = {.x=1, .y=2};
    int d = dot(p, q);
    printf("dot = %d\n", d);       // prints: dot = 11
    printf("coords[1] = %d\n", coords[1]); // prints: coords[1] = 20
    return 0;
}
```

```sh
./bake hello.c -o hello.s && gcc hello.s -o hello && ./hello
# dot = 11
# coords[1] = 20

./bake_wasm -target wasm hello.c -o hello.wat && wat2wasm hello.wat -o hello.wasm
```
