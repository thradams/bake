int printf(char *fmt, ...);
int main() {
    int i;
    for (i = 0; i < 5; i++) {
        printf("%d ", i);
    }
    printf("\n");
    return 0;
}