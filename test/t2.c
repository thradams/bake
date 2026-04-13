int printf(char *fmt, ...);
int main() {
    printf("%d\n", 5 > 3);
    printf("%d\n", 5 < 3);
    printf("%d\n", 5 >= 5);
    printf("%d\n", 5 <= 4);
    printf("%d\n", 5 == 5);
    printf("%d\n", 5 != 3);
    printf("%d\n", 1 && 1);
    printf("%d\n", 1 && 0);
    printf("%d\n", 0 || 1);
    printf("%d\n", 0 || 0);
    return 0;
}