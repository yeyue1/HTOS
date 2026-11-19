/**
 * @file app/main.c
 * @brief HTOS 板级示例应用（STM32F103C8T6，Cortex-M3，72 MHz）。
 *
 * 启动流程：时钟（72 MHz）-> 控制台（USART3，经 RS485 半双工，9600 8N1）
 *           -> LED -> 开机横幅 -> htOSInit() -> 创建两个任务 -> htOSStart()。
 *
 * 两个应用任务都是真循环，阻塞在 htTaskDelay() 上，没有一个忙等：
 *   - "blink" ：每 500 ms 翻转一次 LED，即 1 Hz，用来证明调度器真的在跑；
 *   - "logger"：每 500 ms 打印一次 tick 计数、堆余量历史最低点和最近一次
 *               上电自检错误码，走轮询式控制台输出。
 *
 * 任务里一律不用 printf()：printf 会把 newlib 的格式化代码、栈开销和一把锁
 * 一起拉进来，而本固件是刻意无堆的。横幅与日志行改用下面几个定点小工具拼装，
 * 再交给 board_console_write()，它是一个有界的阻塞式 UART 轮询发送。
 */

#include <stdint.h>

#include "board.h"
#include "htPort.h"
#include "htconfig.h"
#include "htmem.h"
#include "htos.h"
#include "httrace.h"
#include "htutils.h"

/* -------------------------------------------------------------------------- */
/* 应用配置                                                                   */
/* -------------------------------------------------------------------------- */
#define APP_TASK_BLINK_STACK_BYTES 512U /*!< 字节数，RTOS 内部会换算成 word。 */
#define APP_TASK_LOG_STACK_BYTES 768U /*!< 字节数，要放得下日志行缓冲。 */
#define APP_BLINK_HALF_PERIOD_MS 500U /*!< 亮 500 ms、灭 500 ms，合计 1 Hz。 */
#define APP_LOG_PERIOD_MS 500U /*!< 日志任务周期，单位 ms。 */
/* 最坏情况的一行："[log] tick=" + 10 位十进制 + " t=" + 10 位 + "ms heap_free=" +
   10 位 + " heap_min=" + 10 位 + " tasks=" + 10 位 + " last_err=" + 10 位 +
   " cpu=" + 3 位 + " idle=" + 3 位 + CRLF 合计 133 字节，因此取 160 留出余量。
   此值不得小于上述总和。 */
#define APP_LOG_LINE_BYTES 160U /*!< 日志行缓冲，放在任务自己的栈上。 */
/* 任务表：表头 23 字节 + 每任务约 25 字节，三个任务合计不到 100 字节，200 足够。
   仅当 configUSE_STATS_FORMATTING_FUNCTIONS 为 1 时才会用到。 */
#define APP_TASK_STATS_BYTES 200U /*!< htTaskList() 的输出缓冲。 */

/** @brief 把毫秒换算成调度 tick，整数运算，不依赖 libm。 */
#define APP_MS_TO_TICKS(ms) ((TickType_t)(((uint32_t)(ms) * (uint32_t)configTICK_RATE_HZ) / 1000U))

/**
 * @brief 把 tick 换回毫秒，避免 64 位乘法。
 *
 * @note 在配置的 1 kHz tick 下这就是恒等映射，所以走上面那条快速分支。
 */
#if (configTICK_RATE_HZ == 1000U)
#define APP_TICKS_TO_MS(ticks) ((uint32_t)(ticks))
#else
#define APP_TICKS_TO_MS(ticks) ((uint32_t)(((uint32_t)(ticks) * 1000U) / (uint32_t)configTICK_RATE_HZ))
#endif

/** @brief 输出一个字符串字面量，长度在编译期可得，不需要 strlen。 */
#define APP_PUTS(text) board_console_write((text), (uint32_t)(sizeof(text) - 1U))

/* 上电自检错误码，由日志任务以 "last_err" 报出。 */
#define APP_ERR_NONE 0U
#define APP_ERR_OS_INIT 1U
#define APP_ERR_TASK_BLINK 2U
#define APP_ERR_TASK_LOGGER 3U
#define APP_ERR_SCHEDULER_START 4U

