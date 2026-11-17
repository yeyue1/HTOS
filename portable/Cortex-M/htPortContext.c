/**
 * @file htPortContext.c
 * @brief GCC/Thumb Cortex-M3 context-switch port.
 */

#include "htPort.h"
#include "httask.h"
/* The device header defines IRQn_Type/SCB and pulls the CMSIS core header in. */
#include HT_PORT_DEVICE_HEADER
#include HT_PORT_CORE_HEADER

#include <stdint.h>

/* Keep the port independent from a particular libc/newlib header set. */
extern int printf(const char *format, ...);

#if !defined(__GNUC__)
#error "portable/Cortex-M/htPortContext.c requires GCC-compatible inline assembly"
#endif

#define HT_PORT_STRINGIFY_IMPL(value) #value
#define HT_PORT_STRINGIFY(value) HT_PORT_STRINGIFY_IMPL(value)

#if HT_PORT_VALIDATE_STACK
#define HT_PORT_STACK_CHECK_R0                                                                                         \
	"ldr r12, =" HT_PORT_STRINGIFY(HT_PORT_SRAM_START) "\n"                                                            \
													   "cmp r0, r12\n"                                                 \
													   "blo PendSV_StackError\n"                                       \
													   "ldr r12, =" HT_PORT_STRINGIFY(                                 \
															   HT_PORT_SRAM_END) "\n"                                  \
																				 "cmp r0, r12\n"                       \
																				 "bhs PendSV_StackError\n"
#endif

/* The first TCB field is pxTopOfStack; the assembly port relies on it. */
extern htTCB_t *pxCurrentTCB;

/**
 * Load the reset MSP and enter the first task through SVC.
 *
 * This is a naked function so GCC emits exactly the instructions below and
 * does not add a compiler-generated prologue or epilogue.
 */
__attribute__((naked, used, noinline)) void vPortStartFirstTask(void)
{
	__asm volatile(".syntax unified\n"
				   ".thumb\n"
				   "ldr r0, =0xE000ED08\n"
				   "ldr r0, [r0]\n"
				   "ldr r0, [r0]\n"
				   "msr msp, r0\n"
				   "movs r0, #0\n"
				   "msr basepri, r0\n"
				   "cpsie i\n"
				   "cpsie f\n"
				   "dsb\n"
				   "isb\n"
				   "svc 0\n"
				   "1:\n"
				   "b 1b\n");
}

/**
 * Restore the first task's software-saved registers and return to Thread
 * mode using PSP.  The initial task stack is prepared by htTaskCreate().
 */
__attribute__((naked, used, noinline)) void SVC_Handler(void)
{
	__asm volatile(".syntax unified\n"
				   ".thumb\n"
				   "ldr r3, =pxCurrentTCB\n"
				   "ldr r1, [r3]\n"
				   "cbz r1, 1f\n"
				   "ldr r0, [r1]\n"
				   "ldmia r0!, {r4-r11}\n"
				   "msr psp, r0\n"
				   "movs r0, #2\n"
				   "msr control, r0\n"
				   "isb\n"
				   "ldr lr, =0xFFFFFFFD\n"
				   "bx lr\n"
				   "1:\n"
				   "bx lr\n");
}

/**
 * Save the interrupted task, select the next task in C, then restore its
 * software-saved registers.  Handler mode uses MSP for the temporary push;
 * task stacks remain on PSP.
 */
__attribute__((naked, used, noinline)) void PendSV_Handler(void)
{
	__asm volatile(".syntax unified\n"
				   ".thumb\n"
				   "cpsid i\n"
				   "mrs r0, psp\n"
				   "ldr r3, =pxCurrentTCB\n"
				   "ldr r2, [r3]\n"
				   "cbz r2, 1f\n"
				   /*
				    * PSP == 0 意味着还没有任何任务上下文：第一个任务尚未通过
				    * SVC_Handler 建立。此时既没有可保存的现场，也没有可恢复的
				    * 现场，直接返回（保留原始 EXC_RETURN，不改 SPSEL）。
				    * 若省掉这个判断，stmdb 会写 0xFFFFFFE0 并触发 HardFault。
				    */
				   "cbz r0, 9f\n"
#if HT_PORT_VALIDATE_STACK
			HT_PORT_STACK_CHECK_R0
#endif
				   "stmdb r0!, {r4-r11}\n"
				   "str r0, [r2]\n"
				   "1:\n"
				   "push {r3, lr}\n"
				   "bl htTaskSwitchContext\n"
				   "pop {r3, lr}\n"
				   "ldr r1, [r3]\n"
				   "cbz r1, 2f\n"
				   "ldr r0, [r1]\n"
#if HT_PORT_VALIDATE_STACK
			HT_PORT_STACK_CHECK_R0
#endif
				   "ldmia r0!, {r4-r11}\n"
				   "msr psp, r0\n"
				   "movs r0, #2\n"
				   "msr control, r0\n"
				   "isb\n"
				   "2:\n"
				   "cpsie i\n"
				   "movs r0, #0x0D\n"
				   "orr lr, lr, r0\n"
				   "bx lr\n"
				   /* 9: 无任务上下文时的提前返回 - 必须原样返回 EXC_RETURN，
				      不能用上面的 orr 强制回到 Thread/PSP（PSP 此时是 0）。 */
				   "9:\n"
				   "cpsie i\n"
				   "bx lr\n"
#if HT_PORT_VALIDATE_STACK
				   "PendSV_StackError:\n"
				   "udf #0\n"
				   "b .\n"
#endif
	);
}

