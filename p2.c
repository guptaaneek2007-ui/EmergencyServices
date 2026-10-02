#include<stdio.h>
int fib(int);
int main()
{
   int n ,fibonacci;
    printf("Enter the no upto which you want fibonacci summation\n");
    scanf("%d",&n);
    fibonacci=fib(n);
    printf("The sum of fibonacci Series is %d\n",fibonacci);
    return 0;
}
int fib(int n)
{
    int i;
    int fibonacci=0;
    for(i=1;i<=n;i++)
    fibonacci=fibonacci+i;
    return (fibonacci);
}