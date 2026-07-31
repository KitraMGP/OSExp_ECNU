# LAB-5：系统调用流程与用户态虚拟内存管理

## 实验目标

LAB-4 创建了第一个用户进程 `proczero`，并打通了 U-mode、S-mode 和简单系统调用之间的往返路径。本实验将在此基础上完善系统调用分派和用户地址空间管理，使用户程序能够传递参数并管理堆、栈和离散映射区域。

本实验的主要目标包括：

- 建立基于系统调用表的统一分派流程。
- 实现用户空间与内核空间之间的数据复制。
- 通过 `brk` 手动调整用户堆，并通过缺页异常自动扩展用户栈。
- 建立 `mmap_region` 节点仓库，实现 `mmap` 和 `munmap`。
- 实现用户页表的复制与销毁，为 LAB-6 的多进程支持做准备。

## 代码组织结构

本实验主要新增或修改以下模块：

```text
OSExp_ECNU
├── Makefile                         增加 syscall 模块的构建目录
└── src
    ├── kernel
    │   ├── mem
    │   │   ├── uvm.c               用户态虚拟内存管理主体
    │   │   ├── mmap.c              mmap_region 节点仓库
    │   │   ├── method.h            用户内存管理接口
    │   │   └── type.h              mmap 区域及地址边界定义
    │   ├── proc
    │   │   ├── proc.c              初始化进程 mmap 链表
    │   │   └── type.h              在 proc_t 中记录 mmap 区域
    │   ├── syscall
    │   │   ├── syscall.c           系统调用分派与参数读取
    │   │   ├── sysfunc.c           各系统调用的服务逻辑
    │   │   ├── method.h
    │   │   ├── mod.h
    │   │   └── type.h              内核系统调用编号
    │   └── trap
    │       ├── trap_user.c         系统调用和用户缺页异常入口
    │       └── mod.h
    └── user
        ├── initcode.c              按测试需求设置的用户程序
        └── syscall_num.h           用户侧系统调用编号
```

## 原理说明

### 1. 系统调用分派与参数传递

用户程序通过 `ecall` 进入内核。按照本实验采用的 RISC-V 调用约定，`a7` 保存系统调用号，`a0` 到 `a5` 保存最多六个参数，返回值写回 `a0`。`user_vector` 已经将这些寄存器保存到当前进程的 `trapframe`，因此内核可以从 `proc->tf` 读取调用号和参数。

系统调用入口不再直接处理某一个固定服务，而是通过系统调用表完成分派：

```text
U-mode ecall
    -> user_vector
    -> trap_user_handler
    -> syscall
    -> sys_xxx
    -> trap_user_return
```

`arg_uint32()` 和 `arg_uint64()` 用于读取按值传递的参数；指针参数只是用户虚拟地址，内核切换页表后不能直接解引用，必须通过用户页表找到对应物理页后再复制数据。

### 2. 用户空间与内核空间的数据复制

`uvm_copyin()` 将用户数据复制到内核，`uvm_copyout()` 将内核数据复制到用户，`uvm_copyin_str()` 则复制以 `\0` 结尾的用户字符串。

复制过程不能假设起始地址或长度按页对齐。每轮需要完成以下步骤：

1. 使用 `vm_getpte()` 查询当前用户虚拟地址对应的页表项。
2. 检查页表项有效性以及用户访问权限。
3. 计算当前物理页内可连续复制的字节数。
4. 完成本页复制后推进源地址、目标地址和剩余长度。
5. 跨页时重新查询下一页，直到全部完成或字符串遇到 `\0`。

这三个函数是所有带用户指针参数的系统调用的基础，必须阻止未映射地址、内核保留地址和越界字符串被当作有效输入。

### 3. 用户堆的手动管理

用户堆从低地址向高地址增长，`heap_top` 记录当前堆顶。`sys_brk(new_heap_top)` 根据新旧堆顶关系执行不同操作：

- 参数为 `0`：查询当前堆顶。
- 新堆顶更高：申请用户物理页并建立映射。
- 新堆顶更低：解除不再使用的整页映射并回收物理页。
- 新旧堆顶相同：不修改页表。

堆顶不一定按页对齐，因此映射变化应以覆盖堆区间所需的页面为单位。堆不能增长到 `MMAP_BEGIN` 及其上方，否则会与离散映射区重叠。

### 4. 用户栈的自动扩展

