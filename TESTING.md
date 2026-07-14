# LAB-2 测试代码与执行记录

本文集中保存内存管理实验的阶段性测试代码和实际输出。正式构建中的`src/kernel/main.c`只执行双 hart 启动 smoke test；以下测试通过临时替换`main.c`执行，测试结束后均恢复正式入口。

## 1. 运行方法

每次只启用一个测试入口，避免预期 panic 阻止后续用例。修改`main.c`后执行：

```sh
make clean
make build
make run
```

QEMU 在测试结束后通常仍处于内核自旋循环，可按`Ctrl-a x`退出。本文输出省略编译命令、RWE LOAD segment 链接警告和 timeout/QEMU 终止信息。

物理地址取决于测试代码体积、`ALLOC_BEGIN`和分配顺序；判断测试结果时应关注页对齐、区域归属、PTE 层级、flags 和断言结果，而不是固定地址。

## 2. 当前启动 Smoke Test

正式入口见[`src/kernel/main.c`](src/kernel/main.c)。hart 0 完成打印、物理分配器和内核页表初始化，所有 hart 随后启用分页。

实际输出：

```text
cpu 1 is booting!
cpu 0 is booting!
```

两行顺序不固定。能够在写入`satp`后继续取指、访问栈和 UART，说明内核代码、数据、栈及 UART 的恒等映射可用。

## 3. 物理内存测试

### 3.1 双 hart 并发申请和释放全部内核页

两个 hart 各申请、写入并释放 512 个内核页。参考用例会打印全部 1024 个页面；这里保留完整申请过程，但只打印每个 hart 获得的首尾地址。

```c
#include "arch/mod.h"
#include "lib/mod.h"
#include "mem/mod.h"

volatile static int started = 0;
volatile static int over_1 = 0;
volatile static int over_2 = 0;
static int *mem[KERN_PAGES];

static void alloc_kernel_pages(int begin, int end, int cpuid)
{
    for (int i = begin; i < end; i++)
    {
        mem[i] = pmem_alloc(true);
        memset(mem[i], 1, PGSIZE);
        assert(mem[i][0] == 0x01010101, "kernel page write failed");
    }

    printf("cpu %d pages: first = %p, last = %p\n",
           cpuid, mem[begin], mem[end - 1]);
    printf("cpu %d alloc over\n", cpuid);
}

static void free_kernel_pages(int begin, int end, int cpuid)
{
    for (int i = begin; i < end; i++)
        pmem_free((uint64)mem[i]);
    printf("cpu %d free over\n", cpuid);
}

int main()
{
    int cpuid = r_tp();
    int midpoint = KERN_PAGES / 2;

    if (cpuid == 0)
    {
        print_init();
        pmem_init();
        __sync_synchronize();
        started = 1;

        alloc_kernel_pages(0, midpoint, cpuid);
        __sync_synchronize();
        over_1 = 1;
        while (over_2 == 0)
            ;
        __sync_synchronize();
        free_kernel_pages(0, midpoint, cpuid);
    }
    else
    {
        while (started == 0)
            ;
        __sync_synchronize();

        alloc_kernel_pages(midpoint, KERN_PAGES, cpuid);
        __sync_synchronize();
        over_2 = 1;
        while (over_1 == 0)
            ;
        __sync_synchronize();
        free_kernel_pages(midpoint, KERN_PAGES, cpuid);
    }

    while (1)
        ;
}
```

实际输出：

```text
cpu 1 pages: first = 0x0000000080008000, last = 0x000000008029f000
cpu 1 alloc over
cpu 0 pages: first = 0x0000000080007000, last = 0x0000000080406000
cpu 0 alloc over
cpu 0 free over
cpu 1 free over
```

两个 hart 均完成 512 页的操作，没有出现重复分配、写入失败或锁错误。单个 hart 获得的地址不连续是并发取得同一空闲链表头的正常结果。

### 3.2 用户页释放、重新分配清零和内核池耗尽

`test_case_2`验证用户页的区域归属、写入、释放、LIFO 重新分配和自动清零；通过后再运行`test_case_1`，确认内核池耗尽会 panic。

