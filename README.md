# LAB-6: 单进程走向多进程 - 进程调度与生命周期

## 1. 实验目标

LAB-4 创建第一个用户进程 `proczero`，LAB-5 建立系统调用和用户地址空间。本实验将内核从单进程扩展为多进程系统，完成：

- 以固定进程数组管理进程资源和 PID。
- 通过循环扫描调度器运行多个 `RUNNABLE` 进程。
- 通过时钟中断实现抢占式时间片调度。
- 支持 `fork`、`exit`、`wait` 的进程生命周期。
- 支持 `sleep`/`wakeup` 以及基于其构建的睡眠锁。
- 为用户态提供打印、PID、进程和睡眠系统调用。

## 2. 代码结构

```text
src/kernel/
├── lock/
│   ├── spinlock.c      自旋锁和中断嵌套控制
│   └── sleeplock.c     等待资源时阻塞的睡眠锁
├── mem/
│   ├── kvm.c           为 N_PROC 个槽位映射内核栈与保护页
│   └── uvm.c           用户页表复制和销毁
├── proc/
│   ├── proc.c          进程仓库、调度、生命周期、睡眠唤醒
│   ├── swtch.S         保存/恢复内核 callee-saved 寄存器
│   └── type.h          proc_t、context_t、trapframe_t、进程状态
├── syscall/
│   ├── syscall.c       syscall number 到服务函数的分派
│   └── sysfunc.c       进程和打印系统调用实现
└── trap/
    ├── timer.c         系统 tick、定时等待与时钟唤醒
    ├── trap_kernel.c   内核态时钟中断抢占
    └── trap_user.c     用户态时钟中断抢占
src/user/initcode.c     当前启用测试 1；测试 2-4 保留为注释
```

## 3. 进程资源仓库

`proc_list[N_PROC]` 是固定进程仓库；每个槽位永久对应一个内核栈虚拟地址 `KSTACK(i)`。`kvm_init` 为每个槽位分配一页内核物理页并映射，两个相邻内核栈之间保留一页未映射保护页。

`proc_init` 初始化所有槽位、进程锁、PID 锁和 `proc_wait_lk`，并将 `global_pid` 置为 1。`proc_alloc` 顺序扫描 `UNUSED` 槽位，初始化通用字段，设置：

```c
proc->ctx.ra = (uint64)proc_return;
proc->ctx.sp = proc->kstack + PGSIZE;
```

它返回时仍持有 `proc->lk`，调用者完成进程特有初始化后才设置 `RUNNABLE` 并解锁。`proc_free` 只能回收 `ZOMBIE` 进程；它归还 mmap 描述节点，销毁用户页表和 trapframe，最后转为 `UNUSED`。

`proc_make_first` 使用 `proc_alloc` 创建 `proczero`，填充 initcode、用户栈、页表和 trapframe 后置为 `RUNNABLE`。它不直接切换上下文，首次执行由 scheduler 决定。

## 4. 进程状态与调度

### 4.1 状态机

```text
UNUSED --alloc/fork--> RUNNABLE --scheduler--> RUNNING
  ^                                             |
  |                                             | exit
  |                                             v
  +------------------------ wait/free -------- ZOMBIE

RUNNING --yield/timer--> RUNNABLE
RUNNING --sleep--------> SLEEPING --wakeup--> RUNNABLE
```

![进程状态转换](./pictures/proc_state.jpg)

状态与其保护：

| 字段 | 语义 | 保护方式 |
|---|---|---|
| `state` | 进程当前状态 | 对应 `proc->lk` |
| `parent` | 父进程 | `proc_wait_lk` 与子进程锁 |
| `exit_code` | 僵尸进程退出码 | 子进程锁 |
| `sleep_space` | 当前等待对象/channel | 进程锁 |

### 4.2 循环扫描 scheduler

每个 hart 的原生执行流在 `main` 初始化完成后进入 `proc_scheduler()`。scheduler 循环扫描 `proc_list`：

```text
scheduler 获取 P->lk
  P 为 RUNNABLE -> P.state = RUNNING, CPU->proc = P
  swtch(&CPU->ctx, &P->ctx)
  P 让出 CPU 后回到此处
  CPU->proc = NULL，释放 P->lk
```

