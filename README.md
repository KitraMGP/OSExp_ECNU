# LAB-6：单进程走向多进程——进程调度与生命周期

## 实验目标

LAB-4 创建了第一个用户进程 `proczero`，LAB-5 完善了系统调用分派与用户地址空间管理。本实验在"从零到一"的基础上实现"从一到多"：以进程数组为资源仓库，复制出更多进程，并解决随之而来的两个问题：

- **多个进程竞争 CPU 资源**——引入基于循环扫描的调度器和基于时钟中断的抢占式调度。
- **进程的新生与死亡**——引入完整生命周期管理：`fork` 创建、`exit` 退出、`wait` 回收，以及 `sleep`/`wakeup` 睡眠唤醒机制。

本实验的主要目标包括：

- 建立进程数组 `proc_list` 与全局 PID 仓库，实现 `proc_alloc`/`proc_free` 的资源化管理。
- 实现 `proc_scheduler` 循环扫描调度与 `proc_sched` 切换，完成多进程上下文切换。
- 基于时钟中断实现抢占式调度（`proc_yield`）。
- 实现 `proc_fork`/`proc_exit`/`proc_wait` 进程生命周期，以及 `proc_sleep`/`proc_wakeup` 睡眠唤醒。
- 实现睡眠锁 `sleeplock`。
- 封装新系统调用：`print_str`、`print_int`、`getpid`、`fork`、`wait`、`exit`、`sleep`。

## 代码组织结构

本实验主要新增或修改以下模块：

```text
OSExp_ECNU
├── src
│   ├── kernel
│   │   ├── lock
│   │   │   ├── sleeplock.c        新增：睡眠锁 (TODO, 实现睡眠锁)
│   │   │   ├── method.h           CHANGE: 睡眠锁接口声明
│   │   │   └── type.h             CHANGE: sleeplock_t 类型
│   │   ├── mem
│   │   │   └── kvm.c              TODO: kvm_init 从单进程内核栈到多进程内核栈
│   │   ├── proc
│   │   │   ├── proc.c             TODO: 核心工作 (进程仓库/调度/生命周期)
│   │   │   ├── method.h           CHANGE: 进程管理接口
│   │   │   └── type.h             CHANGE: proc_state 枚举、共享字段、N_PROC
│   │   ├── syscall
│   │   │   ├── syscall.c          CHANGE: 跳转表支持新系统调用
│   │   │   ├── sysfunc.c          TODO: 实现新的系统调用
│   │   │   ├── method.h           CHANGE: 新系统调用声明
│   │   │   └── type.h             CHANGE: 系统调用号 (1-10)
│   │   ├── trap
│   │   │   ├── timer.c            TODO: 新增 timer_wait, 增加时钟中断调度逻辑
│   │   │   ├── trap_kernel.c      TODO: 增加时钟中断调度逻辑
│   │   │   ├── trap_user.c        TODO: 增加时钟中断调度逻辑
│   │   │   └── method.h           CHANGE: timer_wait 声明
│   │   └── main.c                 CHANGE: 初始化流程 + 进入 proc_scheduler
│   └── user
│       ├── initcode.c             CHANGE: 按测试需求设置的用户程序
│       └── syscall_num.h          CHANGE: 用户侧系统调用编号
└── pictures
    └── proc_state.jpg             进程状态转换图 (原理图)
```

## 原理说明

### 1. 进程数组与资源仓库

`proc_list` 是进程结构体仓库，最多容纳 `N_PROC` 个进程；`proczero` 从"元素"变成"指向元素的指针"。全局 `global_pid` 为每个进程分配唯一 PID，与 `mmap_region` 节点仓库一样，仓库需要有序管理：

- `proc_init`：对系统资源（static 变量）进行初始化。
- `proc_alloc`：从仓库申请空闲进程结构体，完成通用初始化，上锁返回（`p->ctx.ra` 应设置为 `proc_return`）。
- `proc_free`：向仓库释放进程结构体及其资源。

进程结构体新增了若干**共享字段**（`state`、`parent`、`exit_code`、`sleep_space`），进程 B 可能访问/修改进程 A 的这些字段，因此访问共享字段时通常需要先持有自旋锁 `lk`。

### 2. 基于循环扫描的进程调度

`main` 完成初始化后执行 `proc_scheduler` 永不返回。两个原生 CPU 执行流（原生进程 0/1）从初始化者变成调度选择与缓冲者：

