#include <stdio.h>

int main(){
    int sum = 0;
    for (int i=0; i < 10; i++){
        sum += i;
    }

    for (int j=10; j >= 0; j--){
        sum -= j;
    }

    return 0;
}
