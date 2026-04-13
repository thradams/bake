int printf(char *fmt, ...);
int main() {
    char s[6];
    s[0]='h'; s[1]='e'; s[2]='l'; s[3]='l'; s[4]='o'; s[5]=0;
    printf("%s\n", s);
    int i;
    for (i = 0; s[i]; i++) printf("%d ", s[i]);
    printf("\n");
    return 0;
}
