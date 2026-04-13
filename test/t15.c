int printf(char *fmt, ...);
int my_strlen(char *s) {
    int n; n = 0;
    while (*s) { n++; s++; }
    return n;
}
int main() {
    printf("%d\n", my_strlen(""));
    printf("%d\n", my_strlen("hello"));
    printf("%d\n", my_strlen("hello world"));
    return 0;
}
