# ECNU OSLab 2025

## 项目简介

本仓库是华东师范大学计算机科学与技术学院 2025 秋季学期操作系统课程实验的实现项目。实验使用 C 语言和 RISC-V 架构，从机器启动开始，逐步实现内存管理、中断与异常、进程、系统调用和文件系统，最终形成一个简单的操作系统内核。

实验具有连续性：后续实验建立在前序实验代码之上，各实验的实现分别保存在对应的 Git 分支中。

- 实验指导书和模板代码：[ECNU OSLab 2025 Task](https://gitee.com/xu-ke-123/ecnu-oslab-2025-task)
- xv6 参考代码：[MIT xv6-riscv](https://github.com/mit-pdos/xv6-riscv)；实验指导书使用的 2020 版本可通过`git://g.csail.mit.edu/xv6-labs-2020`获取。

## 实验内容

- **lab-0：实验简介与环境配置。** 了解项目目标、开发流程和前置知识，并配置 RISC-V 交叉编译及 QEMU 实验环境。
- **lab-1：机器启动。** 完成多核启动、M-mode 到 S-mode 的切换、UART 输出、`printf`和自旋锁。
- **lab-2：内存管理初步。** 实现物理页分配器、Sv39 内核页表和内核态虚拟内存映射。
- **lab-3：中断异常初步。** 建立内核 trap 处理流程，实现 UART 外设中断和时钟中断。
- **lab-4：第一个用户进程的诞生。** 定义进程及其地址空间，实现上下文切换、trapframe 和首个用户进程。
- **lab-5：系统调用与用户态虚拟内存。** 建立系统调用流程，实现用户与内核数据传递、堆栈管理、`mmap`以及页表复制和销毁。
- **lab-6：进程调度与生命周期。** 从单进程扩展到多进程，实现抢占式调度、进程状态转换以及`fork`、`exit`、`wait`、`sleep`和`wakeup`。
- **lab-7：文件系统之磁盘管理。** 实现块设备读写、缓冲区系统、超级块和位图管理，为文件系统提供底层存储能力。
- **lab-8：文件系统之数据组织与层次结构。** 实现根目录、inode、目录项和路径解析，建立层次化文件组织。
- **lab-9：文件系统之文件管理与全系统整合。** 完善文件接口，将文件系统接入进程和系统调用，并支持加载执行 ELF 程序。

## 分支使用

查看远程实验分支：

```bash
git branch -r
```

使用`git checkout`切换到对应实验分支，例如：

```bash
git checkout lab-3
```

切换回项目主分支：

```bash
git checkout main
```

## 环境配置

在 Arch Linux 中，安装适用于`riscv64`的 QEMU 和 GCC 工具链：

```bash
sudo pacman -S qemu-system-riscv riscv64-elf-gcc riscv64-elf-gdb
```

Windows 用户请根据实验指导书中的《实验环境配置》教程配置虚拟机和开发环境。

工具链的基本配置位于`common.mk`。根据本机安装的交叉编译工具前缀编辑`TOOLPREFIX`，编译器、链接器和二进制工具会由该前缀派生：

```makefile
TOOLPREFIX = riscv64-elf-
CC = ${TOOLPREFIX}gcc
LD = ${TOOLPREFIX}ld
OBJCOPY = ${TOOLPREFIX}objcopy
OBJDUMP = ${TOOLPREFIX}objdump
```
