#include "mod.h"

/*--------------------part-1: 内核空间与用户空间的数据传递--------------------*/

// 返回用户虚拟地址 va 对应的物理页内地址，并验证用户访问权限。
static uint64 uvm_user_pa(pgtbl_t pgtbl, uint64 va, int perm)
{
    pte_t *pte;

    assert(va < VA_MAX, "uvm_user_pa: invalid user address.");
    pte = vm_getpte(pgtbl, va, false);
    assert(pte != NULL && (*pte & PTE_V), "uvm_user_pa: unmapped user page.");
    assert((*pte & PTE_U) != 0, "uvm_user_pa: non-user page.");
    assert((*pte & perm) == perm, "uvm_user_pa: insufficient user permission.");
    assert(!PTE_CHECK(*pte), "uvm_user_pa: non-leaf PTE.");

    return PTE_TO_PA(*pte) + va % PGSIZE;
}

// 用户地址空间 [src, src + len) 拷贝至内核地址空间 [dst, dst + len)。
void uvm_copyin(pgtbl_t pgtbl, uint64 dst, uint64 src, uint32 len)
{
    uint32 n;

    while (len > 0)
    {
        n = PGSIZE - src % PGSIZE;
        if (n > len)
            n = len;
        assert(n <= VA_MAX - src, "uvm_copyin: user range overflow.");
        memmove((void *)dst, (const void *)uvm_user_pa(pgtbl, src, PTE_R), n);
        dst += n;
        src += n;
        len -= n;
    }
}

// 内核地址空间 [src, src + len) 拷贝至用户地址空间 [dst, dst + len)。
void uvm_copyout(pgtbl_t pgtbl, uint64 dst, uint64 src, uint32 len)
{
    uint32 n;

    while (len > 0)
    {
        n = PGSIZE - dst % PGSIZE;
        if (n > len)
            n = len;
        assert(n <= VA_MAX - dst, "uvm_copyout: user range overflow.");
        memmove((void *)uvm_user_pa(pgtbl, dst, PTE_W), (const void *)src, n);
        dst += n;
        src += n;
        len -= n;
    }
}

// 用户字符串拷贝到内核，最多拷贝 maxlen 字节，遇到 '\0' 终止。
void uvm_copyin_str(pgtbl_t pgtbl, uint64 dst, uint64 src, uint32 maxlen)
{
    uint32 n;

    while (maxlen > 0)
    {
        n = PGSIZE - src % PGSIZE;
        if (n > maxlen)
            n = maxlen;
        assert(n <= VA_MAX - src, "uvm_copyin_str: user range overflow.");
        memmove((void *)dst, (const void *)uvm_user_pa(pgtbl, src, PTE_R), n);
        for (uint32 i = 0; i < n; i++)
            if (((char *)dst)[i] == '\0')
                return;
        dst += n;
        src += n;
        maxlen -= n;
    }
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
    uint64 begin = MMAP_BEGIN;
    mmap_region_t *last = NULL;
    mmap_region_t *tmp = head_mmap;

    while (tmp != NULL)
    {
        assert(tmp->begin >= MMAP_BEGIN && tmp->begin < MMAP_END,
               "uvm_mmap_find: invalid mmap list.");
        assert(tmp->npages > 0, "uvm_mmap_find: empty mmap region.");
        if (len <= tmp->begin - begin)
            break;
        begin = tmp->begin + (uint64)tmp->npages * PGSIZE;
        last = tmp;
        tmp = tmp->next;
    }

    if (begin > MMAP_END || len > MMAP_END - begin)
        return 0;
    *p_last_mmap = last;
    *p_tmp_mmap = tmp;
    return begin;
}

