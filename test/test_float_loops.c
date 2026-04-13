/* test_float_loops.c
   Tests float/double in while loops, for loops, and arrays.
   Build: ./bake -o test_float_loops.s test_float_loops.c && gcc -o test_float_loops test_float_loops.s && ./test_float_loops
   Expected exit code: 0
*/

int printf(char *fmt, ...);

int main(void) {
    int failures;
    failures = 0;

    /* sum 1.0 + 2.0 + ... + 10.0 = 55.0 */
    double sum;
    sum = 0.0;
    double i;
    i = 1.0;
    while (i <= 10.0) {
        sum += i;
        i += 1.0;
    }
    if (sum != 55.0) { printf("FAIL: double while sum\n"); failures = failures + 1; }

    /* float dot product */
    float a[4];
    float b[4];
    a[0] = 1.0f; a[1] = 2.0f; a[2] = 3.0f; a[3] = 4.0f;
    b[0] = 4.0f; b[1] = 3.0f; b[2] = 2.0f; b[3] = 1.0f;
    float dot;
    dot = 0.0f;
    int k;
    k = 0;
    while (k < 4) {
        dot += a[k] * b[k];
        k = k + 1;
    }
    if (dot != 20.0f) { printf("FAIL: float dot product\n"); failures = failures + 1; }

    /* for loop: count doubles above threshold */
    double vals[5];
    vals[0] = 0.5;
    vals[1] = 1.5;
    vals[2] = 2.5;
    vals[3] = 0.1;
    vals[4] = 3.0;
    int cnt;
    cnt = 0;
    int j;
    for (j = 0; j < 5; j = j + 1) {
        if (vals[j] > 1.0) cnt = cnt + 1;
    }
    if (cnt != 3) { printf("FAIL: count doubles > 1.0\n"); failures = failures + 1; }

    /* double array max */
    double mx;
    mx = vals[0];
    for (j = 1; j < 5; j = j + 1) {
        if (vals[j] > mx) mx = vals[j];
    }
    if (mx != 3.0) { printf("FAIL: array max\n"); failures = failures + 1; }

    /* do-while with float counter */
    float fc;
    fc = 0.0f;
    float step;
    step = 0.25f;
    do {
        fc += step;
    } while (fc < 1.0f);
    if (fc != 1.0f) { printf("FAIL: do-while float\n"); failures = failures + 1; }

    if (failures == 0) { printf("All tests passed.\n"); return 0; }
    printf("%d test(s) failed.\n", failures);
    return 1;
}
