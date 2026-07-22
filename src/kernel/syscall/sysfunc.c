#include "mod.h"

enum
{
    COPY_TEST_INTS = 5,
};

// 从用户空间传入 int 数组，成功返回 0。
uint64 sys_copyin()
{
    uint64 user_addr;
    uint32 len;
    int values[COPY_TEST_INTS];

    arg_uint64(0, &user_addr);
    arg_uint32(1, &len);
    assert(len <= COPY_TEST_INTS, "sys_copyin: array is too large.");
    uvm_copyin(myproc()->pgtbl, (uint64)values, user_addr, len * sizeof(values[0]));

    for (uint32 i = 0; i < len; i++)
        printf("get a number from user: %d\n", values[i]);
    return 0;
}

// 向用户空间传出 int 数组，成功返回元素数量。
uint64 sys_copyout()
{
    static const int values[COPY_TEST_INTS] = {1, 2, 3, 4, 5};
    uint64 user_addr;

    arg_uint64(0, &user_addr);
    uvm_copyout(myproc()->pgtbl, user_addr, (uint64)values, sizeof(values));
    return COPY_TEST_INTS;
}

// 从用户空间传入字符串，成功返回 0。
uint64 sys_copyinstr()
{
    char value[STR_MAXLEN + 1];

    memset(value, 0, sizeof(value));
    arg_str(0, value, STR_MAXLEN);
    printf("get string for user: %s\n", value);
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
