/* test_basic_float.c
   Tests basic float and double arithmetic, comparisons, and casts.
   Build: ./bake -o test_basic_float.s test_basic_float.c && gcc -o test_basic_float test_basic_float.s && ./test_basic_float
   Expected exit code: 0
*/

int printf(char *fmt, ...);

static int failures = 0;

void check(int cond, char *msg) {
    if (!cond) {
        printf("FAIL: %s\n", msg);
        failures = failures + 1;
    }
}

int main(void) {
    double a;
    double b;
    a = 3.0;
    b = 4.0;
    check(a + b == 7.0,  "double add");
    check(b - a == 1.0,  "double sub");
    check(a * b == 12.0, "double mul");
    check(a / b == 0.75, "double div");

    float fa;
    float fb;
    fa = 1.5f;
    fb = 2.5f;
    check(fa + fb == 4.0f,  "float add");
    check(fb - fa == 1.0f,  "float sub");
    check(fa * fb == 3.75f, "float mul");
    check(fb / fa == 5.0f / 3.0f, "float div");

    double x;
    double y;
    x = 1.0;
    y = 2.0;
    check(x < y,  "double lt");
    check(y > x,  "double gt");
    check(x <= x, "double leq");
    check(y >= y, "double geq");
    check(x != y, "double neq");
    check(x == x, "double eq");

    int i;
    i = 42;
    double d;
    d = (double)i;
    check(d == 42.0, "int to double");
    int j;
    j = (int)d;
    check(j == 42, "double to int");

    float f;
    f = (float)7;
    check(f == 7.0f, "int to float");
    int k;
    k = (int)f;
    check(k == 7, "float to int");

    float sf;
    sf = 1.0f;
    double sd;
    sd = (double)sf;
    check(sd == 1.0, "float to double");
    float sf2;
    sf2 = (float)2.0;
    check(sf2 == 2.0f, "double to float");

    double neg;
    neg = -a;
    check(neg == -3.0, "double unary minus");
    float fneg;
    fneg = -fa;
    check(fneg == -1.5f, "float unary minus");

    double acc;
    acc = 10.0;
    acc += 5.0;
    check(acc == 15.0, "double +=");
    acc -= 3.0;
    check(acc == 12.0, "double -=");
    acc *= 2.0;
    check(acc == 24.0, "double *=");
    acc /= 4.0;
    check(acc == 6.0,  "double /=");

    if (failures == 0) {
        printf("All tests passed.\n");
        return 0;
    }
    printf("%d test(s) failed.\n", failures);
    return 1;
}