/* -------------------------------------------------------------------------- */
/* 模块状态                                                                   */
/* -------------------------------------------------------------------------- */
static TaskHandle_t s_blink_task = NULL;
static TaskHandle_t s_logger_task = NULL;

/** 最近一次上电自检失败原因；粘滞不清，供日志任务与调试器读取。 */
static volatile uint32_t s_last_error = APP_ERR_NONE;

/* -------------------------------------------------------------------------- */
/* 静态存储：两个任务的栈与控制块都在编译期落地，运行期一个字节都不向堆申请    */
/* -------------------------------------------------------------------------- */
/* 栈深仍按字节配置，这里除以 sizeof(StackType_t) 换成 htTaskCreateStatic 要求的
   word 数（Cortex-M3 上为 4）。放在 .bss 里，与原来的 12 KB TLSF 池互不重叠。 */
static StackType_t s_blink_stack[APP_TASK_BLINK_STACK_BYTES / sizeof(StackType_t)];
static htTCB_t s_blink_tcb;
static StackType_t s_logger_stack[APP_TASK_LOG_STACK_BYTES / sizeof(StackType_t)];
static htTCB_t s_logger_tcb;

/* -------------------------------------------------------------------------- */
/* 轻量格式化工具：不调 printf、不碰堆、输出长度有界                          */
/* -------------------------------------------------------------------------- */
/**
 * @brief 求字符串长度。
 *
 * @param[in] text 以 NUL 结尾的字符串。
 * @return 不含结尾 NUL 的字节数。
 */
static uint32_t prv_strlen(const char *text)
{
	const char *cursor = text;

	while (*cursor != '\0') {
		cursor++;
	}

	return (uint32_t)(cursor - text);
}

/**
 * @brief 把字符串拷到游标处，并返回新的游标。
 *
 * @param[in] cursor 目标游标。
 * @param[in] text 源字符串。
 * @return 指向已写入内容之后的位置。
 * @warning 不做边界检查，调用方必须自行保证缓冲区够大。
 */
static char *prv_str(char *cursor, const char *text)
{
	while (*text != '\0') {
		*cursor = *text;
		cursor++;
		text++;
	}

	return cursor;
}

/**
 * @brief 把无符号整数按十进制写到游标处，并返回新的游标。
 *
 * @param[in] cursor 目标游标。
 * @param[in] value 待输出的数值。
 * @return 指向已写入内容之后的位置。
 * @note digits[10] 恰好够存 32 位十进制的 10 个数字，先反序生成再倒着吐出去。
 */
static char *prv_u32(char *cursor, uint32_t value)
{
	char digits[10];
	uint32_t count = 0U;

	do {
		digits[count] = (char)('0' + (char)(value % 10U));
		count++;
		value /= 10U;
	} while ((value != 0U) && (count < (uint32_t)sizeof(digits)));

	while (count > 0U) {
		count--;
		*cursor = digits[count];
		cursor++;
	}

	return cursor;
}

/**
 * @brief 打印 " <key><value><suffix>" 并立即送出。
 *
 * @param[in] key 键名，含前导空格与对齐用的等号。
 * @param[in] value 十进制数值。
 * @param[in] suffix 行尾字符串，通常带 CRLF。
 */
static void prv_report(const char *key, uint32_t value, const char *suffix)
{
	char line[96]; /* 最长的横幅条目也小于 60 字节，这里留足了余量 */
	char *cursor = line;

	cursor = prv_str(cursor, key);
	cursor = prv_u32(cursor, value);
	cursor = prv_str(cursor, suffix);

	board_console_write(line, (uint32_t)(cursor - line));
}

/**
 * @brief 把 32 位数写成固定 8 位小写十六进制，并返回新的游标。
 *
 * @param[in] cursor 目标游标。
 * @param[in] value 待输出的数值。
 * @return 指向已写入内容之后的位置。
 * @note 不带 0x 前缀，前缀由调用方的键名带出来，这样一张故障现场表能对齐。
 */
static char *prv_hex(char *cursor, uint32_t value)
{
	uint32_t shift = 32U;

	while (shift > 0U) {
		uint32_t nibble;

		shift -= 4U;
		nibble = (value >> shift) & 0xFU;
		*cursor = (char)((nibble < 10U) ? ('0' + (char)nibble) : ('a' + (char)(nibble - 10U)));
		cursor++;
	}

	return cursor;
}

