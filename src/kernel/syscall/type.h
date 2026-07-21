#pragma once
#include "../arch/type.h"

#define SYS_copyin 1
#define SYS_copyout 2
#define SYS_copyinstr 3
#define SYS_brk 4
#define SYS_mmap 5
#define SYS_munmap 6

#define SYS_MAX_NUM 6
#define STR_MAXLEN 127