用户栈从高地址向低地址增长。进程启动时只映射一个栈页，后续访问未映射的栈地址会触发 Load Page Fault 或 Store/AMO Page Fault，对应异常编号 13 和 15。

`trap_user_handler()` 读取 `stval` 中的故障地址，并调用 `uvm_ustack_grow()` 判断这次缺页是否属于合法栈扩展。合法时申请从当前栈底到故障页所缺少的全部页面，更新用户页表和 `ustack_npage`；非法地址则不能作为栈增长处理。

栈最多扩展到 `MMAP_END`，不能进入 mmap 地址区。本实验只实现增长，不回收已经分配的用户栈页。

### 5. mmap 地址区域

堆和栈之间保留 `[MMAP_BEGIN, MMAP_END)` 作为离散映射区域。每个已分配区间由一个 `mmap_region_t` 描述：

```c
typedef struct mmap_region
{
    uint64 begin;
    uint32 npages;
    struct mmap_region *next;
} mmap_region_t;
```

进程通过有序单链表记录已分配的 mmap 区间。保持链表按起始地址递增，可以简化空闲区间查找、重叠检查、相邻区间合并以及 `munmap` 时的删除和拆分。

当 `sys_mmap(begin, len)` 的 `begin` 为 `0` 时，内核从 `MMAP_BEGIN` 开始查找第一个足够大的空闲区间；指定非零地址时，则需要检查页对齐、边界和已有映射是否冲突。映射成功后再申请物理页并修改用户页表。

`sys_munmap(begin, len)` 可能完整删除一个节点、截短节点头部或尾部，也可能从节点中间移除区间并拆成两个节点。操作链表和页表时应保证两者描述的地址空间一致。

### 6. mmap_region 节点仓库

`mmap_region_t` 本身也是有限内核资源。本实验预先分配 `N_MMAP` 个 `mmap_region_node_t`，用空闲单链表构成节点仓库：

```c
typedef struct mmap_region_node
{
    mmap_region_t mmap;
    struct mmap_region_node *next;
} mmap_region_node_t;
```

`mmap_region_alloc()` 从仓库取出节点，`mmap_region_free()` 将节点归还。多个 CPU 可能同时操作仓库，因此空闲链表必须由自旋锁保护，且节点归还前应清理其中的区间状态和链表指针。

### 7. 用户页表的复制与销毁

`uvm_copy_pgtbl()` 为新进程复制旧进程的用户地址空间。不同区域的复制范围由进程元数据确定：

- 代码、数据和堆由 `USER_BASE` 与 `heap_top` 确定。
- mmap 区域由有序的 `mmap_region` 链表确定。
- 用户栈由 `ustack_npage` 和 `TRAPFRAME` 确定。
- `trapframe` 由进程单独创建，`trampoline` 为所有进程共享，因此不在普通用户页复制范围内。

每个有效用户页都需要申请新的物理页、复制原页内容，并在新页表中保留原页权限。复制后的两个进程不能共享普通可写用户物理页。

`uvm_destroy_pgtbl()` 需要递归遍历多级页表。叶子页表项指向用户物理页，应释放其物理页；非叶子页表项指向下一级页表，应先递归销毁子页表，再释放页表页。共享的 trampoline 映射只解除映射，不能释放其物理页。

## 具体工作内容

### 1. 建立系统调用流程和数据迁移接口

- 在 `trap_user_handler()` 中将用户 `ecall` 转交给统一的 `syscall()` 分派函数。
- 实现系统调用表和 `arg_uint32()`、`arg_uint64()`、`arg_str()`。
- 实现 `uvm_copyin()`、`uvm_copyout()` 和 `uvm_copyin_str()`。
- 实现仅用于本实验验证的数据复制系统调用 `sys_copyin()`、`sys_copyout()` 和 `sys_copyinstr()`。

### 2. 实现堆伸缩和栈缺页扩展

- 实现 `sys_brk()`、`uvm_heap_grow()` 和 `uvm_heap_ungrow()`。
- 在用户 trap 中识别异常 13 和 15，并调用 `uvm_ustack_grow()`。
- 检查堆、mmap 区域和栈之间的地址边界。
- 正确维护 `heap_top` 和 `ustack_npage`。

### 3. 实现 mmap_region 节点仓库

- 实现 `mmap_init()`，将全部节点组织为空闲链表。
- 实现加锁的 `mmap_region_alloc()` 和 `mmap_region_free()`。
- 通过多核并发申请和释放验证仓库不会丢失或重复分配节点。