/**
 * @brief 打印 " <key><8 位十六进制><suffix>" 并立即送出。
 *
 * @param[in] key 键名，含前导空格与 "=0x"。
 * @param[in] value 待输出的数值。
 * @param[in] suffix 行尾字符串，通常是 CRLF。
 */
static void prv_report_hex(const char *key, uint32_t value, const char *suffix)
{
	char line[48]; /* 键名不超过 16 字节，加 8 位十六进制与 CRLF 远小于 48 */
	char *cursor = line;

	cursor = prv_str(cursor, key);
	cursor = prv_hex(cursor, value);
	cursor = prv_str(cursor, suffix);

	board_console_write(line, (uint32_t)(cursor - line));
}

/* -------------------------------------------------------------------------- */
/* 内核诊断出口：接管三个弱钩子，全部写 RS485 控制台                          */
/* -------------------------------------------------------------------------- */
/**
 * @brief 内核诊断字符串钩子。
 *
 * @param[in] pcMessage 以 NUL 结尾的字符串，内核传的是静态字面量。
 * @note 内核可能在临界区里调用本钩子，因此这里只做有界的阻塞轮询发送，不碰堆。
 */
void htTrace(const char *pcMessage)
{
	board_console_write(pcMessage, prv_strlen(pcMessage));
}

/**
 * @brief 内核诊断数值钩子，写成 "标签=数值" 一行。
 *
 * @param[in] pcTag 标签文本。
 * @param[in] ulValue 十进制数值。
 * @warning pcTag 必须短于 80 字节；内核侧只会传固定的短标签。
 */
void htTraceValue(const char *pcTag, uint32_t ulValue)
{
	char line[96];
	char *cursor = line;

	cursor = prv_str(cursor, pcTag);
	cursor = prv_str(cursor, "=");
	cursor = prv_u32(cursor, ulValue);
	cursor = prv_str(cursor, "\r\n");

	board_console_write(line, (uint32_t)(cursor - line));
}

/**
 * @brief 硬 fault 现场钩子：把移植层收集好的现场打到控制台。
 *
 * @param[in] pxFrame 现场快照。
 * @note 这里是异常上下文的最深处：中断已关，本函数之后就是死循环，
 *       所以只做阻塞轮询发送，不延时、不申请内存、不做任何调度调用。
 */
void htFaultReport(const htFaultFrame_t *pxFrame)
{
	APP_PUTS("\r\n[HARDFAULT] ");
	prv_report_hex("SP=0x", pxFrame->ulStackPointer, "\r\n");

	prv_report_hex(" PC=0x", pxFrame->ulCore[6], "\r\n");
	prv_report_hex(" LR=0x", pxFrame->ulCore[5], "\r\n");
	prv_report_hex("PSR=0x", pxFrame->ulCore[7], "\r\n");
	prv_report_hex(" R0=0x", pxFrame->ulCore[0], "\r\n");
	prv_report_hex(" R1=0x", pxFrame->ulCore[1], "\r\n");
	prv_report_hex(" R2=0x", pxFrame->ulCore[2], "\r\n");
	prv_report_hex(" R3=0x", pxFrame->ulCore[3], "\r\n");
	prv_report_hex("R12=0x", pxFrame->ulCore[4], "\r\n");
	prv_report_hex("CFSR=0x", pxFrame->ulCfsr, "\r\n");
	prv_report_hex("HFSR=0x", pxFrame->ulHfsr, "\r\n");
	prv_report_hex("DFSR=0x", pxFrame->ulDfsr, "\r\n");
	prv_report_hex("AFSR=0x", pxFrame->ulAfsr, "\r\n");
	prv_report_hex("BFAR=0x", pxFrame->ulBfar, "\r\n");
	prv_report_hex("MMAR=0x", pxFrame->ulMmfar, "\r\n");

	if ((pxFrame->ulFaultFlags & HT_FAULT_FLAG_NOCP) != 0U) {
		APP_PUTS("- coprocessor access error\r\n");
	}
	if ((pxFrame->ulFaultFlags & HT_FAULT_FLAG_INVPC) != 0U) {
		APP_PUTS("- invalid PC load (EXC_RETURN)\r\n");
	}
	if ((pxFrame->ulFaultFlags & HT_FAULT_FLAG_INVSTATE) != 0U) {
		APP_PUTS("- invalid state, Thumb bit cleared\r\n");
	}
	if ((pxFrame->ulFaultFlags & HT_FAULT_FLAG_UNDEFINSTR) != 0U) {
		APP_PUTS("- undefined instruction\r\n");
	}

	APP_PUTS("[fault] halted\r\n");
}

