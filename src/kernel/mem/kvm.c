#include "mod.h"

// in trap/trampoline.S
extern char trampoline[];

// 内核页表
static pgtbl_t kernel_pgtbl;

// 根据pagetable,找到va对应的pte
// 若设置alloc=true 则在PTE无效时尝试申请一个物理页
// 成功返回PTE, 失败返回NULL
// 提示：使用 VA_TO_VPN + PTE_TO_PA + PA_TO_PTE
pte_t *vm_getpte(pgtbl_t pgtbl, uint64 va, bool alloc)
{
    if (va >= VA_MAX)
        panic("vm_getpte(): invalid va.");
    // 从顶级页表向下查找(注意：此循环只处理level = 2, level = 1)
    for (int level = 2; level > 0; level--)
    {
        // VA_TO_VPN(va, level) 获取虚拟地址对应的虚拟页表项编号
        // pgtbl[VPN] 获取页表项
        // &pgtbl[VPN] 获取页表项的地址
        // 取页表项指针的原因是，PTE 无效且 alloc = true 时，需要通过指针将新页面写回页表项
        // 返回页表项指针的原因也是为了调用方写入页表项
        pte_t *pte = &pgtbl[VA_TO_VPN(va, level)];
        if (*pte & PTE_V)
        {
            assert(PTE_CHECK(*pte), "vm_getpte: unexpected leaf PTE.");
            // PTE 有效，进入下一级页表
            pgtbl = (pgtbl_t)PTE_TO_PA(*pte);
        } else {
            // PTE 无效，根据 alloc 判断是否分配新页表页
            if (!alloc)
                return NULL;
            uint64 new_page = (uint64)pmem_alloc(true);
            assert(new_page != 0, "vm_getpte: pmem_alloc() returned 0.");
            // pmem_alloc 已保证页面清零
            // memset((void*)new_page, 0, PGSIZE);
            
            // 置位 PTE_V，但 R W X 均为 0，代表这个页表项指向存放页表的页面
            *pte = PA_TO_PTE(new_page) | PTE_V;
            pgtbl = (pgtbl_t)new_page;
        }
    }
    // 第 0 层，返回 PTE 指针
    return &pgtbl[VA_TO_VPN(va, 0)];
}

// 在pgtbl中建立 [va, va + len) -> [pa, pa + len) 的映射
// 本质是找到va在页表对应位置的pte并修改它
// 检查: va pa 应当是 page-aligned, len(字节数) > 0, va + len <= VA_MAX
// 注意: perm 应该如何使用
void vm_mappages(pgtbl_t pgtbl, uint64 va, uint64 pa, uint64 len, int perm)
{
    if (va % PGSIZE != 0)
        panic("vm_mappages(): va is not page-aligned.");
    if (pa % PGSIZE != 0)
        panic("vm_mappages(): pa is not page-aligned.");
    // ALLOC_BEGIN 地址已经和 4096 对齐，判断的时候不需要减去 ALLOC_BEGIN
    if (len == 0)
        panic("vm_mappages(): len == 0.");
    // 检测 va + len <= VA_MAX 且防止整数溢出
    if (va >= VA_MAX || len > VA_MAX - va)
        panic("vm_mappages(): va + len exceeds VA_MAX.");
    
    uint64 end = va + len;
    while (va < end)
    {
        pte_t *pte = vm_getpte(pgtbl, va, true);
        assert(pte != NULL, "vm_mappages(): vm_getpte returned null.");
        // 如果指向物理页的 TPE 已经有 PTE_V 标志，说明这个物理页已经被
        assert(!(*pte & PTE_V), "vm_mappages(): remapping existing pages.");
        *pte = PA_TO_PTE(pa) | PTE_V | perm;
        va += PGSIZE;
        pa += PGSIZE;
    }
}

// 解除pgtbl中[va, va+len)区域的映射
// 如果freeit == true则释放对应物理页
// 一般是释放用户的物理页的时候 freeit = true，内核物理页一般不会释放
void vm_unmappages(pgtbl_t pgtbl, uint64 va, uint64 len, bool freeit)
{
    if (va % PGSIZE != 0)
        panic("vm_unmappages(): va is not page-aligned.");
    if (len == 0)
        panic("vm_unmappages(): len == 0.");
    
    // 检测 va + len <= VA_MAX 且防止整数溢出
    if (va >= VA_MAX || len > VA_MAX - va)
        panic("vm_unmappages(): va + len exceeds VA_MAX.");
    
    uint64 end = va + len;
    while (va < end)
    {
        pte_t *pte = vm_getpte(pgtbl, va, false);
        if (pte == NULL)
            panic("vm_unmappages(): vm_getpte returned null, page is not mapped.");
        if (!(*pte & PTE_V))
            panic("vm_unmappages(): page is not mapped.");
        // 检查 pte 指向的是实际物理页面，还是页表所在页面
        if (PTE_CHECK(*pte))
            panic("vm_unmappages(): pte is not level 0.");
        if (freeit)
            pmem_free(PTE_TO_PA(*pte));
        // 清空 TPE
        *pte = 0;

        va += PGSIZE;
    }
}

