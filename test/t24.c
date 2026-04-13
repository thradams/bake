int printf(char *fmt, ...);
void sort(int a[], int n) {
    int i; int j; int tmp;
    for (i = 0; i < n-1; i++) {
        for (j = 0; j < n-1-i; j++) {
            if (a[j] > a[j+1]) {
                tmp = a[j]; a[j] = a[j+1]; a[j+1] = tmp;
            }
        }
    }
}
int main() {
    int a[5];
    int i;
    a[0]=5; a[1]=3; a[2]=1; a[3]=4; a[4]=2;
    sort(a, 5);
    for (i = 0; i < 5; i++) printf("%d\n", a[i]);
    return 0;
}