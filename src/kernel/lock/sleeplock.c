#include "mod.h"

// 睡眠锁初始化
void sleeplock_init(sleeplock_t *lk, char *name)
{
    spinlock_init(&lk->lock, name);
    lk->locked = 0;
    lk->name = name;
    lk->pid = 0;
}

// 检查当前进程是否持有睡眠锁
bool sleeplock_holding(sleeplock_t *lk)
{
    bool holding;
    push_off();
    holding = lk->locked && myproc() != NULL && lk->pid == myproc()->pid;
    pop_off();
    return holding;
}

// 当前进程尝试获取睡眠锁, 失败进入睡眠状态
void sleeplock_acquire(sleeplock_t *lk)
{
    spinlock_acquire(&lk->lock);
    while (lk->locked)
        proc_sleep(lk, &lk->lock);
    lk->locked = 1;
    lk->pid = myproc()->pid;
    spinlock_release(&lk->lock);
}

// 释放睡眠锁, 唤醒其他等待睡眠锁的进程
void sleeplock_release(sleeplock_t *lk)
{
    assert(sleeplock_holding(lk), "sleeplock_release: not holding");
    spinlock_acquire(&lk->lock);
    lk->locked = 0;
    lk->pid = 0;
    proc_wakeup(lk);
    spinlock_release(&lk->lock);
}