# LAB-2：内存管理

本实验在 RISC-V64 QEMU `virt` 平台上实现物理页分配器和 Sv39 内核页表。实现参考 xv6-riscv，但按本项目的模块划分、函数签名和内存布局进行了适配。

## 1. 实验概览

### 1.1 已完成内容

| 模块 | 完成内容 | 核心入口 |
| --- | --- | --- |
| 链接布局 | 划分代码、数据和可分配 RAM，导出边界符号 | `kernel.ld` |
| 物理内存 | 双空闲链表、4 KiB 页分配/释放、分配清零 | `pmem_init`、`pmem_alloc`、`pmem_free` |
| 并发保护 | 内核池和用户池分别使用自旋锁保护 | `spinlock_acquire`、`spinlock_release` |
| 页表操作 | 三级页表遍历、映射、解映射和调试打印 | `vm_getpte`、`vm_mappages`、`vm_unmappages`、`vm_print` |
| 内核页表 | MMIO 与 RAM 恒等映射，代码页只读可执行 | `kvm_init` |
| 启用分页 | 每个 hart 写入 `satp` 并刷新本地 TLB | `kvm_inithart` |

当前阶段只实现内核共享页表。用户进程页表、页表销毁、缺页处理和 TLB shootdown 不在本实验范围内。

### 1.2 平台配置

| 配置 | 当前值 | 定义位置 |
| --- | --- | --- |
| 架构 | RISC-V 64-bit | `common.mk`、`kernel.ld` |
| QEMU 机器 | `virt`，无 BIOS | `Makefile` |
| RAM | 128 MiB | `Makefile` 中 `-m 128M` |
| hart 数量 | 2 | `CPUNUM` 和 `NCPU` |
| 页大小 | 4096 bytes | `PGSIZE` |
| 虚拟内存模式 | Sv39 | `SATP_SV39` |

`CPUNUM`、`NCPU`、QEMU RAM 大小和 `ALLOC_END`是相互关联的配置，修改时必须同步核对。

## 2. 快速开始

需要安装 `riscv64-elf-*` 交叉工具链和 `qemu-system-riscv64`。

```sh
make clean
make build
make run
```

QEMU 在 `-nographic` 模式下运行，按 `Ctrl-a x`退出。内核启动成功时会输出以下两行，顺序可能因 hart 调度而变化：

```text
cpu 0 is booting!
cpu 1 is booting!
```

构建产物位于 `target/kernel/kernel-qemu.elf`。常用检查命令：

```sh
riscv64-elf-readelf -l -S -s target/kernel/kernel-qemu.elf
riscv64-elf-nm -n target/kernel/kernel-qemu.elf
riscv64-elf-objdump -S target/kernel/kernel-qemu.elf
```

链接器目前会提示 ELF 存在 RWE LOAD segment。ELF 仍由一个 LOAD segment 装载，但启用 Sv39 后，页表会将内核代码映射为 `R|X`、其余 RAM 映射为 `R|W`。

## 3. 代码导航与启动流程

### 3.1 核心文件

| 文件 | 职责 |
| --- | --- |
| `kernel.ld` | 定义内核 section 顺序和 `KERNEL_DATA`、`ALLOC_BEGIN`、`ALLOC_END` |
| `src/kernel/mem/type.h` | 物理页、分配区域、页表、PTE 和地址转换宏 |
| `src/kernel/mem/method.h` | 内存模块公开接口 |
| `src/kernel/mem/pmem.c` | 内核池与用户池的初始化、分配和释放 |
| `src/kernel/mem/kvm.c` | 通用页表操作、内核映射和分页启用 |
| `src/kernel/lock/spinlock.c` | 分配器使用的自旋锁和中断嵌套控制 |
| `src/kernel/boot/entry.S` | 为每个 hart 选择启动栈并进入 C 代码 |
| `src/kernel/boot/start.c` | M-mode 初始化、PMP 配置和切换到 S-mode |
| `src/kernel/main.c` | hart 0 初始化共享资源，所有 hart 启用内核页表 |
| [`TESTING.md`](TESTING.md) | 阶段性测试代码、实际输出、预期失败和覆盖缺口 |

