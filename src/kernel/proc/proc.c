#include "mod.h"
#include "../../user/initcode.h"

#define initcode target_user_initcode
#define initcode_len target_user_initcode_len

// in trampoline.S
extern char trampoline[];

// in swtch.S
extern void swtch(context_t *old, context_t *new);

// in trap/trap_user.c
extern void trap_user_return();

/* ------------本地变量----------- */

// 进程结构体数组 + 第一个用户进程的指针
static proc_t proc_list[N_PROC];
static proc_t *proczero;

// 全局pid + 保护它的锁
static int global_pid;
static spinlock_t pid_lk;

/* 获取一个pid */
static int alloc_pid()
{
    int tmp = 0;
    spinlock_acquire(&pid_lk);
    assert(global_pid > 0, "alloc_pid: overflow");
    tmp = global_pid++;
    spinlock_release(&pid_lk);
    return tmp;
}

/* 释放进程锁 + trap_user_return */
static void proc_return()
{

}

/* 进程模块初始化 */
void proc_init()
{    

}

/* 
    申请一个UNUSED进程结构体(返回时带锁)
    并执行通用的初始化逻辑
*/
proc_t *proc_alloc()
{
    // TODO(lab-6): 扫描proc_list, 找到UNUSED进程, 完成通用初始化后带锁返回
    return NULL;
}

/* 
    回收一个进程结构体并释放它包含的资源
    tips: 调用者需要持有进程锁
*/
void proc_free(proc_t *p)
{

}

/* 
    获得一个初始化过的用户页表
    完成trapframe和trampoline的映射
*/
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
    // TODO(lab-6): 改为通过proc_alloc申请proczero, 只完成初始化并解锁返回,
    // 不应直接调用swtch; 调度由proc_scheduler统一完成。
    // 迁移阶段: 继承lab-5行为, 直接使用proc_list[0]。
    proczero = &proc_list[0];

    // trapframe保存在用户物理页中，但只通过内核映射访问。
    proczero->pid = 0;
    proczero->tf = (trapframe_t *)pmem_alloc(false);
    proczero->pgtbl = proc_pgtbl_init((uint64)proczero->tf);

    // 用户栈位于TRAPFRAME下方，栈顶从高地址TRAPFRAME开始向下增长。
    uint64 ustack_page = (uint64)pmem_alloc(false);
    vm_mappages(proczero->pgtbl, TRAPFRAME - PGSIZE, ustack_page,
                PGSIZE, PTE_R | PTE_W | PTE_U);

    // initcode同时包含代码和数据，因此首个实验进程暂时使用RWX权限。
    uint64 initcode_page = (uint64)pmem_alloc(false);
    vm_mappages(proczero->pgtbl, USER_BASE, initcode_page,
                PGSIZE, PTE_R | PTE_W | PTE_X | PTE_U);
    assert(initcode_len <= PGSIZE, "proc_make_first: initcode is too large.");
    memmove((void *)initcode_page, initcode, initcode_len);

    // trapframe提供首次sret需要恢复的用户PC和用户栈指针。
    proczero->tf->user_to_kern_epc = USER_BASE;
    proczero->tf->sp = TRAPFRAME;
    proczero->heap_top = USER_BASE + PGSIZE;
    proczero->ustack_npage = 1;
    proczero->mmap = NULL;

    // swtch恢复ra和sp后，会在进程内核栈上执行trap_user_return。
    proczero->kstack = KSTACK(proczero->pid);
    proczero->ctx.ra = (uint64)trap_user_return;
    proczero->ctx.sp = proczero->kstack + PGSIZE;

    // 先登记当前进程，返回用户态时才能通过myproc()取得它。
    cpu_t *cpu = mycpu();
    cpu->proc = proczero;
    swtch(&cpu->ctx, &proczero->ctx);
}

/*
    父进程产生子进程
    UNUSED -> RUNNABLE
*/
int proc_fork()
{
    // TODO(lab-6): 申请进程结构体, 复制父进程资源, 记录父子关系,
    // 子进程返回值置0
    return 0;
}

/*
    进程主动放弃CPU控制权
    RUNNING->RUNNABLE
*/
void proc_yield()
{

}

/*
    当父进程退出时, 让它的所有子进程认proczero为父
    因为proczero永不退出, 可以回收子进程的资源
*/
static void proc_reparent(proc_t *parent)
{

}

/*
    唤醒等待呼叫的进程
    由proc_exit调用
    tips: 调用者需要持有p的进程锁
*/
static void proc_try_wakeup(proc_t *p)
{

}

/*
    进程退出
    RUNNING -> ZOMBIE
*/
void proc_exit(int exit_code)
{

}

/*
    父进程等待一个子进程进入ZOMBIE状态
    1. 如果等到: 释放子进程, 返回子进程的pid, 将子进程的退出状态传出到user_addr
    2. 如果发现没孩子: 返回-1
    3. 如果没等到: 父进程进入睡眠状态 
*/
int proc_wait(uint64 user_addr)
{
    // TODO(lab-6): 扫描proc_list等待ZOMBIE子进程, 用proc_free回收
    return 0;
}

/*
    进程等待sleep_space对应的资源, 进入睡眠状态
    RUNNING -> SLEEPING
*/
void proc_sleep(void *sleep_space, spinlock_t *lock)
{

}

/*
    唤醒所有等待sleep_space的进程
    SLEEPING -> RUNNABLE
*/
void proc_wakeup(void *sleep_space)
{

}

/* 
    用户进程切换到调度器
    tips: 调用者保证持有当前进程的锁
*/
void proc_sched()
{

}

/* 
    调度器
    RUNNABLE->RUNNING
*/
void proc_scheduler()
{

}
