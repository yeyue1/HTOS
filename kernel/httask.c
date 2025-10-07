#include "httask.h"
#include "htmem.h"
#include "htlist.h"
#include "htscheduler.h"
#include <stdio.h> // 添加 stdio 头文件解决 printf 未声明问题
#include <string.h>
#include "stm32f1xx_hal.h"
#include "core_cm3.h" // 添加此头文件以提供NVIC函数和中断相关定义

/* 全局变量 */
htTCB_t *pxCurrentTCB = NULL; /* 当前运行的任务TCB */
UBaseType_t uxCurrentNumberOfTasks = 0; /* 当前任务数量 */
TickType_t xTickCount = 0; /* 系统tick计数 */

/* 任务就绪列表 */
htList_t pxReadyTasksLists[configMAX_PRIORITIES];
/* 延时任务列表 */
htList_t xDelayedTaskList1;
htList_t xDelayedTaskList2;
htList_t *pxDelayedTaskList;
htList_t *pxOverflowDelayedTaskList;
/* 挂起任务列表 */
static htList_t xSuspendedTaskList;
// 所有任务列表
htList_t pxAllocatedTasksList;

/**
 * 检查调度器是否需要切换任务
 * 从htscheduler.c导入函数声明
 */
extern BaseType_t htSchedulerNeedsSwitch(void);

/**
 * 初始化任务调度器
 * 在htSchedulerInit中调用以避免未使用警告
 * 删除static关键字使函数对外可见
 */
void prvInitialiseTaskLists(void)
{
	UBaseType_t uxPriority;

	/* 初始化所有任务就绪列表 */
	for (uxPriority = 0; uxPriority < configMAX_PRIORITIES; uxPriority++) {
		htListInit(&(pxReadyTasksLists[uxPriority]));
	}

	/* 初始化延时任务列表 */
	htListInit(&xDelayedTaskList1);
	htListInit(&xDelayedTaskList2);
	pxDelayedTaskList = &xDelayedTaskList1;
	pxOverflowDelayedTaskList = &xDelayedTaskList2;

	/* 初始化挂起任务列表 */
	htListInit(&xSuspendedTaskList);

	/* 初始化所有任务列表 */
	htListInit(&pxAllocatedTasksList);
}

/**
 * 初始化任务栈
 */
static StackType_t *prvInitialiseTaskStack(StackType_t *pxTopOfStack, TaskFunction_t pxCode, void *pvParameters)
{
	// 确保8字节对齐
	pxTopOfStack = (StackType_t *)((((uint32_t)pxTopOfStack) & ~0x7UL));

	// 首先清零整个栈区域
	memset((void *)((uint32_t)pxTopOfStack - 64), 0, 64);

	// 构建异常框架 - 从底向上
	*(--pxTopOfStack) = 0x01000000; /* xPSR - T位置位 */
	*(--pxTopOfStack) = ((uint32_t)pxCode) | 1UL; /* PC指向任务函数，强制设置LSB确保Thumb模式 */
	*(--pxTopOfStack) = 0xFFFFFFFEUL; /* LR - 异常返回标记 */
	*(--pxTopOfStack) = 0; /* R12 */
	*(--pxTopOfStack) = 0; /* R3 */
	*(--pxTopOfStack) = 0; /* R2 */
	*(--pxTopOfStack) = 0; /* R1 */
	*(--pxTopOfStack) = (uint32_t)pvParameters; /* R0 - 任务参数 */

	// 构建软件保存的寄存器
	*(--pxTopOfStack) = 0; /* R11 */
	*(--pxTopOfStack) = 0; /* R10 */
	*(--pxTopOfStack) = 0; /* R9 */
	*(--pxTopOfStack) = 0; /* R8 */
	*(--pxTopOfStack) = 0; /* R7 */
	*(--pxTopOfStack) = 0; /* R6 */
	*(--pxTopOfStack) = 0; /* R5 */
	*(--pxTopOfStack) = 0; /* R4 */

	// 打印任务栈配置信息 (使用英文避免编码问题)
	printf("Task stack setup: PC=0x%08lx (with Thumb bit), Param=0x%08lx, SP=%p\r\n", ((uint32_t)pxCode) | 1UL,
			(uint32_t)pvParameters, pxTopOfStack);

	return pxTopOfStack;
}

/**
 * 创建新任务 - 确保任务创建成功
 */
