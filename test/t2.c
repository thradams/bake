int printf(char *fmt, ...);
int main() {
    printf("%d\\n", 5 &gt; 3);
    printf("%d\\n", 5 &lt; 3);
    printf("%d\\n", 5 &gt;= 5);
    printf("%d\\n", 5 &lt;= 4);
    printf("%d\\n", 5 == 5);
    printf("%d\\n", 5 != 3);
    printf("%d\\n", 1 &amp;&amp; 1);
    printf("%d\\n", 1 &amp;&amp; 0);
    printf("%d\\n", 0 || 1);
    printf("%d\\n", 0 || 0);
    return 0;
}