#ifndef HTOS_TEST_CORE_CM3_H
#define HTOS_TEST_CORE_CM3_H

#include <stdint.h>

typedef struct {
	volatile uint32_t RESERVED0[5];
	volatile uint32_t ICSR;
	volatile uint32_t RESERVED1[7];
	volatile uint32_t CFSR;
	volatile uint32_t HFSR;
	volatile uint32_t DFSR;
	volatile uint32_t MMFAR;
	volatile uint32_t BFAR;
	volatile uint32_t AFSR;
} SCB_Type;

#define SCB ((SCB_Type *)0xE000ED00UL)
#define SCB_ICSR_PENDSVSET_Msk (1UL << 28U)

typedef enum { PendSV_IRQn = -2, SysTick_IRQn = -1 } IRQn_Type;

static inline void NVIC_SetPriority(IRQn_Type interrupt, uint32_t priority)
{
	(void)interrupt;
	(void)priority;
}

static inline void __disable_irq(void) {}
static inline void __enable_irq(void) {}
static inline void __DSB(void) {}
static inline void __ISB(void) {}

#endif /* HTOS_TEST_CORE_CM3_H */
