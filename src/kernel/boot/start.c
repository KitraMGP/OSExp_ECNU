#include "../arch/mod.h"

// 每个CPU在运行操作系统时需要一个初始的函数栈
__attribute__((aligned(16))) uint8 CPU_stack[4096 * NCPU];

extern void main();

void start()
{
    // 暂时不开启分页，使用物理地址
    w_satp(0);

    // 切换到S-mode后无法访问M-mode的寄存器
    // 所以需要将hartid存到可访问的寄存器tp
    int id = r_mhartid();
    w_tp(id);

    // 修改mstatus寄存器，假装上一个状态是S-mode
    uint64 status = r_mstatus();
    status &= ~MSTATUS_MPP_MASK;
    status |= MSTATUS_MPP_S;
    w_mstatus(status);

    // 配置PMP，允许S-mode访问全部物理内存
    // 否则mret进入S-mode后CPU取指立即被PMP拦截
    w_pmpaddr0(0x3fffffffffffffull);
    w_pmpcfg0(0xf);

    // 设置M-mode的返回地址
    w_mepc((uint64) main);
    // 触发状态迁移，回到上一个状态（M-mode->S-mode）
    asm volatile("mret");
}