```text
原生进程 -> proc_scheduler 扫描 proc_list 找到 RUNNABLE 进程 A
         -> swtch(原生上下文, A上下文)      // 第一次切换: 原生 -> A
用户进程 A -> proc_sched 主动/被动释放 CPU
         -> swtch(A上下文, 原生上下文)      // 第二次切换: A -> 原生
原生进程 -> 继续扫描, 找到新的 RUNNABLE 进程 B......
```

**注意：原生进程执行时 `CPU->proc` 设为 `NULL`，用户进程 A 执行时设为 A。** 用户进程 A 切换到用户进程 B 需要两次上下文切换，调度器在其中起选择与缓冲作用。

### 3. 基于时钟的抢占式调度

仅靠进程主动让出 CPU 无法打断长进程。实现方案：在用户态和内核态的时钟中断处理完成后，调用 `proc_yield` 强迫当前进程从 `RUNNING` 改为 `RUNNABLE` 并执行 `proc_sched`。每个 `RUNNABLE` 进程相当于持有 1 个长度为 1 的时间片（1 个 tick 约 0.1s）。

### 4. 进程状态转换

五种进程状态从冷到热：`unused`（死亡/未初始化）、`zombie`（濒临死亡，等父进程回收）、`sleeping`（等待资源）、`runnable`（准备就绪）、`running`（正在执行）：

```text
unused -> runnable -> running -> zombie -> unused   (短进程)
running -> runnable -> running ...                  (长进程被抢占)
running -> sleeping -> runnable -> ...              (睡眠与唤醒)
```

![进程状态转换](./pictures/proc_state.jpg)

### 5. 进程生命周期：fork / exit / wait

- **fork**：用户进程只有两条产生路径——`proczero`（精心准备的 `proc_make_first`）和其他进程（`proc_fork` 复制父进程）。`proc_fork` 申请空闲进程结构体、复制父进程的一切、记录父子关系、把子进程返回值置 0。活跃用户进程构成以 `proczero` 为根的树。
- **exit**：进程无法亲自回收自己的资源（"人无法给自己办葬礼"），因此子进程只标记自己进入 `ZOMBIE` 状态并设置 `exit_code`，由父进程回收。若父进程先退出，子进程会被"过继"给永不退出的 `proczero`。
- **wait**：父进程循环扫描进程数组，等到 `ZOMBIE` 孩子后调用 `proc_free` 完成回收。

```c
int pid = fork(); // 分支
if (pid == 0) { // 子进程
    do_something_1();
    exit(0); // 退出
} else { // 父进程
    int state;
    wait(&state); // 等待
    do_something_2();
}
```

### 6. 进程睡眠与唤醒 + 睡眠锁

父进程 `wait` 时若只 `proc_yield`，仍是 `RUNNABLE` 状态会被反复调度，耽误子进程执行。因此引入 `SLEEPING` 层级：

- `proc_sleep(sleep_space, lk)`：当前进程以某个资源为锚点进入睡眠（`RUNNING -> SLEEPING`），不可被调度；`lk` 用于保证"设置睡眠状态"与"释放锁"的原子性，避免唤醒丢失。
- `proc_wakeup(sleep_space)`：唤醒所有等待该资源的进程（`SLEEPING -> RUNNABLE`）。
- `proc_try_wakeup`：`proc_exit` 中针对唤醒父进程的特例。

睡眠锁 `sleeplock` 建立在自旋锁与睡眠唤醒机制之上：获取失败时让进程睡眠而非忙等，适合保护长期持有的资源（后续文件系统会用到）。其整体框架与自旋锁一致。

### 7. 相关系统调用

```c
uint64 sys_print_str(char *str);   // 打印字符串
uint64 sys_print_int(int num);     // 打印32位整数
uint64 sys_getpid();               // 获取当前进程的pid
uint64 sys_fork();                 // 进程复制
uint64 sys_wait(uint64 addr);      // 等待子进程退出
uint64 sys_exit(int exit_code);    // 进程退出
uint64 sys_sleep(uint64 ntick);    // 进程睡眠ntick个时钟周期
```

`sys_sleep` 的实现思路：让当前进程以系统时钟 `sys_timer` 为资源进入睡眠；每次时钟中断 `timer_update` 更新系统时钟时唤醒它检查逻辑，未到目标时间就重新入睡。

## 待实现任务（TODO）

已完成本次任务：

