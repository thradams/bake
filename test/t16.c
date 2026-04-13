int printf(char *fmt, ...);
int main() {
    int a; int b;
    a = 0xF0; b = 0x0F;
    printf("%d\\n", a &amp; b);
    printf("%d\\n", a | b);
    printf("%d\\n", a ^ b);
    printf("%d\\n", ~a &amp; 0xFF);
    printf("%d\\n", 1 &lt;&lt; 4);
    printf("%d\\n", 256 &gt;&gt; 4);
    return 0;
}