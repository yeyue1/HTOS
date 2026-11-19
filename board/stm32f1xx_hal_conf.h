/**
 * @file board/stm32f1xx_hal_conf.h
 * @brief HTOS STM32F103C8T6 板级层的 HAL 配置头。
 *
 * 取自参考工程（openOCD_vscode 标准调试 + HTOS + 栈回溯）中本型号已验证可用的
 * CubeMX 文件（Core/Inc/stm32f1xx_hal_conf.h），并按本固件实际链接到的模块裁剪。
 *
 * 启用的模块：CORTEX、RCC、GPIO、UART、PWR（外加始终开启的 HAL_MODULE_ENABLED），
 * 以及 HAL_DMA_MODULE_ENABLED——原因见下面的"强制例外"一节。
 *
 * assert_param 不会拉进 printf：USE_FULL_ASSERT 处于关闭状态，因此
 * assert_param(expr) 是纯粹的空操作（见本文件末尾）。
 */

/* 防止重复包含 --------------------------------------------------------------*/
#ifndef __STM32F1xx_HAL_CONF_H
#define __STM32F1xx_HAL_CONF_H

#ifdef __cplusplus
extern "C" {
#endif

/* ########################## 模块选择 ###################################### */
/**
 * @brief 本工程需要编译进 HAL 驱动层的模块清单。
 */
#define HAL_MODULE_ENABLED
#define HAL_CORTEX_MODULE_ENABLED
#define HAL_RCC_MODULE_ENABLED
#define HAL_GPIO_MODULE_ENABLED
#define HAL_UART_MODULE_ENABLED
#define HAL_PWR_MODULE_ENABLED
#define HAL_IWDG_MODULE_ENABLED

/* ########################## 强制例外 ###################################### */
/*
 * STM32F1 的 HAL 无法做到"开启 UART、关闭 DMA"：
 *   - vendor/STM32F1xx_HAL_Driver/Inc/stm32f1xx_hal_uart.h:182,184
 *       DMA_HandleTypeDef *hdmatx / *hdmarx;   （这两个成员没有 #ifdef 包着）
 *   - vendor/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_uart.c:1427,1566,1900,2133
 *       无条件调用 HAL_DMA_Start_IT / HAL_DMA_Abort / HAL_DMA_GetError /
 *       HAL_DMA_Abort_IT，因此只要 HAL_UART_MODULE_ENABLED 打开，
 *       链接期就必须能找到这些 DMA *符号*。
 * 所以 HAL_DMA_MODULE_ENABLED 是唯一被迫加上的模块。本固件从不使用 DMA：
 * 没有打开任何 DMA 时钟，没有创建任何 DMA 句柄，控制台是阻塞轮询式的。
 * 参考工程在本型号上也同样定义了 HAL_DMA_MODULE_ENABLED（其配置行
 * "HAL_DMA_MODULE_ENABLED"）。
 * 如果坚持要严格裁剪模块清单，就只能把控制台从 HAL_UART_* 换成直接操作
 * CMSIS 寄存器的写法。
 */
#define HAL_DMA_MODULE_ENABLED

/*
 * HAL_RCC_ClockConfig() 会调用 __HAL_FLASH_GET_LATENCY / __HAL_FLASH_SET_LATENCY
 * （vendor/.../Src/stm32f1xx_hal_rcc.c:831..920），且没有模块保护，因此 FLASH 的
 * *宏* 必须可见。这里只包含头文件，FLASH 模块本身保持关闭：本固件从不调用任何
 * HAL_FLASH_* 函数，而模块关闭时 stm32f1xx_hal_flash.c 本来就是空的。
 */
#include "stm32f1xx_hal_flash.h"

/* ########################## 振荡器参数适配 ################################ */
/**
 * @brief 外部高速晶振（HSE）的频率，按本板实际情况设定。
 */
#if !defined(HSE_VALUE)
#define HSE_VALUE 8000000U /*!< 外部晶振频率（Hz），本板为 8 MHz */
#endif /* HSE_VALUE */

#if !defined(HSE_STARTUP_TIMEOUT)
#define HSE_STARTUP_TIMEOUT 100U /*!< HSE 起振超时（ms） */
#endif /* HSE_STARTUP_TIMEOUT */

/**
 * @brief 内部高速振荡器（HSI）的频率。
 */
#if !defined(HSI_VALUE)
#define HSI_VALUE 8000000U /*!< 内部振荡器频率（Hz） */
#endif /* HSI_VALUE */

/**
 * @brief 内部低速振荡器（LSI）的频率。
 */
#if !defined(LSI_VALUE)
#define LSI_VALUE 40000U /*!< LSI 典型频率（Hz） */
#endif /* LSI_VALUE */

/**
 * @brief 外部低速晶振（LSE）的频率。
 */
#if !defined(LSE_VALUE)
#define LSE_VALUE 32768U /*!< 外部低速晶振频率（Hz） */
#endif /* LSE_VALUE */

#if !defined(LSE_STARTUP_TIMEOUT)
#define LSE_STARTUP_TIMEOUT 5000U /*!< LSE 起振超时（ms） */
#endif /* LSE_STARTUP_TIMEOUT */

/* ########################### 系统配置 ##################################### */
/**
 * @brief HAL 系统级配置。
 */
#define VDD_VALUE 3300U /*!< 供电电压 VDD（mV） */
#define TICK_INT_PRIORITY 15U /*!< HAL 时基中断优先级（最低） */
#define USE_RTOS 0U
#define PREFETCH_ENABLE 1U

/*
 * 全部回调注册开关保持 0：本工程用不到 HAL 的"注册回调"机制，
 * 关掉可以省掉每个外设句柄里的函数指针表和相应 ROM。
 */
#define USE_HAL_ADC_REGISTER_CALLBACKS 0U
#define USE_HAL_CAN_REGISTER_CALLBACKS 0U
#define USE_HAL_CEC_REGISTER_CALLBACKS 0U
#define USE_HAL_DAC_REGISTER_CALLBACKS 0U
#define USE_HAL_ETH_REGISTER_CALLBACKS 0U
#define USE_HAL_HCD_REGISTER_CALLBACKS 0U
#define USE_HAL_I2C_REGISTER_CALLBACKS 0U
#define USE_HAL_I2S_REGISTER_CALLBACKS 0U
#define USE_HAL_MMC_REGISTER_CALLBACKS 0U
#define USE_HAL_NAND_REGISTER_CALLBACKS 0U
#define USE_HAL_NOR_REGISTER_CALLBACKS 0U
#define USE_HAL_PCCARD_REGISTER_CALLBACKS 0U
#define USE_HAL_PCD_REGISTER_CALLBACKS 0U
#define USE_HAL_RTC_REGISTER_CALLBACKS 0U
#define USE_HAL_SD_REGISTER_CALLBACKS 0U
#define USE_HAL_SMARTCARD_REGISTER_CALLBACKS 0U
#define USE_HAL_IRDA_REGISTER_CALLBACKS 0U
#define USE_HAL_SRAM_REGISTER_CALLBACKS 0U
#define USE_HAL_SPI_REGISTER_CALLBACKS 0U
#define USE_HAL_TIM_REGISTER_CALLBACKS 0U
#define USE_HAL_UART_REGISTER_CALLBACKS 0U
#define USE_HAL_USART_REGISTER_CALLBACKS 0U
#define USE_HAL_WWDG_REGISTER_CALLBACKS 0U

/* ########################## 断言选择 ###################################### */
/**
 * @brief 取消下面一行的注释即可让 HAL 驱动里的 assert_param 真正展开。
 *
 * 这里刻意保持关闭：assert_param 绝不允许把 printf()/assert_failed() 拉进本固件
 * （那会额外占用 Flash 并引入不可重入的格式化输出）。
 */
/* #define USE_FULL_ASSERT    1U */

/* ################## SPI 外设配置 ########################################## */
#define USE_SPI_CRC 0U

/* 头文件包含 ----------------------------------------------------------------*/
/**
 * @brief 按上面启用的模块包含对应的 HAL 头文件。
 */

#ifdef HAL_RCC_MODULE_ENABLED
#include "stm32f1xx_hal_rcc.h"
#endif /* HAL_RCC_MODULE_ENABLED */

#ifdef HAL_GPIO_MODULE_ENABLED
#include "stm32f1xx_hal_gpio.h"
#endif /* HAL_GPIO_MODULE_ENABLED */

#ifdef HAL_CORTEX_MODULE_ENABLED
#include "stm32f1xx_hal_cortex.h"
#endif /* HAL_CORTEX_MODULE_ENABLED */

#ifdef HAL_DMA_MODULE_ENABLED
#include "stm32f1xx_hal_dma.h"
#endif /* HAL_DMA_MODULE_ENABLED */

#ifdef HAL_PWR_MODULE_ENABLED
#include "stm32f1xx_hal_pwr.h"
#endif /* HAL_PWR_MODULE_ENABLED */

#ifdef HAL_UART_MODULE_ENABLED
#include "stm32f1xx_hal_uart.h"
#endif /* HAL_UART_MODULE_ENABLED */

#ifdef HAL_IWDG_MODULE_ENABLED
#include "stm32f1xx_hal_iwdg.h"
#endif /* HAL_IWDG_MODULE_ENABLED */

/* 对外宏 --------------------------------------------------------------------*/
#ifdef USE_FULL_ASSERT
/**
 * @brief assert_param 宏用于检查函数入参的合法性。
 *
 * @param[in] expr 待检查的表达式。为假时调用 assert_failed()，由其报告失败处的
 *                 源文件名与行号；为真时什么也不做。
 * @return 无返回值。
 */
#define assert_param(expr) ((expr) ? (void)0U : assert_failed((uint8_t *)__FILE__, __LINE__))
/* 对外函数 ------------------------------------------------------------------*/
void assert_failed(uint8_t *file, uint32_t line);
#else
/* 空断言：零代码、不引 printf、也不在 Flash 里留任何字符串常量。 */
#define assert_param(expr) ((void)0U)
#endif /* USE_FULL_ASSERT */

#ifdef __cplusplus
}
#endif

#endif /* __STM32F1xx_HAL_CONF_H */
