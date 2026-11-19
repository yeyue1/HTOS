/**
 * @file board/board.c
 * @brief 板级初始化：72 MHz 时钟树、1 kHz tick、USART3（RS485）控制台、LED。
 *
 * 本文件全部通过 HAL/CMSIS 宏与 API 访问外设，没有出现任何裸外设地址。
 * 时钟序列直接照搬参考工程中本型号已验证可用的 SystemClock_Config()
 * （openOCD_vscode 标准调试 + HTOS + 栈回溯工程，Core/Src/main.c）；
 * USART3 的引脚复用来自参考工程 RS485 口的配置（BSP/bsp_usart.h 与
 * BSP/bsp_gpio.h：PB10 = USART3_TX，PB11 = USART3_RX，PB2 = RS485 方向脚）。
 *
 * 注意：早期版本这里的控制台是 USART1（PA9/PA10），现在已改为 USART3 走 RS485
 * 半双工，波特率 9600；下面各段的注释与代码一致。
 */

#include "board.h"

#include "stm32f1xx_hal.h"
#include "htconfig.h"

/*
 * HAL 时基（HAL_InitTick，HAL_UART_Transmit 的超时计时就靠它）与 RTOS tick 共用
 * 同一个 SysTick，因此 RTOS 的 tick 频率必须正好等于 HAL 的 1 ms 时基。
 * 这里让编译直接失败，而不是带着一个静默的不匹配上路。
 */
#if (configTICK_RATE_HZ != 1000U)
#error "board.c requires configTICK_RATE_HZ == 1000 (shared SysTick with the HAL 1 ms time base)"
#endif

/* -------------------------------------------------------------------------- */
/* 控制台：USART3 走 RS485                                                    */
/* -------------------------------------------------------------------------- */
/* 控制台是从 RS485 收发器出去的，不是 TTL 电平：
 *   - USART3_TX = PB10（复用推挽），USART3_RX = PB11（浮空输入）
 *   - RS485 方向：PB2 高电平使能驱动器，PB2 低电平使能接收器
 *   - 9600 8N1：参考工程里 RS485 口的默认波特率
 *     （StorageData.usartusartbaud[2] = 9600，见 storageUse.c 中的 StorageData）
 * 引脚与方向脚都取自参考工程：
 *   BSP/bsp_usart.h  RS485_USART_TX_GPIO_PIN = GPIO_Pin_10（PB10）
 *   BSP/bsp_usart.h  RS485_USART_RX_GPIO_PIN = GPIO_Pin_11（PB11）
 *   BSP/bsp_gpio.h   RS485_RW_PORT = GPIOB，RS485_RW_PIN = GPIO_Pin_2
 * 实板已验证：PB2 拉高时收发器驱动总线，USB-RS485 适配器能收到文字；
 * PB2 悬空时一个字节也收不到。
 * USART3 挂在 APB1 上，因此它的内核时钟是 PCLK1（72 MHz SYSCLK 下为 36 MHz）。
 * -------------------------------------------------------------------------- */
#define BOARD_CONSOLE_UART USART3
#define BOARD_CONSOLE_DE_GPIO_PORT GPIOB
#define BOARD_CONSOLE_DE_PIN GPIO_PIN_2

/* -------------------------------------------------------------------------- */
/* LED                                                                        */
/* -------------------------------------------------------------------------- */
/* PC13 是 Blue Pill 的板载 LED，低电平点亮（引脚为低 = 灯亮）。
 * 为什么用 PC13 而不是 PB12：
 *   - PB12 在参考硬件上并不空闲：参考工程的 MX_GPIO_Init()（Core/Src/gpio.c）
 *     把 PB12..PB15 配成*输入*（按键/跳线），这里再去驱动 PB12 就可能和外部
 *     驱动器打架。
 *   - PC13 完全不需要外部接线，不拿原理图也能看到灯在闪，因此它本身就是
 *     "调度器在跑"的证据。
 * 注意：PC13 属于低驱动能力的 "TAMPER-RTC" 域，不能灌太大电流；板载 LED 只有
 * 几个 mA，本来就是为它设计的。
 * -------------------------------------------------------------------------- */
