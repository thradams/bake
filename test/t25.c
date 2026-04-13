int printf(char *fmt, ...);
int power(int base, int exp) {
    int result; result = 1;
    while (exp > 0) { result = result * base; exp = exp - 1; }
    return result;
}
int main() {
    printf("%d\n", power(2, 0));
    printf("%d\n", power(2, 8));
    printf("%d\n", power(3, 4));
    printf("%d\n", power(10, 3));
    return 0;
}