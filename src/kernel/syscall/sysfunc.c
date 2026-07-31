#include "mod.h"

enum
{
    COPY_TEST_INTS = 5,
};

// 从用户空间传入 int 数组，成功返回 0。
uint64 sys_copyin()
{
    uint64 user_addr;
    uint32 len;
    int values[COPY_TEST_INTS];

    arg_uint64(0, &user_addr);
    arg_uint32(1, &len);
    assert(len <= COPY_TEST_INTS, "sys_copyin: array is too large.");
    uvm_copyin(myproc()->pgtbl, (uint64)values, user_addr, len * sizeof(values[0]));

    for (uint32 i = 0; i < len; i++)
        printf("get a number from user: %d\n", values[i]);
    return 0;
}

// 向用户空间传出 int 数组，成功返回元素数量。
uint64 sys_copyout()
{
    static const int values[COPY_TEST_INTS] = {1, 2, 3, 4, 5};
    uint64 user_addr;

    arg_uint64(0, &user_addr);
    uvm_copyout(myproc()->pgtbl, user_addr, (uint64)values, sizeof(values));
    return COPY_TEST_INTS;
}

// 从用户空间传入字符串，成功返回 0。
uint64 sys_copyinstr()
{
    char value[STR_MAXLEN + 1];

    memset(value, 0, sizeof(value));
    arg_str(0, value, STR_MAXLEN);
    printf("get string for user: %s\n", value);
    return 0;
}

// 调整用户堆顶；参数为 0 时查询当前堆顶。
uint64 sys_brk()
{
    proc_t *proc = myproc();
    uint64 new_heap_top;
    uint64 result;

    arg_uint64(0, &new_heap_top);
    if (new_heap_top == 0)
    {
        printf("brk lookup: heap_top = %p\n", proc->heap_top);
        vm_print(proc->pgtbl);
        return proc->heap_top;
    }
    if (new_heap_top < USER_BASE + PGSIZE || new_heap_top > MMAP_BEGIN)
        return (uint64)-1;

    if (new_heap_top > proc->heap_top)
        result = uvm_heap_grow(proc->pgtbl, proc->heap_top,
                               new_heap_top - proc->heap_top);
    else
        result = uvm_heap_ungrow(proc->pgtbl, proc->heap_top,
                                 proc->heap_top - new_heap_top);
    if (result != (uint64)-1)
    {
        if (new_heap_top > proc->heap_top)
            printf("brk grow: heap_top = %p -> %p\n", proc->heap_top, result);
        else if (new_heap_top < proc->heap_top)
            printf("brk ungrow: heap_top = %p -> %p\n", proc->heap_top, result);
        else
            printf("brk unchanged: heap_top = %p\n", result);
        proc->heap_top = result;
        vm_print(proc->pgtbl);
    }
    return result;
}

// 增加一段页对齐的用户内存映射。
uint64 sys_mmap()
{
    uint64 begin;
    uint32 len;

    arg_uint64(0, &begin);
    arg_uint32(1, &len);
    if (len == 0 || len % PGSIZE != 0 ||
        (begin != 0 && begin % PGSIZE != 0))
        return (uint64)-1;
    uint64 result = uvm_mmap(begin, len / PGSIZE, PTE_R | PTE_W);
    if (result != (uint64)-1)
    {
        proc_t *proc = myproc();
        printf("mmap: begin = %p, len = %d\n", result, len);
        uvm_show_mmaplist(proc->mmap);
        vm_print(proc->pgtbl);
        printf("\n");
    }
    return result;
}

// 解除一段页对齐的用户内存映射。
uint64 sys_munmap()
{
    uint64 begin;
    uint32 len;

    arg_uint64(0, &begin);
    arg_uint32(1, &len);
    if (len == 0 || len % PGSIZE != 0 || begin % PGSIZE != 0)
        return (uint64)-1;
    uint64 result = uvm_munmap(begin, len / PGSIZE);
    if (result == 0)
    {
        proc_t *proc = myproc();
        printf("munmap: begin = %p, len = %d\n", begin, len);
        uvm_show_mmaplist(proc->mmap);
        vm_print(proc->pgtbl);
        printf("\n");
    }
    return result;
}
