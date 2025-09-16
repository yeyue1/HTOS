/*
 * Copyright (c) 2025, HTOS Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2025-08-29     yeyue        HTOS OS abstraction layer for mCoreDump
 */

#include "../arch/mcd_arch_interface.h"
#include "../inc/coredump.h"
#include "httask.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "htlist.h"

/* ARM CMSIS core functions - use inline assembly for Keil */
// clang-format off
#if defined(__CC_ARM)
__asm static void __disable_irq_impl(void){
  cpsid i
  bx lr
}
// clang-format on
#define __disable_irq() __disable_irq_impl()
#elif defined(__ARMCC_VERSION) && (__ARMCC_VERSION >= 6010050)
#include "cmsis_compiler.h"
#else
/* For other compilers, use direct inline assembly */
static inline void __disable_irq_impl(void)
{
	__asm volatile("cpsid i" : : : "memory");
}
#define __disable_irq() __disable_irq_impl()
#endif

void __aeabi_assert(const char *expr, const char *file, int line)
{

	printf("ASSERT failed: %s, file: %s, line: %d\r\n", expr ? expr : "(null)", file ? file : "(null)", line);

	/* 可调用已有断言钩子以生成 coredump */
	__disable_irq();
	mcd_faultdump(MCD_OUTPUT_SERIAL);

	/* 停在这里，便于调试 */
	for (;;)
		;
}

/* Forward declarations */
core_regset_type *get_cur_core_regset_address(void);
fp_regset_type *get_cur_fp_regset_address(void);
void mcd_gen_coredump(struct thread_info_ops *ops);

/*
 * HTOS OS abstraction layer implementation for MCoreDump
 */
typedef struct {
	int32_t thr_cnts;
	int32_t cur_idx;
} htos_ti_priv_t;

extern htTCB_t *pxCurrentTCB;

static int32_t htos_thr_cnts(struct thread_info_ops *ops)
{
	htos_ti_priv_t *priv = (htos_ti_priv_t *)ops->priv;
	extern UBaseType_t uxCurrentNumberOfTasks;
	// 获取任务数量
	if (priv->thr_cnts == -1) {
		priv->thr_cnts = uxCurrentNumberOfTasks;
		if (priv->thr_cnts < 0) {
			priv->thr_cnts = 2;
		}
	}

	return priv->thr_cnts;
}

static int32_t htos_cur_idx(struct thread_info_ops *ops)
{
	htos_ti_priv_t *priv = (htos_ti_priv_t *)ops->priv;

	if (priv->cur_idx == -1) {
		htos_thr_cnts(ops); /* Initialize if not done */
	}

	return priv->cur_idx;
}

static void htos_thr_rset(
		struct thread_info_ops *ops, int32_t idx, core_regset_type *core_regset, fp_regset_type *fp_regset)
{
	if (!core_regset)
		return;
	mcd_memset(core_regset, 0, sizeof(core_regset_type));
	if (fp_regset)
		mcd_memset(fp_regset, 0, sizeof(fp_regset_type));

	/* 1) 如果架构层已经收集了寄存器（如HardFault时），优先直接使用它 */
	core_regset_type *arch = get_cur_core_regset_address();
	if (idx == 0 && arch && (arch->pc != 0 || arch->xpsr != 0)) {
		*core_regset = *arch;
		return;
	}

	/* 2) 遍历任务列表以找到指定idx的任务TCB */
	htList_t *taskList = &pxAllocatedTasksList; // 返回指针，避免拷贝链表头
	if (taskList == NULL)
		return;
	UBaseType_t n = htListGetLength(taskList); // 固定迭代次数，避免环形死循环
	htListItem_t *item = htListGetHead(taskList);
	for (UBaseType_t current_task_index = 0; current_task_index < n && item != NULL; ++current_task_index) {
		if ((int32_t)current_task_index == idx) {
			htTCB_t *pxTCB = (htTCB_t *)htListGetItemOwner(item);
			if (pxTCB != NULL) {
				/* 找到匹配的任务TCB，开始提取寄存器 */
				uint32_t *stk = (uint32_t *)(pxTCB->pxTopOfStack);
				uint32_t addr = (uint32_t)stk;
				if (stk == NULL || addr < 0x20000000UL || addr > 0x20020000UL) {
					return; /* 堆栈指针无效，直接返回 */
				}
				/* 软件保存的寄存器 R4-R11 */
				core_regset->r4 = stk[0];
				core_regset->r5 = stk[1];
				core_regset->r6 = stk[2];
				core_regset->r7 = stk[3];
				core_regset->r8 = stk[4];
				core_regset->r9 = stk[5];
				core_regset->r10 = stk[6];
				core_regset->r11 = stk[7];
				/* 硬件自动保存的寄存器 */
				core_regset->r0 = stk[8];
				core_regset->r1 = stk[9];
				core_regset->r2 = stk[10];
				core_regset->r3 = stk[11];
				core_regset->r12 = stk[12];
				core_regset->lr = stk[13];
				core_regset->pc = stk[14]; /* Thumb 位保留 */
				core_regset->xpsr = stk[15];
				core_regset->sp = (uint32_t)(stk + 16);
				return; /* 成功找到并处理，退出函数 */
			}
		}
		item = item->pxNext;
	}
}

