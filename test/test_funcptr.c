/* test_funcptr.c
   Tests function pointers: declaration, assignment, call, arrays,
   passing as arguments, storing in structs, and double fp.
   Build: ./bake -o test_funcptr.s test_funcptr.c && gcc -o test_funcptr test_funcptr.s && ./test_funcptr
   Expected exit code: 0
*/

int printf(char *fmt, ...);

int add(int a, int b) { return a + b; }
int mul(int a, int b) { return a * b; }
int sub(int a, int b) { return a - b; }
int identity(int x)   { return x; }

double dadd(double a, double b) { return a + b; }
double dmul(double a, double b) { return a * b; }

/* higher-order: accept fp as parameter */
int apply(int (*fp)(int, int), int x, int y) {
    return fp(x, y);
}

/* higher-order: accept unary fp */
int apply1(int (*fp)(int), int x) {
    return fp(x);
}

/* struct with fp members */
struct MathOps {
    int (*add)(int, int);
    int (*mul)(int, int);
    int (*sub)(int, int);
};

int main(void) {
    int failures;
    failures = 0;

    /* 1. basic declaration and call */
    int (*fp)(int, int);
    fp = add;
    if (fp(3, 4) != 7)  { printf("FAIL: basic fp call\n");   failures = failures + 1; }
    fp = mul;
    if (fp(3, 4) != 12) { printf("FAIL: fp reassign mul\n"); failures = failures + 1; }
    fp = sub;
    if (fp(9, 4) != 5)  { printf("FAIL: fp reassign sub\n"); failures = failures + 1; }

    /* 2. unary fp */
    int (*fp1)(int);
    fp1 = identity;
    if (fp1(99) != 99) { printf("FAIL: unary fp\n"); failures = failures + 1; }

    /* 3. pass fp as argument */
    if (apply(add, 10, 5) != 15) { printf("FAIL: pass fp add\n"); failures = failures + 1; }
    if (apply(mul, 10, 5) != 50) { printf("FAIL: pass fp mul\n"); failures = failures + 1; }
    if (apply1(identity, 77) != 77) { printf("FAIL: pass fp1\n"); failures = failures + 1; }

    /* 4. array of function pointers */
    int (*ops[3])(int, int);
    ops[0] = add;
    ops[1] = mul;
    ops[2] = sub;
    if (ops[0](6, 7) != 13) { printf("FAIL: fp array[0]\n"); failures = failures + 1; }
    if (ops[1](6, 7) != 42) { printf("FAIL: fp array[1]\n"); failures = failures + 1; }
    if (ops[2](6, 7) != -1) { printf("FAIL: fp array[2]\n"); failures = failures + 1; }

    /* 5. loop through fp array */
    int sum;
    sum = 0;
    int i;
    for (i = 0; i < 3; i = i + 1)
        sum = sum + ops[i](i + 1, 2);
    /* add(1,2)=3, mul(2,2)=4, sub(3,2)=1 -> 8 */
    if (sum != 8) { printf("FAIL: fp loop sum=%d\n", sum); failures = failures + 1; }

    /* 6. double function pointer */
    double (*dfp)(double, double);
    dfp = dadd;
    if (dfp(1.5, 2.5) != 4.0) { printf("FAIL: double fp add\n"); failures = failures + 1; }
    dfp = dmul;
    if (dfp(2.0, 3.5) != 7.0) { printf("FAIL: double fp mul\n"); failures = failures + 1; }

    /* 7. struct with fp members */
    struct MathOps ops2;
    ops2.add = add;
    ops2.mul = mul;
    ops2.sub = sub;
    if (ops2.add(3, 4) != 7)  { printf("FAIL: struct fp add\n"); failures = failures + 1; }
    if (ops2.mul(3, 4) != 12) { printf("FAIL: struct fp mul\n"); failures = failures + 1; }
    if (ops2.sub(9, 4) != 5)  { printf("FAIL: struct fp sub\n"); failures = failures + 1; }

    /* 8. null check */
    int (*np)(int, int);
    np = 0;
    if (np != 0) { printf("FAIL: null fp check\n"); failures = failures + 1; }
    np = add;
    if (np == 0) { printf("FAIL: non-null fp\n"); failures = failures + 1; }

    if (failures == 0) { printf("All tests passed.\n"); return 0; }
    printf("%d test(s) failed.\n", failures);
    return 1;
}
