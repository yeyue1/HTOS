/**
 * @file htPort.h
 * @brief Cortex-M 特定的移植层定义。
 *
 * 声明上下文切换入口与两个异常处理函数，并给内核头文件、设备头文件的包含方式留出覆盖点。
 */
#ifndef HT_PORT_H
#define HT_PORT_H

#include "htconfig.h"
#include <stdint.h>

/*
 * 本移植层按 GNU Arm 工具链编写。当默认的 M3 内核头文件不适用时，
 * 可由板级 CMSIS 头文件覆盖内核头文件的名字。
 */
#ifndef HT_PORT_CORE_HEADER
#define HT_PORT_CORE_HEADER "core_cm3.h"
#endif

/*
 * CMSIS 内核头文件会引用 IRQn_Type 与 SCB，这两个类型由设备头文件提供。
 * 先包含设备头文件，移植层才能在真实目标上编译，而不是只能对着主机桩编译。
 * 若板子换到别的 STM32 系列，覆盖此宏即可。
 */
#ifndef HT_PORT_DEVICE_HEADER
#define HT_PORT_DEVICE_HEADER "stm32f1xx.h"
#endif

/*
 * 栈范围检查是可选的：SRAM 的摆放位置属于链接脚本的属性，不是 RTOS 的不变量。
 * 需要这道廉价护栏时，按目标打开开关并给出汇编可用的整型常量：
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
 * @brief 启动第一个任务。
 *
 * @note 由 SVC 指令进入，把 MSP 复位成向量表里记录的初始栈顶，
 *       再把 PSP 指向第一个任务的栈帧并返回，此后运行在任务上下文。
 */
void vPortStartFirstTask(void);

/**
 * @brief PendSV 异常处理函数，负责上下文切换。
 *
 * @note 全部寄存器操作以 naked 内联汇编实现，函数体不生成编译器序言，
 *       因此不能把它当普通 C 函数调用。
 */
void PendSV_Handler(void);

/**
 * @brief SVC 异常处理函数，负责启动第一个任务。
 *
 * @note 只在 vPortStartFirstTask() 发出 svc 0 之后执行一次。
 */
void SVC_Handler(void);

#endif /* HT_PORT_H */
