int printf(char *fmt, ...);
int main() {
    int x; x = 10;
    printf("%d\n", x < 5 ? 1 : 0);
    printf("%d\n", x < 5 ? 1 : 0);
    printf("%s\n", x == 10 ? "yes" : "no");
    return 0;
}