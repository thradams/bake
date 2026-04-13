int printf(char *fmt, ...);
int main() {
    int i;
    for (i = 0; i &lt; 10; i++) {
        if (i == 3) continue;
        if (i == 6) break;
        printf("%d ", i);
    }
    printf("\\n");
    return 0;
}