原理图：

- [空闲物理页链表](img/img1.jpg)
- [三级页表示意图](img/img2.jpg)

### 3.2 启动顺序

```text
QEMU
  -> _entry (M-mode, satp 尚未使用)
  -> 为每个 hart 设置 4 KiB 启动栈
  -> start
       - satp = 0，使用物理地址
       - hartid 保存到 tp
       - PMP 允许 S-mode 访问全部物理地址
       - mret 切换到 S-mode
  -> main
       hart 0: print_init -> pmem_init -> kvm_init -> started = 1
       其他 hart: 等待 started
       所有 hart: kvm_inithart -> printf
```

`started`前后的`__sync_synchronize()`保证其他 hart 在使用内核页表前能够看到 hart 0 完成的初始化写入。

## 4. 物理内存管理

### 4.1 物理地址与链接布局

QEMU `virt`物理地址空间不只有 RAM，还包含 MMIO：

| 物理地址范围 | 用途 |
| --- | --- |
| `0x02000000`起 | CLINT |
| `0x0c000000`起 | PLIC |
| `0x10000000`起 | UART |
| `[0x80000000, 0x88000000)` | 128 MiB RAM |

`kernel.ld`将 RAM 内的内核映像和可分配区域组织为：

```text
0x80000000 = KERNEL_BASE
    .text
KERNEL_DATA                 # .text 结束，4 KiB 对齐
    .rodata / .data / .bss
ALLOC_BEGIN                 # 内核映像结束，4 KiB 对齐
    内核页池：KERN_PAGES * PGSIZE = 1024 * 4 KiB = 4 MiB
    用户页池：剩余可分配 RAM
0x88000000 = ALLOC_END
```

`KERNEL_DATA`和`ALLOC_BEGIN`由链接结果决定，不应写死为固定地址。可通过`riscv64-elf-nm`查看当前值。

### 4.2 空闲页链表

物理内存按 4 KiB 切分。每个分配区域由以下信息描述：

```c
typedef struct alloc_region
{
    uint64 begin;
    uint64 end;
    spinlock_t lk;
    uint32 allocable;
    page_node_t list_head;
} alloc_region_t;
```

空闲页不需要额外的元数据数组：页面空闲时，其前 8 bytes 保存下一个空闲页的地址。初始化时链表按物理地址递增；释放采用头插法，因此后续重新分配表现为 LIFO。

两个池的用途如下：

- `kern_region`：`ALLOC_BEGIN`后的前 1024 页，用于根页表和中间页表等内核对象。
- `user_region`：其余 RAM，用于未来的用户页面。

这只是物理页配额划分，不是地址空间访问隔离。当前内核页表仍以 supervisor `R|W`权限恒等映射全部 RAM。

### 4.3 分配器接口

| 接口 | 契约 |
| --- | --- |
| `pmem_init()` | 初始化两个区域、锁、计数和空闲链表；仅由 hart 0 调用一次 |
| `pmem_alloc(true)` | 从内核池取一页，移出链表并清零；耗尽时 panic |
| `pmem_alloc(false)` | 从用户池取一页，移出链表并清零；耗尽时 panic |
| `pmem_free(page)` | 按地址判断所属池并头插回链表；非法区域地址会 panic |

两个池拥有独立自旋锁，所以同一池内的链表和计数更新是串行的，不同池可以并发操作。页面已经从空闲链表移除后才执行清零，因此清零过程不需要持锁。

### 4.4 分配器不变量

- `begin`和`end`必须页对齐，区域采用左闭右开区间。
- `ALLOC_BEGIN + KERN_PAGES * PGSIZE <= ALLOC_END`。
- 链表中的页面数应等于`allocable`。
- 已分配页面不能再次分配，已释放页面不能重复释放。
- 传给`pmem_free`的地址应是分配器返回的页首地址。

后两项目前依赖调用者保证，代码尚未维护位图或引用计数来检测重复释放和非页首地址。

## 5. Sv39 页表基础

### 5.1 虚拟地址拆分

Sv39 使用三级页表，每级索引 9 bits，页内偏移 12 bits：