### 4. 实现 mmap 和 munmap

- 在 `proc_t` 中记录 mmap 链表，并在进程创建时初始化。
- 实现自动地址查找、指定地址映射和重叠检查。
- 在插入后合并相邻节点，减少节点仓库消耗。
- 实现完整删除、首尾裁剪和中间拆分等解除映射情形。
- 通过 `sys_mmap()` 和 `sys_munmap()` 接入用户系统调用。

### 5. 实现用户页表复制与销毁

- 实现递归页表销毁，区分共享映射和进程私有页面。
- 按代码堆区、mmap 区和用户栈分别复制用户页面。
- 验证复制后页面内容与权限一致，且新旧进程使用不同物理页。

## 已完成的工作

### 任务 1：系统调用流程和用户/内核数据迁移

- 已接入统一的系统调用分派：用户态 `ecall` 在 `trap_user_handler()` 中推进 `sepc` 后调用 `syscall()`；分派表、`arg_uint32()`、`arg_uint64()` 和 `arg_str()` 使用 `trapframe` 中的 `a0`--`a7`。
- 已实现逐页检查权限并处理跨页范围的 `uvm_copyin()`、`uvm_copyout()` 与 `uvm_copyin_str()`。
- 已实现本实验使用的 `sys_copyin()`、`sys_copyout()` 和 `sys_copyinstr()` 测试服务。

### 任务 2：堆伸缩和栈缺页扩展

- `sys_brk()` 支持查询、增长、收缩和保持堆顶不变；失败时返回 `-1`，成功后同步更新 `proc->heap_top`。
- `uvm_heap_grow()` 和 `uvm_heap_ungrow()` 以覆盖半开字节区间所需的页面为单位增删映射，正确处理非页对齐堆顶，并限制堆顶不超过 `MMAP_BEGIN`。
- 用户 trap 已处理异常 13 和 15。`uvm_ustack_grow()` 根据 `stval` 一次映射故障页到旧栈底之间的全部页面，成功后更新 `proc->ustack_npage`，并拒绝低于 `MMAP_END` 的地址。

### 任务 3：mmap_region 节点仓库

- `mmap_init()` 初始化全部 `N_MMAP` 个节点及仓库自旋锁，并在启动 CPU 完成物理内存初始化后调用。
- `mmap_region_alloc()` 和 `mmap_region_free()` 在锁内摘取或归还节点；分配和释放时清理节点状态。
- 释放接口检查节点地址、对齐及重复释放，避免破坏空闲链表。
- 使用两个 CPU 各并发申请、归还 128 个节点，归还后重新取得了 256 个互不重复节点。

### 任务 4：mmap 与 munmap

- 首进程初始化空 mmap 链表；`sys_mmap()` 和 `sys_munmap()` 已接入系统调用表并校验页对齐和长度。
- `uvm_mmap()` 支持指定地址和 `begin == 0` 时的首个适配查找，拒绝越界及重叠请求，并合并前后相邻区域。
- `uvm_munmap()` 支持完整删除、头部裁剪、尾部裁剪和中间拆分，同时释放对应用户物理页。
- 未对齐、越界、重叠或未映射的用户请求返回 `-1`，不会作为用户输入错误触发内核 panic。

### 任务 5：用户页表复制与销毁

- `destroy_pgtbl()` 递归遍历三级 SV39 页表：非叶子项递归销毁下级页表，叶子项释放进程私有用户页，最后释放当前页表页。
- `uvm_destroy_pgtbl()` 先释放进程独有的 trapframe 映射并仅解除共享 trampoline 映射，再递归回收其余用户页和所有页表页。
- `uvm_copy_pgtbl()` 根据进程元数据分别深拷贝代码/数据/堆、每段 mmap 和用户栈；新页保留原 PTE 权限，但使用不同物理页。
- 复制入口验证堆栈边界、mmap 链表顺序和区间范围，避免错误元数据导致重复复制或越界。

## 测试用例与实际结果

### 1. 用户态与内核态数据迁移

先前使用用户数组和字符串运行测试，实际输出为：

测试代码：

```c
int L[5];
char *s = "hello, world";
syscall(SYS_copyout, L);
syscall(SYS_copyin, L, 5);
syscall(SYS_copyinstr, s);
```

