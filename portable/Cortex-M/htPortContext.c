#include "httask.h"
#include "stm32f1xx.h"     // Device specific header file
#include "stm32f1xx_hal.h" // HAL library
#include "core_cm3.h"      // Cortex-M3 core definitions
#include <stdio.h>         // For debug output
#include "gpio.h"          // GPIO definitions
#include "../../Trace/coredump/inc/coredump.h"  // CoreDump support

/* Forward declaration for coredump function */
extern int armv7m_hard_fault_exception_hook(void *context);

/* Task switching control */
extern htTCB_t *pxCurrentTCB;

/**
 * Simplified first task start function - using Keil assembly style
 */
__asm void vPortStartFirstTask(void)
{
    PRESERVE8

    /* Find stack top using NVIC register */
    ldr r0, =0xE000ED08
    ldr r0, [r0]
    ldr r0, [r0]

    /* Set MSP to stack top for safe system init */
    msr msp, r0
    
    /* Clear all safety flags and disable FPU */
    mov r0, #0
    msr basepri, r0
    
    /* Set registers to known state to prevent issues */
    mov r0, #0
    mov r1, #0
    mov r2, #0
    mov r3, #0
    mov r4, #0
    mov r5, #0
    mov r6, #0
    mov r7, #0
    
    /* Enable interrupts */
    cpsie i
    cpsie f
    dsb
    isb
    
    /* Trigger SVC to start first task */
    svc 0
    nop
		nop
}

/**
 * Simple SVC handler - avoiding potential coprocessor access
 */
__asm void SVC_Handler(void)
{
    PRESERVE8
    IMPORT pxCurrentTCB

    /* Get current task TCB */
    ldr r3, =pxCurrentTCB
    ldr r1, [r3]            /* Load TCB pointer */
    cmp r1, #0              /* Check if NULL - using cmp instead of cbz for compatibility */
    beq SVC_Exit            /* Exit if NULL */

    ldr r0, [r1]            /* Read stack pointer */
    ldmia r0!, {r4-r11}     /* Restore r4-r11 registers */
    
    msr psp, r0             /* Update process stack pointer */
    mov r0, #2              /* Set CONTROL.SPSEL to use PSP */
    msr control, r0
    isb                     /* Instruction sync barrier */
    
    mov r0, #0              /* Clear all general registers to prevent issues */
    mov r1, #0
    mov r2, #0
    mov r3, #0

SVC_Exit
    /* Return to thread mode */
    mov lr, #0xFFFFFFFD     /* Set LR value to ensure return to thread mode */
    bx lr                   /* Return */
}

/**
 * Simple PendSV handler - Keil assembly style
 * 完全重写，修复栈帧损坏问题和跳转范围问题
 */
__asm void PendSV_Handler(void)
{
    PRESERVE8
    IMPORT pxCurrentTCB
    IMPORT htTaskSwitchContext
    
    /* 确保我们的LR在返回时包含异常返回值 */
    mov r0, lr
    
    /* 禁用中断以保持原子性 */
    cpsid i
    
    /* 检查pxCurrentTCB是否为NULL */
    ldr r3, =pxCurrentTCB
    ldr r2, [r3]
    cbz r2, PendSV_NoSave
    
    /* 保存上下文: 获取当前任务栈顶 */
    mrs r1, psp
    
    /* 检查栈地址有效性：必须在0x20000000-0x20200000范围内 */
    ldr r12, =0x20000000
    cmp r1, r12
    blt PendSV_StackError
    
    ldr r12, =0x20200000
    cmp r1, r12
    bge PendSV_StackError
    
    /* 栈地址有效，保存核心寄存器 */
    stmdb r1!, {r4-r11}
    
    /* 将更新后的栈指针保存回TCB */
    str r1, [r2]

PendSV_NoSave
    /* 保证安全的上下文切换 */
    push {r3, lr}
    bl htTaskSwitchContext
    pop {r3, lr}
    
    /* 获取新的当前任务 */
    ldr r1, [r3]
    cbz r1, PendSV_Exit
    
    /* 加载新任务栈 */
    ldr r0, [r1]
    
    /* 检查栈地址有效性 */
    ldr r12, =0x20000000
    cmp r0, r12
    blt PendSV_StackError
    
    ldr r12, =0x20200000
    cmp r0, r12 
    bge PendSV_StackError
    
    /* 栈地址有效，恢复核心寄存器 */
    ldmia r0!, {r4-r11}
    
    /* 更新进程栈指针 */
    msr psp, r0
    
    /* 确保使用PSP */
    mov r0, #2
    msr control, r0
    isb

PendSV_Exit
    /* 重新启用中断 */
    cpsie i
    
    /* 确保返回使用进程栈 */
    orr lr, lr, #0xD
    
    /* 返回到线程模式 */
    bx lr
    
PendSV_StackError
    /* 栈地址无效 - 使用长跳转方式而不是直接跳转 */
    mov r0, #0
    mov r1, #0
    mov r2, #0
    
    /* 使用LDR + BX序列而不是直接B指令 */
    ldr r12, =HardFault_Handler_C
    bx r12
		nop
}

