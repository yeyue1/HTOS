/**
 * @file board/board.h
 * @brief STM32F103C8T6（"Blue Pill"）板级初始化接口。
 *
 * 引脚与外设的选型依据、出处都写在 board.c 里对应的定义处，这里只做摘要：
 *   - SYSCLK 72 MHz：HSE 8 MHz 经 PLL x9，取自参考工程可用的
 *     SystemClock_Config（Core/Src/main.c）。
 *   - 控制台 USART3 走 RS485：PB10 = TX（复用推挽）、PB11 = RX、
 *     PB2 = DE（高电平使能驱动器）、9600 8N1、阻塞发送。引脚与方向脚来自
 *     参考工程的 RS485 口（BSP/bsp_usart.h、BSP/bsp_gpio.h）。USART3 挂在
 *     APB1 上，因此时钟源是 PCLK1。
 *   - LED 用 PC13，低电平点亮（Blue Pill 板载 LED）。
 *
 * 本头文件不包含任何 CMSIS/HAL 头，任务调用板级 API 时看不到寄存器定义。
 */

#ifndef HTOS_BOARD_H
#define HTOS_BOARD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/* 控制台：USART3 走 RS485                                                    */
/* -------------------------------------------------------------------------- */
#define BOARD_CONSOLE_BAUDRATE        9600U   /*!< 8N1，无流控 */
/* 阻塞轮询式发送的时间预算必须远大于纯线路时间。9600 8N1 下每个字节约 1.04 ms，
   一条 116 字节的行在空闲 CPU 上就要占用约 121 ms 线路时间；而低优先级的发送者
   在轮询期间还可能被抢占几百毫秒。100 ms 经实测明显不够：在实板上负载较重时，
   一条 62 字节的行只发出约 46 字节就被截断，包含 CRLF 的尾部整段丢失，原因是
   prv_console_tx() 只在 HAL_BUSY 时重试，而 HAL_TIMEOUT 会让这一块静默结束。
   现在的取值是最长已知行的 10 倍。
   注意：超时的分块绝不能从头重发——HAL_UART_Transmit() 不会报告已经有多少字节
   移出了移位寄存器，从头重发会让前缀重复。只有 HAL_BUSY（一个字节都还没排队）
   才是可以安全重试的情形。 */
#define BOARD_CONSOLE_TX_TIMEOUT_MS   1000U   /*!< 单块阻塞发送超时，单位 ms */
#define BOARD_CONSOLE_TX_RETRIES      3U      /*!< UART 忙时的重试次数 */

/**
 * @brief 建立 72 MHz 时钟树（HSE 8 MHz -> PLL x9）和 1 kHz SysTick 时基。
 *
 * SysTick 是唯一的 RTOS tick 来源，所以 tick 频率也在这里一并配好。
 *
 * @note 只调用一次，且必须早于 board_console_init()。
 * @note 失败时不阻塞、不停机：降级的时钟状态通过 board_clock_status() 和开机
 *       横幅报出，一份串口抓包就能看出实际发生了什么。
 */
void board_clock_init(void);

/** @brief 返回 MCU 实际运行的 SYSCLK 频率，单位 Hz。 */
uint32_t board_clock_get_sysclk_hz(void);

/** @brief 返回 APB2 时钟频率，单位 Hz。 */
uint32_t board_clock_get_pclk2_hz(void);

/** @brief 返回 APB1 时钟频率，单位 Hz——即控制台 USART3 的输入时钟。 */
uint32_t board_clock_get_pclk1_hz(void);

/**
 * @brief 返回时钟初始化的结果：0 表示每一步 HAL 调用都返回了 HAL_OK。
 *
 * @return 非 0 值指出第一个失败的步骤，具体含义见下方 BOARD_CLOCK_* 宏。
 */
uint32_t board_clock_status(void);

/** @brief 时钟初始化各步骤的结果码，由 board_clock_status() 返回。 */
#define BOARD_CLOCK_OK            0U /*!< 72 MHz 就绪，SysTick 已配置 */
#define BOARD_CLOCK_ERR_HAL_INIT  1U /*!< HAL_Init() 失败 */
#define BOARD_CLOCK_ERR_OSC       2U /*!< HAL_RCC_OscConfig() 失败（通常是 HSE 起振） */
#define BOARD_CLOCK_ERR_CLK       3U /*!< HAL_RCC_ClockConfig() 失败 */
#define BOARD_CLOCK_ERR_SYSTICK   4U /*!< HAL_SYSTICK_Config() 失败 */

/**
 * @brief 初始化 USART3：PB10 作 TX、PB11 作 RX，波特率 BOARD_CONSOLE_BAUDRATE，
 *        8 位数据、1 位停止、无校验、无流控、收发双工，并把 PB2 配成 RS485
 *        方向输出且置于接收态。
 */
void board_console_init(void);

/**
 * @brief 发送一个字符，阻塞到它被排进 UART 为止。
 *
 * @param[in] c 待发送的字符。
 * @note 在任务里调用是安全的，但它只是轮询，不能替代一个 ISR 安全的发送队列。
 * @warning 不要在任何中断处理函数里调用：轮询会一直占着中断上下文。
 */
void board_console_putc(char c);

/**
 * @brief 发送一段缓冲区，阻塞到它被排进 UART 为止。
 *
 * @param[in] text 待发送的字节，不以 NUL 结尾，可以包含 '\r' '\n'。
 * @param[in] length 待发送的字节数。
 * @warning 不要在任何中断处理函数里调用，理由同 board_console_putc()。
 */
void board_console_write(const char *text, uint32_t length);

/* -------------------------------------------------------------------------- */
/* LED                                                                        */
/* -------------------------------------------------------------------------- */
/** @brief 把 LED 引脚配成推挽输出，初态为灭。 */
void board_led_init(void);

/** @brief 翻转 LED（HAL_GPIO_TogglePin 只写 BSRR，是原子操作，可在中断里用）。 */
void board_led_toggle(void);

#ifdef __cplusplus
}
#endif

#endif /* HTOS_BOARD_H */
