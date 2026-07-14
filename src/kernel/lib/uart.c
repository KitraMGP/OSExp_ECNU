/* low-level driver routines for 16550a UART. */

#include "mod.h"

// from printf.c 终止输出的标志
extern volatile int panicked;

// uart 初始化
void uart_init(void)
{
  	// 关闭中断
	WriteReg(IER, 0x00);

	// 进入设置比特率的模式
	WriteReg(LCR, LCR_BAUD_LATCH);

	// 设置比特率的低位和高位，最终设置为38.4K
	WriteReg(0, 0x03);
  	WriteReg(1, 0x00);

	// 设置传输字节长度为8bit,不校验
	WriteReg(LCR, LCR_EIGHT_BITS);

	// 清零和使能FIFO模式
	WriteReg(FCR, FCR_FIFO_ENABLE | FCR_FIFO_CLEAR);

	// 使能输出队列和接收队列的中断
	WriteReg(IER, IER_TX_ENABLE | IER_RX_ENABLE);
}

// 单个字符输出
void uart_putc_sync(int c)
{
	// 关闭中断
	push_off();

	// 如果错误发生则卡住
	while (panicked)
		;

	// 等待TX队列进入idle状态
	while ((ReadReg(LSR) & LSR_TX_IDLE) == 0)
		;

	// 输出
	WriteReg(THR, c);

	// 开启中断
	pop_off();
}

// 单个字符输入
// 失败返回-1
int uart_getc_sync(void)
{
	if (ReadReg(LSR) & 0x01)
		return ReadReg(RHR);
	else
		return -1;
}

// 中断处理(键盘输入->屏幕输出)
void uart_intr(void)
{
	// 记录当前输入行字符数
	static uint32 line_chars = 0;
	// 上一个字符是否是 CR
	static bool previous_cr = false;
	while (1)
	{
		int c = uart_getc_sync();
		if (c == -1)
			break;
		// 处理换行
		// 若有 CRLF 组合，只识别成一次换行
		if (c == '\r')
		{
			previous_cr = true;
			uart_putc_sync('\r');
			uart_putc_sync('\n');
			line_chars = 0;
			continue;
		} else if (c == '\n') {
			// CRLF 组合中 CR 已经执行换行
			if (!previous_cr)
			{
				uart_putc_sync('\r');
				uart_putc_sync('\n');
				line_chars = 0;
			}
			previous_cr = false;
			continue;
		} else {
			// 不是 CR 也不是 LF
			previous_cr = false;
		}
		// 处理退格
		// '\b' 代表退格符，0x7f 代表删除符
		// '\b' 的作用只是将光标左移 1 字符，因此需要输出空格来覆盖掉要删除的字符
		if (c == '\b' || c == 0x7f)
		{
			if (line_chars == 0)
				continue;
			uart_putc_sync('\b');
			uart_putc_sync(' ');
			uart_putc_sync('\b');
			line_chars--;
			continue;
		}
		line_chars++;
		uart_putc_sync(c);
	}
}
