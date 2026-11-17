/**
 * @file htPort.h
 * @brief Cortex-M 特定的移植层定义
 */
#ifndef HT_PORT_H
#define HT_PORT_H

#include "htconfig.h"
#include <stdint.h>

/*
 * The Cortex-M port is written for the GNU Arm toolchain.  The core header
 * can be overridden by a board-specific CMSIS header when the default M3
 * header is not the right one.
 */
#ifndef HT_PORT_CORE_HEADER
#define HT_PORT_CORE_HEADER "core_cm3.h"
#endif

/*
 * The CMSIS core header refers to IRQn_Type and SCB, which are supplied by the
 * device header.  Including the device header first keeps the port buildable on
 * a real target instead of only against the host stubs.  Override this when the
 * board uses a different STM32 family.
 */
#ifndef HT_PORT_DEVICE_HEADER
#define HT_PORT_DEVICE_HEADER "stm32f1xx.h"
#endif

/*
 * Stack-range checks are optional because SRAM placement is a linker-script
 * property, not an RTOS invariant.  Enable them per target and provide
 * assembler-compatible integer constants when a cheap guard is desired:
 *
 *   -DHT_PORT_VALIDATE_STACK=1
 *   -DHT_PORT_SRAM_START=0x20000000
 *   -DHT_PORT_SRAM_END=0x20005000
 */
#ifndef HT_PORT_VALIDATE_STACK
#define HT_PORT_VALIDATE_STACK 0
#endif

#if HT_PORT_VALIDATE_STACK
#ifndef HT_PORT_SRAM_START
#error "HT_PORT_SRAM_START must be defined when HT_PORT_VALIDATE_STACK is enabled"
#endif
#ifndef HT_PORT_SRAM_END
#error "HT_PORT_SRAM_END must be defined when HT_PORT_VALIDATE_STACK is enabled"
#endif
#endif

/**
 * 启动第一个任务
 */
void vPortStartFirstTask(void);

/**
 * PendSV中断处理函数
 * 用于任务切换
 */
void PendSV_Handler(void);

/**
 * SVC中断处理函数
 * 用于启动第一个任务
 */
void SVC_Handler(void);

#endif /* HT_PORT_H */
