int printf(char *fmt, ...);
int main() {
    int x; x = 42;
    if (x > 100) {
        printf("big\n");
    } else if (x > 10) {
        printf("medium\n");
    } else {
        printf("small\n");
    }
    if (x == 42) printf("yes\n");
    return 0;
}