// 在用户页表和进程 mmap 链中增加 [begin, begin + npages * PGSIZE)。
uint64 uvm_mmap(uint64 begin, uint32 npages, int perm)
{
    proc_t *proc = myproc();
    mmap_region_t *last;
    mmap_region_t *tmp;
    uint64 len = (uint64)npages * PGSIZE;

    assert(proc != NULL, "uvm_mmap: no current process.");
    last = NULL;
    tmp = proc->mmap;
    if (npages == 0 || (begin != 0 && begin % PGSIZE != 0))
        return (uint64)-1;

    if (begin == 0) {
        begin = uvm_mmap_find(proc->mmap, len, &last, &tmp);
        if (begin == 0)
            return (uint64)-1;
    } else {
        if (begin < MMAP_BEGIN || begin >= MMAP_END || len > MMAP_END - begin)
            return (uint64)-1;
        while (tmp != NULL && tmp->begin < begin)
        {
            last = tmp;
            tmp = tmp->next;
        }
        if ((last != NULL &&
             last->begin + (uint64)last->npages * PGSIZE > begin) ||
            (tmp != NULL && begin + len > tmp->begin))
            return (uint64)-1;
    }

    mmap_region_t *new_mmap = mmap_region_alloc();
    new_mmap->begin = begin;
    new_mmap->npages = npages;
    new_mmap->next = tmp;

    if (last == NULL)
        proc->mmap = new_mmap;
    else
        last->next = new_mmap;

    if (last != NULL && last->begin + (uint64)last->npages * PGSIZE == begin)
    {
        last->next = tmp;
        mmap_merge(last, new_mmap, true);
        new_mmap = last;
    }
    if (tmp != NULL && begin + len == tmp->begin)
    {
        new_mmap->next = tmp->next;
        mmap_merge(new_mmap, tmp, true);
    }

    for (uint64 va = begin; va < begin + len; va += PGSIZE)
    {
        uint64 page = (uint64)pmem_alloc(false);
        vm_mappages(proc->pgtbl, va, page, PGSIZE, perm | PTE_U);
    }
    return begin;
}

// 从用户页表和进程 mmap 链中释放 [begin, begin + npages * PGSIZE)。
uint64 uvm_munmap(uint64 begin, uint32 npages)
{
    proc_t *proc = myproc();
    mmap_region_t *last;
    mmap_region_t *tmp;
    uint64 len = (uint64)npages * PGSIZE;
    uint64 end;

    assert(proc != NULL, "uvm_munmap: no current process.");
    last = NULL;
    tmp = proc->mmap;
    if (npages == 0 || begin % PGSIZE != 0 ||
        begin < MMAP_BEGIN || begin >= MMAP_END || len > MMAP_END - begin)
        return (uint64)-1;
    end = begin + len;

    while (tmp != NULL && tmp->begin + (uint64)tmp->npages * PGSIZE <= begin)
    {
        last = tmp;
        tmp = tmp->next;
    }
    if (tmp == NULL || begin < tmp->begin ||
        end > tmp->begin + (uint64)tmp->npages * PGSIZE)
        return (uint64)-1;

    uint64 region_end = tmp->begin + (uint64)tmp->npages * PGSIZE;
    if (begin == tmp->begin && end == region_end) {
        if (last == NULL)
            proc->mmap = tmp->next;
        else
            last->next = tmp->next;
        mmap_region_free(tmp);
    } else if (begin == tmp->begin) {
        tmp->begin = end;
        tmp->npages -= npages;
    } else if (end == region_end) {
        tmp->npages -= npages;
    } else {
        mmap_region_t *right = mmap_region_alloc();
        right->begin = end;
        right->npages = (region_end - end) / PGSIZE;
        right->next = tmp->next;
        tmp->npages = (begin - tmp->begin) / PGSIZE;
        tmp->next = right;
    }

    vm_unmappages(proc->pgtbl, begin, len, true);
    return 0;
}

/*------------------part-3: 用户空间 heap 和 stack 管理------------------*/

uint64 uvm_heap_grow(pgtbl_t pgtbl, uint64 cur_heap_top, uint64 len)
{
    uint64 new_heap_top;
    uint64 old_page_end;
    uint64 new_page_end;

    assert(cur_heap_top >= USER_BASE + PGSIZE && cur_heap_top <= MMAP_BEGIN,
           "uvm_heap_grow: invalid current heap top.");
    if (len > MMAP_BEGIN - cur_heap_top)
        return (uint64)-1;

    new_heap_top = cur_heap_top + len;
    old_page_end = (cur_heap_top + PGSIZE - 1) / PGSIZE * PGSIZE;
    new_page_end = (new_heap_top + PGSIZE - 1) / PGSIZE * PGSIZE;
    for (uint64 va = old_page_end; va < new_page_end; va += PGSIZE)
    {
        uint64 page = (uint64)pmem_alloc(false);
        vm_mappages(pgtbl, va, page, PGSIZE, PTE_R | PTE_W | PTE_U);
    }
    return new_heap_top;
}

