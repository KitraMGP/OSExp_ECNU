# LAB-4：第一个用户进程的诞生

## 实验目标

LAB-1 到 LAB-3 完成了内核启动、内存管理和中断处理等基础设施。从本实验开始，内核将进入用户进程管理阶段。

本实验要创建第一个用户进程 `proczero`，使其从 S-mode 进入 U-mode，主动发起系统调用，并由内核输出：

```text
proczero: hello world!
```

完成这条执行链需要建立进程数据结构和地址空间，实现上下文切换、用户态 trap 入口与返回流程，并处理第一个系统调用。

## 代码组织结构

本实验主要新增或修改以下模块：

```text
OSExp_ECNU
├── Makefile                         构建并嵌入用户程序
├── kernel.ld                       增加 trampoline 段的链接布局
└── src
    ├── kernel
    │   ├── lib
    │   │   ├── cpu.c               获取当前进程
    │   │   ├── method.h
    │   │   └── type.h              扩展 CPU 结构体
    │   ├── mem
    │   │   └── kvm.c               映射 trampoline 和内核栈
    │   ├── proc
    │   │   ├── proc.c              第一个进程的创建逻辑
    │   │   ├── swtch.S             S-mode 上下文切换
    │   │   ├── method.h
    │   │   ├── mod.h
    │   │   └── type.h              proc、context 和 trapframe 定义
    │   ├── trap
    │   │   ├── trap_user.c         用户态 trap 处理
    │   │   ├── trampoline.S        U-mode 与 S-mode 切换入口
    │   │   ├── method.h
    │   │   └── mod.h
    │   └── main.c                  初始化并启动 proczero
    └── user
        ├── initcode.c              第一个用户程序
        ├── sys.h                   syscall 调用封装
        ├── syscall_arch.h          RISC-V ecall 封装
        └── syscall_num.h           syscall 编号
```

## 原理说明

### 1. 进程及其运行环境

程序是磁盘或内存中的静态代码和数据，进程则是程序的一次运行实例。除了可执行代码，一个可运行的用户进程还需要维护：

- `pid`：进程标识符。
- 用户页表：描述该进程可访问的虚拟地址空间。
- 用户栈和用户堆：保存函数调用状态及动态数据。
- 内核栈：进程陷入内核后使用的执行空间。
- `trapframe`：保存跨 U-mode 与 S-mode 切换时的完整用户寄存器状态。
- `context`：保存内核执行流切换时需要恢复的寄存器状态。

因此，`proc_t` 不仅表示一段程序，还集中记录了恢复该进程运行所需的地址空间和处理器状态。

### 2. 用户进程地址空间

内核启动阶段使用 `CPU_stack` 和全局内核页表。用户进程则拥有自己的用户页表，其中至少需要映射：

- 用户程序的代码和数据。
- 用户栈及相邻的用户堆空间。
- `trapframe`，供 trampoline 保存和恢复用户寄存器。
- `trampoline`，供用户态和内核态在切换页表前后执行同一段过渡代码。

内核页表还需要增加两类映射：

- `TRAMPOLINE`：映射 `trampoline.S` 的代码页。
- `KSTACK(0)`：映射 `proczero` 的内核栈。

![用户页表与内核页表的地址空间布局](./pictures/03.png)

图中两侧的 trampoline 通过不同页表映射到相同虚拟地址；内核栈上方保留未映射的间隔页，用于阻止栈溢出越过进程边界。用户栈从高地址向下增长，用户堆则从代码和数据区域上方向高地址增长。

`trampoline` 必须在用户页表和内核页表中位于相同虚拟地址。切换 `satp` 的过程中，处理器才能继续执行这段代码，而不会因地址含义变化失去下一条指令。

### 3. 用户程序如何进入内核镜像

QEMU 启动时只直接加载内核 ELF，因此 `initcode` 需要在构建阶段嵌入内核。构建流程会先编译 `src/user/initcode.c`，再把生成的用户程序转换为可被内核引用的字节数组。`proc.c` 根据数组起始位置和长度，将它复制到为用户代码申请的物理页中。

修改用户程序后需要重新生成对应的中间文件；必要时先执行 `make clean`，避免继续使用旧的 `initcode` 数据。

### 4. S-mode 内的上下文切换

`swtch(old, new)` 在两个 S-mode 执行流之间切换：

1. 将当前执行流需要保留的寄存器写入 `old context`。
2. 从 `new context` 恢复寄存器。
3. 通过恢复后的 `ra` 和 `sp` 转入新的内核执行流。

