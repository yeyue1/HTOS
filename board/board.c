/**
  ******************************************************************************
  * @file    board/board.c
  * @brief   Board bring-up: 72 MHz clock tree, 1 kHz tick, USART1 console, LED.
  *
  * Everything here is HAL/CMSIS-macro based; no raw peripheral address appears
  * anywhere.  The clock sequence is a direct copy of the proven
  * SystemClock_Config() of the reference project for this exact MCU
  * (openOCD_vscode standard-debug + HTOS + stack-unwind, Core/Src/main.c), and
  * the USART1 pin mux is the reference project's HAL_UART_MspInit for USART1
  * (Core/Src/usart.c: PA9 = USART1_TX, PA10 = USART1_RX).
  ******************************************************************************
  */

#include "board.h"

#include "stm32f1xx_hal.h"
#include "htconfig.h"

/*
 * The HAL time base (HAL_InitTick, used by HAL_UART_Transmit timeouts) and the
 * RTOS tick share the same SysTick, so the RTOS tick rate must be exactly the
 * HAL 1 ms time base.  Fail the build rather than ship a silent mismatch.
 */
#if (configTICK_RATE_HZ != 1000U)
#error "board.c requires configTICK_RATE_HZ == 1000 (shared SysTick with the HAL 1 ms time base)"
#endif

/* -------------------------------------------------------------------------- */
/* Console: USART3 over RS485                                                 */
/* -------------------------------------------------------------------------- *
 * The console leaves the board through the RS485 transceiver, not TTL:
 *   - USART3_TX = PB10 (AF push-pull), USART3_RX = PB11 (floating input)
 *   - RS485 direction: PB2 high = driver enabled, PB2 low = receiver enabled
 *   - 9600 8N1: the RS485 port's default rate in the reference project
 *     (StorageData.usartusartbaud[2] = 9600, StorageData writes in storageUse.c)
 * Pin choice and the direction pin are taken from the reference project:
 *   BSP/bsp_usart.h  RS485_USART_TX_GPIO_PIN = GPIO_Pin_10 (PB10)
 *   BSP/bsp_usart.h  RS485_USART_RX_GPIO_PIN = GPIO_Pin_11 (PB11)
 *   BSP/bsp_gpio.h   RS485_RW_PORT = GPIOB, RS485_RW_PIN = GPIO_Pin_2
 * Verified on hardware: with PB2 high the transceiver drives the bus and the
 * text arrives on the USB-RS485 adapter; with PB2 left floating nothing arrives.
 * USART3 is on APB1, so its kernel clock is PCLK1 (36 MHz at 72 MHz SYSCLK).
 * -------------------------------------------------------------------------- */
#define BOARD_CONSOLE_UART              USART3
#define BOARD_CONSOLE_DE_GPIO_PORT      GPIOB
#define BOARD_CONSOLE_DE_PIN            GPIO_PIN_2

/* -------------------------------------------------------------------------- */
/* LED                                                                        */
/* -------------------------------------------------------------------------- *
 * PC13 - the onboard LED of the Blue Pill, active low (pin low = LED on).
 * Why PC13 and not PB12:
 *   - PB12 is NOT free on the reference hardware: the reference project's
 *     MX_GPIO_Init() (Core/Src/gpio.c) configures PB12..PB15 as *inputs*
 *     (buttons/straps), so driving PB12 here could fight an external driver.
 *   - PC13 needs no external wiring at all, so a blink is visible (and therefore
 *     proof that the scheduler runs) without a schematic.
 * Caveat: PC13 is in the low-drive "TAMPER-RTC" domain and must not sink much
 * current; the onboard LED is a few mA and that is what it is designed for.
 * -------------------------------------------------------------------------- */
#define BOARD_LED_GPIO_PORT             GPIOC
#define BOARD_LED_GPIO_CLK_ENABLE()     __HAL_RCC_GPIOC_CLK_ENABLE()
#define BOARD_LED_PIN                   GPIO_PIN_13
#define BOARD_LED_ON                    GPIO_PIN_RESET /*!< active low  */
#define BOARD_LED_OFF                   GPIO_PIN_SET   /*!< active high */

/* Handles -------------------------------------------------------------------- */
static UART_HandleTypeDef s_console;
static uint32_t s_clock_status = BOARD_CLOCK_ERR_HAL_INIT;

/* -------------------------------------------------------------------------- */
/* HAL MSP hooks                                                              */
/* -------------------------------------------------------------------------- */

/**
  * @brief  Global MSP init, called by HAL_Init().
  * @note   Mirrors the reference project's HAL_MspInit() (Core/Src/stm32f1xx_hal_msp.c):
  *         AFIO + PWR clocks on, JTAG-DP disabled but SW-DP kept so ST-Link/SWD
  *         debugging keeps working.
  */