/* -------------------------------------------------------------------------- */
/* 开机横幅                                                                   */
/* -------------------------------------------------------------------------- */
/**
 * @brief 打印开机横幅：主频、外设时钟、tick 频率、任务数、堆大小与时钟状态。
 *
 * @note 横幅同时把控制台与 LED 的实际接法印出来（USART3/RS485、PB10/PB11/PB2、
 *       9600 8N1、PC13），这样一份串口抓包就能自证板级配置，不必回看源码。
 */
static void prv_boot_banner(void)
{
	APP_PUTS("\r\n\r\n");
	APP_PUTS("==================================================\r\n");
	APP_PUTS(" HTOS board bring-up - STM32F103C8T6 (Cortex-M3)\r\n");
	APP_PUTS("==================================================\r\n");

	prv_report(" SYSCLK       = ", board_clock_get_sysclk_hz(), " Hz (HSE 8 MHz, PLL x9)\r\n");
	prv_report(" PCLK1/USART3 = ", board_clock_get_pclk1_hz(), " Hz\r\n");
	prv_report(" TICK RATE    = ", (uint32_t)configTICK_RATE_HZ, " Hz (SysTick, only tick source)\r\n");
	prv_report(" APP TASKS    = ", 2U, " (blink 1 Hz, logger 500 ms) + idle\r\n");
	prv_report(" HEAP         = ", (uint32_t)configTOTAL_HEAP_SIZE, " B TLSF pool (malloc disabled)\r\n");
	prv_report(" CLOCK STATUS = ", board_clock_status(), " (0 = every HAL step OK)\r\n");

	APP_PUTS(" CONSOLE      = USART3 over RS485, PB10=TX PB11=RX PB2=DE, 9600 8N1\r\n");
	APP_PUTS(" LED          = PC13 (onboard, active low)\r\n");
	APP_PUTS("==================================================\r\n");
}

/* -------------------------------------------------------------------------- */
/* 应用任务                                                                   */
/* -------------------------------------------------------------------------- */

/**
 * @brief LED 闪烁任务：每 500 ms 翻转一次，即 1 Hz，全程无忙等。
 *
 * @param[in] param 未使用的任务参数。
 */
static void prv_blink_task(void *param)
{
	(void)param;

	for (;;) {
		board_led_toggle();
		htTaskDelay(APP_MS_TO_TICKS(APP_BLINK_HALF_PERIOD_MS));
	}
}

/**
 * @brief 日志任务：每 500 ms 报告 tick 计数、堆压力和最近一次错误码。
 *
 * @param[in] param 未使用的任务参数。
 * @note 行缓冲是定长数组，就放在本任务自己的栈上；board_console_write() 是
 *       有界的阻塞轮询，所以本任务只是老老实实等 UART 发完，不占 CPU 空转。
 * @note 一条 116 字节的行在 9600 8N1 下要占用约 121 ms 总线时间，
 *       因此发送超时给到了 1000 ms。
 */
static void prv_logger_task(void *param)
{
	(void)param;

	for (;;) {
		char line[APP_LOG_LINE_BYTES];
		char *cursor = line;

		cursor = prv_str(cursor, "[log] tick=");
		cursor = prv_u32(cursor, (uint32_t)xTickCount);
		cursor = prv_str(cursor, " t=");
		cursor = prv_u32(cursor, APP_TICKS_TO_MS((uint32_t)xTickCount));
		cursor = prv_str(cursor, "ms heap_free=");
		cursor = prv_u32(cursor, (uint32_t)htGetFreeHeapSize());
		cursor = prv_str(cursor, " heap_min=");
		cursor = prv_u32(cursor, (uint32_t)htGetMinimumEverFreeHeapSize());
		cursor = prv_str(cursor, " tasks=");
		cursor = prv_u32(cursor, (uint32_t)uxCurrentNumberOfTasks);
		cursor = prv_str(cursor, " last_err=");
		cursor = prv_u32(cursor, s_last_error);
		cursor = prv_str(cursor, " cpu=");
		cursor = prv_u32(cursor, htGetCPUUsage());
		cursor = prv_str(cursor, " idle=");
		cursor = prv_u32(cursor, htGetIdleRunTimePercent());
		cursor = prv_str(cursor, "\r\n");

		board_console_write(line, (uint32_t)(cursor - line));

		htTaskDelay(APP_MS_TO_TICKS(APP_LOG_PERIOD_MS));
	}
}

