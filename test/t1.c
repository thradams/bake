int printf(char *fmt, ...);
int main() {
    int a; int b;
    a = 10; b = 3;
    printf("%d\n", a + b);
    printf("%d\n", a - b);
    printf("%d\n", a * b);
    printf("%d\n", a / b);
    printf("%d\n", a % b);
    return 0;
}