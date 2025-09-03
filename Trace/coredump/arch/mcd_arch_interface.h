/*
 * Copyright (c) 2025, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2025-08-16     Rbb666       first version
 */

#ifndef __MCD_ARCH_INTERFACE_H__
#define __MCD_ARCH_INTERFACE_H__

#include <stdint.h>

/**
 * @brief Architecture-specific hard fault exception handler
 * 
 * This function should be implemented by each supported architecture
 * to handle the architecture-specific stack frame processing.
 * 
 * @param context Pointer to exception context (architecture-specific format)
 * @return int Always returns -1 to indicate fault condition
 */
#ifdef PKG_USING_MCOREDUMP_ARCH_ARMV7M
int armv7m_hard_fault_exception_hook(void *context);

/**
 * @brief Architecture-specific register collection function
 * 
 * This function extracts register values from the architecture-specific
 * stack frame format and stores them in the standard register sets.
 * 
 * @param stack_top Pointer to the stack frame
 * @param core_regset Pointer to store ARM core registers  
 * @param fp_regset Pointer to store FPU registers
 */
void collect_registers_armv7m(uint32_t *stack_top,
                             core_regset_type *core_regset,
                             fp_regset_type *fp_regset);

/* Generic interface macro for architecture-agnostic code */
#define collect_registers collect_registers_armv7m
#endif

#ifdef PKG_USING_MCOREDUMP_ARCH_ARMV8M
int armv8m_hard_fault_exception_hook(void *context);

/**
 * @brief Architecture-specific register collection function
 * 
 * This function extracts register values from the architecture-specific
 * stack frame format and stores them in the standard register sets.
 * 
 * @param stack_top Pointer to the stack frame
 * @param core_regset Pointer to store ARM core registers  
 * @param fp_regset Pointer to store FPU registers
 */
void collect_registers_armv8m(uint32_t *stack_top,
                             core_regset_type *core_regset,
                             fp_regset_type *fp_regset);

/* Generic interface macro for architecture-agnostic code */
#define collect_registers collect_registers_armv8m
#endif

#endif /* __MCD_ARCH_INTERFACE_H__ */