```c
#include "arch/mod.h"
#include "lib/mod.h"
#include "mem/mod.h"

#define TEST_CNT 10

static void test_case_1(void)
{
    printf("=== test_case_1: Exhaust Kernel Pages ===\n");
    for (int i = 0; i <= KERN_PAGES; i++)
        pmem_alloc(true);
}

static void test_case_2(void)
{
    uint64 user_begin = (uint64)ALLOC_BEGIN + KERN_PAGES * PGSIZE;
    uint64 user_end = (uint64)ALLOC_END;
    uint64 user_pages[TEST_CNT];
    uint64 reallocated[TEST_CNT];

    printf("=== test_case_2: Allocate User Pages ===\n");
    for (int i = 0; i < TEST_CNT; i++)
    {
        user_pages[i] = (uint64)pmem_alloc(false);
        assert(user_pages[i] >= user_begin && user_pages[i] < user_end,
               "user page address out of bounds");
        memset((void *)user_pages[i], 0xAA, PGSIZE);
    }
    printf("allocated: first = %p, last = %p\n",
           user_pages[0], user_pages[TEST_CNT - 1]);

    printf("=== test_case_2: Free User Pages ===\n");
    for (int i = 0; i < TEST_CNT; i++)
        pmem_free(user_pages[i]);

    printf("=== test_case_2: Reallocate And Verify Zero ===\n");
    for (int i = 0; i < TEST_CNT; i++)
    {
        reallocated[i] = (uint64)pmem_alloc(false);
        for (int j = 0; j < PGSIZE; j++)
            assert(((uint8 *)reallocated[i])[j] == 0,
                   "reallocated page is not zeroed");
    }
    printf("reallocated: first = %p, last = %p\n",
           reallocated[0], reallocated[TEST_CNT - 1]);

    for (int i = 0; i < TEST_CNT; i++)
        pmem_free(reallocated[i]);
    printf("test_case_2 passed!\n");
}

int main()
{
    if (r_tp() == 0)
    {
        print_init();
        pmem_init();
        test_case_2();
        test_case_1();
    }

    while (1)
        ;
}
```

实际输出：

```text
=== test_case_2: Allocate User Pages ===
allocated: first = 0x0000000080405000, last = 0x000000008040e000
=== test_case_2: Free User Pages ===
=== test_case_2: Reallocate And Verify Zero ===
reallocated: first = 0x000000008040e000, last = 0x0000000080405000
test_case_2 passed!
=== test_case_1: Exhaust Kernel Pages ===
panic! pmem_alloc() failed: no available physical pages in kernel region.
```

重新分配的首尾地址与首次分配相反，符合头插空闲链表的 LIFO 顺序。`test_case_1`的 panic 是预期结果；它必须最后执行或单独运行。

## 4. 页表测试公共入口

下面两个页表测试使用同一个双 hart 初始化入口。将具体测试函数命名为`run_test`，与以下入口一起放入`main.c`：

```c
#include "arch/mod.h"
#include "lib/mod.h"
#include "mem/mod.h"

volatile static int started = 0;

static void run_test(void);

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

    kvm_inithart();
    if (cpuid == 0)
        run_test();

    printf("cpu %d is booting!\n", cpuid);
    while (1)
        ;
}
```

## 5. 跨层级映射与解映射

该用例覆盖 VPN[2] 的 0、1、255 分支，验证不足一页的`len`会覆盖完整页，并检查解映射后中间页表仍然保留。五个 VA 使用五个独立 PA，避免释放一个别名后留下悬空映射。

```c
static void run_test(void)
{
    pgtbl_t pgtbl = (pgtbl_t)pmem_alloc(true);
    uint64 pages[5];

    for (int i = 0; i < 5; i++)
        pages[i] = (uint64)pmem_alloc(false);

    printf("\nmap pages\n\n");
    vm_mappages(pgtbl, 0, pages[0], PGSIZE, PTE_R);
    vm_mappages(pgtbl, PGSIZE * 10, pages[1], PGSIZE / 2,
                PTE_R | PTE_W);
    vm_mappages(pgtbl, PGSIZE * 512, pages[2], PGSIZE - 1,
                PTE_R | PTE_X);
    vm_mappages(pgtbl, PGSIZE * 512 * 512, pages[3], PGSIZE,
                PTE_R | PTE_X);
    vm_mappages(pgtbl, VA_MAX - PGSIZE, pages[4], PGSIZE,
                PTE_R | PTE_W);
    vm_print(pgtbl);

    printf("\nunmap two pages\n\n");
    vm_unmappages(pgtbl, PGSIZE * 10, PGSIZE, true);
    vm_unmappages(pgtbl, PGSIZE * 512, PGSIZE, true);
    vm_print(pgtbl);
}
```

实际输出：

```text
cpu 1 is booting!

map pages

level-2 pgtbl: pa = 0x000000008004b000
.. level-1 pgtbl 0: pa = 0x000000008004c000
.. .. level-0 pgtbl 0: pa = 0x000000008004d000
.. .. .. physical page 0: pa = 0x0000000080404000 flags = 3
.. .. .. physical page 10: pa = 0x0000000080405000 flags = 7
.. .. level-0 pgtbl 1: pa = 0x000000008004e000
.. .. .. physical page 0: pa = 0x0000000080406000 flags = 11
.. level-1 pgtbl 1: pa = 0x000000008004f000
.. .. level-0 pgtbl 0: pa = 0x0000000080050000
.. .. .. physical page 0: pa = 0x0000000080407000 flags = 11
.. level-1 pgtbl 255: pa = 0x0000000080051000
.. .. level-0 pgtbl 511: pa = 0x0000000080052000
.. .. .. physical page 511: pa = 0x0000000080408000 flags = 7

unmap two pages

level-2 pgtbl: pa = 0x000000008004b000
.. level-1 pgtbl 0: pa = 0x000000008004c000
.. .. level-0 pgtbl 0: pa = 0x000000008004d000
.. .. .. physical page 0: pa = 0x0000000080404000 flags = 3
.. .. level-0 pgtbl 1: pa = 0x000000008004e000
.. level-1 pgtbl 1: pa = 0x000000008004f000
.. .. level-0 pgtbl 0: pa = 0x0000000080050000
.. .. .. physical page 0: pa = 0x0000000080407000 flags = 11
.. level-1 pgtbl 255: pa = 0x0000000080051000
.. .. level-0 pgtbl 511: pa = 0x0000000080052000
.. .. .. physical page 511: pa = 0x0000000080408000 flags = 7
cpu 0 is booting!
```

