int printf(char *fmt, ...);
int add(int a, int b) { return a + b; }
int mul(int a, int b) { return a * b; }
int max(int a, int b) { if (a &gt; b) return a; return b; }
int abs_val(int x) { if (x &lt; 0) return -x; return x; }
int main() {
    printf("%d\\n", add(3, 4));
    printf("%d\\n", mul(6, 7));
    printf("%d\\n", max(10, 20));
    printf("%d\\n", max(30, 5));
    printf("%d\\n", abs_val(-42));
    printf("%d\\n", abs_val(7));
    return 0;
}