/* -------------------------------------------------------------------------- */
/* 致命错误路径                                                               */
/* -------------------------------------------------------------------------- */
/**
 * @brief 上报致命错误并停机。
 *
 * @param[in] reason 出错的环节，直接打到控制台。
 * @param[in] code 错误码，同时留在 s_last_error 里给调试器看。
 * @warning 本函数不返回，结尾是死循环；它假设调度器已经不可用。
 */
static void prv_fatal(const char *reason, uint32_t code)
{
	APP_PUTS("\r\n[fatal] ");
	board_console_write(reason, prv_strlen(reason));
	prv_report(" code=", code, "\r\n");
	APP_PUTS("[fatal] halted - attach a debugger and inspect s_last_error\r\n");

	/* 这里故意不调 HAL_Delay()：很可能出问题的正是时基本身。 */
	for (;;) {
		/* 原地空转，等调试器接管 */
	}
}

/* -------------------------------------------------------------------------- */
/* 程序入口                                                                   */
/* -------------------------------------------------------------------------- */
/**
 * @brief 应用入口：板级初始化 -> 开机横幅 -> 内核初始化 -> 建两个任务 -> 启动调度器。
 *
 * @return 正常情况不会返回；只有调度器未能启动时才会走到末尾的 return 0。
 * @note 每一步失败都把原因写进 s_last_error 并调用 prv_fatal()，
 *       因此没有任何一步会静默失败。
 */
int main(void)
{
	board_clock_init();
	board_console_init();
	board_led_init();

	prv_boot_banner();

	if (htOSInit() != 0) {
		s_last_error = APP_ERR_OS_INIT;
		prv_fatal("htOSInit()", s_last_error);
	}

	s_blink_task =
			htTaskCreateStatic(prv_blink_task, "blink", (uint16_t)(APP_TASK_BLINK_STACK_BYTES / sizeof(StackType_t)),
					NULL, (UBaseType_t)HT_LOW_TASK, s_blink_stack, &s_blink_tcb);
	if (s_blink_task == NULL) {
		s_last_error = APP_ERR_TASK_BLINK;
		prv_fatal("blink task", s_last_error);
	}

	s_logger_task =
			htTaskCreateStatic(prv_logger_task, "logger", (uint16_t)(APP_TASK_LOG_STACK_BYTES / sizeof(StackType_t)),
					NULL, (UBaseType_t)HT_NORMAL_TASK, s_logger_stack, &s_logger_tcb);
	if (s_logger_task == NULL) {
		s_last_error = APP_ERR_TASK_LOGGER;
		prv_fatal("logger task", s_last_error);
	}

	prv_report(" TASKS CREATED= ", (uint32_t)uxCurrentNumberOfTasks, "\r\n");

#if (configUSE_STATS_FORMATTING_FUNCTIONS == 1)
	{
		/* 调度器还没启动，此刻各任务都还没跑过，表里的 rt 自然为 0；
		   它真正的用处是运行期按需调用，这里只是证明接口可用。 */
		char stats[APP_TASK_STATS_BYTES];
		size_t used = htTaskList(stats, sizeof(stats));

		APP_PUTS("[stats] task table:\r\n");
		board_console_write(stats, (uint32_t)used);
	}
#endif

	APP_PUTS("[boot] starting scheduler ...\r\n");

	(void)htOSStart();

	/* 只有调度器压根没启动起来才会走到这里。 */
	s_last_error = APP_ERR_SCHEDULER_START;
	prv_fatal("htOSStart() returned", s_last_error);

	return 0;
}
