#include "arch/mod.h"
#include "lib/mod.h"

volatile static bool started = false;

int main()
{
    if (mycpuid() == 0)
    {
        print_init();
        __sync_synchronize();
        started = true;

        printf("cpu %d is booting!\n", mycpuid());
    } else {
        while (!started);
        __sync_synchronize();
        
        printf("cpu %d is booting!\n", mycpuid());
    }
}