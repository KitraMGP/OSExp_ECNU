# LAB-1: 机器启动

## 1. 代码组织结构

```plaintext
ECNU-OSLAB-2025-TASK
├── LICENSE        开源协议
├── .vscode        配置了可视化调试环境
├── registers.xml  配置了可视化调试环境
├── common.mk      Makefile中一些工具链的定义
├── Makefile       编译运行整个项目 (CHANGE, 增加trap和mem目录作为target)
├── kernel.ld      定义了内核程序在链接时的布局 (CHANGE, 增加一些关键位置的标记)
├── pictures       README使用的图片目录 (CHANGE, 日常更新)
├── README.md      实验指导书 (CHANGE, 日常更新)
└── src            源码
    └── kernel     内核源码
        ├── arch   RISC-V相关
        │   ├── method.h
        │   ├── mod.h
        │   └── type.h
        ├── boot   机器启动
        │   ├── entry.S
        │   └── start.c
        ├── lock   锁机制
        │   ├── spinlock.c
        │   ├── method.h
        │   ├── mod.h
        │   └── type.h
        ├── lib    常用库
        │   ├── cpu.c
        │   ├── print.c
        │   ├── uart.c
        │   ├── utils.c (NEW, 工具函数)
        │   ├── method.h (CHANGE, utils.c的函数声明)
        │   ├── mod.h
        │   └── type.h
        ├── mem    内存模块
        │   ├── pmem.c (TODO, 物理内存管理)
        │   ├── kvm.c (TODO, 内核态虚拟内存管理)
        │   ├── method.h (NEW)
        │   ├── mod.h (NEW)
        │   └── type.h (NEW)
        ├── trap   陷阱模块
        │   ├── method.h (NEW)
        │   ├── mod.h (NEW)
        │   └── type.h (NEW, 增加CLINT和PLIC寄存器定义)
        └── main.c (TODO)
```

## 2. 实验核心目标

- 实现物理内存的分页管理、分配、回收
- 实现内核态虚拟内存管理

## 3. 具体工作

### 3.1 物理内存管理

#### 3.1.1 kernel.ld

首先需要关注`kernel.ld`文件，它规定了内核可执行文件`kernel-qemu.elf`在载入内存时的布局。

`kernel.ld`结构：

```
OUTPUT_ARCH("riscv")
ENTRY(_entry)

SECTIONS
{
  . = 0x80000000;       // 设定起始地址（加载地址）

  .text : { ... }       // .text 段
  .rodata : { ... }     // .rodata 段
  .data : { ... }       // .data 段
  .bss : { ... }        // .bss 段
}
```

这个脚本规定了：

- 起始地址：0x80000000，规定了所有段的虚拟地址从这里开始。

- 段的排列顺序：.text，.rodata，.data，.bss。
- 对齐要求：`. = ALIGN(16)`或`. = ALIGN(0x1000)`确保段/节的地址对齐。
- 导出符号：`PROVIDE(etext = .)`，`PROVIDE(ALLOC_BEGIN = .)`等，把这些地址作为符号提供给C/汇编代码使用。

布局信息被链接器写入到ELF文件的头部：

- Program Headers（程序头表/段表）：每个段（segment）的`p_vaddr`（虚拟地址）、`p_offset`（文件偏移）、`p_filesz/p_memsz`（大小）。这是加载器（QEMU、bootloader）用来将内核载入内存的元数据。
- Section Headers（节头表）：每个节（如.text、.data）的`sh_addr`、`sh_offset`等更细粒度的布局信息。

可用`readelf -l kernel-qemu.elf`看到实际写入ELF的Program Headers；可用`readelf -S kernel-qemu.elf`看到各个节的地址信息。

在本项目中，在最后一个段的后面添加以下代码，它导出了物理内存分配的起止地址：

```plaintext
. = ALIGN(4096);
PROVIDE(ALLOC_BEGIN = .);
PROVIDE(ALLOC_END = 0x80000000 + 128M);
```

编辑.text段，导出KERNEL_DATA符号：

```
.text : {
  *(.text .text.*)
  . = ALIGN(0x1000);
  /*
  _trampoline = .;
  *(trampsec)
  . = ALIGN(0x1000);
  ASSERT(. - _trampoline == 0x1000, "error: trampoline larger than one page");
  */
  PROVIDE(KERNEL_DATA = .);
}
```

#### 3.1.2 物理内存分页

##### 物理地址划分

物理地址按照地址空间分为三个部分：

- 0x80000000 ~ KERNEL_DATA存放了**kernel-qemu.elf的代码**
- KERNEL_DATA ~ ALLOC_BEGIN存放了**kernel-qemu.elf的数据**
- ALLOC_BEGIN ~ ALLOC_END属于**未使用的可分配的物理页**

