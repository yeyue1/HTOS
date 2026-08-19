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
/* 自删除任务必须由空闲任务在完成上下文切换后释放。 */
static htList_t xTasksWaitingTermination;
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
	htListInit(&xTasksWaitingTermination);

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
	printf("Task stack setup: PC=0x%08lx (with Thumb bit), Param=0x%08lx, SP=%p\r\n",
			(unsigned long)(((uintptr_t)pxCode) | 1UL), (unsigned long)(uintptr_t)pvParameters, (void *)pxTopOfStack);

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
	if (pxTaskCode == NULL || pcName == NULL) {
		return htFAIL;
	}

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
			/* 预填充栈，供运行时高水位统计。 */
			for (UBaseType_t uxIndex = 0U; uxIndex < usActualStackDepth; uxIndex++) {
				pxStack[uxIndex] = htTASK_STACK_FILL_VALUE;
			}

			/* 初始化TCB内存 */
			memset(pxNewTCB, 0, sizeof(htTCB_t));

			/* 初始化堆栈 */
			pxNewTCB->pxStack = pxStack;
			pxNewTCB->uxStackDepth = usActualStackDepth;

			/* 复制任务名称，避免依赖平台特定的安全字符串扩展。 */
			size_t xNameLength = 0U;
			while (xNameLength < (configMAX_TASK_NAME_LEN - 1U) && pcName[xNameLength] != '\0') {
				xNameLength++;
			}
			memcpy(pxNewTCB->pcTaskName, pcName, xNameLength);
			pxNewTCB->pcTaskName[xNameLength] = '\0';

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
			htListItemInit(&(pxNewTCB->xAllListItem));
			htListSetItemOwner(&(pxNewTCB->xAllListItem), pxNewTCB);
			htListSetItemValue(&(pxNewTCB->xAllListItem), configMAX_PRIORITIES - uxPriority);
			htListInsertEnd(&pxAllocatedTasksList, &(pxNewTCB->xAllListItem));

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
static void prvDeleteTerminatedTasks(void)
{
	for (;;) {
		htEnterCritical();
		htListItem_t *pxItem = htListGetHead(&xTasksWaitingTermination);
		if (pxItem == NULL) {
			htExitCritical();
			return;
		}

		htTCB_t *pxTCB = (htTCB_t *)htListGetItemOwner(pxItem);
		htListRemove(pxItem);
		htExitCritical();

		if (pxTCB != NULL) {
			htPortFree(pxTCB->pxStack);
			htPortFree(pxTCB);
		}
	}
}

