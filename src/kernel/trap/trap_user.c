#include "mod.h"

// in trampoline.S
extern char trampoline[];  // 内核和用户切换的代码
extern char user_vector[]; // 用户触发陷阱进入内核
extern char user_return[]; // 内核处理完毕返回用户

// in trap.S
extern char kernel_vector[]; // 内核态trap处理流程, 进入内核后应当切换中断处理入口

// in trap_kernel.c
extern char *interrupt_info[16]; // 中断错误信息
extern char *exception_info[16]; // 异常错误信息

// 在user_vector()里面调用
// 用户态trap处理的核心逻辑
void trap_user_handler()
{
    proc_t *proc = myproc();
    assert(proc != NULL, "trap_user_handler: no current process.");

    // 进入S-mode后使用内核trap入口，避免内核中的异常再次进入user_vector。
    w_stvec((uint64)kernel_vector);

    uint64 sepc = r_sepc();
    uint64 sstatus = r_sstatus();
    uint64 scause = r_scause();
    uint64 stval = r_stval();

    assert(!(sstatus & SSTATUS_SPP), "trap_user_handler: not from u-mode.");
    assert(intr_get() == 0, "trap_user_handler: interrupt enabled.");

    // 所有trap先保存用户PC；只有ecall需要额外跳过当前指令。
    proc->tf->user_to_kern_epc = sepc;
    uint64 trap_id = scause & 0x7ffffffffffffffful;

    // 最高位代表是中断还是异常
    // 1: 中断
    // 2: 异常
    if (scause & 0x8000000000000000ul)
    {
        switch (trap_id)
        {
        case 1:
            timer_interrupt_handler();
            proc_yield();
            break;
        case 9:
            external_interrupt_handler();
            break;
        default:
            char *info = trap_id < 16 ? interrupt_info[trap_id] : "unknown interrupt";
            printf("\nunexpected user interrupt: %s\n", info);
            printf("trap_id = %p, sepc = %p, stval = %p\n", trap_id, sepc, stval);
            panic("trap_user_handler");
        }
    }
    else
    {
        switch (trap_id)
        {
        // U-mode执行ecall产生的系统调用异常
        case 8:
            // ecall固定为4字节，不推进PC会在返回后重复执行同一系统调用。
            proc->tf->user_to_kern_epc += 4;
            syscall();
            break;
        case 13:
        case 15:
        {
            uint64 new_npage = uvm_ustack_grow(proc->pgtbl,
                                                proc->ustack_npage, stval);
            if (new_npage == (uint64)-1)
            {
                printf("invalid user stack fault: stval = %p\n", stval);
                panic("trap_user_handler: stack growth failed");
            }
            printf("user page fault: trap_id = %d, stval = %p\n",
                   (int)trap_id, stval);
            printf("user stack pages: %d -> %d\n",
                   (int)proc->ustack_npage, (int)new_npage);
            proc->ustack_npage = new_npage;
            break;
        }
        default:
            char *info = trap_id < 16 ? exception_info[trap_id] : "unknown exception";
            printf("\nunexpected user exception: %s\n", info);
            printf("trap_id = %p, sepc = %p, stval = %p\n", trap_id, sepc, stval);
            panic("trap_user_handler");
        }
    }

    trap_user_return();
}

// 调用user_return()
// 内核态返回用户态
void trap_user_return()
{
    proc_t *proc = myproc();
    assert(proc != NULL, "trap_user_return: no current process.");

    // 准备CSR和trapframe期间不能再次响应中断。
    intr_off();

    // stvec需要填写用户页表和内核页表共享的高虚拟地址。
    uint64 user_vector_va = TRAMPOLINE + (uint64)user_vector - (uint64)trampoline;
    w_stvec(user_vector_va);

    // user_vector切回内核页表前，会从trapframe恢复这些内核运行信息。
    proc->tf->user_to_kern_satp = r_satp();
    proc->tf->user_to_kern_sp = proc->kstack + PGSIZE;
    proc->tf->user_to_kern_trapvector = (uint64)trap_user_handler;
    proc->tf->user_to_kern_hartid = r_tp();

    // 清除SPP使sret进入U-mode，SPIE使返回后可以继续接收中断。
    uint64 sstatus = r_sstatus();
    sstatus &= ~SSTATUS_SPP;
    sstatus |= SSTATUS_SPIE;
    w_sstatus(sstatus);
    w_sepc(proc->tf->user_to_kern_epc);

    // 切页表后物理地址proc->tf不再有效，必须传入用户页表中的TRAPFRAME。
    uint64 user_return_va = TRAMPOLINE + (uint64)user_return - (uint64)trampoline;
    uint64 user_satp = MAKE_SATP(proc->pgtbl);
    ((void (*)(uint64, uint64))user_return_va)(TRAPFRAME, user_satp);
}
