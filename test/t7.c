int printf(char *fmt, ...);
int main() {
    int i; int j;
    for (i = 0; i < 3; i++) {
        for (j = 0; j < 3; j++) {
            printf("%d", i * 3 + j);
        }
        printf("\n");
    }
    return 0;
}