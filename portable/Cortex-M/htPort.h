/**
 * @file htPort.h
 * @brief Cortex-M3 移植层统一契约：中断屏蔽、上下文切换、硬件计数、位运算与异常取证。
 *
 * 内核核心代码（kernel 目录）只包含本头文件，不直接包含任何芯片/CMSIS/HAL 头或内联汇编。
 * 换用其他 MCU/架构时只需替换 portable/<Arch>/htPort.h 与对应上下文切换实现。
 */
#ifndef HT_PORT_H
#define HT_PORT_H

#include "htconfig.h"
#include "httypes.h"
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
 * 先包含设备头文件，移植层才能在真实目标上编译；换板子只需覆盖此宏。
 */
#ifndef HT_PORT_DEVICE_HEADER
#define HT_PORT_DEVICE_HEADER "stm32f1xx.h"
#endif

#include HT_PORT_DEVICE_HEADER
#include HT_PORT_CORE_HEADER

#ifndef __NVIC_PRIO_BITS
#define __NVIC_PRIO_BITS 4U
#endif

/** 最低 NVIC 逻辑优先级（4 位优先级下为 15）。 */
#define HT_PORT_LOWEST_INTERRUPT_PRIORITY ((1UL << (uint32_t)__NVIC_PRIO_BITS) - 1UL)

#if (configUSE_BASEPRI_CRITICAL == 1)
#if (configMAX_SYSCALL_INTERRUPT_PRIORITY < 1U) ||                                                                     \
		(configMAX_SYSCALL_INTERRUPT_PRIORITY > ((1UL << __NVIC_PRIO_BITS) - 1UL))
#error "configMAX_SYSCALL_INTERRUPT_PRIORITY must be in [1, (1 << __NVIC_PRIO_BITS) - 1]"
#endif
/** BASEPRI 寄存器实际写入的左移后硬件阈值（例如 4 位优先级、阈值 5 -> 0x50）。 */
#define HT_PORT_SYSCALL_BASEPRI ((uint32_t)(configMAX_SYSCALL_INTERRUPT_PRIORITY) << (8U - (uint32_t)__NVIC_PRIO_BITS))
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

/* ------------------------------------------------------------------ */
/* 1. 中断屏蔽与临界区原语                                             */
/* ------------------------------------------------------------------ */

/** @brief 屏蔽受 RTOS 管理的中断（BASEPRI 分级屏蔽或 PRIMASK 全局屏蔽）。 */
static inline void htPortDisableInterrupts(void)
{
#if (configUSE_BASEPRI_CRITICAL == 1)
	__set_BASEPRI(HT_PORT_SYSCALL_BASEPRI);
	__DSB();
	__ISB();
#else
	__disable_irq();
#endif
}

/** @brief 解除受 RTOS 管理的中断屏蔽。 */
static inline void htPortEnableInterrupts(void)
{
#if (configUSE_BASEPRI_CRITICAL == 1)
	__set_BASEPRI(0U);
#else
	__enable_irq();
#endif
}

/** @brief 全局关闭所有可屏蔽中断（仅用于调度器启动前或不可恢复停机）。 */
static inline void htPortDisableAllInterrupts(void)
{
	__disable_irq();
}

/** @brief 全局打开所有可屏蔽中断。 */
static inline void htPortEnableAllInterrupts(void)
{
	__enable_irq();
}

/**
 * @brief ISR 侧进入临界区：保存当前屏蔽级别并提升至 RTOS 临界阈值。
 * @return 保存的屏蔽状态，退出时传回 htPortExitCriticalFromISR()。
 */
static inline uint32_t htPortEnterCriticalFromISR(void)
{
#if (configUSE_BASEPRI_CRITICAL == 1)
	const uint32_t ulSaved = __get_BASEPRI();
	if ((ulSaved == 0U) || (ulSaved > HT_PORT_SYSCALL_BASEPRI)) {
		__set_BASEPRI(HT_PORT_SYSCALL_BASEPRI);
		__DSB();
		__ISB();
	}
	return ulSaved;
#else
	const uint32_t ulSaved = __get_PRIMASK();
	__disable_irq();
	return ulSaved;
#endif
}

/** @brief ISR 侧退出临界区：恢复保存的屏蔽级别。 */
static inline void htPortExitCriticalFromISR(uint32_t ulSavedMask)
{
#if (configUSE_BASEPRI_CRITICAL == 1)
	__set_BASEPRI(ulSavedMask);
#else
	__set_PRIMASK(ulSavedMask);
#endif
}

