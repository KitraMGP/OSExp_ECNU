#include "mod.h"

// 系统调用号到服务函数的跳转表。
static uint64 (*syscalls[])(void) = {
    [SYS_copyin] = sys_copyin,
    [SYS_copyout] = sys_copyout,
    [SYS_copyinstr] = sys_copyinstr,
    [SYS_brk] = sys_brk,
    [SYS_mmap] = sys_mmap,
    [SYS_munmap] = sys_munmap,
};

void syscall()
{
    proc_t *p = myproc();

    int sys_num = p->tf->a7;
    if (sys_num < 0 || sys_num > SYS_MAX_NUM || syscalls[sys_num] == NULL) {
        printf("unknown syscall %d from pid = %d\n", sys_num, p->pid);
        panic("syscall");
    } else {
        p->tf->a0 = syscalls[sys_num]();
    }
}

static uint64 arg_raw(int n)
{
    proc_t *proc = myproc();

    switch (n)
    {
    case 0:
        return proc->tf->a0;
    case 1:
        return proc->tf->a1;
    case 2:
        return proc->tf->a2;
    case 3:
        return proc->tf->a3;
    case 4:
        return proc->tf->a4;
    case 5:
        return proc->tf->a5;
    default:
        panic("arg_raw: illegal arg num");
        return -1;
    }
}

void arg_uint32(int n, uint32 *ip)
{
    *ip = arg_raw(n);
}

void arg_uint64(int n, uint64 *ip)
{
    *ip = arg_raw(n);
}

void arg_str(int n, char *buf, int maxlen)
{
    proc_t *p = myproc();
    uint64 addr;
    arg_uint64(n, &addr);
    uvm_copyin_str(p->pgtbl, (uint64)buf, addr, maxlen);
}
