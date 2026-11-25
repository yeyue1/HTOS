/**
 * @file portable/Host/htPort.h
 * @brief 主机单元测试侧移植层实现，与 portable/Cortex-M/htPort.h 提供完全一致的内核原语。
 *
 * 内核源码（kernel 目录）只需包含 "htPort.h"，在主机编译时通过 -Iportable/Host 切到本文件，
 * 无需在内核里保留任何 stm32f1xx.h / core_cm3.h 依赖。
 */
#ifndef HT_PORT_H
#define HT_PORT_H

#include "htconfig.h"
#include "httypes.h"
#include "core_cm3.h"
#include <stdint.h>

typedef void (*htPortTaskCode_t)(void *pvParameters);

extern void htHostTestYieldHook(void);

static inline void htPortDisableInterrupts(void)
{
	__disable_irq();
}

static inline void htPortEnableInterrupts(void)
{
	__enable_irq();
}

static inline void htPortDisableAllInterrupts(void)
{
	__disable_irq();
}

static inline void htPortEnableAllInterrupts(void)
{
	__enable_irq();
}

static inline uint32_t htPortEnterCriticalFromISR(void)
{
	const uint32_t ulSaved = __get_PRIMASK();
	__disable_irq();
	return ulSaved;
}

static inline void htPortExitCriticalFromISR(uint32_t ulSavedMask)
{
	__set_PRIMASK(ulSavedMask);
}

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

static inline void htPortYield(void)
{
	htHostTestYieldHook();
}

static inline void htPortRequestSwitchFromISR(void)
{
	/* 主机侧无真实 SCB 寄存器，不访问 0xE000ED00。 */
}

static inline void htPortYieldFromISR(BaseType_t xHigherPriorityTaskWoken)
{
	if (xHigherPriorityTaskWoken != htFALSE) {
		htPortRequestSwitchFromISR();
	}
}

static inline void htPortSetupInterruptPriorities(void)
{
	NVIC_SetPriority(PendSV_IRQn, 15UL);
	NVIC_SetPriority(SysTick_IRQn, 15UL);
	__DSB();
	__ISB();
}

static inline void htPortCycleCounterInit(void)
{
	CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
	if ((DWT->CTRL & DWT_CTRL_CYCCNTENA_Msk) == 0UL) {
		DWT->CYCCNT = 0UL;
		DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
	}
}

static inline uint32_t htPortCycleCounterRead(void)
{
	return DWT->CYCCNT;
}

static inline uint32_t htPortCountLeadingZeros(uint32_t ulValue)
{
	if (ulValue == 0U) {
		return 32U;
	}
#if defined(__GNUC__) || defined(__clang__)
	return (uint32_t)__builtin_clz(ulValue);
#else
	uint32_t ulCount = 0U;
	while ((ulValue & 0x80000000U) == 0U) {
		ulValue <<= 1U;
		ulCount++;
	}
	return ulCount;
#endif
}

static inline uint32_t htPortReverseBits(uint32_t ulValue)
{
	uint32_t ulResult = 0U;
	for (uint32_t i = 0U; i < 32U; i++) {
		ulResult = (ulResult << 1U) | (ulValue & 1U);
		ulValue >>= 1U;
	}
	return ulResult;
}

static inline StackType_t *htPortInitialiseStack(StackType_t *pxTopOfStack, htPortTaskCode_t pxCode, void *pvParameters)
{
	pxTopOfStack = (StackType_t *)((uintptr_t)pxTopOfStack & ~(uintptr_t)0x7U);
	*(--pxTopOfStack) = 0x01000000U;
	*(--pxTopOfStack) = ((uint32_t)(uintptr_t)pxCode) & ~(uint32_t)0x1U;
	*(--pxTopOfStack) = 0U;
	*(--pxTopOfStack) = 0U;
	*(--pxTopOfStack) = 0U;
	*(--pxTopOfStack) = 0U;
	*(--pxTopOfStack) = 0U;
	*(--pxTopOfStack) = (uint32_t)(uintptr_t)pvParameters;
	for (uint32_t i = 0U; i < 8U; i++) {
		*(--pxTopOfStack) = 0U;
	}
	return pxTopOfStack;
}

#endif /* HT_PORT_H */