/**
 * Hard fault handler C function
 */
void HardFault_Handler_C(uint32_t *fault_args)
{
    volatile uint32_t stacked_r0 = fault_args[0];
    volatile uint32_t stacked_r1 = fault_args[1];
    volatile uint32_t stacked_r2 = fault_args[2];
    volatile uint32_t stacked_r3 = fault_args[3];
    volatile uint32_t stacked_r12 = fault_args[4];
    volatile uint32_t stacked_lr = fault_args[5];
    volatile uint32_t stacked_pc = fault_args[6];
    volatile uint32_t stacked_psr = fault_args[7];
    
    /* Collect standard error information */
    volatile uint32_t _CFSR = SCB->CFSR;
    volatile uint32_t _HFSR = SCB->HFSR;
    volatile uint32_t _DFSR = SCB->DFSR;
    volatile uint32_t _AFSR = SCB->AFSR;
    volatile uint32_t _BFAR = SCB->BFAR;
    volatile uint32_t _MMAR = SCB->MMFAR;
    
    /* Print error information */
    printf("\r\n\n[HARDFAULT]\r\n");
    printf("SP: 0x%08lx\r\n", (uint32_t)fault_args);
    printf("R0: 0x%08lx, R1: 0x%08lx\r\n", stacked_r0, stacked_r1);
    printf("R2: 0x%08lx, R3: 0x%08lx\r\n", stacked_r2, stacked_r3);
    printf("R12: 0x%08lx, LR: 0x%08lx\r\n", stacked_r12, stacked_lr);
    printf("PC: 0x%08lx, PSR: 0x%08lx\r\n", stacked_pc, stacked_psr);
    printf("CFSR: 0x%08lx, HFSR: 0x%08lx\r\n", _CFSR, _HFSR);
    printf("DFSR: 0x%08lx, AFSR: 0x%08lx\r\n", _DFSR, _AFSR);
    printf("BFAR: 0x%08lx, MMAR: 0x%08lx\r\n", _BFAR, _MMAR);
    
    /* Generate coredump using the enhanced architecture hook */
    printf("Generating CoreDump...\r\n");
    
    /* Call the architecture-specific coredump function */
    armv7m_hard_fault_exception_hook(fault_args);

    
    /* Check common error causes */
    if (_CFSR & SCB_CFSR_NOCP_Msk)
        printf("- Coprocessor access error\r\n");
    if (_CFSR & SCB_CFSR_INVPC_Msk)
        printf("- Invalid PC load (EXC_RETURN)\r\n");
    if (_CFSR & SCB_CFSR_INVSTATE_Msk)
        printf("- Invalid state - possible Thumb bit error\r\n");
    if (_CFSR & SCB_CFSR_UNDEFINSTR_Msk)
        printf("- Undefined instruction\r\n");

    /* Stop at error with LED blink */
    while(1) {

    }
}

/**
 * Hard fault handler assembly entry point
 */
__asm void HardFault_Handler(void)
{
    IMPORT HardFault_Handler_C
    
    PRESERVE8
    
    tst lr, #4              /* Test which stack was used */
    ite eq                  /* if-then-else block */
    mrseq r0, msp          /* If MSP, read MSP */
    mrsne r0, psp          /* Otherwise read PSP */
    
    b HardFault_Handler_C   /* Jump to C handler */
}