static void htprvIdleTask(void *pvParameters)
{
	/* 防止编译器警告 */
	(void)pvParameters;

	/* 空闲任务永远运行 */
	for (;;) {
		prvDeleteTerminatedTasks();

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
 * 按绝对周期延时，避免周期任务逐次漂移。
 */
BaseType_t htTaskDelayUntil(TickType_t *pxPreviousWakeTime, TickType_t xTimeIncrement)
{
	if (pxPreviousWakeTime == NULL || xTimeIncrement == 0U || xTimeIncrement > INT32_MAX) {
		return htFAIL;
	}

	const TickType_t xNextWakeTime = *pxPreviousWakeTime + xTimeIncrement;
	const int32_t xTicksRemaining = (int32_t)(xNextWakeTime - xTickCount);
	*pxPreviousWakeTime = xNextWakeTime;
	if (xTicksRemaining <= 0) {
		return htFAIL;
	}

	htTaskDelay((TickType_t)xTicksRemaining);
	return htPASS;
}

UBaseType_t htTaskPriorityGet(TaskHandle_t xTask)
{
	htTCB_t *pxTCB = xTask == NULL ? pxCurrentTCB : (htTCB_t *)xTask;
	return pxTCB == NULL ? 0U : pxTCB->uxPriority;
}

void htTaskPrioritySet(TaskHandle_t xTask, UBaseType_t uxNewPriority)
{
	htTCB_t *pxTCB = xTask == NULL ? pxCurrentTCB : (htTCB_t *)xTask;
	if (pxTCB == NULL) {
		return;
	}
	if (uxNewPriority >= configMAX_PRIORITIES) {
		uxNewPriority = configMAX_PRIORITIES - 1U;
	}

	BaseType_t xYieldRequired = htFALSE;
	htEnterCritical();
	const UBaseType_t uxOldPriority = pxTCB->uxPriority;
	if (uxOldPriority != uxNewPriority) {
		if (pxTCB->uxTaskState == HT_TASK_READY || pxTCB->uxTaskState == HT_TASK_RUNNING) {
			htListRemove(&(pxTCB->xStateListItem));
		}

		htList_t *pxEventList = (htList_t *)pxTCB->xEventListItem.pxContainer;
		if (pxEventList != NULL) {
			htListRemove(&(pxTCB->xEventListItem));
		}

		pxTCB->uxPriority = uxNewPriority;
		pxTCB->uxBasePriority = uxNewPriority;
		htListSetItemValue(&(pxTCB->xAllListItem), configMAX_PRIORITIES - uxNewPriority);
		htListSetItemValue(&(pxTCB->xEventListItem), configMAX_PRIORITIES - uxNewPriority);

		if (pxTCB->uxTaskState == HT_TASK_READY || pxTCB->uxTaskState == HT_TASK_RUNNING) {
			htListSetItemValue(&(pxTCB->xStateListItem), configMAX_PRIORITIES - uxNewPriority);
			htListInsertEnd(&(pxReadyTasksLists[uxNewPriority]), &(pxTCB->xStateListItem));
		}
		if (pxEventList != NULL) {
			htListInsert(pxEventList, &(pxTCB->xEventListItem));
		}

		if (pxCurrentTCB != NULL && ((pxTCB == pxCurrentTCB && uxNewPriority < uxOldPriority) ||
											(pxTCB != pxCurrentTCB && pxTCB->uxTaskState == HT_TASK_READY &&
													uxNewPriority > pxCurrentTCB->uxPriority))) {
			xYieldRequired = htTRUE;
		}
	}
	htExitCritical();
	if (xYieldRequired == htTRUE) {
		htTaskYield();
	}
}

void htTaskSuspend(TaskHandle_t xTaskToSuspend)
{
	htTCB_t *pxTCB = xTaskToSuspend == NULL ? pxCurrentTCB : (htTCB_t *)xTaskToSuspend;
	if (pxTCB == NULL || pxTCB->uxTaskState == HT_TASK_SUSPENDED) {
		return;
	}

	const BaseType_t xSuspendCurrent = pxTCB == pxCurrentTCB ? htTRUE : htFALSE;
	htEnterCritical();
	htListRemove(&(pxTCB->xStateListItem));
	htListRemove(&(pxTCB->xEventListItem));
	pxTCB->uxTaskState = HT_TASK_SUSPENDED;
	htListSetItemValue(&(pxTCB->xStateListItem), 0U);
	htListInsertEnd(&xSuspendedTaskList, &(pxTCB->xStateListItem));
	htExitCritical();

	if (xSuspendCurrent == htTRUE) {
		htTaskYield();
	}
}

static BaseType_t prvResumeTask(htTCB_t *pxTCB)
{
	if (pxTCB == NULL || pxTCB->uxTaskState != HT_TASK_SUSPENDED ||
			pxTCB->xStateListItem.pxContainer != &xSuspendedTaskList) {
		return htFALSE;
	}

	htListRemove(&(pxTCB->xStateListItem));
	htListSetItemValue(&(pxTCB->xStateListItem), configMAX_PRIORITIES - pxTCB->uxPriority);
	pxTCB->uxTaskState = HT_TASK_READY;
	htListInsertEnd(&(pxReadyTasksLists[pxTCB->uxPriority]), &(pxTCB->xStateListItem));
	return pxCurrentTCB == NULL || pxTCB->uxPriority > pxCurrentTCB->uxPriority ? htTRUE : htFALSE;
}

void htTaskResume(TaskHandle_t xTaskToResume)
{
	htTCB_t *pxTCB = (htTCB_t *)xTaskToResume;
	BaseType_t xYieldRequired;
	htEnterCritical();
	xYieldRequired = prvResumeTask(pxTCB);
	htExitCritical();
	if (xYieldRequired == htTRUE) {
		htTaskYield();
	}
}

BaseType_t htTaskResumeFromISR(TaskHandle_t xTaskToResume)
{
	return prvResumeTask((htTCB_t *)xTaskToResume);
}

TaskHandle_t htTaskGetCurrentTaskHandle(void)
{
	return (TaskHandle_t)pxCurrentTCB;
}

char *htTaskGetName(TaskHandle_t xTaskToQuery)
{
	htTCB_t *pxTCB = xTaskToQuery == NULL ? pxCurrentTCB : (htTCB_t *)xTaskToQuery;
	return pxTCB == NULL ? NULL : pxTCB->pcTaskName;
}

htTaskStatus_t htTaskGetState(TaskHandle_t xTask)
{
	htTCB_t *pxTCB = xTask == NULL ? pxCurrentTCB : (htTCB_t *)xTask;
	return pxTCB == NULL ? HT_TASK_INVALID : (htTaskStatus_t)pxTCB->uxTaskState;
}

void htTaskStartScheduler(void)
{
	htStartScheduler();
}

/**
 * 任务调度函数
 * 让出CPU控制权，切换到其他任务
 */
void htTaskYield(void)
{
#if defined(HTOS_HOST_TEST)
	extern void htHostTestYieldHook(void);
	htHostTestYieldHook();
#else
	/* 触发PendSV中断，进行上下文切换 */
	*((volatile uint32_t *)0xE000ED04) = 0x10000000; /* 设置PENDSVSET位 */
#endif

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
	// 确保当前延时列表有效
	if (pxDelayedTaskList == NULL) {
		return;
	}

	// 获取当前滴答计数
	TickType_t xCurrentTickCount = xTickCount;

	// 溢出列表要等 tick 回绕交换后才能检查，否则会被提前唤醒。
	if (pxDelayedTaskList->uxNumberOfItems == 0) {
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
				// 超时唤醒时也必须从队列/信号量等待列表中摘除。
				htListRemove(&(pxTCB->xEventListItem));
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
	if (xTickCount == 0U) {
		// tick 回绕后，之前跨越回绕点的任务成为当前延时任务。
		htList_t *pxTemporary = pxDelayedTaskList;
		pxDelayedTaskList = pxOverflowDelayedTaskList;
		pxOverflowDelayedTaskList = pxTemporary;
	}

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
	BaseType_t xDeleteCurrentTask;

	/* 如果参数为NULL，则删除当前任务 */
	if (xTaskToDelete == NULL) {
		pxTCB = pxCurrentTCB;
	} else {
		pxTCB = (htTCB_t *)xTaskToDelete;
	}
	if (pxTCB == NULL) {
		return;
	}
	xDeleteCurrentTask = (pxTCB == pxCurrentTCB) ? htTRUE : htFALSE;

	/* 先禁用中断 */
	htEnterCritical();

	/* 从状态、事件和全局跟踪列表中移除任务。 */
	htListRemove(&(pxTCB->xStateListItem));
	htListRemove(&(pxTCB->xEventListItem));
	htListRemove(&(pxTCB->xAllListItem));

	if (xDeleteCurrentTask == htTRUE) {
		pxTCB->uxTaskState = HT_TASK_SUSPENDED;
		htListSetItemValue(&(pxTCB->xStateListItem), 0U);
		htListInsertEnd(&xTasksWaitingTermination, &(pxTCB->xStateListItem));
	}

	/* 递减任务计数器 */
	if (uxCurrentNumberOfTasks > 0U) {
		uxCurrentNumberOfTasks--;
	}

	/* 恢复中断 */
	htExitCritical();

	/* 如果删除的是当前任务，需要调度另一个任务 */
	if (xDeleteCurrentTask == htTRUE) {
		/* 强制任务切换 */
		htTaskYield();
		return;
	}

	/* 非当前任务已不再执行，可以立即释放。 */
	htPortFree(pxTCB->pxStack);
	htPortFree(pxTCB);
}

/* */
