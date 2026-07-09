#include "arch/mod.h"
#include "lib/mod.h"

int main()
{
    print_init();
    
    printf("cpu %d is booting!\n", mycpuid());
}