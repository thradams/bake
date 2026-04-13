/* test_float_mixed.c
   Tests mixed int/float expressions, ternary, logical ops, and global float vars.
   Build: ./bake -o test_float_mixed.s test_float_mixed.c && gcc -o test_float_mixed test_float_mixed.s && ./test_float_mixed
   Expected exit code: 0
*/

int printf(char *fmt, ...);

double PI = 3.141592653589793;
float  E  = 2.718281828f;

double scale(double v, int factor) {
    return v * (double)factor;
}

int main(void) {
    int failures;
    failures = 0;

    /* global float/double vars */
    if (PI < 3.14 || PI > 3.15) { printf("FAIL: global double PI\n"); failures = failures + 1; }
    if (E  < 2.71f || E > 2.72f){ printf("FAIL: global float E\n");   failures = failures + 1; }

    /* mixed int * double */
    double r;
    r = scale(2.0, 5);
    if (r != 10.0) { printf("FAIL: mixed int/double arg\n"); failures = failures + 1; }

    /* ternary with double result */
    double t;
    t = (1 < 2) ? 9.0 : 1.0;
    if (t != 9.0) { printf("FAIL: ternary double true\n");  failures = failures + 1; }
    t = (1 > 2) ? 9.0 : 1.0;
    if (t != 1.0) { printf("FAIL: ternary double false\n"); failures = failures + 1; }

    /* logical && and || with doubles */
    double p;
    double q;
    p = 1.0;
    q = 0.0;
    if (!(p && 1))  { printf("FAIL: double && true\n");  failures = failures + 1; }
    if (q && 1)     { printf("FAIL: double && false\n"); failures = failures + 1; }
    if (!(q || p))  { printf("FAIL: double || true\n");  failures = failures + 1; }
    if (q || 0.0)   { printf("FAIL: double || false\n"); failures = failures + 1; }

    /* !double */
    if (!p)  { printf("FAIL: !nonzero double\n"); failures = failures + 1; }
    if (!(!q)){ printf("FAIL: !zero double\n");   failures = failures + 1; }

    /* int + double promotion */
    int n;
    n = 3;
    double res;
    res = (double)n + 0.5;
    if (res != 3.5) { printf("FAIL: int+double promotion\n"); failures = failures + 1; }

    /* float pre/post inc */
    float f;
    f = 1.0f;
    float old;
    old = f++;
    if (old != 1.0f) { printf("FAIL: float post-inc old\n"); failures = failures + 1; }
    if (f   != 2.0f) { printf("FAIL: float post-inc new\n"); failures = failures + 1; }
    ++f;
    if (f != 3.0f)   { printf("FAIL: float pre-inc\n");      failures = failures + 1; }
    f--;
    if (f != 2.0f)   { printf("FAIL: float pre-dec\n");      failures = failures + 1; }

    if (failures == 0) { printf("All tests passed.\n"); return 0; }
    printf("%d test(s) failed.\n", failures);
    return 1;
}