`swtch(old, new)` 先保存当前执行流的内核上下文至 `old`，再恢复 `new`。因此：

```text
scheduler -> process: swtch(&cpu->ctx, &proc->ctx)
process -> scheduler: swtch(&proc->ctx, &cpu->ctx)
```

进程首次被运行时，其 `ctx.ra` 为 `proc_return`。该函数释放 scheduler 跨上下文转交的进程锁，再调用 `trap_user_return` 通过 `sret` 进入用户态。

`proc_sched` 只能在持有当前进程锁、中断关闭且只保留一层 `push_off` 时调用。它切回 scheduler，由 scheduler 在返回点释放进程锁。

### 4.3 时钟抢占

M-mode 时钟中断转化为 S-mode software interrupt。用户态和内核态的 `trap_id == 1` 分支均先调用 `timer_interrupt_handler`，再在存在当前进程时调用 `proc_yield`：

```text
RUNNING -> RUNNABLE
proc_sched() -> scheduler
```

scheduler 空闲运行时 `myproc() == NULL`，内核态时钟分支不会尝试抢占空进程。

## 5. 生命周期: fork, exit, wait

### 5.1 fork

`proc_fork`：

1. 通过 `proc_alloc` 申请子进程槽位。
2. 分配子 trapframe，复制父 trapframe。
3. 创建子用户页表，复制代码、堆、mmap 区域和用户栈的物理页。
4. 逐个复制 mmap 描述节点，保持地址顺序。
5. 设置 `child->parent = parent`、`child->tf->a0 = 0`。
6. 设置子进程 `RUNNABLE`；父进程得到子 PID。

父子返回值满足：父进程返回正 PID，子进程返回 0。

### 5.2 exit 与 wait

`proc_exit` 不释放当前进程自身资源。它持有 `proc_wait_lk` 和自身锁，写入 `exit_code`、将状态置为 `ZOMBIE`、过继其子进程给永不退出的 `proczero`，并唤醒等待自己的父进程；随后通过 `proc_sched` 永久离开执行流。

`proc_wait` 持有 `proc_wait_lk` 扫描子进程：

- 找到 `ZOMBIE` 子进程：通过 `uvm_copyout` 将退出码写入用户地址，调用 `proc_free`，返回子 PID。
- 没有任何子进程：返回 `-1`。
- 存在未退出子进程：以父进程指针为等待对象调用 `proc_sleep`。

`proc_wait_lk` 使“扫描不到僵尸子进程”和“登记为等待者”成为一个受保护的连续操作，避免子进程在两者之间退出导致父进程永久错过唤醒。

## 6. sleep, wakeup 与睡眠锁

### 6.1 通用睡眠协议

`proc_sleep(sleep_space, lock)` 的调用者已经持有用于检查资源条件的 `lock`。该函数的锁状态转换为：

```text
调用前: 持有 lock，不持有 proc->lk
获取 proc->lk
设置 sleep_space 和 SLEEPING
释放 lock
切换到 scheduler
被唤醒并重新运行
重新获取 lock
释放 proc->lk
返回后: 持有 lock，不持有 proc->lk
```

这样“设置 `SLEEPING`”先于“释放条件锁”，资源生产者不能在进程尚未登记为等待者时完成唤醒，从而避免 lost wakeup。

`proc_wakeup(sleep_space)` 扫描进程表，将所有同时满足以下条件的进程转为 `RUNNABLE`：

```c
proc->state == SLEEPING && proc->sleep_space == sleep_space
```

### 6.2 定时睡眠

`timer_wait(ntick)` 在持有 `sys_timer.lk` 时计算目标 tick：

```c
uint64 target = sys_timer.ticks + ntick;
while (sys_timer.ticks < target)
    proc_sleep(&sys_timer, &sys_timer.lk);
```

CPU-0 在 `timer_update` 中递增 `sys_timer.ticks` 后调用 `proc_wakeup(&sys_timer)`。被唤醒的进程重新获取时钟锁，检查目标是否已达到；未达到则再次睡眠。

### 6.3 睡眠锁

`sleeplock` 用内部自旋锁保护 `locked`、`pid` 和锁名称。获取者若发现锁已持有，以 sleeplock 自身为 `sleep_space` 睡眠；释放者清除占有者 PID 后调用 `proc_wakeup(lk)`。因此它适合未来文件系统等长时间持有的资源，避免自旋锁的忙等。

