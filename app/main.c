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
#include "htconfig.h"
#include "htmem.h"
#include "htos.h"

/* -------------------------------------------------------------------------- */
/* 应用配置                                                                   */
/* -------------------------------------------------------------------------- */
#define APP_TASK_BLINK_STACK_BYTES   512U   /*!< 字节数，RTOS 内部会换算成 word。 */
#define APP_TASK_LOG_STACK_BYTES     768U   /*!< 字节数，要放得下日志行缓冲。 */
#define APP_BLINK_HALF_PERIOD_MS     500U   /*!< 亮 500 ms、灭 500 ms，合计 1 Hz。 */
#define APP_LOG_PERIOD_MS            500U   /*!< 日志任务周期，单位 ms。 */
/* 最坏情况的一行："[log] tick=" + 10 位十进制 + " t=" + 10 位 + "ms heap_free=" +
   10 位 + " heap_min=" + 10 位 + " tasks=" + 10 位 + " last_err=" + 10 位 + CRLF
   合计 116 字节，因此取 128 留出余量。此值不得小于上述总和。 */
#define APP_LOG_LINE_BYTES           128U   /*!< 日志行缓冲，放在任务自己的栈上。 */

/** @brief 把毫秒换算成调度 tick，整数运算，不依赖 libm。 */
#define APP_MS_TO_TICKS(ms)   ((TickType_t)(((uint32_t)(ms) * (uint32_t)configTICK_RATE_HZ) / 1000U))

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
#define APP_ERR_NONE             0U
#define APP_ERR_OS_INIT          1U
#define APP_ERR_TASK_BLINK       2U
#define APP_ERR_TASK_LOGGER      3U
#define APP_ERR_SCHEDULER_START  4U

/* -------------------------------------------------------------------------- */
/* 模块状态                                                                   */
/* -------------------------------------------------------------------------- */
static TaskHandle_t s_blink_task = NULL;
static TaskHandle_t s_logger_task = NULL;

/** 最近一次上电自检失败原因；粘滞不清，供日志任务与调试器读取。 */
static volatile uint32_t s_last_error = APP_ERR_NONE;

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

	s_blink_task = htOSTaskCreate("blink", prv_blink_task, NULL, APP_TASK_BLINK_STACK_BYTES, (uint8_t)HT_LOW_TASK);
	if (s_blink_task == NULL) {
		s_last_error = APP_ERR_TASK_BLINK;
		prv_fatal("blink task", s_last_error);
	}

	s_logger_task = htOSTaskCreate("logger", prv_logger_task, NULL, APP_TASK_LOG_STACK_BYTES, (uint8_t)HT_NORMAL_TASK);
	if (s_logger_task == NULL) {
		s_last_error = APP_ERR_TASK_LOGGER;
		prv_fatal("logger task", s_last_error);
	}

	prv_report(" TASKS CREATED= ", (uint32_t)uxCurrentNumberOfTasks, "\r\n");
	APP_PUTS("[boot] starting scheduler ...\r\n");

	(void)htOSStart();

	/* 只有调度器压根没启动起来才会走到这里。 */
	s_last_error = APP_ERR_SCHEDULER_START;
	prv_fatal("htOSStart() returned", s_last_error);

	return 0;
}
