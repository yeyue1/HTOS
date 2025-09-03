/*
 * Copyright (c) 2025, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2025-08-16     Rbb666       first version
 */

#ifndef __COREDUMP_H__
#define __COREDUMP_H__

#include "mcd_cfg.h"
#include "mcd_arm_define.h"

/**
 * @brief Function pointer type for coredump data output
 * 
 * This function pointer type defines the interface for writing coredump data
 * to different output destinations (memory, serial, filesystem, etc.).
 * 
 * @param data Pointer to the data buffer to be written
 * @param len Length of the data buffer in bytes
 * 
 * @note The implementation should handle the data appropriately based on the
 *       chosen output mode and ensure data integrity.
 */
typedef void (*mcd_writeout_func_t)(uint8_t *, int);

/**
 * @brief Thread information operations structure
 * 
 * This structure contains function pointers for retrieving thread information
 * in a RTOS environment. It's designed to avoid dynamic memory allocation
 * during fault handling, as malloc operations may fail during hard faults.
 * 
 * The structure provides a generic interface for different RTOS systems
 * to supply thread information for multi-threaded coredump generation.
 */
struct thread_info_ops
{
    /**
     * @brief Get total number of threads in the system
     * @param ops Pointer to this operations structure
     * @return Total thread count
     */
    int32_t (*get_threads_count)(struct thread_info_ops *);
    
    /**
     * @brief Get current executing thread index
     * @param ops Pointer to this operations structure  
     * @return Current thread index
     */
    int32_t (*get_current_thread_idx)(struct thread_info_ops *);
    
    /**
     * @brief Get register set for a specific thread
     * @param ops Pointer to this operations structure
     * @param thread_idx Thread index to get registers for
     * @param core_regset Pointer to store ARM core registers
     * @param fp_regset Pointer to store FPU registers
     */
    void (*get_thread_regset)(struct thread_info_ops *, int32_t,
                              core_regset_type *core_regset,
                              fp_regset_type *fp_regset);
    
    /**
     * @brief Get number of memory areas to dump
     * @param ops Pointer to this operations structure
     * @return Number of memory areas
     */
    int32_t (*get_memarea_count)(struct thread_info_ops *);
    
    /**
     * @brief Get memory area information
     * @param ops Pointer to this operations structure
     * @param area_idx Memory area index
     * @param addr Pointer to store memory area start address
     * @param size Pointer to store memory area size
     * @return 0 on success, negative on error
     */
    int32_t (*get_memarea)(struct thread_info_ops *, int32_t,
                           uint32_t *, uint32_t *);
    
    /** Private data for RTOS-specific implementations */
    void *priv;
};

/**
 * @brief Initialize the mCoreDump system
 *
 * This function initializes the mCoreDump system with automatic FPU detection.
 * It must be called before any coredump generation operations.
 *
 * FPU support is automatically detected based on compiler definitions:
 * - Enabled if __VFP_FP__ is defined and __SOFTFP__ is not defined
 * - Disabled if no hardware FPU support is detected
 *
 * @param func Function pointer to write the coredump data out.
 *             This callback will be called repeatedly with chunks of coredump data.
 *
 * @note The writeout function should handle data appropriately based on the
 *       chosen output mode (memory, serial, filesystem).
 *
 * @warning This function should be called from a safe context, preferably
 *          during system initialization or before fault occurs.
 *
 * @see mcd_writeout_func_t for callback function requirements
 */
void mcd_init(mcd_writeout_func_t func);

/**
 * @brief Generate coredump for current call chain only
 *
 * This function generates a minimal coredump containing only the current
 * execution context (single thread). It's faster and uses less memory
 * compared to multi-threaded coredump generation.
 *
 * Use this function when:
 * - Quick fault analysis is needed
 * - Memory is limited
 * - Only current thread context is relevant
 *
 * @note This function captures the current thread's register state and
 *       stack information at the point of call.
 *
 * @warning Must call mcd_init() before using this function.
 *
 * @see mcd_multi_dump() for full system coredump
 */
void mcd_mini_dump(void);

/**
 * @brief Generate coredump with all threads information
 *
 * This function generates a comprehensive coredump containing information
 * about all threads in the system, including their register states,
 * stack contents, and memory areas.
 *
 * Use this function when:
 * - Complete system state analysis is needed
 * - Multi-threading issues need to be debugged
 * - Full context switch information is required
 *
 * @note This function may take longer to execute and use more memory
 *       compared to mcd_mini_dump().
 *
 * @warning Must call mcd_init() before using this function.
 *          Should be used carefully in interrupt context due to execution time.
 *
 * @see mcd_mini_dump() for single thread coredump
 */
void mcd_multi_dump(void);

/**
 * @brief Generate coredump using provided thread operations
 *
 * This is the core coredump generation function that uses the provided
 * thread_info_ops structure to gather system information and generate
 * the ELF format coredump.
 *
 * @param ops Pointer to thread information operations structure.
 *            Contains function pointers for retrieving thread and memory information.
 *
 * @note This is a low-level function typically called by mcd_mini_dump()
 *       and mcd_multi_dump() after setting up appropriate ops structure.
 *
 * @warning The ops structure must be properly initialized with valid
 *          function pointers before calling this function.
 */
void mcd_gen_coredump(struct thread_info_ops *ops);

/**
 * @brief Setup thread operations for RTOS environment
 *
 * This function initializes the thread_info_ops structure with function
 * pointers appropriate for the current RTOS environment (RT-Thread).
 *
 * @param ops Pointer to thread operations structure to be initialized
 *
 * @note This function is typically called internally by mcd_multi_dump()
 *       to set up multi-threaded coredump generation.
 */
void mcd_rtos_thread_ops(struct thread_info_ops *ops);

/**
 * @brief Setup thread operations for single thread dump
 *
 * This function initializes the thread_info_ops structure for minimal
 * coredump generation (current thread only).
 *
 * @param ops Pointer to thread operations structure to be initialized
 *
 * @note This function is typically called internally by mcd_mini_dump()
 *       to set up single-threaded coredump generation.
 */
void mcd_mini_dump_ops(struct thread_info_ops *ops);

/**
 * @brief           Generate coredump and save to specified output.
 *
 * @param[in]       output_mode Output mode (MCD_OUTPUT_SERIAL, MCD_OUTPUT_MEMORY, or MCD_OUTPUT_FILESYSTEM).
 *
 * @return          Whether save operation success.
 * @retval          MCD_OK      success.
 * @retval          MCD_ERROR   failed.
 */
int mcd_faultdump(mcd_output_mode_t output_mode);

/**
 * @brief Print coredump memory information at startup
 *
 * This function should be called during system initialization
 */
void mcd_print_memoryinfo(void);

/**
 * @brief Get address of current core register set
 *
 * This function returns a pointer to the current thread's core register
 * set storage area. The register values are captured during fault handling.
 *
 * @return Pointer to core register set structure
 *
 * @note The returned pointer points to static storage that gets updated
 *       each time a fault occurs or registers are captured.
 */
core_regset_type *get_cur_core_regset_address(void);

/**
 * @brief Get address of current floating-point register set
 *
 * This function returns a pointer to the current thread's floating-point
 * register set storage area. The register values are captured during fault handling.
 *
 * @return Pointer to floating-point register set structure
 *
 * @note The returned pointer points to static storage that gets updated
 *       each time a fault occurs or registers are captured.
 *       Returns valid data only if FPU is available and enabled.
 */
fp_regset_type *get_cur_fp_regset_address(void);

#endif /* __MCOREDUMP_H__ */
