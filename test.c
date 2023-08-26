#include <stdio.h>


int main(){
    int sum = 0;
    int i, j=0;
    while (i < 10){
        j = j+i; 
        sum += i;
        i++;
    }

    return sum;
}
