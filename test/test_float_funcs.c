/* test_float_funcs.c
   Tests passing float/double as function arguments and return values.
   Build: ./bake -o test_float_funcs.s test_float_funcs.c && gcc -o test_float_funcs test_float_funcs.s && ./test_float_funcs
   Expected exit code: 0
*/

int printf(char *fmt, ...);

double square(double x) {
    return x * x;
}

double lerp(double a, double b, double t) {
    return a + (b - a) * t;
}

float fclamp(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

double sum4(double a, double b, double c, double d) {
    return a + b + c + d;
}

int sign(double x) {
    if (x > 0.0) return 1;
    if (x < 0.0) return -1;
    return 0;
}

double absval(double x) {
    if (x < 0.0) return -x;
    return x;
}

int main(void) {
    int failures;
    failures = 0;

    if (square(5.0) != 25.0)  { printf("FAIL: square\n");      failures = failures + 1; }
    if (square(0.0) != 0.0)   { printf("FAIL: square zero\n"); failures = failures + 1; }
    if (square(-3.0) != 9.0)  { printf("FAIL: square neg\n");  failures = failures + 1; }

    if (lerp(0.0, 10.0, 0.5) != 5.0)  { printf("FAIL: lerp mid\n");   failures = failures + 1; }
    if (lerp(0.0, 10.0, 0.0) != 0.0)  { printf("FAIL: lerp start\n"); failures = failures + 1; }
    if (lerp(0.0, 10.0, 1.0) != 10.0) { printf("FAIL: lerp end\n");   failures = failures + 1; }

    if (fclamp(1.5f,  0.0f, 1.0f) != 1.0f) { printf("FAIL: fclamp hi\n");  failures = failures + 1; }
    if (fclamp(-0.5f, 0.0f, 1.0f) != 0.0f) { printf("FAIL: fclamp lo\n");  failures = failures + 1; }
    if (fclamp(0.7f,  0.0f, 1.0f) != 0.7f) { printf("FAIL: fclamp mid\n"); failures = failures + 1; }

    if (sum4(1.0, 2.0, 3.0, 4.0) != 10.0) { printf("FAIL: sum4\n"); failures = failures + 1; }

    if (sign(3.14)  !=  1) { printf("FAIL: sign pos\n");  failures = failures + 1; }
    if (sign(-2.72) != -1) { printf("FAIL: sign neg\n");  failures = failures + 1; }
    if (sign(0.0)   !=  0) { printf("FAIL: sign zero\n"); failures = failures + 1; }

    if (absval(-5.0) != 5.0) { printf("FAIL: absval neg\n"); failures = failures + 1; }
    if (absval(5.0)  != 5.0) { printf("FAIL: absval pos\n"); failures = failures + 1; }
    if (absval(0.0)  != 0.0) { printf("FAIL: absval zero\n"); failures = failures + 1; }

    if (failures == 0) { printf("All tests passed.\n"); return 0; }
    printf("%d test(s) failed.\n", failures);
    return 1;
}