## 7. 用户系统调用

| 编号 | 调用 | 结果 |
|---:|---|---|
| 1 | `brk` | 调整或查询用户堆顶 |
| 2 | `mmap` | 创建用户内存映射 |
| 3 | `munmap` | 解除用户内存映射 |
| 4 | `print_str` | 将用户字符串复制到内核缓冲区并打印 |
| 5 | `print_int` | 打印 32 位有符号整数 |
| 6 | `getpid` | 返回当前进程 PID |
| 7 | `fork` | 创建子进程；父返回 PID，子返回 0 |
| 8 | `wait` | 等待并回收子进程，返回其 PID |
| 9 | `exit` | 设置退出码并成为僵尸进程，不返回 |
| 10 | `sleep` | 睡眠指定 tick 数，1 tick 约 0.1 s |

系统调用号从 `a7` 读取，参数位于 `a0`-`a5`，返回值写回 `a0`。用户字符串由 `arg_str` 和 `uvm_copyin_str` 访问；`wait` 的退出码地址由 `uvm_copyout` 写回，内核不直接解引用用户指针。

## 8. 构建与运行

在仓库根目录执行：

```sh
make build
make run
```

`make run` 启动双 hart、128 MiB RAM 的 QEMU `virt` 机器。用户测试程序位于 `src/user/initcode.c`；当前启用测试 1，测试 2-4 保留为注释块。切换测试后运行 `make run` 会重新生成 `src/user/initcode.h` 并构建内核。

## 9. 测试与实际输出

### 测试 1: getpid 与字符串打印

```c
int pid = syscall(SYS_getpid);
if (pid == 1) {
    syscall(SYS_print_str, "\nproczero: hello ");
    syscall(SYS_print_str, "world!\n");
}
while (1);
```

实际输出：

```text
cpu 0 is booting!
cpu 1 is booting!

proczero: hello world!
```

验证 `proczero` 创建、scheduler 首次切换、用户态 syscall、PID 返回和打印。

### 测试 2: 两次 fork

```c
syscall(SYS_print_str, "level-1!\n");
syscall(SYS_fork);
syscall(SYS_print_str, "level-2!\n");
syscall(SYS_fork);
syscall(SYS_print_str, "level-3!\n");
while (1);
```

实际输出：

```text
level-1!
level-2!
level-2!
level-3!
level-3!
level-3!
level-3!
```

验证第一次 fork 后有两个执行流，第二次 fork 后有四个最终执行流，以及 timer preemption 下的循环调度。

### 测试 3: fork, mmap, brk, wait, exit

测试先在 mmap、堆和用户栈中分别写入字符串；子进程打印这些字符串并以 `1234` 退出；父进程 `wait` 后检查退出码。

实际关键输出：

```text
--------test begin--------
child proc: hello!
MMAP_REGION
HEAP_REGION
STACK_REGION

parent proc: hello!
2good boy!
--------test end----------
```

其中 `2` 是该次运行分配给子进程的 PID。该测试验证用户页表深复制、mmap 描述复制、exit/wait 退出码传递和僵尸回收。

### 测试 4: 定时 sleep

```c
int pid = syscall(SYS_fork);
if (pid == 0) {
    syscall(SYS_print_str, "Ready to sleep!\n");
    syscall(SYS_sleep, 30);
    syscall(SYS_print_str, "Ready to exit!\n");
    syscall(SYS_exit, 0);
} else {
    syscall(SYS_wait, 0);
    syscall(SYS_print_str, "Child exit!\n");
}
while (1);
```

实际输出：

```text
Ready to sleep!
Ready to exit!
Child exit!
```

验证子进程在 30 tick 等待期间不参与调度、时钟唤醒、子进程退出和父进程 wait 唤醒。

## 10. 完成情况与边界

LAB-6 的进程仓库、内核栈映射、循环扫描调度、时钟抢占、fork/exit/wait、sleep/wakeup、睡眠锁和系统调用均已实现并通过上述端到端测试。

本实验的 `fork` 采用立即复制用户物理页；不实现 copy-on-write。`proczero` 不允许退出；孤儿进程会被过继给它。磁盘、文件系统和 `exec` 不属于本实验，将在 LAB-7 至 LAB-9 引入。
