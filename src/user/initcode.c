#include "sys.h"

#define VA_MAX       (1ul << 38)
#define PGSIZE       4096
#define MMAP_END     (VA_MAX - (16 * 256 + 2) * PGSIZE)
#define MMAP_BEGIN   (MMAP_END - 64 * 256 * PGSIZE)
int main()
{
    char *auto_map;
    char *final_map;

    if (syscall(SYS_brk, PGSIZE) != -1 ||
        syscall(SYS_brk, MMAP_BEGIN + 1) != -1)
        while (1)
            ;

    syscall(SYS_mmap, MMAP_BEGIN + 4 * PGSIZE, 3 * PGSIZE);
    syscall(SYS_mmap, MMAP_BEGIN + 10 * PGSIZE, 2 * PGSIZE);
    syscall(SYS_mmap, MMAP_BEGIN + 2 * PGSIZE, 2 * PGSIZE);
    syscall(SYS_mmap, MMAP_BEGIN + 12 * PGSIZE, PGSIZE);
    syscall(SYS_mmap, MMAP_BEGIN + 7 * PGSIZE, 3 * PGSIZE);
    syscall(SYS_mmap, MMAP_BEGIN, 2 * PGSIZE);
    auto_map = (char *)syscall(SYS_mmap, 0, 10 * PGSIZE);
    if (auto_map != (char *)(MMAP_BEGIN + 13 * PGSIZE))
        while (1)
            ;
    if (syscall(SYS_mmap, MMAP_BEGIN + 4 * PGSIZE, PGSIZE) != -1 ||
        syscall(SYS_mmap, MMAP_BEGIN + 1, PGSIZE) != -1 ||
        syscall(SYS_mmap, MMAP_END, PGSIZE) != -1 ||
        syscall(SYS_munmap, MMAP_BEGIN + 23 * PGSIZE, PGSIZE) != -1)
        while (1)
            ;

    syscall(SYS_munmap, MMAP_BEGIN + 10 * PGSIZE, 5 * PGSIZE);
    syscall(SYS_munmap, MMAP_BEGIN, 10 * PGSIZE);
    syscall(SYS_munmap, MMAP_BEGIN + 17 * PGSIZE, 2 * PGSIZE);
    syscall(SYS_munmap, MMAP_BEGIN + 15 * PGSIZE, 2 * PGSIZE);
    syscall(SYS_munmap, MMAP_BEGIN + 19 * PGSIZE, 2 * PGSIZE);
    syscall(SYS_munmap, MMAP_BEGIN + 22 * PGSIZE, PGSIZE);
    syscall(SYS_munmap, MMAP_BEGIN + 21 * PGSIZE, PGSIZE);

    final_map = (char *)syscall(SYS_mmap, 0, 4 * PGSIZE);
    if (final_map != (char *)MMAP_BEGIN)
        while (1)
            ;
    final_map[0] = 'm';
    final_map[1] = 'm';
    final_map[2] = 'a';
    final_map[3] = 'p';
    final_map[4] = '\0';
    syscall(SYS_copyinstr, final_map);
    syscall(SYS_munmap, MMAP_BEGIN, 4 * PGSIZE);

    while (1)
        ;
    return 0;
}
