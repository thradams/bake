int printf(char *fmt, ...);
int counter = 0;
void increment() { counter = counter + 1; }
int main() {
    printf("%d\n", counter);
    increment();
    increment();
    increment();
    printf("%d\n", counter);
    return 0;
}