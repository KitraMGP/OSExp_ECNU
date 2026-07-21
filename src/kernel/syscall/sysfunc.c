#include "mod.h"

// 从用户空间传入 int 数组，成功返回 0。
uint64 sys_copyin()
{
    // TODO: 读取用户数组地址和长度，并复制到内核后输出。
    return 0;
}

// 向用户空间传出 int 数组，成功返回元素数量。
uint64 sys_copyout()
{
    // TODO: 将内核测试数组复制到用户提供的地址。
    return 0;
}

// 从用户空间传入字符串，成功返回 0。
uint64 sys_copyinstr()
{
    // TODO: 读取并输出用户传入的字符串。
    return 0;
}

// 调整用户堆顶；参数为 0 时查询当前堆顶。
uint64 sys_brk()
{
    // TODO: 根据目标堆顶调用堆增长或收缩函数并更新进程状态。
    return -1;
}

// 增加一段页对齐的用户内存映射。
uint64 sys_mmap()
{
    // TODO: 校验系统调用参数并创建用户内存映射。
    return 0;
}

// 解除一段页对齐的用户内存映射。
uint64 sys_munmap()
{
    // TODO: 校验系统调用参数并解除用户内存映射。
    return 0;
}