void HAL_MspInit(void)
{
	__HAL_RCC_AFIO_CLK_ENABLE();
	__HAL_RCC_PWR_CLK_ENABLE();
	__HAL_AFIO_REMAP_SWJ_NOJTAG();
}

/**
  * @brief  UART MSP init, called from HAL_UART_Init().
  * @param  huart UART being initialised; only USART1 is routed on this board.
  */
void HAL_UART_MspInit(UART_HandleTypeDef *huart)
{
	GPIO_InitTypeDef gpio = {0};

	if (huart->Instance != BOARD_CONSOLE_UART) {
		return;
	}

	__HAL_RCC_USART3_CLK_ENABLE();
	__HAL_RCC_GPIOB_CLK_ENABLE();

	/* PB10 -> USART3_TX, alternate function push-pull. */
	gpio.Pin = GPIO_PIN_10;
	gpio.Mode = GPIO_MODE_AF_PP;
	gpio.Pull = GPIO_NOPULL;
	gpio.Speed = GPIO_SPEED_FREQ_HIGH;
	HAL_GPIO_Init(GPIOB, &gpio);

	/* PB11 -> USART3_RX, floating input (same as the reference project). */
	gpio.Pin = GPIO_PIN_11;
	gpio.Mode = GPIO_MODE_INPUT;
	gpio.Pull = GPIO_NOPULL;
	gpio.Speed = GPIO_SPEED_FREQ_HIGH;
	HAL_GPIO_Init(GPIOB, &gpio);

	/* PB2 -> RS485 DE. Drive it to the receive state *before* switching the pin
	   to an output so the driver never fights the bus during bring-up. */
	HAL_GPIO_WritePin(BOARD_CONSOLE_DE_GPIO_PORT, BOARD_CONSOLE_DE_PIN, GPIO_PIN_RESET);
	gpio.Pin = BOARD_CONSOLE_DE_PIN;
	gpio.Mode = GPIO_MODE_OUTPUT_PP;
	gpio.Pull = GPIO_NOPULL;
	gpio.Speed = GPIO_SPEED_FREQ_HIGH;
	HAL_GPIO_Init(BOARD_CONSOLE_DE_GPIO_PORT, &gpio);

	/* No NVIC enable and no USART3_IRQHandler on purpose: the console is
	   polled/blocking only, so there is no serial interrupt at all. */
}

/* -------------------------------------------------------------------------- */
/* Clock                                                                      */
/* -------------------------------------------------------------------------- */

/**
  * @brief  72 MHz from an 8 MHz HSE through the PLL (x9), SysTick at 1 kHz.
  */
void board_clock_init(void)
{
	RCC_OscInitTypeDef osc = {0};
	RCC_ClkInitTypeDef clk = {0};

	/* Flash prefetch, NVIC priority grouping, HAL time base (SysTick 1 ms). */
	if (HAL_Init() != HAL_OK) {
		s_clock_status = BOARD_CLOCK_ERR_HAL_INIT;
		return;
	}

	/* HSE 8 MHz -> PLL x9 = 72 MHz (reference: SystemClock_Config). */
	osc.OscillatorType = RCC_OSCILLATORTYPE_HSE;
	osc.HSEState = RCC_HSE_ON;
	osc.HSEPredivValue = RCC_HSE_PREDIV_DIV1;
	osc.HSIState = RCC_HSI_ON;
	osc.PLL.PLLState = RCC_PLL_ON;
	osc.PLL.PLLSource = RCC_PLLSOURCE_HSE;
	osc.PLL.PLLMUL = RCC_PLL_MUL9;
	if (HAL_RCC_OscConfig(&osc) != HAL_OK) {
		s_clock_status = BOARD_CLOCK_ERR_OSC;
		return;
	}

	/* AHB = 72 MHz, APB1 = 36 MHz, APB2 = 72 MHz, 2 wait states. */
	clk.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
	clk.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
	clk.AHBCLKDivider = RCC_SYSCLK_DIV1;
	clk.APB1CLKDivider = RCC_HCLK_DIV2;
	clk.APB2CLKDivider = RCC_HCLK_DIV1;
	if (HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_2) != HAL_OK) {
		s_clock_status = BOARD_CLOCK_ERR_CLK;
		return;
	}

	/*
	 * HAL_RCC_ClockConfig() already rearmed the 1 ms HAL time base, but the
	 * RTOS tick rate is stated here explicitly so that the scheduler tick does
	 * not silently depend on the HAL default (HCLK source, 1 kHz).
	 */
	if (HAL_SYSTICK_Config(SystemCoreClock / (uint32_t)configTICK_RATE_HZ) != 0U) {
		s_clock_status = BOARD_CLOCK_ERR_SYSTICK;
		return;
	}

	s_clock_status = BOARD_CLOCK_OK;
}