uint64 uvm_heap_ungrow(pgtbl_t pgtbl, uint64 cur_heap_top, uint64 len)
{
    uint64 heap_begin = USER_BASE + PGSIZE;
    uint64 new_heap_top;
    uint64 unmap_begin;
    uint64 unmap_end;

    assert(cur_heap_top >= heap_begin && cur_heap_top <= MMAP_BEGIN,
           "uvm_heap_ungrow: invalid current heap top.");
    if (len > cur_heap_top - heap_begin)
        return (uint64)-1;

    new_heap_top = cur_heap_top - len;
    unmap_begin = (new_heap_top + PGSIZE - 1) / PGSIZE * PGSIZE;
    unmap_end = (cur_heap_top + PGSIZE - 1) / PGSIZE * PGSIZE;
    if (unmap_begin < unmap_end)
        vm_unmappages(pgtbl, unmap_begin, unmap_end - unmap_begin, true);
    return new_heap_top;
}

// 处理用户栈增长引起的 page fault，成功返回新页数，失败返回 -1。
uint64 uvm_ustack_grow(pgtbl_t pgtbl, uint64 old_ustack_npage, uint64 fault_addr)
{
    uint64 old_bottom;
    uint64 new_bottom;
    uint64 new_npage;

    assert(old_ustack_npage > 0, "uvm_ustack_grow: empty current stack.");
    assert(old_ustack_npage <= (TRAPFRAME - MMAP_END) / PGSIZE,
           "uvm_ustack_grow: invalid current stack size.");
    old_bottom = TRAPFRAME - old_ustack_npage * PGSIZE;
    if (fault_addr < MMAP_END || fault_addr >= old_bottom)
        return (uint64)-1;

    new_bottom = fault_addr / PGSIZE * PGSIZE;
    new_npage = (TRAPFRAME - new_bottom) / PGSIZE;
    for (uint64 va = new_bottom; va < old_bottom; va += PGSIZE)
    {
        uint64 page = (uint64)pmem_alloc(false);
        vm_mappages(pgtbl, va, page, PGSIZE, PTE_R | PTE_W | PTE_U);
    }
    return new_npage;
}

/*----------------------part-4: 用户页表管理----------------------*/

// 递归释放页表占用的物理页和页表管理的用户物理页，顶级页表 level 为 3。
static void destroy_pgtbl(pgtbl_t pgtbl, uint32 level)
{
    assert(pgtbl != NULL, "destroy_pgtbl: NULL page table.");
    assert(level > 0 && level <= 3, "destroy_pgtbl: invalid level.");

    for (uint32 i = 0; i < PGSIZE / sizeof(pte_t); i++)
    {
        pte_t pte = pgtbl[i];
        if (!(pte & PTE_V))
            continue;

        uint64 pa = PTE_TO_PA(pte);
        if (PTE_CHECK(pte)) {
            assert(level > 1, "destroy_pgtbl: non-leaf at last level.");
            destroy_pgtbl((pgtbl_t)pa, level - 1);
        } else {
            assert(level == 1, "destroy_pgtbl: unexpected huge page.");
            assert(pte & PTE_U, "destroy_pgtbl: unexpected kernel leaf.");
            pmem_free(pa);
        }
        pgtbl[i] = 0;
    }
    pmem_free((uint64)pgtbl);
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
    assert(old != NULL && new != NULL, "uvm_copy_pgtbl: NULL page table.");
    assert(heap_top >= USER_BASE + PGSIZE && heap_top <= MMAP_BEGIN,
           "uvm_copy_pgtbl: invalid heap top.");
    assert(ustack_npage > 0 &&
           ustack_npage <= (TRAPFRAME - MMAP_END) / PGSIZE,
           "uvm_copy_pgtbl: invalid stack size.");

    uint64 heap_end = (heap_top + PGSIZE - 1) / PGSIZE * PGSIZE;
    copy_range(old, new, USER_BASE, heap_end);

    uint64 last_end = MMAP_BEGIN;
    for (mmap_region_t *region = mmap; region != NULL; region = region->next)
    {
        assert(region->npages > 0, "uvm_copy_pgtbl: empty mmap region.");
        assert(region->begin >= last_end && region->begin < MMAP_END,
               "uvm_copy_pgtbl: invalid mmap list.");
        uint64 len = (uint64)region->npages * PGSIZE;
        assert(len <= MMAP_END - region->begin,
               "uvm_copy_pgtbl: mmap range overflow.");
        copy_range(old, new, region->begin, region->begin + len);
        last_end = region->begin + len;
    }

    copy_range(old, new, TRAPFRAME - ustack_npage * PGSIZE, TRAPFRAME);
}
