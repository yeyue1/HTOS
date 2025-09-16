/*
 * Copyright (c) 2025, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2025-08-16     Yeyue       Configuration file for mCoreDump
 */

#ifndef __MCD_CFG_H__
#define __MCD_CFG_H__

/*
 * ===============================================================
 * Operating System Detection and Abstraction Layer
 * ===============================================================
 */

/* Auto-detect operating system */
#ifdef __RTTHREAD__
#define MCD_OS_RTTHREAD 1
#include <rtthread.h>
#else
#define MCD_OS_BAREMETAL 1
#include <stdbool.h>
#include <stdint.h>

#endif

#define MCD_ALIGN(x, y) (((x - 1) / y + 1) * y)

/*
 * ===============================================================
 * Print configuration - Printf function abstraction
 * ===============================================================
 */

/* OS-specific print function abstraction */
#ifdef MCD_OS_RTTHREAD
/* RT-Thread print functions */
#ifndef RT_USING_ULOG
/* Use rt_kprintf directly */
#define mcd_println(...)                                                       \
  rt_kprintf(__VA_ARGS__);                                                     \
  rt_kprintf("\r\n")
#define mcd_print(...) rt_kprintf(__VA_ARGS__)
#else /* RT_USING_ULOG */
/* Use ulog for unified logging */
#include <ulog.h>
#define MCD_LOG_TAG "mcd"
#define mcd_println(...)                                                       \
  ulog_e(MCD_LOG_TAG, __VA_ARGS__);                                            \
  ulog_flush()
#define mcd_print(...) ulog_raw(__VA_ARGS__)
#endif /* RT_USING_ULOG */
#else
/* Bare metal - use standard printf or define custom */
#include <stdio.h>
#define mcd_println(...)                                                       \
  printf(__VA_ARGS__);                                                         \
  printf("\r\n")
#define mcd_print(...) printf(__VA_ARGS__)
#endif /* MCD_OS_RTTHREAD */

/*
 * ===============================================================
 * Memory management abstraction
 * ===============================================================
 */

#ifdef MCD_OS_RTTHREAD
#define mcd_malloc(size) rt_malloc(size)
#define mcd_free(ptr) rt_free(ptr)
#define mcd_memset(ptr, val, size) rt_memset(ptr, val, size)
#define mcd_memcpy(dst, src, size) rt_memcpy(dst, src, size)
#else
/* Standard C library */
#include <stdlib.h>
#include <string.h>
#define mcd_malloc(size) malloc(size)
#define mcd_free(ptr) free(ptr)
#define mcd_memset(ptr, val, size) memset(ptr, val, size)
#define mcd_memcpy(dst, src, size) memcpy(dst, src, size)
#endif

/*
 * ===============================================================
 * Interrupt control abstraction
 * ===============================================================
 */

#ifdef MCD_OS_RTTHREAD
typedef rt_base_t mcd_irq_state_t;
#define mcd_irq_disable() rt_hw_interrupt_disable()
#define mcd_irq_enable(state) rt_hw_interrupt_enable(state)
#else
/* Bare metal - define as needed for specific platform */
typedef uint32_t mcd_irq_state_t;
#define mcd_irq_disable() __disable_irq();
#define mcd_irq_enable(state) __enable_irq()
#endif

/*
 * ===============================================================
 * Command export abstraction
 * ===============================================================
 */

#ifdef MCD_OS_RTTHREAD
#define MCD_CMD_EXPORT(cmd, desc) MSH_CMD_EXPORT(cmd, desc)
#else
/* Other OS or bare metal */
#define MCD_CMD_EXPORT(cmd, desc) /* No command export */
#endif

/*
 * ===============================================================
 * Boolean type abstraction
 * ===============================================================
 */

#ifdef MCD_OS_RTTHREAD
#define mcd_bool_t rt_bool_t
#define MCD_TRUE RT_TRUE
#define MCD_FALSE RT_FALSE
#else
#include <stdbool.h>
#define mcd_bool_t bool
#define MCD_TRUE true
#define MCD_FALSE false
#endif