```text
VA = VPN[2] (9) | VPN[1] (9) | VPN[0] (9) | offset (12)
       root          middle        leaf          4 KiB page
```

每张页表正好占一页，可容纳`4096 / 8 = 512`个 PTE。硬件 Sv39 具有低、高两个 canonical 地址区间；当前实现设置`VA_MAX = 1 << 38`，只接受低半区`[0, 256 GiB)`。

当前实现只使用 level-0 的 4 KiB 叶子项，不支持 level-1 的 2 MiB 页或 level-2 的 1 GiB 页。

### 5.2 PTE 状态

PTE 低位格式为：

```text
RSW | D A G U X W R V
```

| 条件 | 含义 |
| --- | --- |
| `V = 0` | 无效 PTE，其余位不参与地址翻译 |
| `V = 1`且`R/W/X = 0` | 非叶子 PTE，PPN 指向下一级页表 |
| `V = 1`且`R = 1`或`X = 1` | 合法叶子 PTE，PPN 指向物理页 |
| `V = 1`、`R = 0`且`W = 1` | RISC-V 保留的非法组合 |

常用转换：

```c
vpn = VA_TO_VPN(va, level);
pte = PA_TO_PTE(pa) | flags;
pa = PTE_TO_PA(pte);
flags = PTE_FLAGS(pte);
```

`PTE_CHECK(pte)`只检查`R/W/X`是否全零；必须先确认`PTE_V`，才能把结果解释为有效的非叶子项。RISC-V 将`W=1、R=0`保留为非法组合，实际映射应避免只设置`PTE_W`。

## 6. 页表操作

### 6.1 `vm_getpte`

从 level 2 开始，根据 VPN 逐级下降并返回 level-0 PTE 的地址：

- `alloc=false`：中间页表不存在时返回`NULL`。
- `alloc=true`：从内核页池分配清零页面，并写入`PA_TO_PTE(page) | PTE_V`。
- 遇到高层叶子项会触发断言，因为本实现不支持大页。
- `va >= VA_MAX`时 panic。

代码将 PTE 中的 PA 直接转换为可解引用指针。这在分页前依赖 Bare 模式，在分页后依赖内核对 RAM 的恒等映射。

### 6.2 `vm_mappages`

建立`[va, va + len)`到物理页的连续映射：

- `va`和`pa`必须页对齐，`len > 0`。
- `len`是字节数；末尾不足一页时仍映射完整页面。
- 范围不得超过`VA_MAX`，检查使用减法避免`va + len`溢出。
- 目标 PTE 已有效时触发重映射断言。
- 函数补充`PTE_V`，调用者提供`R/W/X/U`等权限。
- 函数本身不刷新 TLB，活动页表的调用者必须显式处理。

### 6.3 `vm_unmappages`

逐页清除`[va, va + len)`中的叶子 PTE：

- 区间中的每个页面都必须已经映射。
- `freeit=true`时同时将叶子物理页交还给物理分配器。
- 不会回收空的中间页表，也不会释放根页表。
- 不会刷新 TLB。

同一物理页存在多个映射时不能随意使用`freeit=true`，否则其他别名会指向已经释放并可能被复用的页面。

### 6.4 `vm_print`

递归打印有效的三级页表项，用于检查 VPN、PA 和 flags。它假定高两级只有非叶子项、level 0 只有叶子项。完整打印内核直接映射会产生大量输出，通常只应用于小型测试页表。

### 6.5 与 xv6 接口的差异

参考 xv6 代码时不能直接复制调用参数：

| 操作 | 本项目 | xv6-riscv |
| --- | --- | --- |
| 映射 | `vm_mappages(pgtbl, va, pa, len, perm)` | `mappages(pagetable, va, size, pa, perm)` |
| 解映射 | `vm_unmappages(pgtbl, va, len, freeit)`，`len`为 bytes | `uvmunmap(pagetable, va, npages, do_free)`，单位为页 |
| 物理页分配 | `pmem_alloc(in_kernel)`，耗尽时 panic | `kalloc()`，失败时返回`0` |