#define BOARD_LED_GPIO_PORT GPIOC
#define BOARD_LED_GPIO_CLK_ENABLE() __HAL_RCC_GPIOC_CLK_ENABLE()
#define BOARD_LED_PIN GPIO_PIN_13
#define BOARD_LED_ON GPIO_PIN_RESET /*!< 低电平点亮 */
#define BOARD_LED_OFF GPIO_PIN_SET /*!< 高电平熄灭 */

/* 句柄与状态 ----------------------------------------------------------------- */
static UART_HandleTypeDef s_console;
static IWDG_HandleTypeDef s_watchdog;
static uint32_t s_clock_status = BOARD_CLOCK_ERR_HAL_INIT;

/* -------------------------------------------------------------------------- */
/* HAL MSP 钩子                                                               */
/* -------------------------------------------------------------------------- */

/**
 * @brief 全局 MSP 初始化，由 HAL_Init() 调用。
 *
 * 对应参考工程的 HAL_MspInit()（Core/Src/stm32f1xx_hal_msp.c）：
 * 打开 AFIO 与 PWR 时钟，关闭 JTAG-DP 但保留 SW-DP，使 ST-Link/SWD 调试仍可用。
 *
 * @note 关 JTAG、留 SWD 这一句很重要：它省下 PB3/PB4/PA15 三个引脚，
 *       同时保证调试器不被锁在门外。
 */
void HAL_MspInit(void)
{
	__HAL_RCC_AFIO_CLK_ENABLE();
	__HAL_RCC_PWR_CLK_ENABLE();
	__HAL_AFIO_REMAP_SWJ_NOJTAG();
}

/**
 * @brief UART 的 MSP 初始化，由 HAL_UART_Init() 回调。
 *
 * @param[in] huart 正在初始化的 UART；本板只接了 USART3（控制台），
 *                  其他实例直接返回，不做任何配置。
 */
void HAL_UART_MspInit(UART_HandleTypeDef *huart)
{
	GPIO_InitTypeDef gpio = {0};

	if (huart->Instance != BOARD_CONSOLE_UART) {
		return;
	}

	__HAL_RCC_USART3_CLK_ENABLE();
	__HAL_RCC_GPIOB_CLK_ENABLE();

	/* PB10 -> USART3_TX，复用推挽输出。 */
	gpio.Pin = GPIO_PIN_10;
	gpio.Mode = GPIO_MODE_AF_PP;
	gpio.Pull = GPIO_NOPULL;
	gpio.Speed = GPIO_SPEED_FREQ_HIGH;
	HAL_GPIO_Init(GPIOB, &gpio);

	/* PB11 -> USART3_RX，浮空输入（与参考工程一致）。 */
	gpio.Pin = GPIO_PIN_11;
	gpio.Mode = GPIO_MODE_INPUT;
	gpio.Pull = GPIO_NOPULL;
	gpio.Speed = GPIO_SPEED_FREQ_HIGH;
	HAL_GPIO_Init(GPIOB, &gpio);

	/* PB2 -> RS485 DE。先把输出寄存器写成接收态，再把引脚切成输出，
	   这样初始化过程中驱动器不会去抢总线。 */
	HAL_GPIO_WritePin(BOARD_CONSOLE_DE_GPIO_PORT, BOARD_CONSOLE_DE_PIN, GPIO_PIN_RESET);
	gpio.Pin = BOARD_CONSOLE_DE_PIN;
	gpio.Mode = GPIO_MODE_OUTPUT_PP;
	gpio.Pull = GPIO_NOPULL;
	gpio.Speed = GPIO_SPEED_FREQ_HIGH;
	HAL_GPIO_Init(BOARD_CONSOLE_DE_GPIO_PORT, &gpio);

	/* 这里故意不使能 NVIC，也不提供 USART3_IRQHandler：控制台只用轮询/阻塞发送，
	   因此整条串口路径上一个中断都没有。 */
}

