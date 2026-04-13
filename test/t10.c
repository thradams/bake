int printf(char *fmt, ...);
int fact(int n) {
    if (n <= 1) return 1;
    return n * fact(n - 1);
}
int fib(int n) {
    if (n <= 1) return n;
    return fib(n-1) + fib(n-2);
}
int main() {
    printf("%d\n", fact(1));
    printf("%d\n", fact(5));
    printf("%d\n", fact(10));
    printf("%d\n", fib(0));
    printf("%d\n", fib(1));
    printf("%d\n", fib(10));
    return 0;
}