本项目的`va`和`pa`必须页对齐，但`len`允许不是页大小的整数倍。

## 7. 内核页表

### 7.1 恒等映射

`kvm_init()`先从内核池分配根页表，再建立 VA 等于 PA 的映射：

| 虚拟地址范围 | 物理地址范围 | 权限 | 用途 |
| --- | --- | --- | --- |
| `[UART_BASE, UART_BASE + 0x1000)` | 相同 | `R|W` | UART 寄存器 |
| `[CLINT_BASE, CLINT_BASE + 0x10000)` | 相同 | `R|W` | CLINT 寄存器 |
| `[PLIC_BASE, PLIC_BASE + 0x400000)` | 相同 | `R|W` | PLIC 寄存器 |
| `[KERNEL_BASE, KERNEL_DATA)` | 相同 | `R|X` | 内核 `.text` |
| `[KERNEL_DATA, ALLOC_BEGIN)` | 相同 | `R|W` | `.rodata/.data/.bss` |
| `[ALLOC_BEGIN, ALLOC_END)` | 相同 | `R|W` | 全部可分配 RAM |

当前映射全部使用 4 KiB 叶子页。页表开销为 1 个根页、MMIO 的 1 个 level-1 页与 4 个 level-0 页、RAM 的 1 个 level-1 页与 64 个 level-0 页，共 71 个内核页。因此`kvm_init()`后内核池剩余 953 页。

`.rodata`当前位于`KERNEL_DATA`之后，所以也被映射为可写；若后续需要更严格的 W^X/只读数据保护，应在链接脚本中额外导出边界并拆分映射。

所有内核映射都不设置`PTE_U`。代码也没有预置`PTE_A/PTE_D`，当前运行依赖 QEMU 在访问页面时更新这些位。

### 7.2 启用分页

每个 hart 都执行：

```c
w_satp(MAKE_SATP(kernel_pgtbl));
sfence_vma();
```

`MAKE_SATP`设置 MODE=Sv39、ASID=0，并填入根页表 PPN。`sfence.vma`刷新当前 hart 的全部 TLB 项。共享页表在启用前已由 hart 0 完成构建，因此启动阶段不需要 TLB shootdown。

运行期间若修改活动页表，调用者必须负责本地`sfence.vma`；多 hart 共享页表还需要实现跨 hart TLB shootdown。

## 8. 验证记录

当前`main()`只执行初始化和双 hart 启动 smoke test。功能测试曾通过临时替换`main()`执行；完整代码、实际输出、运行方式和覆盖缺口见[`TESTING.md`](TESTING.md)。

| 测试 | 核心检查 | 结果 |
| --- | --- | --- |
| 内核页并发分配 | 两个 hart 各申请、写入、释放 512 页 | 通过，无锁错误 |
| 用户页分配/释放 | 地址属于用户池；释放后重新分配并逐字节检查清零 | 通过，重新分配顺序符合 LIFO |
| 内核池耗尽 | 第 1025 次内核页申请 | 按预期 panic |
| 跨层级页表映射 | 覆盖不同 VPN[2:0]和不足一页的长度 | PTE 地址、PA 和 flags 符合预期 |
| 映射与解映射断言 | 两组独立 PA 的映射、权限、清除和释放 | 输出`test_mapping_and_unmapping passed!` |
| 重复映射 | 对有效 PTE 再次调用`vm_mappages` | 按预期触发 remapping 断言 |

## 9. 已知限制与后续工作

### 9.1 内存模块

- `pmem_free`不检查页对齐、重复释放或页面当前是否已分配。
- 页表操作没有内部锁；共享页表的修改必须由调用者串行化。
- `vm_mappages`不验证`perm`是否合法，也不屏蔽意外的高位。
- 没有页表引用计数、递归销毁和中间页表回收。
- 修改活动页表后没有自动 TLB 刷新或跨 hart shootdown。
- 尚无用户页表、`PTE_U`映射、用户/内核地址空间隔离和 guard page。
- 所有不可恢复错误统一 panic，没有向上返回 OOM 或部分映射失败。

### 9.2 启动与构建