- `proc_init` 初始化进程数组、进程锁、全局 PID，并为每个进程槽位记录固定内核栈虚拟地址。
- `proc_alloc` 扫描 `proc_list` 申请 `UNUSED` 槽位，分配 PID，初始化通用字段和进程上下文；返回时持有进程锁，初始返回地址为 `proc_return`。
- `proc_free` 在持有进程锁的前提下释放 mmap 描述节点和用户页表资源，将进程恢复为 `UNUSED`。
- `proc_make_first` 改为使用 `proc_alloc` 创建 `proczero`，初始化完成后置为 `RUNNABLE` 并解锁，调度切换由后续调度器负责。
- `kvm_init` 为全部 `N_PROC` 个进程槽位分配并映射独立内核栈，栈之间保留保护页。

仍待完成：

- `proc_sched`/`proc_scheduler`：循环扫描调度。
- `proc_yield`：抢占式调度，并在 `trap_user.c`/`trap_kernel.c` 的时钟中断处理完成后调用。
- `proc_fork`/`proc_exit`/`proc_wait`/`proc_reparent`/`proc_try_wakeup`：生命周期。
- `proc_sleep`/`proc_wakeup`：睡眠唤醒。
- `sleeplock_*`：睡眠锁。
- `timer_wait` 与 `timer_update` 中的 `proc_wakeup` 逻辑。
- 新系统调用 `sys_print_str`/`sys_print_int`/`sys_getpid`/`sys_fork`/`sys_wait`/`sys_exit`/`sys_sleep` 的实现。

本次验证：`make build` 成功；`make run` 实际输出 `cpu 0 is booting!`、`cpu 1 is booting!`，随后因调度器尚未实现而输出 `panic! main: never back!`。

## 测试用例

以下测试来自实验指南，只需修改 `user/initcode.c`。**尚未运行，实际输出待功能实现后补充记录。**

**测试 1**（getpid + 打印）

```c
#include "sys.h"

int main()
{
	int pid = syscall(SYS_getpid);
	if (pid == 1) {
		syscall(SYS_print_str, "\nproczero: hello ");
		syscall(SYS_print_str, "world!\n");
	}
	while (1);	
}
```

**测试 2**（fork 分支，需在内核合适位置增加提示性输出）

```c
#include "sys.h"

int main()
{
	syscall(SYS_print_str, "level-1!\n");
	syscall(SYS_fork);
	syscall(SYS_print_str, "level-2!\n");
	syscall(SYS_fork);
	syscall(SYS_print_str, "level-3!\n");
	while(1) {}
}
```

**测试 3**（fork/wait/exit 综合：子进程读写 mmap 与堆区域后退出，父进程回收并核对退出码）

```c
#include "sys.h"

#define PGSIZE 4096
#define VA_MAX (1ul << 38)
#define MMAP_END (VA_MAX - (2 + 16 * 256) * PGSIZE)
#define MMAP_BEGIN (MMAP_END - 64 * 256 * PGSIZE)

int main()
{
	int pid, i;
	char *str1, *str2, *str3 = "STACK_REGION\n\n";
	char *tmp1 = "MMAP_REGION\n", *tmp2 = "HEAP_REGION\n";
	
	str1 = (char*)syscall(SYS_mmap, MMAP_BEGIN, PGSIZE);
	for (i = 0; tmp1[i] != '\0'; i++)
		str1[i] = tmp1[i];
	str1[i] = '\0';	

	str2 = (char*)syscall(SYS_brk, 0);
	syscall(SYS_brk, (long long int)str2 + PGSIZE);
	for (i = 0; tmp2[i] != '\0'; i++)
		str2[i] = tmp2[i];
	str2[i] = '\0';	

	syscall(SYS_print_str, "\n--------test begin--------\n");
	pid = syscall(SYS_fork);

	if (pid == 0) { // 子进程
		syscall(SYS_print_str, "child proc: hello!\n");
		syscall(SYS_print_str, str1);
		syscall(SYS_print_str, str2);
		syscall(SYS_print_str, str3);
		syscall(SYS_exit, 1234);
	} else { // 父进程
		int exit_state = 0;
		syscall(SYS_wait, &exit_state);
		syscall(SYS_print_str, "parent proc: hello!\n");
		syscall(SYS_print_int, pid);
		if (exit_state == 1234)
			syscall(SYS_print_str, "good boy!\n");
		else
			syscall(SYS_print_str, "bad boy!\n"); 
	}

	syscall(SYS_print_str, "--------test end----------\n");

	while (1);
	
	return 0;
}
```

**测试 4**（sleep，需在内核合适位置增加提示性输出）

```c
#include "sys.h"

int main()
{
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
	while(1);
}
```

## 尾声

第二阶段围绕进程管理主题：从一到多，构建进程调度与生命周期管理，同时完善 `lock | mem | syscall | trap` 模块。下一步（lab-7 到 lab-9）将引入磁盘外设，基于磁盘自底向上构建文件系统。
