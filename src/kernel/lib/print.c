/* 标准输出和报错机制 */

#include "mod.h"
#include <stdarg.h>

static char digits[] = "0123456789abcdef";

/* printf的自旋锁 */
static spinlock_t print_lk;

/* 初始化uart + 初始化printf锁 */
void print_init(void)
{
    uart_init();
    spinlock_init(&print_lk, "printf");
}

/* %d %p */
static void printint(int xx, int base, int sign)
{
    char buf[16];
    int i;
    uint32 x;

    if (sign && (sign = xx < 0))
        x = -xx;
    else
        x = xx;

    i = 0;
    do
    {
        buf[i++] = digits[x % base];
    } while ((x /= base) != 0);

    if (sign)
        buf[i++] = '-';

    while (--i >= 0)
        uart_putc_sync(buf[i]);
}

/* %x */
static void printptr(uint64 x)
{
    uart_putc_sync('0');
    uart_putc_sync('x');
    for (int i = 0; i < (sizeof(uint64) * 2); i++, x <<= 4)
        uart_putc_sync(digits[x >> (sizeof(uint64) * 8 - 4)]);
}

/*
    标准化输出, 支持:
    1. %d (32位有符号数,以10进制输出)
    2. %p (32位无符号数,以16进制输出)
    3. %x (64位无符号数,以0x开头的16进制输出)
    4. %c (单个字符)
    5. %s (字符串)
*/
void printf(const char *fmt, ...)
{
    spinlock_acquire(&print_lk);

    if (fmt == NULL)
        panic("printf: fmt is null");
    
    uint64 idx = 0;
    va_list ap;
    va_start(ap, fmt);
    while (fmt[idx] != '\0')
    {
        if (fmt[idx] == '%')
        {
            if (fmt[idx + 1] == '\0')
            {
                uart_putc_sync('%');
                idx++;
                break;
            }
            switch (fmt[idx + 1])
            {
                case 'd':
                    printint(va_arg(ap, int32), 10, 1);
                    break;
                case 'p':
                    printptr(va_arg(ap, uint64));
                    break;
                case 'x':
                    printint(va_arg(ap, int32), 16, 1);
                    break;
                case 'c':
                    uart_putc_sync(va_arg(ap, int));
                    break;
                case 's':
                    char* s = va_arg(ap, char*);
                    if (s == NULL)
                        s = "(NULL)";
                    while(*s != '\0')
                        uart_putc_sync(*(s++));
                    break;
                default:
                    uart_putc_sync('%');
                    uart_putc_sync(fmt[idx]);
                    break;
            }
            idx += 2;
        } else {
            uart_putc_sync(fmt[idx++]);
        }
    }
    
    spinlock_release(&print_lk);
}



/* 如果发生panic, UART的停止标志 */
volatile int panicked = 0;

/* 报错并终止输出 */
void panic(const char *s)
{
    printf("panic! %s\n", s);
    panicked = 1;
    while (1)
        ;
}

/* 如果不满足条件, 则调用panic */
void assert(bool condition, const char *warning)
{

}
