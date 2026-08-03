#include "mod.h"

/*
    用户堆空间伸缩
    uint64 new_heap_top (如果是0, 代表查询当前堆顶位置)
    成功返回new_heap_top, 失败返回-1
*/
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

/*
    增加一段内存映射
    uint64 start 起始地址
    uint32 len   范围 (字节,需检查是否是page-aligned)
    成功返回映射空间的起始地址, 失败返回-1
*/
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

/*
    解除一段内存映射
    uint64 start 起始地址
    uint32 len   范围 (字节, 需检查是否是page-aligned)
    成功返回0 失败返回-1
*/
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

/*
    打印一个字符串
    char *str
    成功返回0
*/
uint64 sys_print_str()
{
    char str[STR_MAXLEN + 1];
    arg_str(0, str, STR_MAXLEN);
    str[STR_MAXLEN] = '\0';
    printf("%s", str);
    return 0;
}

/*
    打印一个32位整数
    int num
    成功返回0
*/
uint64 sys_print_int()
{
    uint32 num;
    arg_uint32(0, &num);
    printf("%d", (int)num);
    return 0;
}

/*
    进程复制
    返回子进程的pid
*/
uint64 sys_fork()
{
    return proc_fork();
}

/*
    等待子进程退出
    uint64 addr_exit_state
*/
uint64 sys_wait()
{
    uint64 addr;
    arg_uint64(0, &addr);
    return proc_wait(addr);
}

/*
    进程退出
    int exit_code
    不返回
*/
uint64 sys_exit()
{
    uint32 exit_code;
    arg_uint32(0, &exit_code);
    proc_exit((int)exit_code);
    return 0;
}

/*
    让进程睡眠一段时间
    uint32 ntick (1个tick大约0.1秒)
    成功返回0
*/
uint64 sys_sleep()
{
    uint32 ntick;
    arg_uint32(0, &ntick);
    timer_wait(ntick);
    return 0;
}

/*
    返回当前进程的pid
*/
uint64 sys_getpid()
{
    proc_t *proc = myproc();
    assert(proc != NULL, "sys_getpid: no current process.");
    return proc->pid;
}