第一次启动 `proczero` 时，旧上下文属于当前 CPU，新上下文属于 `proczero`。进程初始化需要预先设置：

- `context.ra = trap_user_return`，让切换后的第一条返回路径进入用户态准备函数。
- `context.sp` 指向该进程内核栈顶部。

CPU 结构体还需要保存自己的调度上下文以及当前运行进程指针，供 `myproc()` 等内核代码访问。

### 5. U-mode 与 S-mode 间的切换

跨特权级切换需要保存的状态更多，因此使用 `trapframe`，而不是仅保存少量寄存器的 `context`。

从内核返回用户态时，执行链为：

```text
trap_user_return -> user_return -> sret -> U-mode
```

其中 `trap_user_return` 设置用户 trap 入口、`sepc` 和 `sstatus`，并将内核页表、内核栈等信息写入 `trapframe`。`user_return` 位于 trampoline 中，负责切换至用户页表、恢复用户寄存器并执行 `sret`。

用户态发生 trap 时，执行链为：

```text
U-mode -> user_vector -> trap_user_handler -> trap_user_return -> user_return
```

`user_vector` 先保存用户寄存器，再恢复内核运行所需的信息、切换到内核页表并调用 C 语言处理函数。处理完成后，复用返回路径恢复用户现场。

### 6. 用户态 trap 与系统调用

用户态 trap 处理与内核态 trap 处理的总体判断逻辑相似，但还需要完成以下工作：

- 进入内核后将 trap 入口切换为内核态入口，防止 S-mode 内再次发生 trap 时错误进入用户入口。
- 保存发生 trap 时的用户 `sepc`。
- 处理用户态时钟中断和串口中断。
- 识别 U-mode 发出的 `ecall`，其异常编号为 8。

系统调用由用户程序和内核共同遵守调用约定：用户侧在指定寄存器中放入 syscall 编号和参数，再执行 `ecall`；内核侧读取编号、分派服务，并把返回值写回约定寄存器。

虽然 `ecall` 属于同步异常，完成系统调用后却不能再次执行同一条指令，因此处理函数需要令 `sepc += 4`，再返回用户态。

本实验只实现 `SYS_helloworld`，暂不建立完整的系统调用分发表。

## 具体工作内容

### 1. 完善实验 3 的串口输入

在正式进入实验 4 之前，先完善实验 3 的串口输入功能。我们用固定长度行缓冲区、逻辑光标位置以及 ANSI CSI/SS3 控制序列状态机实现了更完善的行编辑功能。

具体行为包括：

- 分别维护当前行长度和光标在行内的位置。
- 将左右方向键限制在行首和行尾之间，忽略上下方向键。
- 在行中输入字符时移动缓冲区内容，并重绘光标右侧文本。
- Backspace 删除光标前的字符，Delete 删除光标所在字符。
- 回车后清空当前行及控制序列解析状态。

### 2. 迁移实验 4 的基础代码

- 增加 `proc` 模块、用户态 trap 文件、trampoline 汇编和第一个用户程序。
- 更新公共头文件，使进程、CPU、内存和 trap 模块能够互相引用所需类型与接口。
- 修改 `Makefile`，编译用户程序并将其数据嵌入内核。
- 修改 `kernel.ld`，保证 trampoline 按页对齐并可被页表正确映射。

### 3. 补充内核地址空间

在 `kvm_init()` 创建内核页表时，增加 trampoline 与 `KSTACK(0)` 映射。映射时需要检查页对齐、权限位和物理页来源，避免用户代码获得不应有的内核写权限。

实现记录：

- 已将链接脚本按页对齐的 `trampoline` 物理页映射到内核虚拟地址 `TRAMPOLINE`，权限为只读、可执行。
- 已从内核物理页区域申请 `proczero` 的内核栈，将其映射到 `KSTACK(0)`，权限为可读、可写。
- trampoline 和内核栈映射均未设置 `PTE_U`，只能由内核态访问。

### 4. 创建 proczero

用户页表初始化部分已经完成：`proc_pgtbl_init()` 从内核物理页区域申请顶级页表，并建立 trampoline 和 trapframe 映射。用户页表中的 trampoline 权限为只读、可执行，trapframe 权限为可读、可写；两者都不设置 `PTE_U`，防止用户程序直接访问内核过渡代码和保存的寄存器现场。

实现 `proc_make_first()`，按以下顺序准备第一个用户进程：