```text
cpu 0 is booting!
get a number from user: 1
get a number from user: 2
get a number from user: 3
get a number from user: 4
get a number from user: 5
get string for user: hello, world
```

### 2. 堆和栈管理

用户测试先查询堆顶，将堆扩展九页加 123 字节，在第九个新增页写入 `heap`；随后收缩五页，在仍保留的页面写入 `kept`。测试还使用 16 KiB 局部数组分别访问高端和低端地址，触发一次跨多个页面的栈扩展。实际输出为：

用户测试的关键代码：

```c
char *heap = (char *)syscall(SYS_brk, 0);
char stack[PGSIZE * 4];

syscall(SYS_brk, heap + 9 * PGSIZE + 123);
syscall(SYS_brk, heap + 9 * PGSIZE + 123); // 堆顶不变
heap[8 * PGSIZE] = 'h';                    // 访问新增堆页
syscall(SYS_brk, heap + 4 * PGSIZE + 123);
heap[3 * PGSIZE] = 'k';                    // 访问收缩后保留页

stack[3 * PGSIZE] = 'd';                   // 触发第一次栈增长
stack[0] = 's';                             // 一次跨多个页面增长
```

```text
cpu 0 is booting!
brk lookup: heap_top = 0x0000000000002000
brk grow: heap_top = 0x0000000000002000 -> 0x000000000000b07b
brk unchanged: heap_top = 0x000000000000b07b
get string for user: heap
brk ungrow: heap_top = 0x000000000000b07b -> 0x000000000000607b
get string for user: kept
user page fault: trap_id = 15, stval = 0x0000003fffffcff0
user stack pages: 1 -> 2
get string for user: deep
user page fault: trap_id = 15, stval = 0x0000003fffff9ff0
user stack pages: 2 -> 5
get string for user: stack
```

### 3. mmap_region 节点仓库

临时内核测试让两个 CPU 各申请一半节点，并发归还后由 CPU 0 重新申请全部节点并逐一检查地址唯一性。实际输出为：

临时双核测试的核心同步和链表操作代码：

```c
for (int i = cpuid * (N_MMAP / 2);
     i < (cpuid + 1) * (N_MMAP / 2); i++)
    nodes[i] = mmap_region_alloc();
allocated[cpuid] = true;
while (!allocated[0] || !allocated[1]);

for (int i = cpuid * (N_MMAP / 2);
     i < (cpuid + 1) * (N_MMAP / 2); i++)
    mmap_region_free(nodes[i]);
```

```text
cpu 0 is booting!
cpu 1 is booting!
mmap node concurrency: 256 unique nodes recovered
```

验证完成后已恢复正常的 `main()` 启动路径，未保留测试屏障和临时数组。

### 4. mmap 与 munmap

当前 `src/user/initcode.c` 使用乱序指定地址建立映射，使节点发生前向、后向和两侧合并；随后使用 `begin == 0` 验证首个适配地址，并覆盖完整删除、头部裁剪、尾部裁剪和中间拆分。测试还确认重叠、未对齐、越界及未映射请求返回 `-1`。全部释放后重新自动映射四页，实际写入并读回字符串：

当前 `src/user/initcode.c` 中的核心测试代码：

```c
syscall(SYS_mmap, MMAP_BEGIN + 4 * PGSIZE, 3 * PGSIZE);
syscall(SYS_mmap, MMAP_BEGIN + 10 * PGSIZE, 2 * PGSIZE);
syscall(SYS_mmap, MMAP_BEGIN + 2 * PGSIZE, 2 * PGSIZE);
syscall(SYS_mmap, MMAP_BEGIN + 12 * PGSIZE, PGSIZE);
syscall(SYS_mmap, MMAP_BEGIN + 7 * PGSIZE, 3 * PGSIZE);
syscall(SYS_mmap, MMAP_BEGIN, 2 * PGSIZE);
char *auto_map = (char *)syscall(SYS_mmap, 0, 10 * PGSIZE);

// 重叠、未对齐、越界和未映射请求必须失败。
if (syscall(SYS_mmap, MMAP_BEGIN + 4 * PGSIZE, PGSIZE) != -1 ||
    syscall(SYS_mmap, MMAP_BEGIN + 1, PGSIZE) != -1 ||
    syscall(SYS_mmap, MMAP_END, PGSIZE) != -1 ||
    syscall(SYS_munmap, MMAP_BEGIN + 23 * PGSIZE, PGSIZE) != -1)
    while (1);

// 覆盖中间拆分、完整删除、头部裁剪和尾部裁剪。
syscall(SYS_munmap, MMAP_BEGIN + 10 * PGSIZE, 5 * PGSIZE);
syscall(SYS_munmap, MMAP_BEGIN, 10 * PGSIZE);
syscall(SYS_munmap, MMAP_BEGIN + 17 * PGSIZE, 2 * PGSIZE);
syscall(SYS_munmap, MMAP_BEGIN + 15 * PGSIZE, 2 * PGSIZE);
```

