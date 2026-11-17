/**
  ******************************************************************************
  * @file    app/main.c
  * @brief   HTOS bring-up application for STM32F103C8T6 (Cortex-M3, 72 MHz).
  *
  * Boot flow:  clock (72 MHz) -> console (USART1 115200 8N1) -> LED -> banner
  *             -> htOSInit() -> two tasks -> htOSStart().
  *
  * Two application tasks, both real loops that block in htTaskDelay():
  *   - "blink"  : toggles the LED every 500 ms (1 Hz), proves the scheduler runs.
  *   - "logger" : every 500 ms prints tick count, heap high-water mark and the
  *                last bring-up error code over the polled console.
  *
  * No printf() anywhere in a task: printf pulls in newlib's formatting/stack cost
  * and a lock, and this firmware is deliberately heap-free.  The banner and log
  * lines are assembled with the tiny fixed-point helpers below and handed to
  * board_console_write(), which is a bounded blocking UART poll.
  ******************************************************************************
  */

#include <stdint.h>

#include "board.h"
#include "htconfig.h"
#include "htmem.h"
#include "htos.h"

/* -------------------------------------------------------------------------- */
/* Application configuration                                                  */
/* -------------------------------------------------------------------------- */
#define APP_TASK_BLINK_STACK_BYTES   512U   /*!< bytes (RTOS converts to words) */
#define APP_TASK_LOG_STACK_BYTES     768U   /*!< bytes, room for the log buffer */
#define APP_BLINK_HALF_PERIOD_MS     500U   /*!< 500 ms on / 500 ms off = 1 Hz  */
#define APP_LOG_PERIOD_MS            500U   /*!< logger period                  */
/* Worst case line: "[log] tick=" + 10d + " t=" + 10d + "ms heap_free=" + 10d +
   " heap_min=" + 10d + " tasks=" + 10d + " last_err=" + 10d + CRLF = 116 bytes,
   so 128 leaves margin.  Keep this >= that sum. */
#define APP_LOG_LINE_BYTES           128U   /*!< stack log line buffer          */

/** @brief Convert milliseconds to scheduler ticks (integer, no libm). */
#define APP_MS_TO_TICKS(ms)   ((TickType_t)(((uint32_t)(ms) * (uint32_t)configTICK_RATE_HZ) / 1000U))

/**
  * @brief Convert ticks back to milliseconds without a 64-bit multiply.
  * @note  At the configured 1 kHz tick this is the identity.
  */
#if (configTICK_RATE_HZ == 1000U)
#define APP_TICKS_TO_MS(ticks) ((uint32_t)(ticks))
#else
#define APP_TICKS_TO_MS(ticks) ((uint32_t)(((uint32_t)(ticks) * 1000U) / (uint32_t)configTICK_RATE_HZ))
#endif

/** @brief Write a string literal (compile-time length, no strlen). */
#define APP_PUTS(text) board_console_write((text), (uint32_t)(sizeof(text) - 1U))

/* Bring-up error codes reported by the logger ("last_err"). */
#define APP_ERR_NONE             0U
#define APP_ERR_OS_INIT          1U
#define APP_ERR_TASK_BLINK       2U
#define APP_ERR_TASK_LOGGER      3U
#define APP_ERR_SCHEDULER_START  4U

/* -------------------------------------------------------------------------- */
/* State                                                                      */
/* -------------------------------------------------------------------------- */
static TaskHandle_t s_blink_task = NULL;
static TaskHandle_t s_logger_task = NULL;

/** Last bring-up failure; sticky, read by the logger task and a debugger. */
static volatile uint32_t s_last_error = APP_ERR_NONE;

/* -------------------------------------------------------------------------- */
/* Tiny formatting helpers (no printf, no heap, bounded output)                */
/* -------------------------------------------------------------------------- */
static uint32_t prv_strlen(const char *text)
{
	const char *cursor = text;

	while (*cursor != '\0') {
		cursor++;
	}

	return (uint32_t)(cursor - text);
}

static char *prv_str(char *cursor, const char *text)
{
	while (*text != '\0') {
		*cursor = *text;
		cursor++;
		text++;
	}

	return cursor;
}

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

/** @brief Print " <key><value><suffix>" and flush it. */
static void prv_report(const char *key, uint32_t value, const char *suffix)
{
	char line[96]; /* longest banner entry is < 60 bytes; margin included */
	char *cursor = line;

	cursor = prv_str(cursor, key);
	cursor = prv_u32(cursor, value);
	cursor = prv_str(cursor, suffix);

	board_console_write(line, (uint32_t)(cursor - line));
}

/* -------------------------------------------------------------------------- */
/* Bring-up banner                                                            */
/* -------------------------------------------------------------------------- */
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
/* Tasks                                                                      */
/* -------------------------------------------------------------------------- */

/**
  * @brief  LED blinker: toggle every 500 ms -> 1 Hz blink, no busy-wait.
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
  * @brief  Logger: every 500 ms report tick count, heap pressure and last error.
  * @note   Uses the fixed-size line buffer on its own stack; board_console_write()
  *         is a bounded blocking poll, so the task simply waits for the UART.
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
/* Fatal path                                                                 */
/* -------------------------------------------------------------------------- */
static void prv_fatal(const char *reason, uint32_t code)
{
	APP_PUTS("\r\n[fatal] ");
	board_console_write(reason, prv_strlen(reason));
	prv_report(" code=", code, "\r\n");
	APP_PUTS("[fatal] halted - attach a debugger and inspect s_last_error\r\n");

	/* No HAL_Delay() here: the time base may be exactly what failed. */
	for (;;) {
		/* spin */
	}
}

/* -------------------------------------------------------------------------- */
/* Entry point                                                                */
/* -------------------------------------------------------------------------- */
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

	/* Only reached if the scheduler never started. */
	s_last_error = APP_ERR_SCHEDULER_START;
	prv_fatal("htOSStart() returned", s_last_error);

	return 0;
}
