/**
 * @file stm32f1xx.h
 * @brief 主机测试用的 STM32F1 设备头替身，只把 CMSIS 转发给 core_cm3.h。
 *
 * 内核与端口层会 #include "stm32f1xx.h"；主机上没有真实设备头，
 * 这个空壳满足该依赖，真实固件构建时由 ARM 工具链的 CMSIS/HAL 提供同名文件。
 */
#ifndef HTOS_TEST_STM32F1XX_H
#define HTOS_TEST_STM32F1XX_H

/* 只引入 CMSIS 核心定义（SCB、中断开关、屏障指令），不引入任何外设寄存器。 */
#include "core_cm3.h"

#endif /* HTOS_TEST_STM32F1XX_H */