/* -------------------------------------------------------------------------- */
/* 时钟                                                                       */
/* -------------------------------------------------------------------------- */

/**
 * @brief 用 8 MHz HSE 经 PLL x9 得到 72 MHz，并把 SysTick 配成 1 kHz。
 *
 * @note 这是唯一允许配置时钟树的地方：HAL_Init() 之后任何一步失败都只记录到
 *       s_clock_status 里然后返回，绝不在这里死等或死循环，
 *       这样即使只剩内部 HSI，串口横幅也能把降级状态报出来。
 */
void board_clock_init(void)
{
	RCC_OscInitTypeDef osc = {0};
	RCC_ClkInitTypeDef clk = {0};

	/* Flash 预取、NVIC 优先级分组、HAL 时基（SysTick 1 ms）。 */
	if (HAL_Init() != HAL_OK) {
		s_clock_status = BOARD_CLOCK_ERR_HAL_INIT;
		return;
	}

	/* HSE 8 MHz -> PLL x9 = 72 MHz（对应参考工程 SystemClock_Config）。 */
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

	/* AHB = 72 MHz，APB1 = 36 MHz，APB2 = 72 MHz，Flash 2 个等待周期。 */
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
	 * HAL_RCC_ClockConfig() 其实已经重装了 1 ms 的 HAL 时基，但这里仍显式再配一次
	 * RTOS tick，避免调度器的心跳静默地依赖 HAL 的默认值（HCLK 源、1 kHz）。
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
/* 控制台                                                                     */
/* -------------------------------------------------------------------------- */
/**
 * @brief 初始化控制台串口 USART3 并配置 RS485 方向脚。
 *
 * @note 失败时没有别的地方可抱怨：横幅干脆不出现，这个"不出现"本身就是诊断信息，
 *       所以这里刻意 (void) 掉返回值。
 */
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

	/* 这里若失败就没有控制台可以喊了：横幅干脆不出现，这本身就是诊断信息。 */
	(void)HAL_UART_Init(&s_console);
}

/**
 * @brief 阻塞发送一块数据；HAL 报 HAL_BUSY 时重试若干次。
 *
 * @param[in] text 待发送字节，可为 NULL（此时直接返回）。
 * @param[in] length 字节数，0 时直接返回。
 * @note 只在 HAL_BUSY（句柄被别的调用者占着，一个字节都还没排进去）时重试，
 *       次数由 BOARD_CONSOLE_TX_RETRIES 限制，绝不会无限循环。
 * @warning 超时（HAL_TIMEOUT）不重试：HAL_UART_Transmit() 不会告诉你已经有多少
 *          字节移出了移位寄存器，从头重发会让前缀在总线上重复一遍。
 */
static void prv_console_tx(const char *text, uint32_t length)
{
	uint32_t attempt;

	if ((text == NULL) || (length == 0U)) {
		return;
	}

	/* RS485 是半双工：先使能驱动器，发完再把总线放开，让适配器有机会回话。
	   HAL_UART_Transmit() 是在 TC 之后才返回的，也就是最后一个停止位已经移出
	   移位寄存器，因此这时释放驱动器是安全的。 */
	HAL_GPIO_WritePin(BOARD_CONSOLE_DE_GPIO_PORT, BOARD_CONSOLE_DE_PIN, GPIO_PIN_SET);

	/* 只在外设仍然忙（一个字节都还没排队）时才重试：对已经开始往外移位的数据块
	   重发，会让它的前缀在总线上出现两遍。详见 board.h 里的说明。 */
	for (attempt = 0U; attempt < BOARD_CONSOLE_TX_RETRIES; attempt++) {
		if (HAL_UART_Transmit(&s_console, (const uint8_t *)text, (uint16_t)length, BOARD_CONSOLE_TX_TIMEOUT_MS) !=
				HAL_BUSY) {
			break;
		}
	}

	HAL_GPIO_WritePin(BOARD_CONSOLE_DE_GPIO_PORT, BOARD_CONSOLE_DE_PIN, GPIO_PIN_RESET);
}