uint32_t board_clock_get_sysclk_hz(void)
{
	return (uint32_t)HAL_RCC_GetSysClockFreq();
}

uint32_t board_clock_get_pclk2_hz(void)
{
	return (uint32_t)HAL_RCC_GetPCLK2Freq();
}

uint32_t board_clock_get_pclk1_hz(void)
{
	return (uint32_t)HAL_RCC_GetPCLK1Freq();
}

uint32_t board_clock_status(void)
{
	return s_clock_status;
}

/* -------------------------------------------------------------------------- */
/* Console                                                                    */
/* -------------------------------------------------------------------------- */

void board_console_init(void)
{
	s_console.Instance = BOARD_CONSOLE_UART;
	s_console.Init.BaudRate = BOARD_CONSOLE_BAUDRATE;
	s_console.Init.WordLength = UART_WORDLENGTH_8B;
	s_console.Init.StopBits = UART_STOPBITS_1;
	s_console.Init.Parity = UART_PARITY_NONE;
	s_console.Init.Mode = UART_MODE_TX_RX;
	s_console.Init.HwFlowCtl = UART_HWCONTROL_NONE;
	s_console.Init.OverSampling = UART_OVERSAMPLING_16;

	/* If this fails there is no console to complain on; the banner simply never
	   shows up, which is itself the diagnostic. */
	(void)HAL_UART_Init(&s_console);
}

/**
  * @brief  Blocking transmit of one chunk; retries a few times if HAL reports
  *         HAL_BUSY (another caller owns the handle). Bounded, never infinite.
  */
static void prv_console_tx(const char *text, uint32_t length)
{
	uint32_t attempt;

	if ((text == NULL) || (length == 0U)) {
		return;
	}

	/* RS485 is half duplex: enable the driver, send, then release the bus so the
	   adapter can talk back. HAL_UART_Transmit() returns after TC, i.e. after the
	   last stop bit has left the shift register, so releasing here is safe. */
	HAL_GPIO_WritePin(BOARD_CONSOLE_DE_GPIO_PORT, BOARD_CONSOLE_DE_PIN, GPIO_PIN_SET);

	/* Retry only while the peripheral is still busy, i.e. before the first byte
	   was queued: re-sending a chunk that already started shifting bytes out
	   would duplicate its prefix on the wire. See the note in board.h. */
	for (attempt = 0U; attempt < BOARD_CONSOLE_TX_RETRIES; attempt++) {
		if (HAL_UART_Transmit(&s_console, (const uint8_t *)text, (uint16_t)length, BOARD_CONSOLE_TX_TIMEOUT_MS) !=
				HAL_BUSY) {
			break;
		}
	}

	HAL_GPIO_WritePin(BOARD_CONSOLE_DE_GPIO_PORT, BOARD_CONSOLE_DE_PIN, GPIO_PIN_RESET);
}

void board_console_putc(char c)
{
	char byte = c;

	prv_console_tx(&byte, 1U);
}

void board_console_write(const char *text, uint32_t length)
{
	if ((text == NULL) || (length == 0U)) {
		return;
	}

	/* HAL_UART_Transmit() takes a uint16_t size, so chunk long buffers. */
	while (length > 0U) {
		uint32_t chunk = (length > 0xFFFFU) ? 0xFFFFU : length;

		prv_console_tx(text, chunk);
		text += chunk;
		length -= chunk;
	}
}

/* -------------------------------------------------------------------------- */
/* LED                                                                        */
/* -------------------------------------------------------------------------- */

void board_led_init(void)
{
	GPIO_InitTypeDef gpio = {0};

	BOARD_LED_GPIO_CLK_ENABLE();

	/* Drive the pin to its inactive level *before* switching it to output, so
	   the LED does not flash during init. */
	HAL_GPIO_WritePin(BOARD_LED_GPIO_PORT, BOARD_LED_PIN, BOARD_LED_OFF);

	gpio.Pin = BOARD_LED_PIN;
	gpio.Mode = GPIO_MODE_OUTPUT_PP;
	gpio.Pull = GPIO_NOPULL;
	gpio.Speed = GPIO_SPEED_FREQ_LOW;
	HAL_GPIO_Init(BOARD_LED_GPIO_PORT, &gpio);
}

void board_led_toggle(void)
{
	HAL_GPIO_TogglePin(BOARD_LED_GPIO_PORT, BOARD_LED_PIN);
}