BaseType_t htTaskCreate(TaskFunction_t pxTaskCode, const char *const pcName, const uint16_t usStackDepth,
		void *const pvParameters, UBaseType_t uxPriority, TaskHandle_t *const pxCreatedTask)
{
	htTCB_t *pxNewTCB;
	StackType_t *pxStack;
	BaseType_t xReturn = htPASS;

	/* 检查优先级范围 */
	if (uxPriority >= configMAX_PRIORITIES) {
		uxPriority = configMAX_PRIORITIES - 1;
	}

	/* 分配TCB内存 */
	pxNewTCB = (htTCB_t *)htPortMalloc(sizeof(htTCB_t));

	if (pxNewTCB != NULL) {
		/* 分配任务堆栈内存 */
		// 确保分配的栈大小至少是128字节
		uint16_t usActualStackDepth = usStackDepth;
		if (usActualStackDepth < configMINIMAL_STACK_SIZE) {
			usActualStackDepth = configMINIMAL_STACK_SIZE;
		}

		pxStack = (StackType_t *)htPortMalloc(usActualStackDepth * sizeof(StackType_t));

		if (pxStack != NULL) {
			/* 确保栈初始化为0，避免垃圾值 */
			memset(pxStack, 0, usActualStackDepth * sizeof(StackType_t));

			/* 初始化TCB内存 */
			memset(pxNewTCB, 0, sizeof(htTCB_t));

			/* 初始化堆栈 */
			pxNewTCB->pxStack = pxStack;
			pxNewTCB->uxStackDepth = usActualStackDepth;

			/* 复制任务名称 */
			strncpy(pxNewTCB->pcTaskName, pcName, configMAX_TASK_NAME_LEN - 1);
			pxNewTCB->pcTaskName[configMAX_TASK_NAME_LEN - 1] = '\0';

			/* 使用安全的栈初始化函数 */
			pxNewTCB->pxTopOfStack =
					prvInitialiseTaskStack(pxNewTCB->pxStack + usActualStackDepth - 1, pxTaskCode, pvParameters);

			/* 输出初始化结果用于调试 */
			printf("Task %s stack: base=%p, top=%p\r\n", pxNewTCB->pcTaskName, (void *)pxNewTCB->pxStack,
					(void *)pxNewTCB->pxTopOfStack);

			/* 初始化任务优先级 */
			pxNewTCB->uxPriority = uxPriority;
			pxNewTCB->uxBasePriority = uxPriority;

			/* 初始化列表项 */
			htListItemInit(&(pxNewTCB->xStateListItem));
			htListItemInit(&(pxNewTCB->xEventListItem));
			/* 添加到所有任务列表 */
			/* 为所有任务列表分配独立的链表项（避免侵入冲突） */
			htListItem_t *pxAllocItem = (htListItem_t *)htPortMalloc(sizeof(htListItem_t));
			if (pxAllocItem != NULL) {
				htListItemInit(pxAllocItem);
				/* 所有任务列表中保存对 TCB 的所有权，以便信息查询 */
				htListSetItemOwner(pxAllocItem, pxNewTCB);
				/* 可选：将值设为优先级或其它标记 */
				htListSetItemValue(pxAllocItem, configMAX_PRIORITIES - uxPriority);
				htListInsertEnd(&pxAllocatedTasksList, pxAllocItem);
			} else {
				/* 分配失败则忽略，不影响任务创建 */
			}

			/* 设置列表项的所有者 */
			htListSetItemOwner(&(pxNewTCB->xStateListItem), pxNewTCB);
			htListSetItemOwner(&(pxNewTCB->xEventListItem), pxNewTCB);

			/* 设置列表项值为优先级，用于优先级排序 */
			htListSetItemValue(&(pxNewTCB->xStateListItem), configMAX_PRIORITIES - uxPriority);

			/* 添加到就绪列表 */
			pxNewTCB->uxTaskState = HT_TASK_READY;
			htListInsertEnd(&(pxReadyTasksLists[uxPriority]), &(pxNewTCB->xStateListItem));

			/* 更新任务计数 */
			uxCurrentNumberOfTasks++;

			/* 如果创建的是第一个任务，设为当前任务 */
			if (pxCurrentTCB == NULL) {
				pxCurrentTCB = pxNewTCB;
			}

			/* 返回任务句柄 */
			if (pxCreatedTask != NULL) {
				*pxCreatedTask = (TaskHandle_t)pxNewTCB;
			}
		} else {
			/* 堆栈分配失败，释放TCB内存并返回失败 */
			htPortFree(pxNewTCB);
			xReturn = htFAIL;
		}
	} else {
		xReturn = htFAIL;
	}

	return xReturn;
}

/**
 * 增加系统时钟计数
 * 此函数用在htSchedulerTickHandler中以避免未使用警告
 */
void htIncrementTick(void)
{
	xTickCount++;
}

/**
 * 空闲任务函数 - 当没有其他任务准备运行时，运行此任务
 */