前两个区域被内核持续占用，不可被动态分配和回收，只有第三部分需要我们管理。

##### 页面管理模式

本内核中对物理内存的管理模式是：**4KB物理页切分 + 空闲链表组织**。

ALLOC_BEGIN ~ ALLOC_END 这块物理空间被切分成若干个4KB内存页（不会有剩余）。

为了实现内核空间与用户空间的隔离，我们设置了两个alloc_regiion，基于KERNEL_PAGE进行边界划分。

`kernel_region`记录了内核空间的空闲物理页情况，`user_region`记录了用户空间的空闲物理页情况，

`alloc_region`描述了一组空闲页链表，包含起止位置、空闲页面数量、链表头节点、保证一致性的锁。

```c
// 物理页节点
typedef struct page_node
{
    struct page_node *next;
} page_node_t;

// 许多物理页构成一个可分配的区域
typedef struct alloc_region
{
    uint64 begin;          // 起始物理地址
    uint64 end;            // 终止物理地址
    spinlock_t lk;         // 自旋锁(保护下面两个变量)
    uint32 allocable;      // 可分配页面数
    page_node_t list_head; // 可分配链的链头节点
} alloc_region_t;
```

和用数组管理页表相比，用链表管理空闲物理页可以快速找到空闲页面，无需遍历，提高内存分配速度。

下面的图片展示了用链表组织空闲物理内存页的原理：

![链表管理物理页示意图](img/img1.jpg)

##### 物理内存管理功能实现

实现`pmem.c`中的`pmem_init()`方法初始化物理内存，其功能是填写`kern_region`和`user_region`的内容，包含数值和链表内容。

在`src/kernel/mem/type.h`中定义了`KERN_PAGES`宏和`PGSIZE`宏分别表示内核使用可分配空间开头多少个页面，以及页面的大小。

这个链表的构建方法较为特殊。本链表直接使用每个空闲页面的前64位存储next。具体构建方法见后面的代码。

`pmem_init()`实现：

```c
// 物理内存的初始化
// 本质上就是填写kern_region和user_region, 包括基本数值和空闲链表
void pmem_init(void)
{
    // 内核可分配区域：可分配 KERN_PAGES 个页面
    spinlock_init(&kern_region.lk, "kern_region");
    kern_region.begin = (uint64)ALLOC_BEGIN;
    kern_region.end = (uint64)ALLOC_BEGIN + KERN_PAGES * PGSIZE;
    kern_region.allocable = KERN_PAGES;
    // 构建链表。本链表利用每个空闲页面的前 64 位存储 next。
    kern_region.list_head.next = (page_node_t *)kern_region.begin;
    for (uint64 pg = kern_region.begin; pg < kern_region.end; pg += PGSIZE)
    {
        page_node_t* node = (page_node_t*)pg;
        node->next = (pg + PGSIZE < kern_region.end) ? (page_node_t*)(pg + PGSIZE) : NULL;
    }

    // 用户可分配区域：可分配剩下所有页面
    spinlock_init(&user_region.lk, "user_region");
    user_region.begin = kern_region.end;
    user_region.end = (uint64)ALLOC_END;
    user_region.allocable = (user_region.end - user_region.begin) / PGSIZE;
    // 构建链表。和上面相同
    user_region.list_head.next = (page_node_t*)user_region.begin;
    for (uint64 pg = user_region.begin; pg < user_region.end; pg += PGSIZE)
    {
        page_node_t* node = (page_node_t*)pg;
        node->next = (pg + PGSIZE < user_region.end) ? (page_node_t*)(pg + PGSIZE) : NULL;
    }
}
```

实现`pmem_alloc()`和`pmem_free()`，实现对物理页的分配和释放功能。

##### 测试

所有的测试用例均测试通过。

#### 3.1.3 上一节实现过程中遇到的问题

运行第一组测试用例的时候，在释放已分配的块的时候出现了`panic: spinlock_acquire`错误。调试发现，

GDB调试信息：

```
(gdb) b src/kernel/lock/spinlock.c:56
Breakpoint 1 at 0x80000818: file src/kernel/lock/spinlock.c, line 56.
(gdb) c
Continuing.

Thread 1 hit Breakpoint 1, spinlock_acquire (lk=lk@entry=0x80005398 <kern_region+16>) at src/kernel/lock/spinlock.c:56
56              panic("spinlock_acquire");
(gdb) p lk->name
$1 = 0x800011d8 "kern_region"
(gdb) bt
#0  spinlock_acquire (lk=lk@entry=0x80005398 <kern_region+16>) at src/kernel/lock/spinlock.c:56
#1  0x0000000080000bdc in pmem_free (page=2147532800, in_kernel=in_kernel@entry=true) at src/kernel/mem/pmem.c:81
#2  0x00000000800001dc in main () at src/kernel/main.c:32
```