/**
 * @brief 发送一个字符。
 *
 * @param[in] c 待发送的字符。
 * @note 内部按单字符块走阻塞发送，复用 prv_console_tx() 的半双工时序。
 */
void board_console_putc(char c)
{
	char byte = c;

	prv_console_tx(&byte, 1U);
}

/**
 * @brief 发送一段缓冲区，按 HAL 的长度上限自动分块。
 *
 * @param[in] text 待发送字节，可为 NULL（此时直接返回）。
 * @param[in] length 字节数。
 */
void board_console_write(const char *text, uint32_t length)
{
	if ((text == NULL) || (length == 0U)) {
		return;
	}

	/* HAL_UART_Transmit() 的长度参数是 uint16_t，长缓冲区必须切块。 */
	while (length > 0U) {
		uint32_t chunk = (length > 0xFFFFU) ? 0xFFFFU : length;

		prv_console_tx(text, chunk);
		text += chunk;
		length -= chunk;
	}
}

/* -------------------------------------------------------------------------- */
/* 硬件独立看门狗                                                             */
/* -------------------------------------------------------------------------- */
/**
 * @brief 启动 IWDG：256 分频、12 位重装值，喂狗前名义超时约 26.2 s。
 *
 * @return 0 表示已启动；非 0 表示 HAL_IWDG_Init() 失败。
 * @note  一旦启动就无法用软件关闭，只能靠复位清掉；因此本函数只应在
 *        监控任务与注册表都就绪之后调用一次。
 */
uint32_t board_watchdog_init(void)
{
	s_watchdog.Instance = IWDG;
	s_watchdog.Init.Prescaler = IWDG_PRESCALER_256;
	s_watchdog.Init.Reload = 0x0FFFU;
	return (HAL_IWDG_Init(&s_watchdog) == HAL_OK) ? 0U : 1U;
}

/**
 * @brief 刷新 IWDG 计数器。
 *
 * @note 由监控任务在 htWatchdogCheck() 判定健康后调用，任何更早的刷新
 *       都会让漏报的任务被无条件放行。
 */
void board_watchdog_refresh(void)
{
	(void)HAL_IWDG_Refresh(&s_watchdog);
}

/* -------------------------------------------------------------------------- */
/* LED                                                                        */
/* -------------------------------------------------------------------------- */
/**
 * @brief 把 LED 引脚配成推挽输出，初态为灭。
 */
void board_led_init(void)
{
	GPIO_InitTypeDef gpio = {0};

	BOARD_LED_GPIO_CLK_ENABLE();

	/* 先把输出寄存器写成灭的（无效）电平，再把引脚切成输出，
	   这样初始化过程中 LED 不会闪一下。 */
	HAL_GPIO_WritePin(BOARD_LED_GPIO_PORT, BOARD_LED_PIN, BOARD_LED_OFF);

	gpio.Pin = BOARD_LED_PIN;
	gpio.Mode = GPIO_MODE_OUTPUT_PP;
	gpio.Pull = GPIO_NOPULL;
	gpio.Speed = GPIO_SPEED_FREQ_LOW;
	HAL_GPIO_Init(BOARD_LED_GPIO_PORT, &gpio);
}

/**
 * @brief 翻转 LED 状态。
 *
 * @note HAL_GPIO_TogglePin 只对 BSRR 做一次写操作，是原子动作，可以在中断里调用。
 */
void board_led_toggle(void)
{
	HAL_GPIO_TogglePin(BOARD_LED_GPIO_PORT, BOARD_LED_PIN);
}
