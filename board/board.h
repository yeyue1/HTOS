/**
  ******************************************************************************
  * @file    board/board.h
  * @brief   Board bring-up API for the STM32F103C8T6 ("Blue Pill") target.
  *
  * Pin / peripheral choices and where each one comes from are documented at the
  * definition in board.c.  Summary:
  *   - SYSCLK 72 MHz: HSE 8 MHz -> PLL x9 (proven SystemClock_Config of the
  *     reference project, Core/Src/main.c).
  *   - Console USART3 over RS485: PB10 = TX (AF push-pull), PB11 = RX,
  *     PB2 = DE (high = driver enabled), 9600 8N1, blocking.  Pins and the
  *     direction pin are the reference project's RS485 port (BSP/bsp_usart.h,
  *     BSP/bsp_gpio.h).  USART3 is clocked from PCLK1.
  *   - LED PC13, active low (onboard LED of the Blue Pill).
  *
  * Nothing in this header pulls in CMSIS/HAL headers, so tasks can use the board
  * API without seeing any register definition.
  ******************************************************************************
  */

#ifndef HTOS_BOARD_H
#define HTOS_BOARD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/* Console (USART3 over RS485)                                                */
/* -------------------------------------------------------------------------- */
#define BOARD_CONSOLE_BAUDRATE        9600U   /*!< 8N1, no flow control          */
#define BOARD_CONSOLE_TX_TIMEOUT_MS   100U    /*!< per-chunk blocking TX timeout */
#define BOARD_CONSOLE_TX_RETRIES      3U      /*!< retries if the UART is busy   */

/**
  * @brief  Bring up the 72 MHz clock tree (HSE 8 MHz -> PLL x9) and the 1 kHz
  *         SysTick time base used as the only RTOS tick source.
  * @note   Call once, before board_console_init().  Never blocks on failure:
  *         a degraded clock is reported through board_clock_status() and the
  *         banner, so a serial capture shows what actually happened.
  */
void board_clock_init(void);

/** @brief  Frequency the MCU is really running at (Hz). */
uint32_t board_clock_get_sysclk_hz(void);

/** @brief  APB2 clock in Hz. */
uint32_t board_clock_get_pclk2_hz(void);

/** @brief  APB1 clock in Hz - the USART3 (console) input clock. */
uint32_t board_clock_get_pclk1_hz(void);

/**
  * @brief  Clock bring-up result: 0 = every HAL step returned HAL_OK.
  *         Non-zero identifies the first failing step, see BOARD_CLOCK_* below.
  */
uint32_t board_clock_status(void);

/** @brief  Clock bring-up step results reported by board_clock_status(). */
#define BOARD_CLOCK_OK            0U /*!< 72 MHz, SysTick configured          */
#define BOARD_CLOCK_ERR_HAL_INIT  1U /*!< HAL_Init() failed                   */
#define BOARD_CLOCK_ERR_OSC       2U /*!< HAL_RCC_OscConfig() failed (HSE?)   */
#define BOARD_CLOCK_ERR_CLK       3U /*!< HAL_RCC_ClockConfig() failed        */
#define BOARD_CLOCK_ERR_SYSTICK   4U /*!< HAL_SYSTICK_Config() failed         */

/**
  * @brief  Initialise USART3 on PB10 (TX) / PB11 (RX) at BOARD_CONSOLE_BAUDRATE,
  *         8 data bits, 1 stop bit, no parity, no flow control, TX + RX, and
  *         configure PB2 as the RS485 direction output (receive state).
  */
void board_console_init(void);

/**
  * @brief  Send one character, blocking until it is queued in the UART.
  * @note   Safe from a task.  Not a substitute for a proper ISR-safe queue: it
  *         polls, so do not call it from an interrupt handler.
  */
void board_console_putc(char c);

/**
  * @brief  Send a buffer, blocking until it is queued in the UART.
  * @param  text   bytes to send (not NUL terminated, may contain '\r' '\n')
  * @param  length number of bytes to send
  */
void board_console_write(const char *text, uint32_t length);

/* -------------------------------------------------------------------------- */
/* LED                                                                        */
/* -------------------------------------------------------------------------- */
/** @brief  Configure the LED pin as a push-pull output, LED off. */
void board_led_init(void);

/** @brief  Flip the LED (HAL_GPIO_TogglePin writes BSRR, atomic, ISR-safe). */
void board_led_toggle(void);

#ifdef __cplusplus
}
#endif

#endif /* HTOS_BOARD_H */