代表性的实际输出如下；每次合法操作后还会通过 `vm_print()` 输出对应页表，以下只保留便于核对链表变化的行：

```text
cpu 0 is booting!
mmap: begin = 0x0000003ffb002000, len = 12288
alloced mmap_region: 0x0000003ffb002000 ~ 0x0000003ffb005000
mmap: begin = 0x0000003ffb005000, len = 12288
alloced mmap_region: 0x0000003ffb000000 ~ 0x0000003ffb00b000
munmap: begin = 0x0000003ffb008000, len = 20480
alloced mmap_region: 0x0000003ffaffe000 ~ 0x0000003ffb008000
alloced mmap_region: 0x0000003ffb00d000 ~ 0x0000003ffb015000
mmap: begin = 0x0000003ffaffe000, len = 16384
get string for user: mmap
munmap: begin = 0x0000003ffaffe000, len = 16384
```

该输出只有在自动映射地址、非法请求返回值、解除映射各分支及最终用户页读写全部通过后才会出现。

### 5. 用户页表复制与销毁

内核测试在首进程启动前构造两套独立页表。旧地址空间包含三页代码/堆、两段共三页 mmap 和三页用户栈；新旧页表各自还有独立 trapframe，并共享 trampoline。测试逐页检查：

- 内容逐字节一致；
- PTE 权限完全一致；
- 新旧虚拟页对应不同物理页；
- 修改副本后原页内容不变；
- 两套页表销毁后，全部用户页和三级页表页可从对应物理页池重新分配。

内核测试的关键断言代码：

```c
uvm_copy_pgtbl(old, new, USER_BASE + 3 * PGSIZE, 3, &mmap_1);
for (uint32 i = 0; i < sizeof(vas) / sizeof(vas[0]); i++) {
    pte_t *old_pte = vm_getpte(old, vas[i], false);
    pte_t *new_pte = vm_getpte(new, vas[i], false);
    assert(PTE_FLAGS(*old_pte) == PTE_FLAGS(*new_pte),
           "pgtbl test: permissions changed.");
    assert(PTE_TO_PA(*old_pte) != PTE_TO_PA(*new_pte),
           "pgtbl test: user page shared.");
}
uvm_destroy_pgtbl(old);
uvm_destroy_pgtbl(new);
test_expect_recycled(test_user_pages, test_user_npage, false);
test_expect_recycled(test_pgtbl_pages, test_pgtbl_npage, true);
```

实际输出为：

```text
cpu 0 is booting!
pgtbl copy: content permissions isolation pass
pgtbl destroy: 20 user pages and 14 table pages recycled
get string for user: mmap
```

其中 20 个用户页包括两套地址空间各自的 9 个普通用户页和 1 个 trapframe；14 个内核页包括两套地址空间实际建立的全部三级页表页。最后一行说明销毁测试结束后，原有 mmap 用户场景仍能正常启动和运行。

## 测试结果

- `make build` 成功完成用户程序、内核对象和内核 ELF 的编译链接。
- 堆和栈测试在 QEMU 双核配置下通过。
- mmap 节点仓库双核并发测试通过，归还后 256 个节点全部可重新分配且地址唯一。
- mmap/munmap 综合测试和非法参数测试在 QEMU 下通过。
- 页表深拷贝测试通过：内容、权限和修改隔离符合预期，新旧普通用户页没有共享物理页。
- 页表销毁测试通过：20 个用户页和 14 个三级页表页全部回收到对应物理页池，共享 trampoline 未被释放。

## 总结

LAB-5 已完成统一系统调用与数据迁移、用户堆伸缩、缺页驱动的用户栈扩展、带锁 mmap 节点仓库、mmap/munmap，以及用户地址空间的深拷贝与递归销毁。系统已具备 LAB-6 创建、复制和回收多进程地址空间所需的内存管理基础。