static void htprvIdleTask(void *pvParameters)
{
	/* 防止编译器警告 */
	(void)pvParameters;

	/* 空闲任务永远运行 */
	for (;;) {

		/* 执行低功耗管理，可选择进入睡眠模式 */
#if configUSE_IDLE_HOOK
		/* 如果定义了空闲钩子函数，调用它 */
		vApplicationIdleHook();
#endif

		/* 在调试模式下，定期输出系统状态 */
#if configDEBUG_IDLE_STATS
		static uint32_t idleCounter = 0;
		if ((++idleCounter % 100000) == 0) {
			printf("Idle: %lu ticks\r\n", (uint32_t)xTickCount);
			idleCounter = 0;
		}
#endif
	}
}

/**
 * 创建空闲任务
 */
BaseType_t htprvCreateIdleTask(void)
{
	BaseType_t xReturn = htFAIL;
	static htTCB_t *pxIdleTaskTCB = NULL;

	/* 检查空闲任务是否已经创建 */
	if (pxIdleTaskTCB == NULL) {
		/* 创建空闲任务，使用最低优先级 */
		xReturn = htTaskCreate(htprvIdleTask, "IDLE", configMINIMAL_STACK_SIZE, NULL,
				HT_IDLE_TASK, /* 使用优先级0(最低) */
				(TaskHandle_t *)&pxIdleTaskTCB);
	} else {
		/* 空闲任务已经存在 */
		xReturn = htPASS;
	}

	return xReturn;
}

/**
 * Task delay function - optimized implementation
 */
void htTaskDelay(TickType_t xTicksToDelay)
{
	// 如果延时为0，让出CPU但不阻塞
	if (xTicksToDelay == 0) {
		htTaskYield();
		return;
	}

	// 禁用中断来确保操作原子性
	__disable_irq();

	// 验证必要的数据结构
	if (pxDelayedTaskList == NULL || pxCurrentTCB == NULL) {
		printf("ERROR: Delay list or current task invalid!\r\n");
		__enable_irq();
		return;
	}

	// 计算任务唤醒时间
	TickType_t xTimeToWake = xTickCount + xTicksToDelay;

	// 从就绪列表中移除当前任务
	if (pxCurrentTCB->xStateListItem.pxContainer) {
		htListRemove(&(pxCurrentTCB->xStateListItem));
	}

	// 设置唤醒时间值并进行按值插入
	htListSetItemValue(&(pxCurrentTCB->xStateListItem), xTimeToWake);

	if (xTimeToWake > xTickCount) {
		pxCurrentTCB->xStateListItem.pxContainer = pxDelayedTaskList;
		htListInsert(pxDelayedTaskList, &(pxCurrentTCB->xStateListItem));
	} else {
		pxCurrentTCB->xStateListItem.pxContainer = pxOverflowDelayedTaskList;
		htListInsert(pxOverflowDelayedTaskList, &(pxCurrentTCB->xStateListItem));
	}

	// 更新任务状态
	pxCurrentTCB->uxTaskState = HT_TASK_BLOCKED;

	// 启用中断
	__enable_irq();
	htTaskYield();
}

/**
 * 任务调度函数
 * 让出CPU控制权，切换到其他任务
 */
void htTaskYield(void)
{
	/* 触发PendSV中断，进行上下文切换 */
	*((volatile uint32_t *)0xE000ED04) = 0x10000000; /* 设置PENDSVSET位 */

	//    /* 内存屏障，确保之前的写操作完成 */
	//    __asm volatile ("dsb" ::: "memory");
	//    __asm volatile ("isb");
}

htList_t htTaskGetAllTaskInfo(void)
{
	return pxAllocatedTasksList;
}

/**
 * Check and update delayed task list - fixed implementation for accurate timing
 */
