#include<stdio.h>
int isprime(int);
int main()
{
    int n,Prime;
    char x;
    do
    {
     printf("Enter the no. you want to check\n");
     scanf("%d",&n);
     if(isprime(n))
      printf("Prime\n");
     else
    { 
     printf("Not Prime\n");
    }
    printf("Do you want to continue\n");
    scanf(" %c",&x);
   }
    while(x=='Y'||x=='y');
 printf("The checker is closed\n");
}
int isprime(int n)
{
    int i;
    int Prime=2;
    if(n==0||n==1)
    {  
     printf("void nos.\n");
     return 0; 
    }
    for(i=2;i<n;i++)
    {
        if(n%i==0)
        return 0;
    }
    return 1;
}