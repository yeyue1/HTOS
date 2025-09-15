/*
 * Copyright (c) 2025, HTOS Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2025-08-29     yeyue        HTOS OS abstraction layer for mCoreDump
 */

<<<<<<< HEAD
<<<<<<< HEAD
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../inc/coredump.h"
#include "../arch/mcd_arch_interface.h"
#include "httask.h"

/* ARM CMSIS core functions - use inline assembly for Keil */
#if defined(__CC_ARM)
__asm static void __disable_irq_impl(void)
{
    cpsid i
    bx lr
}
=======
=======
>>>>>>> main/main
#include "../arch/mcd_arch_interface.h"
#include "../inc/coredump.h"
#include "httask.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/* ARM CMSIS core functions - use inline assembly for Keil */
#if defined(__CC_ARM)
__asm static void __disable_irq_impl(void){cpsid i bx lr}
<<<<<<< HEAD
>>>>>>> db6a41e (change)
=======
>>>>>>> main/main
#define __disable_irq() __disable_irq_impl()
#elif defined(__ARMCC_VERSION) && (__ARMCC_VERSION >= 6010050)
#include "cmsis_compiler.h"
#else
/* For other compilers, use direct inline assembly */
<<<<<<< HEAD
<<<<<<< HEAD
static inline void __disable_irq_impl(void)
{
    __asm volatile ("cpsid i" : : : "memory");
=======
static inline void __disable_irq_impl(void) {
  __asm volatile("cpsid i" : : : "memory");
>>>>>>> db6a41e (change)
=======
static inline void __disable_irq_impl(void) {
  __asm volatile("cpsid i" : : : "memory");
>>>>>>> main/main
}
#define __disable_irq() __disable_irq_impl()
#endif

<<<<<<< HEAD
<<<<<<< HEAD
=======
=======
>>>>>>> main/main
void __aeabi_assert(const char *expr, const char *file, int line) {

  printf("ASSERT failed: %s, file: %s, line: %d\r\n", expr ? expr : "(null)",
         file ? file : "(null)", line);

  /* 可调用已有断言钩子以生成 coredump */
  __disable_irq();
  mcd_faultdump(MCD_OUTPUT_SERIAL);

  /* 停在这里，便于调试 */
  for (;;)
    ;
}

<<<<<<< HEAD
>>>>>>> db6a41e (change)
=======
>>>>>>> main/main
/* Forward declarations */
core_regset_type *get_cur_core_regset_address(void);
fp_regset_type *get_cur_fp_regset_address(void);
void mcd_gen_coredump(struct thread_info_ops *ops);

/*
 * HTOS OS abstraction layer implementation for MCoreDump
 */
<<<<<<< HEAD
<<<<<<< HEAD
typedef struct
{
    int32_t thr_cnts;
    int32_t cur_idx;
=======
typedef struct {
  int32_t thr_cnts;
  int32_t cur_idx;
>>>>>>> db6a41e (change)
=======
typedef struct {
  int32_t thr_cnts;
  int32_t cur_idx;
>>>>>>> main/main
} htos_ti_priv_t;

extern htTCB_t *pxCurrentTCB;

<<<<<<< HEAD
<<<<<<< HEAD
static int32_t htos_thr_cnts(struct thread_info_ops *ops)
{
    htos_ti_priv_t *priv = (htos_ti_priv_t *)ops->priv;
    
    if (priv->thr_cnts == -1)
    {
        /* For simplified implementation, we'll just count current task + 1 idle task */
        priv->thr_cnts = 2;  /* Current task + idle task */
        priv->cur_idx = 0;   /* Current task is at index 0 */
    }

    return priv->thr_cnts;
}

static int32_t htos_cur_idx(struct thread_info_ops *ops)
{
    htos_ti_priv_t *priv = (htos_ti_priv_t *)ops->priv;

    if (priv->cur_idx == -1)
    {
        htos_thr_cnts(ops);  /* Initialize if not done */
    }

    return priv->cur_idx;
=======
=======
>>>>>>> main/main
static int32_t htos_thr_cnts(struct thread_info_ops *ops) {
  htos_ti_priv_t *priv = (htos_ti_priv_t *)ops->priv;

  if (priv->thr_cnts == -1) {
    /* For simplified implementation, we'll just count current task + 1 idle
     * task */
    priv->thr_cnts = 2; /* Current task + idle task */
    priv->cur_idx = 0;  /* Current task is at index 0 */
  }

  return priv->thr_cnts;
}

static int32_t htos_cur_idx(struct thread_info_ops *ops) {
  htos_ti_priv_t *priv = (htos_ti_priv_t *)ops->priv;

  if (priv->cur_idx == -1) {
    htos_thr_cnts(ops); /* Initialize if not done */
  }

  return priv->cur_idx;
<<<<<<< HEAD
>>>>>>> db6a41e (change)
=======
>>>>>>> main/main
}

static void htos_thr_rset(struct thread_info_ops *ops, int32_t idx,
                          core_regset_type *core_regset,
<<<<<<< HEAD
<<<<<<< HEAD
                          fp_regset_type *fp_regset)
{
    if (!core_regset) return;
    mcd_memset(core_regset, 0, sizeof(core_regset_type));
    if (fp_regset) mcd_memset(fp_regset, 0, sizeof(fp_regset_type));

    /* 1) 如果架构层已经收集了寄存器（如HardFault时），优先直接使用它 */
    core_regset_type *arch = get_cur_core_regset_address();
    if (arch && (arch->pc != 0 || arch->xpsr != 0))
    {
        *core_regset = *arch;

        return;
    }

    /* 2) 非异常场景，从当前任务TCB的pxTopOfStack恢复 */
    if (idx != 0 || pxCurrentTCB == NULL) return;

    uint32_t *stk = (uint32_t *)(pxCurrentTCB->pxTopOfStack);
    if (stk == NULL)
    {
        
        return;
    }
    uint32_t addr = (uint32_t)stk;
    if (addr < 0x20000000UL || addr > 0x20020000UL)
    {
 
        return;
    }

    /* Cortex-M硬件异常帧: R0,R1,R2,R3,R12,LR,PC,xPSR */
    core_regset->r0   = stk[0];
    core_regset->r1   = stk[1];
    core_regset->r2   = stk[2];
    core_regset->r3   = stk[3];
    core_regset->r12  = stk[4];
    core_regset->lr   = stk[5];
    core_regset->pc   = stk[6];     /* 保留Thumb位 */
    core_regset->xpsr = stk[7];
    core_regset->sp   = (uint32_t)stk;

    /* 可能的软件保存R4-R11（按你的端口实际布局调整） */
    uint32_t *maybe_r4 = stk + 8;
    if ((uint32_t)maybe_r4 >= 0x20000000UL && (uint32_t)maybe_r4 < 0x20020000UL)
    {
        core_regset->r4  = maybe_r4[0];
        core_regset->r5  = maybe_r4[1];
        core_regset->r6  = maybe_r4[2];
        core_regset->r7  = maybe_r4[3];
        core_regset->r8  = maybe_r4[4];
        core_regset->r9  = maybe_r4[5];
        core_regset->r10 = maybe_r4[6];
        core_regset->r11 = maybe_r4[7];
    }
}

static int32_t htos_get_mem_cnts(struct thread_info_ops *ops)
{
    /* Return 2 memory areas: current task stack + main RAM */
    return 2;
}

static int32_t htos_get_memarea(struct thread_info_ops *ops, int32_t idx,
                                uint32_t *addr, uint32_t *memlen)
{
    if (idx == 0 && pxCurrentTCB != NULL)  /* Current task stack */
    {
        /* Get current task stack area - make sure it's valid */
        if (pxCurrentTCB->pxStack != NULL)
        {
            uint32_t stack_addr = (uint32_t)((uintptr_t)pxCurrentTCB->pxStack);
            /* Validate stack address is in SRAM range */
            if (stack_addr >= 0x20000000 && stack_addr < 0x20005000)
            {
                *addr = stack_addr;
                *memlen = 4096;  /* Capture smaller stack slice (减少内存占用) */
                return 0;
            }
        }
        /* Fallback if current task stack is invalid */
        *addr = 0x20000000;  /* Start of SRAM */
        *memlen = 4096;
        return 0;
    }
    else if (idx == 1)  /* Main RAM area */
    {
        /* Capture main SRAM area */
        *addr = 0x20000000;  /* Start of SRAM */
        *memlen = 4096;      /* 降低为 1KB，进一步减少内存占用 */
        return 0;
    }
    
    return -1;
}

void mcd_htos_thread_ops(struct thread_info_ops *ops)
{
    static htos_ti_priv_t priv;
    ops->get_threads_count = htos_thr_cnts;
    ops->get_current_thread_idx = htos_cur_idx;
    ops->get_thread_regset = htos_thr_rset;
    ops->get_memarea_count = htos_get_mem_cnts;
    ops->get_memarea = htos_get_memarea;
    ops->priv = &priv;
    priv.cur_idx = -1;
    priv.thr_cnts = -1;
}

MCD_WEAK int htos_hard_fault_exception_hook(void *context)
{
    arch_hard_fault_exception_hook(context);
    return -1;
}

MCD_WEAK void htos_assert_hook(const char *file, int line, const char *expr)
{
    volatile uint8_t _continue = 1;

    __disable_irq();

    mcd_print("ASSERT failed at %s:%d: %s\n", file, line, expr);

    mcd_faultdump(MCD_OUTPUT_SERIAL);

    while (_continue == 1);
}

static int mcd_coredump_init(void) 
{
    static mcd_bool_t is_init = MCD_FALSE;

    if (is_init)
    {
        return 0;
    }

    mcd_print("HTOS CoreDump initialized\n");
    mcd_print_memoryinfo();
    
    is_init = MCD_TRUE;
    return 0;
=======
=======
>>>>>>> main/main
                          fp_regset_type *fp_regset) {
  if (!core_regset)
    return;
  mcd_memset(core_regset, 0, sizeof(core_regset_type));
  if (fp_regset)
    mcd_memset(fp_regset, 0, sizeof(fp_regset_type));

  /* 1) 如果架构层已经收集了寄存器（如HardFault时），优先直接使用它 */
  core_regset_type *arch = get_cur_core_regset_address();
  if (arch && (arch->pc != 0 || arch->xpsr != 0)) {
    *core_regset = *arch;

    return;
  }

  /* 2) 非异常场景，从当前任务TCB的pxTopOfStack恢复 */
  if (idx != 0 || pxCurrentTCB == NULL)
    return;

  uint32_t *stk = (uint32_t *)(pxCurrentTCB->pxTopOfStack);
  if (stk == NULL) {

    return;
  }
  uint32_t addr = (uint32_t)stk;
  if (addr < 0x20000000UL || addr > 0x20020000UL) {

    return;
  }

  /* Cortex-M硬件异常帧: R0,R1,R2,R3,R12,LR,PC,xPSR */
  core_regset->r0 = stk[0];
  core_regset->r1 = stk[1];
  core_regset->r2 = stk[2];
  core_regset->r3 = stk[3];
  core_regset->r12 = stk[4];
  core_regset->lr = stk[5];
  core_regset->pc = stk[6]; /* 保留Thumb位 */
  core_regset->xpsr = stk[7];
  core_regset->sp = (uint32_t)stk;

  /* 可能的软件保存R4-R11（按你的端口实际布局调整） */
  uint32_t *maybe_r4 = stk + 8;
  if ((uint32_t)maybe_r4 >= 0x20000000UL && (uint32_t)maybe_r4 < 0x20020000UL) {
    core_regset->r4 = maybe_r4[0];
    core_regset->r5 = maybe_r4[1];
    core_regset->r6 = maybe_r4[2];
    core_regset->r7 = maybe_r4[3];
    core_regset->r8 = maybe_r4[4];
    core_regset->r9 = maybe_r4[5];
    core_regset->r10 = maybe_r4[6];
    core_regset->r11 = maybe_r4[7];
  }
}

static int32_t htos_get_mem_cnts(struct thread_info_ops *ops) {
  /* Return 2 memory areas: current task stack + main RAM */
  return 2;
}

static int32_t htos_get_memarea(struct thread_info_ops *ops, int32_t idx,
                                uint32_t *addr, uint32_t *memlen) {
  if (idx == 0 && pxCurrentTCB != NULL) /* Current task stack */
  {
    /* Get current task stack area - make sure it's valid */
    if (pxCurrentTCB->pxStack != NULL) {
      uint32_t stack_addr = (uint32_t)((uintptr_t)pxCurrentTCB->pxStack);
      /* Validate stack address is in SRAM range */
      if (stack_addr >= 0x20000000 && stack_addr < 0x20005000) {
        *addr = stack_addr;
        *memlen = 4096; /* Capture smaller stack slice (减少内存占用) */
        return 0;
      }
    }
    /* Fallback if current task stack is invalid */
    *addr = 0x20000000; /* Start of SRAM */
    *memlen = 4096;
    return 0;
  } else if (idx == 1) /* Main RAM area */
  {
    /* Capture main SRAM area */
    *addr = 0x20000000; /* Start of SRAM */
    *memlen = 4096;     /* 降低为 1KB，进一步减少内存占用 */
    return 0;
  }

  return -1;
}

void mcd_htos_thread_ops(struct thread_info_ops *ops) {
  static htos_ti_priv_t priv;
  ops->get_threads_count = htos_thr_cnts;
  ops->get_current_thread_idx = htos_cur_idx;
  ops->get_thread_regset = htos_thr_rset;
  ops->get_memarea_count = htos_get_mem_cnts;
  ops->get_memarea = htos_get_memarea;
  ops->priv = &priv;
  priv.cur_idx = -1;
  priv.thr_cnts = -1;
}

MCD_WEAK int htos_hard_fault_exception_hook(void *context) {
  arch_hard_fault_exception_hook(context);
  return -1;
}

MCD_WEAK void htos_assert_hook(const char *file, int line, const char *expr) {
  volatile uint8_t _continue = 1;

  __disable_irq();

  mcd_print("ASSERT failed at %s:%d: %s\n", file, line, expr);

  mcd_faultdump(MCD_OUTPUT_SERIAL);

  while (_continue == 1)
    ;
}

static int mcd_coredump_init(void) {
  static mcd_bool_t is_init = MCD_FALSE;

  if (is_init) {
    return 0;
  }

  mcd_print("HTOS CoreDump initialized\n");
  mcd_print_memoryinfo();

  is_init = MCD_TRUE;
  return 0;
<<<<<<< HEAD
>>>>>>> db6a41e (change)
=======
>>>>>>> main/main
}

/*
 * =============================================================================
 * 对外接口实现 - 简化版本，供 main 和任务调用
 * =============================================================================
 */

/**
 * @brief 初始化 MCoreDump 系统
<<<<<<< HEAD
<<<<<<< HEAD
 * 
 * @return int 0:成功, -1:失败
 */
int mcd_htos_init(void)
{
    return mcd_coredump_init();
}

/**
 * @brief 生成 CoreDump 并通过串口输出
 * 
 * 用法：在 main 或任务中调用
 * 
 * @return int 0:成功, -1:失败
 */
int mcd_dump_serial(void)
{
    printf("Starting CoreDump via Serial...\n");
    return mcd_faultdump(MCD_OUTPUT_SERIAL);
=======
=======
>>>>>>> main/main
 *
 * @return int 0:成功, -1:失败
 */
int mcd_htos_init(void) { return mcd_coredump_init(); }

/**
 * @brief 生成 CoreDump 并通过串口输出
 *
 * 用法：在 main 或任务中调用
 *
 * @return int 0:成功, -1:失败
 */
int mcd_dump_serial(void) {
  printf("Starting CoreDump via Serial...\n");
  return mcd_faultdump(MCD_OUTPUT_SERIAL);
<<<<<<< HEAD
>>>>>>> db6a41e (change)
=======
>>>>>>> main/main
}

/**
 * @brief 生成 CoreDump 并保存到内存
<<<<<<< HEAD
<<<<<<< HEAD
 * 
 * 用法：在异常处理或需要保存现场时调用
 * 
 * @return int 0:成功, -1:失败
 */
int mcd_dump_memory(void)
{
    printf("Starting CoreDump to Memory...\n");
    return mcd_faultdump(MCD_OUTPUT_MEMORY);
=======
=======
>>>>>>> main/main
 *
 * 用法：在异常处理或需要保存现场时调用
 *
 * @return int 0:成功, -1:失败
 */
int mcd_dump_memory(void) {
  printf("Starting CoreDump to Memory...\n");
  return mcd_faultdump(MCD_OUTPUT_MEMORY);
<<<<<<< HEAD
>>>>>>> db6a41e (change)
=======
>>>>>>> main/main
}

/**
 * @brief 触发一个断言错误进行测试
<<<<<<< HEAD
<<<<<<< HEAD
 * 
 * 用法：mcd_test_assert();
 */
void mcd_test_assert(void)
{
    int x = 42;
    int y = 24;
    mcd_assert(x == y);  /* 这会触发断言失败 */
=======
=======
>>>>>>> main/main
 *
 * 用法：mcd_test_assert();
 */
void mcd_test_assert(void) {
  int x = 42;
  int y = 24;
  mcd_assert(x == y); /* 这会触发断言失败 */
<<<<<<< HEAD
>>>>>>> db6a41e (change)
=======
>>>>>>> main/main
}

/**
 * @brief 触发一个内存访问错误进行测试
<<<<<<< HEAD
<<<<<<< HEAD
 * 
 * 用法：mcd_test_fault();
 */
void mcd_test_fault(void)
{
    volatile uint32_t *bad_ptr = (volatile uint32_t *)0xFFFFFFFF;
    *bad_ptr = 0x12345678;  /* 这会触发 HardFault */
=======
=======
>>>>>>> main/main
 *
 * 用法：mcd_test_fault();
 */
void mcd_test_fault(void) {
  volatile uint32_t *bad_ptr = (volatile uint32_t *)0xFFFFFFFF;
  *bad_ptr = 0x12345678; /* 这会触发 HardFault */
<<<<<<< HEAD
>>>>>>> db6a41e (change)
=======
>>>>>>> main/main
}

/**
 * @brief 执行 CoreDump 测试命令
<<<<<<< HEAD
<<<<<<< HEAD
 * 
 * @param command 命令字符串："SERIAL", "MEMORY", "ASSERT", "FAULT"
 * @return int 0:成功, -1:失败
 */
int mcd_test_htos(const char *command)
{
    if (command == NULL)
    {
        printf("Usage: mcd_test_htos <SERIAL|MEMORY|ASSERT|FAULT>\n");
        return -1;
    }

    if (strcmp(command, "SERIAL") == 0)
    {
        return mcd_dump_serial();
    }
    else if (strcmp(command, "MEMORY") == 0)
    {
        return mcd_dump_memory();
    }
    else if (strcmp(command, "ASSERT") == 0)
    {
        mcd_test_assert();
        return 0;  /* 不会执行到这里 */
    }
    else if (strcmp(command, "FAULT") == 0)
    {
        mcd_test_fault();
        return 0;  /* 不会执行到这里 */
    }
    else
    {
        printf("Unknown command: %s\n", command);
        return -1;
    }
}

/* 兼容层：提供 coredump core 期望的导出接口 */
void mcd_rtos_thread_ops(struct thread_info_ops *ops)
{
    mcd_htos_thread_ops(ops);
}

/* arch 硬fault hook 默认弱实现（可被平台实现覆盖） */
MCD_WEAK int arch_hard_fault_exception_hook(void *context)
{
    (void)context;
    return -1;
=======
=======
>>>>>>> main/main
 *
 * @param command 命令字符串："SERIAL", "MEMORY", "ASSERT", "FAULT"
 * @return int 0:成功, -1:失败
 */
int mcd_test_htos(const char *command) {
  if (command == NULL) {
    printf("Usage: mcd_test_htos <SERIAL|MEMORY|ASSERT|FAULT>\n");
    return -1;
  }

  if (strcmp(command, "SERIAL") == 0) {
    return mcd_dump_serial();
  } else if (strcmp(command, "MEMORY") == 0) {
    return mcd_dump_memory();
  } else if (strcmp(command, "ASSERT") == 0) {
    mcd_test_assert();
    return 0; /* 不会执行到这里 */
  } else if (strcmp(command, "FAULT") == 0) {
    mcd_test_fault();
    return 0; /* 不会执行到这里 */
  } else {
    printf("Unknown command: %s\n", command);
    return -1;
  }
}

/* 兼容层：提供 coredump core 期望的导出接口 */
void mcd_rtos_thread_ops(struct thread_info_ops *ops) {
  mcd_htos_thread_ops(ops);
}

/* arch 硬fault hook 默认弱实现（可被平台实现覆盖） */
MCD_WEAK int arch_hard_fault_exception_hook(void *context) {
  (void)context;
  return -1;
<<<<<<< HEAD
>>>>>>> db6a41e (change)
=======
>>>>>>> main/main
}