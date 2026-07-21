#include "mod.h"

// 这个文件通过make build生成, 是proczero对应的ELF文件
#include "../../user/initcode.h"
#define initcode target_user_initcode
#define initcode_len target_user_initcode_len

// in trampoline.S
extern char trampoline[];

// in swtch.S
extern void swtch(context_t *old, context_t *new);

// in trap/trap_user.c
extern void trap_user_return();

// 第一个用户进程
static proc_t proczero;

// 获得一个初始化过的用户页表
// 完成trapframe和trampoline的映射
pgtbl_t proc_pgtbl_init(uint64 trapframe)
{
    // 页表自身属于内核资源，用户物理页区域只存放进程的数据页面。
    pgtbl_t pgtbl = (pgtbl_t)pmem_alloc(true);

    // 两个过渡页面不设置PTE_U，防止用户程序直接访问。
    vm_mappages(pgtbl, TRAMPOLINE, (uint64)trampoline,
                PGSIZE, PTE_R | PTE_X);
    vm_mappages(pgtbl, TRAPFRAME, trapframe,
                PGSIZE, PTE_R | PTE_W);

    return pgtbl;
}

/*
    第一个用户态进程的创建
    它的代码和数据位于initcode.h的initcode数组

    第一个进程的用户地址空间布局:
    trapoline   (1 page)
    trapframe   (1 page)
    ustack      (1 page)
    .......
                        <--heap_top
    code + data (1 page)
    empty space (1 page) 最低的4096字节 不分配物理页，同时不可访问

	注意: 用用户空间的地址映射需要标记 PTE_U
*/
void proc_make_first()
{
    // trapframe保存在用户物理页中，但只通过内核映射访问。
    proczero.pid = 0;
    proczero.tf = (trapframe_t *)pmem_alloc(false);
    proczero.pgtbl = proc_pgtbl_init((uint64)proczero.tf);

    // 用户栈位于TRAPFRAME下方，栈顶从高地址TRAPFRAME开始向下增长。
    uint64 ustack_page = (uint64)pmem_alloc(false);
    vm_mappages(proczero.pgtbl, TRAPFRAME - PGSIZE, ustack_page,
                PGSIZE, PTE_R | PTE_W | PTE_U);

    // initcode同时包含代码和数据，因此首个实验进程暂时使用RWX权限。
    uint64 initcode_page = (uint64)pmem_alloc(false);
    vm_mappages(proczero.pgtbl, USER_BASE, initcode_page,
                PGSIZE, PTE_R | PTE_W | PTE_X | PTE_U);
    assert(initcode_len <= PGSIZE, "proc_make_first: initcode is too large.");
    memmove((void *)initcode_page, initcode, initcode_len);

    // trapframe提供首次sret需要恢复的用户PC和用户栈指针。
    proczero.tf->user_to_kern_epc = USER_BASE;
    proczero.tf->sp = TRAPFRAME;
    proczero.heap_top = USER_BASE + PGSIZE;
    proczero.ustack_npage = 1;

    // swtch恢复ra和sp后，会在进程内核栈上执行trap_user_return。
    proczero.kstack = KSTACK(proczero.pid);
    proczero.ctx.ra = (uint64)trap_user_return;
    proczero.ctx.sp = proczero.kstack + PGSIZE;

    // 先登记当前进程，返回用户态时才能通过myproc()取得它。
    cpu_t *cpu = mycpu();
    cpu->proc = &proczero;
    swtch(&cpu->ctx, &proczero.ctx);
}