1. 初始化 `pid`，申请并清零 `trapframe`。
2. 创建用户页表，映射 trampoline 和 trapframe。
3. 申请用户栈和用户代码物理页，建立用户态映射。
4. 将内嵌的 `initcode` 复制到用户代码页。
5. 设置用户入口地址、用户栈顶和进程内存边界。
6. 设置内核栈、`context.ra` 和 `context.sp`。
7. 记录当前进程并通过 `swtch()` 切换到它的内核上下文。

实现记录：上述创建流程已经完成。用户代码映射在 `USER_BASE`，用户栈映射在 `TRAPFRAME` 下方一页；首次上下文切换恢复 `proczero` 的内核栈，并通过 `context.ra` 进入 `trap_user_return()`。

### 5. 实现用户态 trap 往返

- 在 `trap_user_return()` 中准备返回用户态所需的 CSR 和 trapframe 字段。
- 理解并接入 `trampoline.S` 中的 `user_vector` 与 `user_return`。
- 在 `trap_user_handler()` 中识别中断、异常和用户系统调用。
- 处理完成后返回 `trap_user_return()`，恢复用户页表和寄存器。

实现记录：用户态 trap 往返已经完成。`trap_user_return()` 会关闭中断，将 `stvec` 指向用户 trap 入口，填写内核页表、内核栈和 hart ID，设置 `sepc` 与 `sstatus`，最后跳转到 trampoline 中的 `user_return` 切换用户页表并执行 `sret`。`trap_user_handler()` 进入内核后切换回内核 trap 入口，保存用户 PC，并分别处理时钟中断、UART 中断和用户异常。

### 6. 响应第一个系统调用

在 `initcode.c` 中调用 `syscall(SYS_helloworld)`。内核识别 syscall 编号后输出 `proczero: hello world!`，将 `sepc` 移到 `ecall` 的下一条指令，再返回用户态。

实现记录：用户程序通过 `a7` 传递系统调用号，内核对 `SYS_helloworld` 输出消息并在 `a0` 中返回 0。处理 `ecall` 后将保存的用户 PC 增加 4，保证连续两次调用都能被执行；未知系统调用返回 -1。

### 7. 启动第一个用户进程

在 `main()` 完成已有内核资源初始化后，由一个 CPU 创建并启动 `proczero`。其他 CPU 暂时保持现有等待逻辑，避免多个 CPU 同时初始化同一个进程。

实现记录：CPU 0 在完成物理内存、内核页表和 trap 初始化后调用 `proc_make_first()`，通过首次上下文切换启动 `proczero`。

## 测试用例

### 1. UART 行编辑测试

启动 QEMU 后依次检查：

- 在行尾继续按右方向键，光标保持不动。
- 在行首继续按左方向键，光标保持不动。
- 上下方向键不会改变光标位置。
- 将光标移到行中后输入字符，原有后缀整体右移。
- Backspace 和 Delete 分别删除光标前、光标处的字符，屏幕与缓冲区保持一致。

### 2. 系统调用测试

让 `initcode.c` 连续发出两次 `SYS_helloworld`：

```c
int main()
{
    syscall(SYS_helloworld);
    syscall(SYS_helloworld);
    while (1)
        ;
    return 0;
}
```

预期内核输出两次：

```text
proczero: hello world!
proczero: hello world!
```

若只输出一次并持续陷入 syscall，优先检查 `sepc` 是否增加 4；若进入用户态前发生页错误，则检查用户代码、用户栈、trapframe 和 trampoline 的映射与权限。

### 3. 用户态中断测试

`proczero` 进入死循环后，继续观察时钟 tick，并从串口输入字符。时钟和 UART 中断都应经由用户态 trap 入口进入内核，处理后返回原用户执行流。

### 4. 完成验证

- `make build` 可以完整编译并链接内核和 `initcode`。
- QEMU 启动后连续输出两次 `proczero: hello world!`，验证系统调用处理和 `sepc += 4` 正常。
- `proczero` 进入用户态死循环后，延迟输入 `uart-test` 仍能正确回显；运行期间周期性时钟中断也未打断用户执行流。

## 总结

LAB-4 将内核从“能够处理中断的裸机程序”推进到“能够承载用户程序并提供服务”的状态。完成实验后，应能解释以下两条独立但衔接的切换链路：

- `swtch` 使用 `context` 在 S-mode 内切换执行流。
- trampoline 使用 `trapframe` 在 U-mode 与 S-mode 之间保存和恢复完整现场。

后续实验将在此基础上扩展用户内存管理、系统调用体系、进程生命周期和多进程调度。
