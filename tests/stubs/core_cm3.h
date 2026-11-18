/**
 * @file core_cm3.h
 * @brief 主机测试用的 Cortex-M3 CMSIS 最小替身。
 *
 * 只提供内核源码实际引用到的符号：SCB 寄存器块、PendSV 的 ICSR 置位掩码、
 * 两个异常号，以及中断开关与屏障指令的空实现。
 * 主机上没有真实中断，__disable_irq()/__enable_irq() 是空操作，
 * 因此临界区的配对只能靠 uxCriticalNesting 计数在用例里断言。
 *
 * @warning 这里的地址与位定义只用于让代码在 x86 主机上编译通过，
 *          不代表可以在主机上真正访问 0xE000ED00 处的寄存器。
 */
#ifndef HTOS_TEST_CORE_CM3_H
#define HTOS_TEST_CORE_CM3_H

#include <stdint.h>

typedef struct {
	volatile uint32_t RESERVED0[5]; /**< 保留区，把 ICSR 顶到真实偏移 0x04。 */
	volatile uint32_t ICSR; /**< 中断控制与状态寄存器，bit28 是 PendSV 的 SETPEND。 */
	volatile uint32_t RESERVED1[7]; /**< 保留区，把 CFSR 顶到真实偏移 0x28。 */
	volatile uint32_t CFSR; /**< 可配置故障状态寄存器，UFSR 的 INVSTATE 在 bit17。 */
	volatile uint32_t HFSR; /**< 硬件故障状态寄存器。 */
	volatile uint32_t DFSR; /**< 调试故障状态寄存器。 */
	volatile uint32_t MMFAR; /**< 存储管理故障地址寄存器。 */
	volatile uint32_t BFAR; /**< 总线故障地址寄存器。 */
	volatile uint32_t AFSR; /**< 辅助故障状态寄存器。 */
} SCB_Type;

/** Cortex-M3 系统控制块基址（0xE000ED00），布局与真实 SCB 的偏移一致。 */
#define SCB ((SCB_Type *)0xE000ED00UL)
/** ICSR 的 bit28：写 1 挂起 PendSV，上下文切换靠它触发。 */
#define SCB_ICSR_PENDSVSET_Msk (1UL << 28U)

/** 两个内核异常号，与 ARM 的定义一致（系统异常用负数编号）。 */
typedef enum { PendSV_IRQn = -2, SysTick_IRQn = -1 } IRQn_Type;

/**
 * @brief 设置异常优先级——主机上空实现，只吞掉参数避免未使用告警。
 *
 * @param[in] interrupt 异常号，主机侧不使用。
 * @param[in] priority 优先级数值，主机侧不使用。
 */
static inline void NVIC_SetPriority(IRQn_Type interrupt, uint32_t priority)
{
	(void)interrupt;
	(void)priority;
}

/* 以下四条是 GCC 内建指令的主机替身：中断开关与屏障在 x86 上都没有对应语义，
   全部空实现。失去真实关中断能力后，临界区的正确性只能靠计数配对来间接验证。 */
static inline void __disable_irq(void) {}
static inline void __enable_irq(void) {}
static inline void __DSB(void) {}
static inline void __ISB(void) {}

#endif /* HTOS_TEST_CORE_CM3_H */
