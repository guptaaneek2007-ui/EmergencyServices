#include<stdio.h>
int fact(int);
int main()
{
    int num,factorial;
    printf("Enter the number you want to calculate factorial\n");
    scanf("%d",&num);
    factorial=fact(num);
    printf("The factorial of provided no is %d\n",factorial);
return 0;
}
int fact(int num)
{
    int i;
    int factorial=1;
    for(i=1;i<=num;i++)
    factorial=i*factorial;
    return (factorial);
}