/* low-level driver routines for 16550a UART. */

#include "mod.h"

#define UART_LINE_BUFFER_SIZE 128

typedef enum
{
	UART_INPUT_NORMAL, // 普通字符输入状态
	UART_INPUT_ESC,    // 已收到 ESC，等待转义序列类型
	// 已收到 CSI 的 ESC [ 或 SS3 的 ESC O，等待参数及最终字符
	UART_INPUT_CONTROL_SEQUENCE,
} uart_input_state_t;

// from printf.c 终止输出的标志
extern volatile int panicked;

static void uart_move_cursor(int direction, uint32 count)
{
	// direction 为 ANSI CSI 光标移动方向：'A' 上移，'B' 下移，'C' 右移，'D' 左移
	while (count-- > 0)
	{
		uart_putc_sync('\x1b');
		uart_putc_sync('[');
		uart_putc_sync(direction);
	}
}

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

	// 当前驱动同步发送字符，只需要通过中断处理接收队列。
	WriteReg(IER, IER_RX_ENABLE);
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
	// 当前正在编辑的行，末尾保留一个字节存放 '\0'
	static char line_buf[UART_LINE_BUFFER_SIZE];
	// 行缓冲区中已有的字符数
	static uint32 line_len = 0;
	// 逻辑光标在行缓冲区中的位置，范围为 [0, line_len]
	static uint32 cursor_pos = 0;
	// ANSI 转义序列解析状态
	static uart_input_state_t input_state = UART_INPUT_NORMAL;
	// 当前 CSI 序列的第一个数值参数
	static uint32 csi_param = 0;
	// 当前 CSI 序列是否包含数值参数
	static bool csi_has_param = false;
	// 当前 CSI 序列是否包含暂不支持的参数或中间字节
	static bool csi_unsupported = false;
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
			input_state = UART_INPUT_NORMAL;
			uart_putc_sync('\r');
			uart_putc_sync('\n');
			line_len = 0;
			cursor_pos = 0;
			line_buf[0] = '\0';
			continue;
		} else if (c == '\n') {
			// CRLF 组合中 CR 已经执行换行
			if (!previous_cr)
			{
				uart_putc_sync('\r');
				uart_putc_sync('\n');
			}
			previous_cr = false;
			input_state = UART_INPUT_NORMAL;
			line_len = 0;
			cursor_pos = 0;
			line_buf[0] = '\0';
			continue;
		} else {
			// 不是 CR 也不是 LF
			previous_cr = false;
		}

		// 方向键由 ANSI 转义序列表示：ESC [ A/B/C/D
		if (input_state == UART_INPUT_ESC)
		{
			// CSI 以 ESC [ 开始，SS3 以 ESC O 开始，二者共用后续方向键处理
			if (c == '[' || c == 'O')
			{
				input_state = UART_INPUT_CONTROL_SEQUENCE;
				csi_param = 0;
				csi_has_param = false;
				csi_unsupported = false;
			} else {
				input_state = UART_INPUT_NORMAL;
			}
			continue;
		}

		if (input_state == UART_INPUT_CONTROL_SEQUENCE)
		{
			if (c >= '0' && c <= '9')
			{
				csi_has_param = true;
				csi_param = csi_param * 10 + c - '0';
				continue;
			}

			// 暂不处理带多个参数或中间字节的 CSI 序列
			if ((c >= 0x20 && c <= 0x2f) || (c >= 0x30 && c <= 0x3f))
			{
				csi_unsupported = true;
				continue;
			}

			input_state = UART_INPUT_NORMAL;
			if (csi_unsupported)
				continue;

			uint32 count = csi_has_param && csi_param != 0 ? csi_param : 1;
			if (c == 'C')
			{
				if (count > line_len - cursor_pos)
					count = line_len - cursor_pos;
				uart_move_cursor('C', count);
				cursor_pos += count;
				continue;
			}
			if (c == 'D')
			{
				if (count > cursor_pos)
					count = cursor_pos;
				uart_move_cursor('D', count);
				cursor_pos -= count;
				continue;
			}
			// 上下方向键不回显，终端光标不会离开当前输入行
			if (c == 'A' || c == 'B')
				continue;

			// Delete 键发送 ESC [ 3 ~，删除光标位置的字符
			if (c == '~' && csi_has_param && csi_param == 3)
			{
				if (cursor_pos == line_len)
					continue;

				for (uint32 i = cursor_pos; i + 1 < line_len; i++)
					line_buf[i] = line_buf[i + 1];
				line_len--;
				line_buf[line_len] = '\0';

				for (uint32 i = cursor_pos; i < line_len; i++)
					uart_putc_sync(line_buf[i]);
				uart_putc_sync(' ');
				uart_move_cursor('D', line_len - cursor_pos + 1);
			}
			continue;
		}

		if (c == '\x1b')
		{
			input_state = UART_INPUT_ESC;
			continue;
		}

		// 处理退格
		// '\b' 代表退格符，0x7f 代表删除符
		// 两者都删除逻辑光标前的字符
		if (c == '\b' || c == 0x7f)
		{
			if (cursor_pos == 0)
				continue;

			cursor_pos--;
			for (uint32 i = cursor_pos; i + 1 < line_len; i++)
				line_buf[i] = line_buf[i + 1];
			line_len--;
			line_buf[line_len] = '\0';

			uart_move_cursor('D', 1);
			for (uint32 i = cursor_pos; i < line_len; i++)
				uart_putc_sync(line_buf[i]);
			uart_putc_sync(' ');
			uart_move_cursor('D', line_len - cursor_pos + 1);
			continue;
		}

		// 只将可打印 ASCII 字符插入行缓冲区
		if (c < ' ' || c > '~' || line_len == UART_LINE_BUFFER_SIZE - 1)
			continue;

		for (uint32 i = line_len; i > cursor_pos; i--)
			line_buf[i] = line_buf[i - 1];
		line_buf[cursor_pos] = c;
		line_len++;
		line_buf[line_len] = '\0';

		for (uint32 i = cursor_pos; i < line_len; i++)
			uart_putc_sync(line_buf[i]);
		cursor_pos++;
		uart_move_cursor('D', line_len - cursor_pos);
	}
}
