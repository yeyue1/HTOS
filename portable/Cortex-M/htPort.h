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

/**
 * @brief 硬 fault 现场快照：异常压栈的 8 个寄存器加 SCB 的故障状态寄存器。
 *
 * @note ulCore 的固定顺序是 R0、R1、R2、R3、R12、LR、PC、xPSR，与 Cortex-M
 *       硬件压栈顺序一致，按这个下标取即可，不需要额外的名字表。
 */
typedef struct htFaultFrame {
	uint32_t ulStackPointer; /*!< 异常栈帧的地址，即 ulCore 的来源。 */
	uint32_t ulCore[8]; /*!< 见 @note：R0 R1 R2 R3 R12 LR PC xPSR。 */
	uint32_t ulCfsr; /*!< 可配置故障状态寄存器。 */
	uint32_t ulHfsr; /*!< 硬 fault 状态寄存器。 */
	uint32_t ulDfsr; /*!< 调试故障状态寄存器。 */
	uint32_t ulAfsr; /*!< 辅助故障状态寄存器。 */
	uint32_t ulBfar; /*!< 总线 fault 地址寄存器。 */
	uint32_t ulMmfar; /*!< 存储器管理 fault 地址寄存器。 */
	uint32_t ulFaultFlags; /*!< CFSR 里最容易误读的几位，见下面 HT_FAULT_FLAG_*。 */
} htFaultFrame_t;

/** @brief 协处理器访问错误（CFSR.NOCP）。 */
#define HT_FAULT_FLAG_NOCP 0x1U
/** @brief 非法 PC 装入，通常意味着 EXC_RETURN 已被破坏（CFSR.INVPC）。 */
#define HT_FAULT_FLAG_INVPC 0x2U
/** @brief 非法状态，最常见的是 Thumb 位被清零（CFSR.INVSTATE）。 */
#define HT_FAULT_FLAG_INVSTATE 0x4U
/** @brief 未定义指令（CFSR.UNDEFINSTR）。 */
#define HT_FAULT_FLAG_UNDEFINSTR 0x8U

/**
 * @brief 硬 fault 现场的出口钩子。
 *
 * @param[in] pxFrame 现场快照，指向的存储不会在调用期间失效。
 *
 * @note 移植层提供空默认实现（弱符号）。应用层给出同名强符号即可接管输出，
 *       内核与移植层因此彻底不依赖 libc 的格式化输出。
 * @warning 调用时中断已关闭，且本钩子之后就是死循环：实现里不许阻塞、
 *          不许延时、不许申请内存，也不许返回错误码。
 */
void htFaultReport(const htFaultFrame_t *pxFrame);

#endif /* HT_PORT_H */