// 完成UART、CLINT、PLIC、内核代码区、内核数据区、可分配区域、trampoline、内核栈的页表映射
// 相当于部分填充kernel_pgtbl
void kvm_init()
{
    kernel_pgtbl = (pgtbl_t)pmem_alloc(true);

    // MMIO direct mappings.
    vm_mappages(kernel_pgtbl, UART_BASE, UART_BASE,
                PGSIZE, PTE_R | PTE_W);
    vm_mappages(kernel_pgtbl, CLINT_BASE, CLINT_BASE,
                0x10000, PTE_R | PTE_W);
    vm_mappages(kernel_pgtbl, PLIC_BASE, PLIC_BASE,
                0x400000, PTE_R | PTE_W);

    // 内核 .text 段
    vm_mappages(kernel_pgtbl, KERNEL_BASE, KERNEL_BASE,
                (uint64)KERNEL_DATA - KERNEL_BASE,
                PTE_R | PTE_X);

    // 内核 data、bss 段和物理页表分配区.
    vm_mappages(kernel_pgtbl,
                (uint64)KERNEL_DATA, (uint64)KERNEL_DATA,
                (uint64)ALLOC_BEGIN - (uint64)KERNEL_DATA,
                PTE_R | PTE_W);
    vm_mappages(kernel_pgtbl,
                (uint64)ALLOC_BEGIN, (uint64)ALLOC_BEGIN,
                (uint64)ALLOC_END - (uint64)ALLOC_BEGIN,
                PTE_R | PTE_W);

    // trampoline在内核页表和用户页表中必须使用相同的虚拟地址。
    vm_mappages(kernel_pgtbl, TRAMPOLINE, (uint64)trampoline,
                PGSIZE, PTE_R | PTE_X);

    // 每个进程槽位拥有一页固定内核栈，槽位之间保留一页未映射的保护页。
    for (int i = 0; i < N_PROC; i++)
    {
        uint64 kstack_page = (uint64)pmem_alloc(true);
        vm_mappages(kernel_pgtbl, KSTACK(i), kstack_page,
                    PGSIZE, PTE_R | PTE_W);
    }
}

// 每个CPU都需要调用, 从不使用页表切换到使用内核页表
// 切换后需要刷新TLB里面的缓存
void kvm_inithart()
{
    w_satp(MAKE_SATP(kernel_pgtbl));
    sfence_vma();
}

// 输出页表内容(for debug)
void vm_print(pgtbl_t pgtbl)
{
    // 顶级页表，次级页表，低级页表
    pgtbl_t pgtbl_2 = pgtbl, pgtbl_1 = NULL, pgtbl_0 = NULL;
    pte_t pte;

    printf("level-2 pgtbl: pa = %p\n", pgtbl_2);
    for (int i = 0; i < PGSIZE / sizeof(pte_t); i++)
    {
        pte = pgtbl_2[i];
        if (!((pte)&PTE_V))
            continue;
        assert(PTE_CHECK(pte), "vm_print: pte check fail (1)");
        pgtbl_1 = (pgtbl_t)PTE_TO_PA(pte);
        printf(".. level-1 pgtbl %d: pa = %p\n", i, pgtbl_1);

        for (int j = 0; j < PGSIZE / sizeof(pte_t); j++)
        {
            pte = pgtbl_1[j];
            if (!((pte)&PTE_V))
                continue;
            assert(PTE_CHECK(pte), "vm_print: pte check fail (2)");
            pgtbl_0 = (pgtbl_t)PTE_TO_PA(pte);
            printf(".. .. level-0 pgtbl %d: pa = %p\n", j, pgtbl_0);

            for (int k = 0; k < PGSIZE / sizeof(pte_t); k++)
            {
                pte = pgtbl_0[k];
                if (!((pte)&PTE_V))
                    continue;
                assert(!PTE_CHECK(pte), "vm_print: pte check fail (3)");
                printf(".. .. .. physical page %d: pa = %p flags = %d\n", k, (uint64)PTE_TO_PA(pte), (int)PTE_FLAGS(pte));
            }
        }
    }
}
