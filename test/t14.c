int printf(char *fmt, ...);
void swap(int *a, int *b) {
    int tmp; tmp = *a; *a = *b; *b = tmp;
}
int main() {
    int x; int y;
    x = 5; y = 10;
    swap(&x, &y);
    printf("%d %d\n", x, y);
    return 0;
}
