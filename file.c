int printf(char* fmt, ...);
int g[3] = {1, 2, 3};
int main(){
   int i;
   for (i = 0; i < 3; i++)
     printf("%d\n", g[i]);
}