/*
 * ===============================================================
 * ASSERT type abstraction
 * ===============================================================
 */

#ifdef MCD_OS_RTTHREAD
#define mcd_assert(expr) RT_ASSERT(expr)
#else
#include <assert.h>
#define mcd_assert(expr) assert(expr)
#endif /* MCD_OS_RTTHREAD */

/*
 * ===============================================================
 * Weak attribute abstraction
 * ===============================================================
 */

#ifdef MCD_OS_RTTHREAD
#ifndef RT_WEAK
#define MCD_WEAK rt_weak
#else
#define MCD_WEAK RT_WEAK
#endif /* RT_WEAK */
#else
/* Standard GCC weak attribute */
#define MCD_WEAK __attribute__((weak))
#endif /* MCD_OS_RTTHREAD */

/*
 * ===============================================================
 * Floating Point Unit (FPU) configuration
 * ===============================================================
 */

/* Automatic FPU detection - based on compiler definitions */
#if /* ARMCC */ (                                                              \
    (defined(__CC_ARM) && defined(__TARGET_FPU_VFP)) /* Clang */ ||            \
    (defined(__clang__) && defined(__VFP_FP__) &&                              \
     !defined(__SOFTFP__)) /* IAR */                                           \
    || (defined(__ICCARM__) && defined(__ARMVFP__)) /* GNU */ ||               \
    (defined(__GNUC__) && defined(__VFP_FP__) && !defined(__SOFTFP__)))
#define MCD_FPU_SUPPORT 1 /* FPU supported and enabled */
#else
#define MCD_FPU_SUPPORT 0 /* No FPU support */
#endif

/*
 * ===============================================================
 * Memory configuration
 * ===============================================================
 */

/* Default memory buffer size (can be overridden by Kconfig) */
// #ifndef PKG_MCOREDUMP_MEMORY_SIZE
// #define MCD_DEFAULT_MEMORY_SIZE           (8 * 1024)    /* Default 8KB */
// #else
// #define MCD_DEFAULT_MEMORY_SIZE           PKG_MCOREDUMP_MEMORY_SIZE
// #endif /* PKG_MCOREDUMP_MEMORY_SIZE */

/*
 * ===============================================================
 * Filesystem configuration
 * ===============================================================
 */

#ifdef PKG_USING_MCOREDUMP_FILESYSTEM
/* Default filesystem settings */
#ifndef PKG_MCOREDUMP_FILESYSTEM_DIR
#define MCD_DEFAULT_COREDUMP_DIR "/sdcard"
#else
#define MCD_DEFAULT_COREDUMP_DIR PKG_MCOREDUMP_FILESYSTEM_DIR
#endif /* PKG_MCOREDUMP_FILESYSTEM_DIR*/

#ifndef PKG_MCOREDUMP_FILESYSTEM_PREFIX
#define MCD_DEFAULT_COREDUMP_PREFIX "core_"
#else
#define MCD_DEFAULT_COREDUMP_PREFIX PKG_MCOREDUMP_FILESYSTEM_PREFIX
#endif /* PKG_MCOREDUMP_FILESYSTEM_PREFIX */

#define MCD_DEFAULT_COREDUMP_EXT ".elf"
#endif /* PKG_USING_MCOREDUMP_FILESYSTEM */

/*
 * ===============================================================
 * Output mode definitions
 * ===============================================================
 */

/* Coredump output modes */
typedef enum {
  MCD_OUTPUT_SERIAL = 0, /* Output via serial port */
  MCD_OUTPUT_MEMORY,     /* Store in memory buffer */
  MCD_OUTPUT_FILESYSTEM, /* Save to filesystem */
  MCD_OUTPUT_FLASH       /* Store in flash (reserved) */
} mcd_output_mode_t;

/* Return codes */
enum {
  MCD_ERROR = -1,
  MCD_OK = 0,
};

#endif /* __MCD_CFG_H__ */