flags `3`、`7`、`11`分别为`V|R`、`V|R|W`、`V|R|X`。解映射后 physical page 10 和 VPN[1]=1 下的叶子消失，但对应 level-0 页表仍存在，符合`vm_unmappages`不回收中间页表的设计。

## 6. 映射与解映射断言测试

该测试不依赖`vm_print`文本，而是直接检查 PTE 地址、有效位、PA、权限和解映射结果。

```c
static void run_test(void)
{
    pte_t *pte;
    pgtbl_t pgtbl = (pgtbl_t)pmem_alloc(true);
    uint64 va_1 = 0x100000;
    uint64 va_2 = 0x8000;
    uint64 pa_1 = (uint64)pmem_alloc(false);
    uint64 pa_2 = (uint64)pmem_alloc(false);

    vm_mappages(pgtbl, va_1, pa_1, PGSIZE, PTE_R | PTE_W);
    vm_mappages(pgtbl, va_2, pa_2, PGSIZE, PTE_R);

    pte = vm_getpte(pgtbl, va_1, false);
    assert(pte != NULL, "pte_1 not found");
    assert((*pte & PTE_V) != 0, "pte_1 not valid");
    assert(PTE_TO_PA(*pte) == pa_1, "pa_1 mismatch");
    assert((*pte & (PTE_R | PTE_W)) == (PTE_R | PTE_W),
           "flag_1 mismatch");

    pte = vm_getpte(pgtbl, va_2, false);
    assert(pte != NULL, "pte_2 not found");
    assert((*pte & PTE_V) != 0, "pte_2 not valid");
    assert(PTE_TO_PA(*pte) == pa_2, "pa_2 mismatch");
    assert((*pte & PTE_R) == PTE_R, "flag_2 mismatch");

    vm_unmappages(pgtbl, va_1, PGSIZE, true);
    vm_unmappages(pgtbl, va_2, PGSIZE, true);

    pte = vm_getpte(pgtbl, va_1, false);
    assert(pte != NULL, "pte_1 not found after unmap");
    assert((*pte & PTE_V) == 0, "pte_1 still valid");

    pte = vm_getpte(pgtbl, va_2, false);
    assert(pte != NULL, "pte_2 not found after unmap");
    assert((*pte & PTE_V) == 0, "pte_2 still valid");

    printf("test_mapping_and_unmapping passed!\n");
}
```

实际输出：

```text
cpu 1 is booting!
test_mapping_and_unmapping passed!
cpu 0 is booting!
```

测试通过说明两组独立映射的 PTE 元数据和释放结果符合预期。该测试没有切换到测试页表后通过`va_1/va_2`实际读写，因此不覆盖 MMU 数据访问和权限故障。

## 7. 预期失败：重复映射

对同一 VA 重复建立映射应触发`vm_mappages`中的防重映射断言。该用例会永久进入 panic 循环，必须单独执行。

```c
static void run_test(void)
{
    pgtbl_t pgtbl = (pgtbl_t)pmem_alloc(true);
    uint64 pa = (uint64)pmem_alloc(false);

    vm_mappages(pgtbl, 0, pa, PGSIZE, PTE_R);
    printf("\nremap test\n\n");
    vm_mappages(pgtbl, 0, pa, PGSIZE, PTE_R | PTE_W);
}
```

实际结果：

```text
remap test

assert failed: vm_mappages(): remapping existing pages.
panic! assert failed!
```

## 8. 覆盖范围与缺口

当前测试已经覆盖：

- 内核页池的双 hart 并发分配和释放。
- 用户页区域、写入、释放、LIFO 复用和分配清零。
- 内核页池耗尽。
- 页表跨 level-0/1/2 边界扩展。
- 非页对齐长度的按页覆盖。
- PTE 的 PA、flags、有效位和重复映射检查。
- 解映射及可选物理页释放。

尚未覆盖：

- 切换到独立测试页表后的实际虚拟地址读写。
- 代码页写保护、数据页执行保护和 page fault 路径。
- 页表页销毁和中间页表回收。
- 活动页表修改后的本地 TLB 刷新和多 hart TLB shootdown。
- `pmem_free`的非页首地址、重复释放等错误输入。
