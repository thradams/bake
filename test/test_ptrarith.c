int printf(char *fmt, ...);

int main(void) {
    int failures;
    failures = 0;

    /* --- basic pointer increment --- */
    int arr[5];
    arr[0] = 10; arr[1] = 20; arr[2] = 30; arr[3] = 40; arr[4] = 50;

    int *p;
    p = arr;
    if (*p != 10) { printf("FAIL: *p initial\n"); failures = failures + 1; }
    p = p + 1;
    if (*p != 20) { printf("FAIL: p+1\n"); failures = failures + 1; }
    p = p + 2;
    if (*p != 40) { printf("FAIL: p+2\n"); failures = failures + 1; }
    p = p - 1;
    if (*p != 30) { printf("FAIL: p-1\n"); failures = failures + 1; }

    /* --- pointer difference --- */
    int *a;
    int *b;
    a = arr;
    b = arr + 3;
    int diff;
    diff = b - a;
    if (diff != 3) { printf("FAIL: ptr diff (got %d)\n", diff); failures = failures + 1; }

    /* --- pointer comparison --- */
    if (!(a < b))  { printf("FAIL: ptr lt\n");  failures = failures + 1; }
    if (!(b > a))  { printf("FAIL: ptr gt\n");  failures = failures + 1; }
    if (!(a <= a)) { printf("FAIL: ptr leq\n"); failures = failures + 1; }
    if (!(b >= b)) { printf("FAIL: ptr geq\n"); failures = failures + 1; }
    if (a == b)    { printf("FAIL: ptr eq\n");  failures = failures + 1; }
    if (!(a != b)) { printf("FAIL: ptr neq\n"); failures = failures + 1; }

    /* --- pre/post increment/decrement --- */
    int *q;
    q = arr + 1;
    int *old;
    old = q++;
    if (*old != 20) { printf("FAIL: q++ old val\n"); failures = failures + 1; }
    if (*q   != 30) { printf("FAIL: q++ new val\n"); failures = failures + 1; }
    ++q;
    if (*q != 40) { printf("FAIL: ++q\n"); failures = failures + 1; }
    q--;
    if (*q != 30) { printf("FAIL: q--\n"); failures = failures + 1; }
    --q;
    if (*q != 20) { printf("FAIL: --q\n"); failures = failures + 1; }

    /* --- char pointer arithmetic (1-byte steps) --- */
    char str[4];
    str[0] = 'a'; str[1] = 'b'; str[2] = 'c'; str[3] = '\0';
    char *cp;
    cp = str;
    if (*cp != 'a') { printf("FAIL: char *cp\n"); failures = failures + 1; }
    cp = cp + 1;
    if (*cp != 'b') { printf("FAIL: char cp+1\n"); failures = failures + 1; }
    cp++;
    if (*cp != 'c') { printf("FAIL: char cp++\n"); failures = failures + 1; }

    /* --- pointer indexing equivalence: p[i] == *(p+i) --- */
    int *r;
    r = arr;
    if (r[0] != *(r+0)) { printf("FAIL: r[0]\n"); failures = failures + 1; }
    if (r[2] != *(r+2)) { printf("FAIL: r[2]\n"); failures = failures + 1; }
    if (r[4] != *(r+4)) { printf("FAIL: r[4]\n"); failures = failures + 1; }

    /* --- double pointer step size --- */
    double darr[3];
    darr[0] = 1.1; darr[1] = 2.2; darr[2] = 3.3;
    double *dp;
    dp = darr;
    dp = dp + 1;
    if (*dp != 2.2) { printf("FAIL: double ptr+1\n"); failures = failures + 1; }
    dp++;
    if (*dp != 3.3) { printf("FAIL: double ptr++\n"); failures = failures + 1; }

    if (failures == 0) { printf("All tests passed.\n"); return 0; }
    printf("%d test(s) failed.\n", failures);
    return 1;
}
