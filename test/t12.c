int printf(char *fmt, ...);
int sum(int a[], int n) {
    int s; int i;
    s = 0;
    for (i = 0; i < n; i++) s = s + a[i];
    return s;
}
int main() {
    int a[5];
    a[0]=1; a[1]=2; a[2]=3; a[3]=4; a[4]=5;
    printf("%d\n", sum(a, 5));
    return 0;
}