static int32_t htos_get_mem_cnts(struct thread_info_ops *ops)
{
	/* 获取内存区域数量 */

	// 获取目前的任务数量
	htos_ti_priv_t *priv = (htos_ti_priv_t *)ops->priv;
	if (priv->thr_cnts == -1) {
		htos_thr_cnts(ops);
	}
	int32_t cnts = 2; /* 默认2个内存区域 */
	// 根据目前任务数量，决定是否增加任务栈区域
	if (priv->thr_cnts > 0) {
		cnts = priv->thr_cnts + 1; /* 主栈+每个任务一个栈区域  */
	}

	return cnts;
}

static int32_t htos_get_memarea(struct thread_info_ops *ops, int32_t idx, uint32_t *addr, uint32_t *memlen)
{
	htos_ti_priv_t *priv = (htos_ti_priv_t *)ops->priv;

	/* 确保已经初始化任务数量 */
	if (priv->thr_cnts == -1) {
		htos_thr_cnts(ops);
	}

	/* 主内存区域 (idx == 0) */
	if (idx == 0) {
		*addr = 0x20000000; /* SRAM 起始地址 */
		*memlen = 2048; /* 主内存区域，稍大一些 */
		return 0;
	}

	/* 任务堆栈区域 (idx >= 1) */
	int32_t task_idx = idx - 1; /* 任务索引从0开始 */
	if (task_idx >= 0 && task_idx < priv->thr_cnts) {
		/* 遍历任务列表找到对应的任务 */
		htList_t taskList = pxAllocatedTasksList;
		htListItem_t *item = htListGetHead(&taskList);
		int32_t current_task_index = 0;

		while (item != NULL) {
			if (current_task_index == task_idx) {
				htTCB_t *pxTCB = (htTCB_t *)htListGetItemOwner(item);
				if (pxTCB != NULL && pxTCB->pxStack != NULL) {
					uint32_t stack_addr = (uint32_t)((uintptr_t)pxTCB->pxStack);
					/* 验证堆栈地址在有效范围内 */
					if (stack_addr >= 0x20000000 && stack_addr < 0x20020000) {
						*addr = stack_addr;
						*memlen = 2048; /* 每个任务堆栈区域 */
						return 0;
					}
				}
				break; /* 找到对应任务但堆栈无效，跳出循环 */
			}
			current_task_index++;
			item = item->pxNext;
		}

		/* 如果找不到有效的任务堆栈，返回一个默认的堆栈区域 */
		*addr = 0x20001000 + (task_idx * 2048); /* 为每个任务分配不同的默认区域 */
		*memlen = 2048;
		return 0;
	}

	return -1; /* 超出范围 */
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

	while (_continue == 1)
		;
}

static int mcd_coredump_init(void)
{
	static mcd_bool_t is_init = MCD_FALSE;

	if (is_init) {
		return 0;
	}

	mcd_print("HTOS CoreDump initialized\n");
	mcd_print_memoryinfo();

	is_init = MCD_TRUE;
	return 0;
}

/*
 * =============================================================================
 * 对外接口实现 - 简化版本，供 main 和任务调用
 * =============================================================================
 */

/**
 * @brief 初始化 MCoreDump 系统
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
}

/**
 * @brief 生成 CoreDump 并保存到内存
 *
 * 用法：在异常处理或需要保存现场时调用
 *
 * @return int 0:成功, -1:失败
 */
int mcd_dump_memory(void)
{
	printf("Starting CoreDump to Memory...\n");
	return mcd_faultdump(MCD_OUTPUT_MEMORY);
}

/**
 * @brief 触发一个断言错误进行测试
 *
 * 用法：mcd_test_assert();
 */
void mcd_test_assert(void)
{
	int x = 42;
	int y = 24;
	mcd_assert(x == y); /* 这会触发断言失败 */
}

/**
 * @brief 触发一个内存访问错误进行测试
 *
 * 用法：mcd_test_fault();
 */
void mcd_test_fault(void)
{
	volatile uint32_t *bad_ptr = (volatile uint32_t *)0xFFFFFFFF;
	*bad_ptr = 0x12345678; /* 这会触发 HardFault */
}

/**
 * @brief 执行 CoreDump 测试命令
 *
 * @param command 命令字符串："SERIAL", "MEMORY", "ASSERT", "FAULT"
 * @return int 0:成功, -1:失败
 */
int mcd_test_htos(const char *command)
{
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
void mcd_rtos_thread_ops(struct thread_info_ops *ops)
{
	mcd_htos_thread_ops(ops);
}

/* arch 硬fault hook 默认弱实现（可被平台实现覆盖） */
MCD_WEAK int arch_hard_fault_exception_hook(void *context)
{
	(void)context;
	return -1;
}
