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

/** 调试异常与监视控制寄存器块，只有 DEMCR.TRCENA 一位在主机侧有意义。 */
typedef struct {
	volatile uint32_t DEMCR; /**< 调试异常与监视控制寄存器，bit24 是 TRCENA。 */
} CoreDebug_Type;

/** DEMCR 的 bit24：DWT 等跟踪单元的总开关。 */
#define CoreDebug_DEMCR_TRCENA_Msk (1UL << 24U)

/**
 * 数据观察点与跟踪单元。主机侧只模拟周期计数器：CYCCNT 是一块普通内存，
 * 用例可以自己写它来构造时间差，从而在没有真时钟的情况下验证统计逻辑。
 */
typedef struct {
	volatile uint32_t CTRL; /**< 控制寄存器，bit0 是周期计数器使能。 */
	volatile uint32_t CYCCNT; /**< 32 位周期计数器。 */
} DWT_Type;

/**
 * 主机上没有 0xE0001000 那块内存，写进去会直接触发访问违例，因此 DWT 与 CoreDebug
 * 都落到两块真实的静态存储上，定义放在 tests/stubs/host_cmsis.c，全进程只有一份，
 * 多个编译单元看到的是同一份数据（用例写入、被测代码读出）。
 */
extern DWT_Type htHostDwt; /**< 主机侧 DWT 替身。 */
extern CoreDebug_Type htHostCoreDebug; /**< 主机侧 CoreDebug 替身。 */

/** 目标板上 DWT 的基址；主机上指向 htHostDwt。 */
#define DWT (&htHostDwt)
/** 目标板上 CoreDebug 的基址；主机上指向 htHostCoreDebug。 */
#define CoreDebug (&htHostCoreDebug)
/** CTRL 的 bit0：周期计数器使能。 */
#define DWT_CTRL_CYCCNTENA_Msk (1UL << 0U)
/** ICSR 的 bit28：写 1 挂起 PendSV，上下文切换靠它触发。 */
#define SCB_ICSR_PENDSVSET_Msk (1UL << 28U)

/** 两个内核异常号，与 ARM 的定义一致（系统异常用负数编号）。 */
typedef enum { PendSV_IRQn = -2, SysTick_IRQn = -1 } IRQn_Type;

#ifndef __NVIC_PRIO_BITS
#define __NVIC_PRIO_BITS 4U
#endif

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

/* PRIMASK / BASEPRI 在主机上没有对应物：保存/恢复对恒为 0，让临界区代码照常编译通过。 */
static inline uint32_t __get_PRIMASK(void)
{
	return 0U;
}
static inline void __set_PRIMASK(uint32_t primask)
{
	(void)primask;
}
static inline uint32_t __get_BASEPRI(void)
{
	return 0U;
}
static inline void __set_BASEPRI(uint32_t basepri)
{
	(void)basepri;
}

static inline uint8_t __CLZ(uint32_t value)
{
	if (value == 0U) {
		return 32U;
	}
	uint8_t count = 0U;
	while ((value & 0x80000000U) == 0U) {
		value <<= 1U;
		count++;
	}
	return count;
}

static inline uint32_t __RBIT(uint32_t value)
{
	uint32_t result = 0U;
	for (uint32_t i = 0U; i < 32U; i++) {
		result = (result << 1U) | (value & 1U);
		value >>= 1U;
	}
	return result;
}

#endif /* HTOS_TEST_CORE_CM3_H */
