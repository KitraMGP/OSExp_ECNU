#include "arch/mod.h"
#include "lib/mod.h"
#include "mem/mod.h"

volatile static int started = 0;

int main()
{
    int cpuid = r_tp();
    if (cpuid == 0)
    {
        print_init();
        pmem_init();
        kvm_init();

        __sync_synchronize();
        started = 1;
    }
    else
    {
        while (started == 0)
            ;
        __sync_synchronize();
    }

    // 两个 CPU 都需要调用
    kvm_inithart();

    printf("cpu %d is booting!\n", cpuid);

    while (1)
        ;
}
