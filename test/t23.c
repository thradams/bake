int printf(char *fmt, ...);
int main() {
    int a[4];
    int *p;
    a[0]=10; a[1]=20; a[2]=30; a[3]=40;
    p = a;
    printf("%d\n", *p);
    p = p + 1;
    printf("%d\n", *p);
    p = p + 1;
    printf("%d\n", *p);
    return 0;
}
