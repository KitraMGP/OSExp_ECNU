# LAB-1: 机器启动

## 1. 代码组织结构

```plaintext
ECNU-OSLAB-2025-TASK  
├── LICENSE        开源协议  
├── .vscode        配置了可视化调试环境
├── registers.xml  配置了可视化调试环境  
├── Makefile       编译运行整个项目  
├── common.mk      Makefile中一些工具链的定义  
├── kernel.ld      定义了内核程序在链接时的布局   
├── README.md      实验指导书  
└── src            源码
    └── kernel     内核源码
        ├── arch   RISC-V相关
        │   ├── method.h  
        │   ├── mod.h  
        │   └── type.h  
        ├── boot   机器启动
        │   ├── entry.S  
        │   └── start.c (TODO)  
        ├── lock   锁机制
        │   ├── spinlock.c (TODO)  
        │   ├── method.h  
        │   ├── mod.h  
        │   └── type.h  
        ├── lib    常用库
        │   ├── cpu.c  
        │   ├── print.c (TODO)  
        │   ├── uart.c  
        │   ├── method.h  
        │   ├── mod.h  
        │   └── type.h  
        └── main.c (TODO)  
```

## 2. 实验核心目标

完成双核的机器启动, 进入main函数并输出启动信息：

```plaintext
cpu 1 is booting!
cpu 0 is booting!
```

## 3. 具体工作

### 3.1 配置开发环境

在 Arch Linux 中，安装适用于 `riscv64` 的 QEMU 和 GCC 工具链：

```bash
sudo pacman -S qemu-system-riscv riscv64-elf-gcc riscv64-elf-gdb
```

编辑 `common.mk` 文件，调整编译选项：

```Makefile
# 将 GCC 工具链前缀改成实际安装的名称
TOOLPREFIX = riscv64-elf-

# -std=gnu17 指定使用 C17 标准，默认最新 C 标准内置了 bool 类型会和 src/kernel/arch/type.h 单独定义的 bool 类型冲突
# -Wno-error=unused-function 防止出现“函数未使用”报错导致无法编译，设置后它只会展示为警告
CFLAGS += -std=gnu17 -Wno-error=unused-function
```

### 3.2 编辑引导代码

编辑 `src/kernel/boot/start.c`，在 `start()` 中添加代码：

```c
void start()
{
    // ...

    // 配置PMP，允许S-mode访问全部物理内存
    // 否则mret进入S-mode后CPU取指立即被PMP拦截
    w_pmpaddr0(0x3fffffffffffffull);
    w_pmpcfg0(0xf);

    // 设置M-mode的返回地址
    w_mepc((uint64) main);
    // 触发状态迁移，回到上一个状态（M-mode->S-mode）
    asm volatile("mret");
}
```

在我的环境下，必须配置PMP才能正常引导到main()。start()的最后两条语句用于转移到S-mode并运行main()函数。

为了测试是否到达main()函数，可用GDB来调试内核。在项目根目录编写`.gdbinit`：

```plaintext
set confirm off
set architecture riscv:rv64
target remote 127.0.0.1:26000
symbol-file target/kernel/kernel-qemu.elf
set disassemble-next-line auto
```

启动QEMU并等待调试器连接：

```bash
make debug
```

启动GDB：

```bash
riscv64-elf-gdb
```

进入调试器后，使用命令`b main`即可在main()设置断点，用`c`命令即可继续执行到断点。其他常见GDB命令示例：

```plaintext
# 运行到当前文件第25行
until 25
# 在start.c:25处设置断点
b src/kernel/boot/start.c:25
# 在当前文件25行设置临时断点（到达后自动删除）
tb 25
# 单步执行（进入函数）
step
# 单步执行一条汇编指令（进入函数）
stepi
# 单步执行（跳过函数）
next
# 单步执行一条汇编指令（跳过函数）
next
```

可以给printf()编写一个简易占位实现，从而在主函数中测试串口是否能正常打印。

### 3.3 实现自旋锁

自旋锁是一种最简单的锁，可以确保共享资源在同一时刻只被一个CPU核使用，自旋锁的实现要点：

- 获取锁后要关中断，防止自旋等待被中断
- 要用原子操作获取和释放锁，避免多个核心同时获得锁

具体自旋锁实现说明见`src/kernel/lock/spinlock.c`。

### 3.4 实现printf函数

printf函数用来向串口打印格式化字符串，串口属于共享资源，需要用锁来保证同一时刻只有一个CPU核在打印字符。

具体printf实现见`src/kernel/lib/print.c`。

## 4. 课后实验

这里有两个额外的实验帮助你理解锁的用处

### 4.1 并行加法

```c
    volatile static int started = 0;

    volatile static int sum = 0;

    int main()
    {
        int cpuid = r_tp();
        if(cpuid == 0) {
            print_init();
            printf("cpu %d is booting!\n", cpuid);        
            __sync_synchronize();
            started = 1;
            for(int i = 0; i < 1000000; i++)
                sum++;
            printf("cpu %d report: sum = %d\n", cpuid, sum);
        } else {
            while(started == 0);
            __sync_synchronize();
            printf("cpu %d is booting!\n", cpuid);
            for(int i = 0; i < 1000000; i++)
                sum++;
            printf("cpu %d report: sum = %d\n", cpuid, sum);
        }   
        while (1);    
    }  
```

在 **main.c** 中测试上述代码，很明显，我们的预期是后report的cpu应该告诉我们 `sum = 2000000`

但是实际结果可能是这样的  

```
cpu 0 is booting!
cpu 1 is booting!
cpu 0 report: sum = 1128497
cpu 1 report: sum = 1143332
```

考虑如何使用锁进行修正，修正后的输出可能是这样的  

```
cpu 0 is booting!
cpu 1 is booting!
cpu 0 report: sum = 1996573
cpu 1 report: sum = 2000000
```

简单说明上锁和解锁的位置不同会有什么影响（tips: 锁的粒度粗细）

### 4.2 并行输出  

尝试去掉`printf`里的锁，参考4.1的实验思路，设计测试方法使得`printf`的输出出现交错的情况  

4.1和4.2的测试代码和实验结果可以附在你的README中, 但是不要体现在你的代码里

## 5. 关于代码仓库的维护

1. 每次实验需要在上次实验的基础上继续往下做，假设你已经完成lab-0(master)

    那么你此时应该在lab-0(master)分支下使用`git checkout -b lab-1`命令创建并切换到新的分支lab-1  

    此时新建的lab-1会继承lab-0(master)的内容，但你对lab-1的修改不会影响到lab-0  

    以此类推，当你从lab-1开始走到lab-9时，你会获得越来越完整和强大的内核  

2. 你的代码仓库应该由 **代码 + Markdown文档** 两部分构成  

    文档内容不做明确要求，你有很高的自由度决定写什么和写多少

    提供一些建议: 
    
    - 本次实验新增了哪些功能，实现了什么效果

    - 对本次实验中某个过程的理解和思考

    - 本次实验和之前的实验构成什么样的逻辑联系

    - 本次实验花费的时间, 你和队友的贡献分别是什么

    - 可以使用markdown的分层分点来增加条理性，便于别人阅读和抓住重点

    **总之，这是你的代码仓库，请对你自己的代码和文档负责**  
    
    **注意，代码是继承和连续发展的, 但文档不是，每次的文档都是全新一页**  

3. 提醒: 之所以要求大家维护代码仓库，是为了查看大家的提交记录

    所以请及时同步当天写的代码到线上仓库，不要攒到最后一口气提交，否则可能被误判为不当行为
