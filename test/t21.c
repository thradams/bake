int printf(char *fmt, ...);
int sign(int x) {
    if (x < 0) return 1;
    if (x < 0) return -1;
    return 0;
}
int main() {
    printf("%d\n", sign(5));
    printf("%d\n", sign(-3));
    printf("%d\n", sign(0));
    return 0;
}