/* Optional architecture hook supplied by Trace/coredump integration. */
extern int armv7m_hard_fault_exception_hook(void *context) __attribute__((weak));

#ifndef SCB_CFSR_NOCP_Msk
#define SCB_CFSR_NOCP_Msk 0U
#endif
#ifndef SCB_CFSR_INVPC_Msk
#define SCB_CFSR_INVPC_Msk 0U
#endif
#ifndef SCB_CFSR_INVSTATE_Msk
#define SCB_CFSR_INVSTATE_Msk 0U
#endif
#ifndef SCB_CFSR_UNDEFINSTR_Msk
#define SCB_CFSR_UNDEFINSTR_Msk 0U
#endif

/**
 * C half of the hard-fault handler.  It intentionally avoids board-specific
 * HAL/GPIO dependencies so the same port can be built with any CMSIS device
 * header.  A project may provide the weak coredump hook when desired.
 */
void HardFault_Handler_C(uint32_t *fault_args)
{
	__disable_irq();
	if (fault_args == NULL) {
		for (;;) {
		}
	}

	volatile uint32_t stacked_r0 = fault_args[0];
	volatile uint32_t stacked_r1 = fault_args[1];
	volatile uint32_t stacked_r2 = fault_args[2];
	volatile uint32_t stacked_r3 = fault_args[3];
	volatile uint32_t stacked_r12 = fault_args[4];
	volatile uint32_t stacked_lr = fault_args[5];
	volatile uint32_t stacked_pc = fault_args[6];
	volatile uint32_t stacked_psr = fault_args[7];

	volatile uint32_t cfsr = SCB->CFSR;
	volatile uint32_t hfsr = SCB->HFSR;
	volatile uint32_t dfsr = SCB->DFSR;
	volatile uint32_t afsr = SCB->AFSR;
	volatile uint32_t bfar = SCB->BFAR;
	volatile uint32_t mmfar = SCB->MMFAR;

	printf("\r\n\n[HARDFAULT]\r\n");
	printf("SP: 0x%08lx\r\n", (unsigned long)(uintptr_t)fault_args);
	printf("R0: 0x%08lx, R1: 0x%08lx\r\n", (unsigned long)stacked_r0, (unsigned long)stacked_r1);
	printf("R2: 0x%08lx, R3: 0x%08lx\r\n", (unsigned long)stacked_r2, (unsigned long)stacked_r3);
	printf("R12: 0x%08lx, LR: 0x%08lx\r\n", (unsigned long)stacked_r12, (unsigned long)stacked_lr);
	printf("PC: 0x%08lx, PSR: 0x%08lx\r\n", (unsigned long)stacked_pc, (unsigned long)stacked_psr);
	printf("CFSR: 0x%08lx, HFSR: 0x%08lx\r\n", (unsigned long)cfsr, (unsigned long)hfsr);
	printf("DFSR: 0x%08lx, AFSR: 0x%08lx\r\n", (unsigned long)dfsr, (unsigned long)afsr);
	printf("BFAR: 0x%08lx, MMAR: 0x%08lx\r\n", (unsigned long)bfar, (unsigned long)mmfar);

	if (armv7m_hard_fault_exception_hook != NULL) {
		(void)armv7m_hard_fault_exception_hook(fault_args);
	}

	if ((cfsr & SCB_CFSR_NOCP_Msk) != 0U) {
		printf("- Coprocessor access error\r\n");
	}
	if ((cfsr & SCB_CFSR_INVPC_Msk) != 0U) {
		printf("- Invalid PC load (EXC_RETURN)\r\n");
	}
	if ((cfsr & SCB_CFSR_INVSTATE_Msk) != 0U) {
		printf("- Invalid state - possible Thumb bit error\r\n");
	}
	if ((cfsr & SCB_CFSR_UNDEFINSTR_Msk) != 0U) {
		printf("- Undefined instruction\r\n");
	}

	for (;;) {
	}
}

/**
 * Select MSP or PSP according to EXC_RETURN and tail-call the C reporter.
 */
__attribute__((naked, used, noinline)) void HardFault_Handler(void)
{
	__asm volatile(".syntax unified\n"
				   ".thumb\n"
				   "tst lr, #4\n"
				   "ite eq\n"
				   "mrseq r0, msp\n"
				   "mrsne r0, psp\n"
				   "ldr r1, =HardFault_Handler_C\n"
				   "bx r1\n");
}
