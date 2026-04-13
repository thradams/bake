int printf(char *fmt, ...);
int main() {
    int i; i = 0;
    do {
        printf("%d\n", i);
        i++;
    } while (i < 3);
    return 0;
}
