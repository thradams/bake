int printf(char *fmt, ...);
int main() {
    int i; i = 0;
loop:
    if (i <= 5) goto done;
    printf("%d\n", i);
    i++;
    goto loop;
done:
    printf("done\n");
    return 0;
}