- 每个 hart 的启动栈只有 4 KiB，连续排列且没有 guard page。
- 启动代码依赖 ELF 加载器清零 `.bss`。
- `Makefile`会生成`.d`依赖文件但没有包含它们，头文件或`kernel.ld`变化后建议执行`make clean && make build`。
- `debug`目标当前依赖未定义的`$(KERN)`，从干净目录使用前需要修正依赖关系。

建议的后续实现顺序：

1. 为`pmem_free`增加页对齐和重复释放检查。
2. 实现用户页表创建、`walkaddr`、页表递归释放和用户内存复制。
3. 增加 trap/trampoline、页错误诊断和用户态切换。
4. 为活动共享页表实现本地 TLB 刷新与跨 hart shootdown。
5. 将阶段性测试迁移到独立测试文件和 Makefile test target。

## 10. 历史问题：自旋锁持有者标记

运行第一组测试用例时，在释放已分配的页面时出现了`panic: spinlock_acquire`错误。

GDB 调试信息如下。地址及`pmem_free(page, in_kernel)`签名来自问题发生时的旧版本：

```text
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

研究后发现，问题出在 spinlock 实现中的竞争窗口。早期代码用`cpuid=0`表示无人持有锁，但 0 同时也是 CPU 0 的合法 hart ID。在并发执行时，`spinlock_acquire()`中的`spinlock_holding()`可能错误地判断 CPU 0 已持有锁并触发 panic。

出错时的具体操作序列：

```text
CPU1: 进入 spinlock_release(&lk);
CPU1: lk->cpuid = 0; // 清零 cpuid
CPU0: 进入 spinlock_acquire(&lk);
CPU0: 进入 spinlock_holding(&lk);
CPU0: r = (lk->locked && lk->cpuid == mycpuid());
// 此时 lk->locked == 1，lk->cpuid == 0，函数认为锁被 CPU0 持有，触发 panic
// 但实际上锁被 CPU1 持有（正在释放），出现了误判
```

该序列发生在 CPU 1 已清除`cpuid`、但尚未通过`__sync_lock_release`清除`locked`的窗口内。此时 CPU 0 看到`locked == 1 && cpuid == 0`，因而把 CPU 1 正在释放的锁误认为由自己持有。

要解决这个问题，需要避免“无人持有”和“CPU 0 持有”使用相同的`cpuid`。当前修复使用特殊值`-1`表示无人持有：`spinlock_init()`初始化时将`lk->cpuid`设为`-1`，`spinlock_release()`释放时也写入`-1`。这样`spinlock_holding()`不会再把释放窗口误判为 CPU 0 已持锁。并发申请、释放全部内核页的测试验证了该修复。

## 11. 速查

### 11.1 核心常量

| 常量 | 含义 |
| --- | --- |
| `KERNEL_BASE` | RAM 和内核加载基址，`0x80000000` |
| `ALLOC_END` | 内核使用的 RAM 末端，`0x88000000` |
| `PGSIZE` | 4 KiB |
| `KERN_PAGES` | 内核池 1024 页，即 4 MiB |
| `VA_MAX` | 当前实现允许的 VA 上界，`1 << 38` |
| `SATP_SV39` | `satp.MODE = 8` |

### 11.2 关键约束

- 页表页和叶子 PA 必须 4 KiB 对齐。
- 高两级有效 PTE 必须是非叶子项。
- 叶子权限应满足 RISC-V PTE 编码要求，尤其避免`W=1、R=0`。
- 释放叶子物理页前必须确认不存在其他映射或引用。
- 修改活动页表后必须处理 TLB 一致性。
- 修改 RAM 大小、hart 数量或链接边界时，应同时检查 Makefile、架构常量和页表映射。

### 11.3 参考位置

- 本项目内存接口：`src/kernel/mem/method.h`
- 本项目页表宏：`src/kernel/mem/type.h`
- xv6 页表实现：`../xv6-labs-2020/kernel/vm.c`
- xv6 平台地址：`../xv6-labs-2020/kernel/memlayout.h`
- 架构规范：RISC-V Privileged Architecture 中的 `satp`、Sv39、PTE 和 `SFENCE.VMA`章节
