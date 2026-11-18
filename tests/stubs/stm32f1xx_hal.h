/**
 * @file stm32f1xx_hal.h
 * @brief 主机测试用的 HAL 头替身：仅作为 CMSIS/HAL 的编译边界存在。
 *
 * 内核源码会 #include "stm32f1xx.h" / "stm32f1xx_hal.h"，主机上没有真实 HAL，
 * 这里用空壳把头文件依赖转发到 core_cm3.h，让静态检查与单元测试能够编译。
 *
 * @note 本文件不提供任何外设 API；主机用例不得假借 HAL 行为做断言。
 */
#ifndef HTOS_TEST_STM32F1XX_HAL_H
#define HTOS_TEST_STM32F1XX_HAL_H

#include "core_cm3.h"

#endif /* HTOS_TEST_STM32F1XX_HAL_H */
