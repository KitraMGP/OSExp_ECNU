#include "mod.h"

/*--------------------part-1: 内核空间与用户空间的数据传递--------------------*/

// 用户地址空间 [src, src + len) 拷贝至内核地址空间 [dst, dst + len)。
void uvm_copyin(pgtbl_t pgtbl, uint64 dst, uint64 src, uint32 len)
{
    // TODO: 逐页查询用户页表并将用户数据复制到内核。
}

// 内核地址空间 [src, src + len) 拷贝至用户地址空间 [dst, dst + len)。
void uvm_copyout(pgtbl_t pgtbl, uint64 dst, uint64 src, uint32 len)
{
    // TODO: 逐页查询用户页表并将内核数据复制到用户空间。
}

// 用户字符串拷贝到内核，最多拷贝 maxlen 字节，遇到 '\0' 终止。
void uvm_copyin_str(pgtbl_t pgtbl, uint64 dst, uint64 src, uint32 maxlen)
{
    // TODO: 跨页复制用户字符串，并在 '\0' 或 maxlen 处停止。
}

/*--------------------part-2: mmap_region 相关--------------------*/

void uvm_show_mmaplist(mmap_region_t *mmap)
{
    mmap_region_t *tmp = mmap;
    printf("\nalloced mmap_space:\n");
    if (tmp == NULL)
        printf("empty\n");
    while (tmp != NULL)
    {
        printf("alloced mmap_region: %p ~ %p\n", tmp->begin, tmp->begin + tmp->npages * PGSIZE);
        tmp = tmp->next;
    }
}

// 合并相邻的两个区域；保留一个节点并归还另一个节点，不操作 next 指针。
static void mmap_merge(mmap_region_t *mmap_1, mmap_region_t *mmap_2, bool keep_mmap_1)
{
    assert(mmap_1 != NULL && mmap_2 != NULL, "mmap_merge: NULL");
    assert(mmap_1->begin + mmap_1->npages * PGSIZE == mmap_2->begin, "mmap_merge: check fail");

    if (keep_mmap_1) {
        mmap_1->npages += mmap_2->npages;
        mmap_region_free(mmap_2);
    } else {
        mmap_2->begin -= mmap_1->npages * PGSIZE;
        mmap_2->npages += mmap_1->npages;
        mmap_region_free(mmap_1);
    }
}

// 在 mmap 区间内寻找长度为 len 的空闲地址；成功返回起始地址，失败返回 0。
static uint64 uvm_mmap_find(mmap_region_t *head_mmap, uint64 len,
                            mmap_region_t **p_last_mmap, mmap_region_t **p_tmp_mmap)
{
    // TODO: 在 mmap 地址区中查找首个足够大的空闲区间。
    return 0;
}

// 在用户页表和进程 mmap 链中增加 [begin, begin + npages * PGSIZE)。
void uvm_mmap(uint64 begin, uint32 npages, int perm)
{
    // TODO: 插入并合并 mmap 节点，再申请物理页和建立映射。
}

// 从用户页表和进程 mmap 链中释放 [begin, begin + npages * PGSIZE)。
void uvm_munmap(uint64 begin, uint32 npages)
{
    // TODO: 处理 mmap 节点删除、裁剪或拆分，并解除页面映射。
}

/*------------------part-3: 用户空间 heap 和 stack 管理------------------*/

uint64 uvm_heap_grow(pgtbl_t pgtbl, uint64 cur_heap_top, uint32 len)
{
    // TODO: 检查堆边界并映射新增的用户页面。
    return (uint64)-1;
}

uint64 uvm_heap_ungrow(pgtbl_t pgtbl, uint64 cur_heap_top, uint32 len)
{
    // TODO: 解除不再使用的堆页面并回收物理页。
    return (uint64)-1;
}

// 处理用户栈增长引起的 page fault，成功返回新页数，失败返回 -1。
uint64 uvm_ustack_grow(pgtbl_t pgtbl, uint64 old_ustack_npage, uint64 fault_addr)
{
    // TODO: 校验缺页地址并映射栈增长所需的全部页面。
    return (uint64)-1;
}

/*----------------------part-4: 用户页表管理----------------------*/

// 递归释放页表占用的物理页和页表管理的用户物理页，顶级页表 level 为 3。
static void destroy_pgtbl(pgtbl_t pgtbl, uint32 level)
{
    // TODO: 递归释放叶子用户页、下级页表和当前页表页。
}

void uvm_destroy_pgtbl(pgtbl_t pgtbl)
{
    vm_unmappages(pgtbl, TRAPFRAME, PGSIZE, true);
    vm_unmappages(pgtbl, TRAMPOLINE, PGSIZE, false);
    destroy_pgtbl(pgtbl, 3);
}

static void copy_range(pgtbl_t old, pgtbl_t new, uint64 begin, uint64 end)
{
    uint64 va, pa, page;
    int flags;
    pte_t *pte;

    for (va = begin; va < end; va += PGSIZE)
    {
        pte = vm_getpte(old, va, false);
        assert(pte != NULL, "uvm_copy_pgtbl: pte == NULL");
        assert((*pte) & PTE_V, "uvm_copy_pgtbl: pte not valid");

        pa = (uint64)PTE_TO_PA(*pte);
        flags = (int)PTE_FLAGS(*pte);

        page = (uint64)pmem_alloc(false);
        memmove((char *)page, (const char *)pa, PGSIZE);
        vm_mappages(new, va, page, PGSIZE, flags);
    }
}

// 拷贝用户页表管理的各区域，不包括 trapframe 和 trampoline。
void uvm_copy_pgtbl(pgtbl_t old, pgtbl_t new, uint64 heap_top,
                    uint64 ustack_npage, mmap_region_t *mmap)
{
    // TODO: 分别复制代码堆区、mmap 区和用户栈的页面。
}