/**
 * @brief 轻量原始原子段（保存/恢复 PRIMASK，不改动 uxCriticalNesting）。
 * @note 供延迟仪表与 64 位统计快照使用，避免仪表统计自身或与任何高优先级中断撕裂 64 位读。
 */
static inline uint32_t htPortEnterAtomicRaw(void)
{
	const uint32_t ulSaved = __get_PRIMASK();
	__disable_irq();
	return ulSaved;
}

static inline void htPortExitAtomicRaw(uint32_t ulSavedPrimask)
{
	__set_PRIMASK(ulSavedPrimask);
}

/* ------------------------------------------------------------------ */
/* 2. 调度触发、优先级配置、DWT 周期计数与硬件位扫描                   */
/* ------------------------------------------------------------------ */

/** @brief 任务侧主动请求 PendSV 切换，带 DSB+ISB 保证紧随指令不越过切换边界。 */
static inline void htPortYield(void)
{
	SCB->ICSR = SCB_ICSR_PENDSVSET_Msk;
	__DSB();
	__ISB();
}

/** @brief 中断侧挂起 PendSV（无需 ISB，中断返回时尾链进入 PendSV）。 */
static inline void htPortRequestSwitchFromISR(void)
{
	SCB->ICSR = SCB_ICSR_PENDSVSET_Msk;
}

/** @brief 根据 FromISR 的唤醒标志按需请求上下文切换。 */
static inline void htPortYieldFromISR(BaseType_t xHigherPriorityTaskWoken)
{
	if (xHigherPriorityTaskWoken != htFALSE) {
		htPortRequestSwitchFromISR();
	}
}

/** @brief 将 PendSV 与 SysTick 统一配置为最低逻辑优先级，避免抢占硬实时外设中断。 */
static inline void htPortSetupInterruptPriorities(void)
{
	NVIC_SetPriority(PendSV_IRQn, HT_PORT_LOWEST_INTERRUPT_PRIORITY);
	NVIC_SetPriority(SysTick_IRQn, HT_PORT_LOWEST_INTERRUPT_PRIORITY);
	__DSB();
	__ISB();
}

/** @brief 幂等初始化 Cortex-M3 DWT 周期计数器（若已启动则不清零 CYCCNT）。 */
static inline void htPortCycleCounterInit(void)
{
	CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
	if ((DWT->CTRL & DWT_CTRL_CYCCNTENA_Msk) == 0UL) {
		DWT->CYCCNT = 0UL;
		DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
	}
}

/** @brief 读取当前 32 位周期计数器值。 */
static inline uint32_t htPortCycleCounterRead(void)
{
	return DWT->CYCCNT;
}

/** @brief 硬件单周期前导零计数（CLZ），供 O(1) 就绪位图与 TLSF 位图使用。 */
static inline uint32_t htPortCountLeadingZeros(uint32_t ulValue)
{
	return (uint32_t)__CLZ(ulValue);
}

/** @brief 硬件单周期位反转（RBIT），配合 CLZ 实现单周期尾随零查找。 */
static inline uint32_t htPortReverseBits(uint32_t ulValue)
{
	return (uint32_t)__RBIT(ulValue);
}

/* ------------------------------------------------------------------ */
/* 3. 上下文切换、栈帧构造与硬 Fault 现场                              */
/* ------------------------------------------------------------------ */

/** @brief 任务入口函数签名别名（与 httask.h 的 TaskFunction_t 同型）。 */
typedef void (*htPortTaskCode_t)(void *pvParameters);

/**
 * @brief 在任务栈顶构造 Cortex-M3 初始异常返回帧 + R4-R11 软件帧。
 *
 * @param[in] pxTopOfStack 栈顶高地址。
 * @param[in] pxCode 任务入口函数。
 * @param[in] pvParameters 传给任务入口的 R0 参数。
 * @return 构造完成后的新栈顶指针，写入 TCB.pxTopOfStack。
 */
StackType_t *htPortInitialiseStack(StackType_t *pxTopOfStack, htPortTaskCode_t pxCode, void *pvParameters);

/**
 * @brief 启动第一个任务。
 *
 * @note 由 SVC 指令进入，把 MSP 复位成向量表里记录的初始栈顶，
 *       再把 PSP 指向第一个任务的栈帧并返回，此后运行在任务上下文。
 */
void vPortStartFirstTask(void);

/** @brief PendSV 异常处理函数，负责上下文切换。 */
void PendSV_Handler(void);

/** @brief SVC 异常处理函数，负责启动第一个任务。 */
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
 * @brief 硬 fault 现场的出口钩子（弱符号，应用层可提供强符号接管输出）。
 */
void htFaultReport(const htFaultFrame_t *pxFrame);

#endif /* HT_PORT_H */
