/**
  ******************************************************************************
  * @file    board/stm32f1xx_hal_conf.h
  * @brief   HAL configuration for the HTOS STM32F103C8T6 board bring-up layer.
  *
  * Derived from the known-good CubeMX file of the reference project
  * (openOCD_vscode standard-debug + HTOS + stack-unwind, Core/Inc/stm32f1xx_hal_conf.h)
  * and trimmed to what this firmware actually links.
  *
  * Enabled modules: CORTEX, RCC, GPIO, UART, PWR (+ always-on HAL_MODULE_ENABLED)
  * plus HAL_DMA_MODULE_ENABLED, see "FORCED EXCEPTION" below.
  *
  * No printf is pulled in by assert_param: USE_FULL_ASSERT is off, so
  * assert_param(expr) is a pure no-op (see the bottom of this file).
  ******************************************************************************
  */

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __STM32F1xx_HAL_CONF_H
#define __STM32F1xx_HAL_CONF_H

#ifdef __cplusplus
 extern "C" {
#endif

/* ########################## Module Selection ############################## */
/**
  * @brief This is the list of modules to be used in the HAL driver
  */
#define HAL_MODULE_ENABLED
#define HAL_CORTEX_MODULE_ENABLED
#define HAL_RCC_MODULE_ENABLED
#define HAL_GPIO_MODULE_ENABLED
#define HAL_UART_MODULE_ENABLED
#define HAL_PWR_MODULE_ENABLED

/* ########################## FORCED EXCEPTION ############################## */
/*
 * STM32F1 HAL cannot have "UART enabled, DMA disabled":
 *   - vendor/STM32F1xx_HAL_Driver/Inc/stm32f1xx_hal_uart.h:182,184
 *       DMA_HandleTypeDef *hdmatx / *hdmarx;   (no #ifdef around them)
 *   - vendor/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_uart.c:1427,1566,1900,2133
 *       calls HAL_DMA_Start_IT / HAL_DMA_Abort / HAL_DMA_GetError / HAL_DMA_Abort_IT
 *       unconditionally, so the DMA *symbols* are needed at link time as soon as
 *       HAL_UART_MODULE_ENABLED is on.
 * Therefore HAL_DMA_MODULE_ENABLED is the single forced addition.  DMA is never
 * used by this firmware: no DMA clock is enabled, no DMA handle is created, and
 * the console is blocking-polled.  The reference project for this exact MCU also
 * defines HAL_DMA_MODULE_ENABLED (its conf line "HAL_DMA_MODULE_ENABLED").
 * If you would rather keep the module list strict, the console has to be moved
 * from HAL_UART_* to raw CMSIS register access instead.
 */
#define HAL_DMA_MODULE_ENABLED

/*
 * HAL_RCC_ClockConfig() calls __HAL_FLASH_GET_LATENCY / __HAL_FLASH_SET_LATENCY
 * (vendor/.../Src/stm32f1xx_hal_rcc.c:831..920) with no module guard, so the
 * FLASH *macros* must be visible.  Only the header is included, the FLASH module
 * itself stays disabled: this firmware never calls a HAL_FLASH_* function, and
 * stm32f1xx_hal_flash.c is empty when the module is off.
 */
#include "stm32f1xx_hal_flash.h"

/* ########################## Oscillator Values adaptation ####################*/
/**
  * @brief Adjust the value of External High Speed oscillator (HSE) used in your application.
  */
#if !defined  (HSE_VALUE)
  #define HSE_VALUE    8000000U /*!< Value of the External oscillator in Hz */
#endif /* HSE_VALUE */

#if !defined  (HSE_STARTUP_TIMEOUT)
  #define HSE_STARTUP_TIMEOUT    100U   /*!< Time out for HSE start up, in ms */
#endif /* HSE_STARTUP_TIMEOUT */

/**
  * @brief Internal High Speed oscillator (HSI) value.
  */
#if !defined  (HSI_VALUE)
  #define HSI_VALUE    8000000U /*!< Value of the Internal oscillator in Hz*/
#endif /* HSI_VALUE */

/**
  * @brief Internal Low Speed oscillator (LSI) value.
  */
#if !defined  (LSI_VALUE)
 #define LSI_VALUE               40000U    /*!< LSI Typical Value in Hz */
#endif /* LSI_VALUE */

/**
  * @brief External Low Speed oscillator (LSE) value.
  */
#if !defined  (LSE_VALUE)
  #define LSE_VALUE    32768U /*!< Value of the External oscillator in Hz*/
#endif /* LSE_VALUE */

#if !defined  (LSE_STARTUP_TIMEOUT)
  #define LSE_STARTUP_TIMEOUT    5000U   /*!< Time out for LSE start up, in ms */
#endif /* LSE_STARTUP_TIMEOUT */

/* ########################### System Configuration ######################### */
/**
  * @brief This is the HAL system configuration section
  */
#define  VDD_VALUE                    3300U /*!< Value of VDD in mv */
#define  TICK_INT_PRIORITY            15U   /*!< HAL time-base interrupt priority (lowest) */
#define  USE_RTOS                     0U
#define  PREFETCH_ENABLE              1U

#define  USE_HAL_ADC_REGISTER_CALLBACKS         0U
#define  USE_HAL_CAN_REGISTER_CALLBACKS         0U
#define  USE_HAL_CEC_REGISTER_CALLBACKS         0U
#define  USE_HAL_DAC_REGISTER_CALLBACKS         0U
#define  USE_HAL_ETH_REGISTER_CALLBACKS         0U
#define  USE_HAL_HCD_REGISTER_CALLBACKS         0U
#define  USE_HAL_I2C_REGISTER_CALLBACKS         0U
#define  USE_HAL_I2S_REGISTER_CALLBACKS         0U
#define  USE_HAL_MMC_REGISTER_CALLBACKS         0U
#define  USE_HAL_NAND_REGISTER_CALLBACKS        0U
#define  USE_HAL_NOR_REGISTER_CALLBACKS         0U
#define  USE_HAL_PCCARD_REGISTER_CALLBACKS      0U
#define  USE_HAL_PCD_REGISTER_CALLBACKS         0U
#define  USE_HAL_RTC_REGISTER_CALLBACKS         0U
#define  USE_HAL_SD_REGISTER_CALLBACKS          0U
#define  USE_HAL_SMARTCARD_REGISTER_CALLBACKS   0U
#define  USE_HAL_IRDA_REGISTER_CALLBACKS        0U
#define  USE_HAL_SRAM_REGISTER_CALLBACKS        0U
#define  USE_HAL_SPI_REGISTER_CALLBACKS         0U
#define  USE_HAL_TIM_REGISTER_CALLBACKS         0U
#define  USE_HAL_UART_REGISTER_CALLBACKS        0U
#define  USE_HAL_USART_REGISTER_CALLBACKS       0U
#define  USE_HAL_WWDG_REGISTER_CALLBACKS        0U

/* ########################## Assert Selection ############################## */
/**
  * @brief Uncomment the line below to expand the "assert_param" macro in the
  *        HAL drivers code.  Kept OFF on purpose: assert_param must never pull
  *        printf()/assert_failed() into this firmware.
  */
/* #define USE_FULL_ASSERT    1U */

/* ################## SPI peripheral configuration ########################## */
#define USE_SPI_CRC                     0U

/* Includes ------------------------------------------------------------------*/
/**
  * @brief Include module's header file
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

/* Exported macro ------------------------------------------------------------*/
#ifdef  USE_FULL_ASSERT
/**
  * @brief  The assert_param macro is used for function's parameters check.
  * @param  expr If expr is false, it calls assert_failed function
  *         which reports the name of the source file and the source
  *         line number of the call that failed.
  *         If expr is true, it returns no value.
  * @retval None
  */
#define assert_param(expr) ((expr) ? (void)0U : assert_failed((uint8_t *)__FILE__, __LINE__))
/* Exported functions ------------------------------------------------------- */
void assert_failed(uint8_t* file, uint32_t line);
#else
/* No-op assertion: zero code, no printf, no string literals in flash. */
#define assert_param(expr) ((void)0U)
#endif /* USE_FULL_ASSERT */

#ifdef __cplusplus
}
#endif

#endif /* __STM32F1xx_HAL_CONF_H */
