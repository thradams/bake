int printf(char *fmt, ...);
int main() {
    int x; int y;
    int *p;
    x = 10;
    p = &amp;x;
    printf("%d\\n", *p);
    *p = 99;
    printf("%d\\n", x);
    y = 7;
    p = &amp;y;
    printf("%d\\n", *p);
    return 0;
}