研究后发现，问题出在spinlock的实现中存在竞争问题，`spinlock_t.locked`在锁没有被占用和被CPU0占用时均取值为0，在并发执行时，`spinlock_acquire()`函数中的`spinlock_holding()`调用会错误地判断本核心已持有锁，产生报错。

出错时的具体操作序列：

```
CPU1: 进入spinlock_release(&lk);
CPU1: lk->cpuid = 0; // 清零 cpuid
CPU0: 进入spinlock_acquire(&lk);
CPU0: 进入spinlock_holding(&lk);
CPU0: r = (lk->locked && lk->cpuid == mycpuid());
// 此时 lk->locked == 1，lk->cpuid == 0，函数认为锁被 CPU0 持有，触发 panic
// 但实际上锁被 CPU1 持有（正在释放），出现了误判
```

要解决这个bug，就要避免清零cpuid后被误判成CPU0持有锁，方法是让特殊值-1代表无人持有锁，释放锁的时候执行`lk->cpuid = -1;`，`spinlock_holding()`不会出现误判。此外，自旋锁初始化函数`spinlock_init`中也一致地将`lk->cpuid`设为-1。

### 3.2 内核态虚拟内存

由于应用程序要求能独占整个地址空间，并且各个程序持有的内存需要有记录，因此需要有虚拟内存管理。

实现基于内核态的虚拟内存管理，使用三级页表管理。

#### 3.2.1 内核态虚拟内存：页表+页表项

在RIST-V体系结构中，要建立页表并通过MMU自动完成翻译，需要遵循SV39规范，即39bit虚拟地址的虚拟内存，该规范在`src/kernel/mem/type.h`的注释中介绍。

```c
/*
    内核使用RISC-V体系结构中的SV39作为虚拟内存的设计规范

    1. 页表与satp寄存器
    
    satp寄存器的bit结构: MODE(4bit) + ASID(16bit) + PPN(44bit)
    - MODE控制虚拟内存模式
    - ASID与Flash刷新有关
    - PPN存放页表基地址
    可以通过w_satp(MAKE_SATP(pgtbl))命令将页表地址填入satp寄存器并启动地址翻译
    在无页表到有页表 + 切换页表的情况下会用到

    2. SV39的虚拟地址与三级映射表

    物理页是最基本的内存资源, 有的物理页用于存放数据, 有的物理页用于存放描述"物理页映射关系"的页表
    VA: VPN[2] + VPN[1] + VPN[0] + offset
          9    +   9    +   9    +   12    = 39 (使用uint64存储) => 最大虚拟地址为512GB
    SV39使用三级页表对应三级VPN, VPN[2]称为顶级页表、VPN[1]称为次级页表、VPN[0]称为低级页表
    为什么每一级页框号是"9": 4KB/sizeof(PTE) = 512 = 2^9 所以一个物理页可以存放512个页表项
    生活中的例子: 假设你要在全国范围内找一个不认识的大学老师, 
    - 你可以先到教育部(顶级页表)查询, 得知这个老师属于大学A
    - 你接着来到大学A(次级页表)查询, 得知这个老师属于学院B
    - 你最后来到学院B(低级页表)查询, 得知这个老师属于办公室C
    - 最后你来到办公室C(物理页), 并在某个位置(offset)上找到了他

    3. 页表的基本组成单位-页表项(PTE)
    reserved + PPN[2] + PPN[1] + PPN[0] + RSW + D A G U X W R V  共64bit
       10        26       9        9       2    1 1 1 1 1 1 1 1
    需要关注的部分:
    - V : valid
    - X W R : execute write read (全0意味着这是页表所在的物理页)
    - U : 用户态是否可以访问
    - PPN区域 : 存放物理页号

*/
```

页表（pgtbl）由页表项（PTE）构成，一个页表项对应一个物理页，页表项主要由两部分组成：

- 它所管理的物理页的页号（PPN字段）
- 它所管理的物理页的标志位（低10bit）

页表本身也是存放在物理页中，这种物理页的特点是PTE的标志位中PTR_R PTE_W PTE_X都是0（不可读不可写不可执行）

下图显示了页表的示意图和实际状态：

![页表示意图](img/img2.jpg)

页表刚初始化的时候只是一个清空的4KB物理页， 随着mmap操作的增加，页表开始延伸出去，直到完全长成一个能管理512GB内存空间的树。

页表的三级组织结构：

- 顶级页表的PTE中有次级页表所在的物理页的物理页号和标志位
- 次级页表的PTE中有低级页表所在的物理页的物理页号和标志位
- 低级页表的PTE中有一般物理页的物理页号和标志位

##### 实现虚拟内存管理

需要依次实现`vm_getpte`、`vm_mappages`、`vm_unmappages`三个函数。

