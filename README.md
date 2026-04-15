# Vibe coding C compiler

This is a simplified C89 compiler

- no preprocessor
- no typedef
- no constant expressions
- no switch
- no enum
- type check is minimal for codegen
- array sizes are given. Example: int a[2] = {1, 2}
- long long is implemented.
- no others c99, c11, c23 features need to be implemented


```c

gcc bake.c -o bake

./bake file.c > file.s
as -o file.o file.s
gcc -o file file.o

./file


```