void htTaskCheckDelayedTasks(void)
{
	// 确保列表有效
	if (pxDelayedTaskList == NULL && pxOverflowDelayedTaskList == NULL) {
		return;
	}

	// 获取当前滴答计数
	TickType_t xCurrentTickCount = xTickCount;

	// 如果列表为空，直接返回
	if (pxDelayedTaskList->uxNumberOfItems == 0 && pxOverflowDelayedTaskList->uxNumberOfItems == 0) {
		return;
	}
	// 处理延时到期的任务 - 采用循环检查所有到期任务
	htListItem_t *pxItem;
	htTCB_t *pxTCB;
	TickType_t xItemValue;
	BaseType_t xListItemRemoved = htFALSE; // 标记是否从列表中移除了项目

	// 检查所有已到期的任务
	do {
		xListItemRemoved = htFALSE;

		// 获取延时列表中的第一个项目
		pxItem = htListGetHead(pxDelayedTaskList);
		if (pxItem == NULL) {
			// 列表为空，退出循环
			break;
		}

		// 获取此任务的唤醒时间
		xItemValue = htListGetItemValue(pxItem);

		// 如果到达或超过唤醒时间
		if (xItemValue <= xCurrentTickCount) {
			// 获取任务控制块
			pxTCB = (htTCB_t *)htListGetItemOwner(pxItem);
			if (pxTCB != NULL) {
				// 从延时列表移除任务
				htListRemove(pxItem);
				xListItemRemoved = htTRUE;

				// 将任务添加回就绪列表
				if (pxTCB->uxPriority < configMAX_PRIORITIES) {
					// 更新任务状态
					pxTCB->uxTaskState = HT_TASK_READY;

					// 设置就绪列表为容器并添加到列表
					pxItem->pxContainer = &(pxReadyTasksLists[pxTCB->uxPriority]);
					htListInsertEnd(&(pxReadyTasksLists[pxTCB->uxPriority]), pxItem);
				}
			} else {
				// 无效任务，移除并继续
				htListRemove(pxItem);
				xListItemRemoved = htTRUE;
			}
		}

		// 如果从列表中移除了项目，重新检查，因为可能有多个项目到期
	} while (xListItemRemoved == htTRUE);
	xListItemRemoved = htFALSE; // 标记是否从列表中移除了项目

	do {
		xListItemRemoved = htFALSE;

		// 获取延时列表中的第一个项目
		pxItem = htListGetHead(pxOverflowDelayedTaskList);
		if (pxItem == NULL) {
			// 列表为空，退出循环
			break;
		}

		// 获取此任务的唤醒时间
		xItemValue = htListGetItemValue(pxItem);

		// 如果到达或超过唤醒时间
		if (xItemValue <= xCurrentTickCount) {
			// 获取任务控制块
			pxTCB = (htTCB_t *)htListGetItemOwner(pxItem);
			if (pxTCB != NULL) {
				// 从延时列表移除任务
				htListRemove(pxItem);
				xListItemRemoved = htTRUE;

				// 将任务添加回就绪列表
				if (pxTCB->uxPriority < configMAX_PRIORITIES) {
					// 更新任务状态
					pxTCB->uxTaskState = HT_TASK_READY;

					// 设置就绪列表为容器并添加到列表
					pxItem->pxContainer = &(pxReadyTasksLists[pxTCB->uxPriority]);
					htListInsertEnd(&(pxReadyTasksLists[pxTCB->uxPriority]), pxItem);
				}
			} else {
				// 无效任务，移除并继续
				htListRemove(pxItem);
				xListItemRemoved = htTRUE;
			}
		}

		// 如果从列表中移除了项目，重新检查，因为可能有多个项目到期
	} while (xListItemRemoved == htTRUE);
}

/**
 * System tick processing function - simplified output
 */
void htTaskTickInc(void)
{
	// 确保原子操作
	__disable_irq();

	// 递增系统滴答计数
	xTickCount++;

	// 检查延时任务
	htTaskCheckDelayedTasks();

	// 如果当前任务有效，检查是否需要调度
	if (pxCurrentTCB != NULL) {
		if (htSchedulerNeedsSwitch()) {
			// 触发PendSV中断进行任务切换
			SCB->ICSR = SCB_ICSR_PENDSVSET_Msk;
		}
	}
	// 重新启用中断
	__enable_irq();
}

/**
 * 删除任务
 */
void htTaskDelete(TaskHandle_t xTaskToDelete)
{
	htTCB_t *pxTCB;

	/* 如果参数为NULL，则删除当前任务 */
	if (xTaskToDelete == NULL) {
		pxTCB = pxCurrentTCB;
	} else {
		pxTCB = (htTCB_t *)xTaskToDelete;
	}

	/* 先禁用中断 */
	htEnterCritical();

	/* 从任何列表中移除任务 */
	htListRemove(&(pxTCB->xStateListItem));
	htListRemove(&(pxTCB->xEventListItem));
	/* 从所有任务列表中移除 */
	htListRemove(&(pxTCB->xStateListItem));
	htListRemove(&(pxTCB->xEventListItem));

	/* 递减任务计数器 */
	uxCurrentNumberOfTasks--;

	/* 恢复中断 */
	htExitCritical();

	/* 如果删除的是当前任务，需要调度另一个任务 */
	if (pxTCB == pxCurrentTCB) {
		/* 强制任务切换 */
		htTaskYield();
	}

	/* 释放任务栈和TCB内存 */
	if (pxTCB->pxStack != NULL) {
		htPortFree(pxTCB->pxStack);
	}
	htPortFree(pxTCB);
}

/* */
