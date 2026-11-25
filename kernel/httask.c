/**
 * @file httask.c
 * @brief 任务管理核心：创建、删除、延时、挂起/恢复、优先级与任务通知。
 *
 * 一个任务的全部状态都放在 htTCB_t 里，并通过三个链表项挂到不同链表上：
 * 状态项挂就绪链表或延时链表，事件项挂队列/信号量等待链表，跟踪项挂
 * pxAllocatedTasksList 以便调试时遍历全部任务。任务切换靠请求 PendSV 实现，
 * 真正的寄存器现场由 portable/Cortex-M/htPortContext.c 保存。
 */
#include "httask.h"
#include "htmem.h"
#include "htlist.h"
#include "htscheduler.h"
#include "htschedpolicy.h"
#include "htlatency.h"
#include "httrace.h"
#include "htutils.h"
#include "htPort.h"
#include "htkernel_internal.h"
#include <string.h>

/* 全局变量 */
htTCB_t *pxCurrentTCB = NULL; /* 当前运行的任务 TCB */
UBaseType_t uxCurrentNumberOfTasks = 0; /* 当前任务数量 */
TickType_t xTickCount = 0; /* 系统tick计数 */
TickType_t xNextTaskUnblockTime = htBLOCKED_INDEFINITELY; /* 延时表头唤醒时刻缓存 */

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
#if (configUSE_TRACE_FACILITY == 1)
/* 所有任务链表：调试时可遍历全部在册任务。 */
htList_t pxAllocatedTasksList;
#endif

/**
 * @brief 初始化全部任务链表：各优先级就绪链表、延时/溢出链表、挂起与终止链表。
 *
 * @note 由 htSchedulerInit() 调用；去掉 static 只是为了让调度器初始化能调用
 *       它，并不属于对外 API。
 */
static void prvResetNextTaskUnblockTime(void)
{
	htListItem_t *pxHead = (pxDelayedTaskList != NULL) ? htListGetHead(pxDelayedTaskList) : NULL;
	xNextTaskUnblockTime = (pxHead != NULL) ? htListGetItemValue(pxHead) : htBLOCKED_INDEFINITELY;
}

void prvInitialiseTaskLists(void)
{
	UBaseType_t uxPriority;

	/* 初始化所有任务就绪列表并清空 O(1) 优先级位图 */
	for (uxPriority = 0; uxPriority < configMAX_PRIORITIES; uxPriority++) {
		htListInit(&(pxReadyTasksLists[uxPriority]));
	}
	htSchedResetReadyBitmap();

	/* 初始化延时任务列表 */
	htListInit(&xDelayedTaskList1);
	htListInit(&xDelayedTaskList2);
	pxDelayedTaskList = &xDelayedTaskList1;
	pxOverflowDelayedTaskList = &xDelayedTaskList2;
	xNextTaskUnblockTime = htBLOCKED_INDEFINITELY;

	/* 初始化挂起任务列表 */
	htListInit(&xSuspendedTaskList);
	htListInit(&xTasksWaitingTermination);

#if (configUSE_TRACE_FACILITY == 1)
	/* 初始化所有任务列表 */
	htListInit(&pxAllocatedTasksList);
#endif
}

/**
 * @brief 初始化一个新任务的 TCB：填栈哨兵、建初始栈帧、挂入就绪链表。
 *
 * @param[in] pxTaskCode 任务入口函数。
 * @param[in] pcName 任务名，最多 configMAX_TASK_NAME_LEN - 1 个字符。
 * @param[in] usStackDepth 栈深度，单位 StackType_t 字。
 * @param[in] pvParameters 传给任务的参数。
 * @param[in] uxPriority 优先级，越界时被钳到 configMAX_PRIORITIES - 1。
 * @param[in] pxStack 栈底地址。
 * @param[in,out] pxNewTCB 待填充的 TCB。
 * @param[in] ucStaticallyAllocated 非 0 表示 TCB 与栈都由用户静态提供。
 *
 * @return 初始化成功返回 htPASS，参数非法返回 htFAIL。
 *
 * @note pxCurrentTCB == NULL 的分支只覆盖"第一个任务"；之后创建的任务不会改写
 *       pxCurrentTCB，启动调度前必须由调度器重新选一次，否则会栈交叉污染。
 */
static BaseType_t prvInitialiseNewTask(TaskFunction_t pxTaskCode, const char *pcName, uint16_t usStackDepth,
		void *pvParameters, UBaseType_t uxPriority, StackType_t *pxStack, htTCB_t *pxNewTCB,
		uint8_t ucStaticallyAllocated)
{
	if (pxTaskCode == NULL || pcName == NULL || pxStack == NULL || pxNewTCB == NULL || usStackDepth == 0U) {
		return htFAIL;
	}
	if (uxPriority >= configMAX_PRIORITIES) {
		uxPriority = configMAX_PRIORITIES - 1U;
	}

	/* 先用哨兵值填满整个栈，供栈溢出检查判断水位线。 */
	for (UBaseType_t uxIndex = 0U; uxIndex < usStackDepth; uxIndex++) {
		pxStack[uxIndex] = htTASK_STACK_FILL_VALUE;
	}
	memset(pxNewTCB, 0, sizeof(*pxNewTCB));
	pxNewTCB->pxStack = pxStack;
	pxNewTCB->uxStackDepth = usStackDepth;
	pxNewTCB->ucStaticallyAllocated = ucStaticallyAllocated;

	/* 任务名超过上限就截断，并保证以结束符结尾。 */
	size_t xNameLength = 0U;
	while (xNameLength < (configMAX_TASK_NAME_LEN - 1U) && pcName[xNameLength] != '\0') {
		xNameLength++;
	}
	memcpy(pxNewTCB->pcTaskName, pcName, xNameLength);
	pxNewTCB->pcTaskName[xNameLength] = '\0';

	pxNewTCB->pxTopOfStack = htPortInitialiseStack(pxStack + usStackDepth - 1U, pxTaskCode, pvParameters);

	pxNewTCB->uxPriority = (uint8_t)uxPriority;
	pxNewTCB->uxBasePriority = (uint8_t)uxPriority;
#if (configUSE_TASK_NOTIFICATIONS == 1)
	pxNewTCB->uxNotificationStatus = HT_NOTIFY_NOT_WAITING;
#endif

	/* 链表项初始化：状态项挂就绪/延时链表，事件项挂等待链表，跟踪项挂全局链表。 */
	htListItemInit(&(pxNewTCB->xStateListItem));
	htListItemInit(&(pxNewTCB->xEventListItem));
	htListSetItemOwner(&(pxNewTCB->xStateListItem), pxNewTCB);
	htListSetItemOwner(&(pxNewTCB->xEventListItem), pxNewTCB);
	htListSetItemValue(&(pxNewTCB->xStateListItem), configMAX_PRIORITIES - uxPriority);
#if (configUSE_TRACE_FACILITY == 1)
	htListItemInit(&(pxNewTCB->xAllListItem));
	htListSetItemOwner(&(pxNewTCB->xAllListItem), pxNewTCB);
	htListSetItemValue(&(pxNewTCB->xAllListItem), configMAX_PRIORITIES - uxPriority);
#endif
	pxNewTCB->uxTaskState = HT_TASK_READY;

	/* 将新任务挂入全局总表与就绪表必须在临界区内完成，避免运行期建任务被中断撕裂链表。 */
	BaseType_t xYieldRequired = htFALSE;
	htEnterCritical();
#if (configUSE_TRACE_FACILITY == 1)
	htListInsertEnd(&pxAllocatedTasksList, &(pxNewTCB->xAllListItem));
#endif
	htListInsertEnd(&(pxReadyTasksLists[uxPriority]), &(pxNewTCB->xStateListItem));
	htSchedRecordReadyPriority(uxPriority);
	uxCurrentNumberOfTasks++;

	/* 通知当前策略：新任务已经进入就绪表，策略可以初始化自己的私有字段。 */
	htSchedNotifyTaskCreated(pxNewTCB);
	if (pxCurrentTCB == NULL) {
		pxCurrentTCB = pxNewTCB;
	}
#if (configUSE_PREEMPTION == 1)
	else if (htSchedulerIsStarted() == htTRUE && pxNewTCB->uxPriority > pxCurrentTCB->uxPriority) {
		xYieldRequired = htTRUE;
	}
#endif
	htExitCritical();

	if (xYieldRequired == htTRUE) {
		htTaskYield();
	}
	return htPASS;
}

/**
 * @brief 动态创建任务：从内核堆分别分配 TCB 与任务栈。
 *
 * @param[in] pxTaskCode 任务入口函数，不允许返回。
 * @param[in] pcName 任务名。
 * @param[in] usStackDepth 栈深度（字）；小于 configMINIMAL_STACK_SIZE 时按最小值处理。
 * @param[in] pvParameters 传给任务的参数。
 * @param[in] uxPriority 优先级。
 * @param[out] pxCreatedTask 返回任务句柄，可为 NULL。
 *
 * @return 成功返回 htPASS；内存不足或参数非法返回 htFAIL。
 *
 * @note 任何一步失败都会回滚此前已分配的内存，不会泄漏。
 */
BaseType_t htTaskCreate(TaskFunction_t pxTaskCode, const char *const pcName, const uint16_t usStackDepth,
		void *const pvParameters, UBaseType_t uxPriority, TaskHandle_t *const pxCreatedTask)
{
	if (pxCreatedTask != NULL) {
		*pxCreatedTask = NULL;
	}
	if (pxTaskCode == NULL || pcName == NULL) {
		return htFAIL;
	}

	const uint16_t usActualStackDepth =
			usStackDepth < configMINIMAL_STACK_SIZE ? configMINIMAL_STACK_SIZE : usStackDepth;
	htTCB_t *pxNewTCB = (htTCB_t *)htPortMalloc(sizeof(htTCB_t));
	if (pxNewTCB == NULL) {
		return htFAIL;
	}
	StackType_t *pxStack = (StackType_t *)htPortMalloc(usActualStackDepth * sizeof(StackType_t));
	if (pxStack == NULL) {
		htPortFree(pxNewTCB);
		return htFAIL;
	}

	if (prvInitialiseNewTask(pxTaskCode, pcName, usActualStackDepth, pvParameters, uxPriority, pxStack, pxNewTCB, 0U) !=
			htPASS) {
		htPortFree(pxStack);
		htPortFree(pxNewTCB);
		return htFAIL;
	}
	if (pxCreatedTask != NULL) {
		*pxCreatedTask = (TaskHandle_t)pxNewTCB;
	}
	return htPASS;
}

/**
 * @brief 静态创建任务：TCB 与栈都由调用者提供，不占堆空间。
 *
 * @param[in] pxTaskCode 任务入口函数，不允许返回。
 * @param[in] pcName 任务名。
 * @param[in] usStackDepth 栈深度（字），不得小于 configMINIMAL_STACK_SIZE。
 * @param[in] pvParameters 传给任务的参数。
 * @param[in] uxPriority 优先级。
 * @param[in] puxStackBuffer 调用者提供的栈空间。
 * @param[in,out] pxTaskBuffer 调用者提供的 TCB。
 *
 * @return 成功返回任务句柄，失败返回 NULL。
 *
 * @note configSUPPORT_STATIC_ALLOCATION 为 0 时只吞掉参数并返回 NULL。
 */
TaskHandle_t htTaskCreateStatic(TaskFunction_t pxTaskCode, const char *const pcName, uint16_t usStackDepth,
		void *const pvParameters, UBaseType_t uxPriority, StackType_t *puxStackBuffer, htTCB_t *pxTaskBuffer)
{
#if configSUPPORT_STATIC_ALLOCATION == 1
	if (usStackDepth < configMINIMAL_STACK_SIZE || prvInitialiseNewTask(pxTaskCode, pcName, usStackDepth, pvParameters,
														   uxPriority, puxStackBuffer, pxTaskBuffer, 1U) != htPASS) {
		return NULL;
	}
	return (TaskHandle_t)pxTaskBuffer;
#else
	(void)pxTaskCode;
	(void)pcName;
	(void)usStackDepth;
	(void)pvParameters;
	(void)uxPriority;
	(void)puxStackBuffer;
	(void)pxTaskBuffer;
	return NULL;
#endif
}

/**
 * @brief 直接递增系统滴答计数，供测试或调度器内部使用。
 *
 * @note 不走 htTaskTickInc() 的完整流程：不扫描延时任务，也不请求上下文切换。
 *       保留这个入口是为了避免 htSchedulerTickHandler 里出现未使用符号告警。
 */
void htIncrementTick(void)
{
	xTickCount++;
}

/**
 * @brief 回收已删除任务占用的内存，由空闲任务循环调用。
 *
 * @note 自删除任务不能立刻释放自己的栈（还在用），只能先挂到
 *       xTasksWaitingTermination，等切换到别的任务之后再回收。
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

		/* 先脱离临界区再释放内存：htPortFree 不需要关中断，也能缩短关中断时间。 */
		if (pxTCB != NULL) {
			/* 静态分配的 TCB 与栈不属于内核，不能释放。 */
			if (pxTCB->ucStaticallyAllocated == 0U) {
				htPortFree(pxTCB->pxStack);
				htPortFree(pxTCB);
			}
		}
	}
}

/**
 * @brief 空闲任务主体：优先级最低，只在没有其他任务可运行时执行。
 *
 * @param[in] pvParameters 未使用。
 *
 * @note 每轮先回收已删除任务，再按配置决定是否让出 CPU、调用空闲钩子或打印统计。
 */
static void htprvIdleTask(void *pvParameters)
{
	/* 防止编译器警告 */
	(void)pvParameters;

	/* 空闲任务永远运行 */
	for (;;) {
		prvDeleteTerminatedTasks();

		/*
		 * 协作式调度下必须显式让出。
		 *
		 * 抢占关闭后，就绪任务把当前任务叫醒的路径只剩"对方主动 yield"。
		 * 空闲任务优先级最低，总在无人可跑时才执行；若不在此让出，一个
		 * 因延时到期而就绪的高优先级任务将永远等不到 CPU。
		 */
#if (configUSE_PREEMPTION == 0)
		htTaskYield();
#endif

		/* 执行低功耗管理，可选择进入睡眠模式 */
#if configUSE_IDLE_HOOK
		/* 如果定义了空闲钩子函数，调用它 */
		vApplicationIdleHook();
#endif

		/* 在调试模式下，定期输出系统状态 */
#if configDEBUG_IDLE_STATS
		static uint32_t idleCounter = 0;
		if ((++idleCounter % 100000) == 0) {
			htTraceValue("[idle] ticks", (uint32_t)xTickCount);
			idleCounter = 0;
		}
#endif
	}
}

#if (configTOTAL_HEAP_SIZE == 0U)
/*
 * 纯静态配置下没有堆，内核自己的空闲任务也必须由编译期提供存储。
 * 这是整个内核唯一自带存储的对象：configMINIMAL_STACK_SIZE 个字 + 一个 TCB。
 * 省下来的 12 KB 池远大于这点开销，换来的是"堆被完全裁掉仍能调度"。
 */
static StackType_t xIdleStackBuffer[configMINIMAL_STACK_SIZE];
static htTCB_t xIdleTaskTCB;
#endif

/**
 * @brief 创建内核空闲任务（幂等，重复调用不会重复创建）。
 *
 * @return 已存在或创建成功返回 htPASS，失败返回 htFAIL。
 *
 * @note 用函数内的静态变量记住 TCB，保证只创建一次。
 */
#if (configGENERATE_RUN_TIME_STATS == 1)
/** @brief 空闲任务的控制块，运行时间统计用它把空闲时间单独记账。 */
static TaskHandle_t s_xIdleTaskHandle = NULL;
#endif

BaseType_t htprvCreateIdleTask(void)
{
	BaseType_t xReturn = htFAIL;
	static htTCB_t *pxIdleTaskTCB = NULL;

	/* 检查空闲任务是否已经创建 */
	if (pxIdleTaskTCB == NULL) {
#if (configTOTAL_HEAP_SIZE > 0U)
		/* 创建空闲任务，使用最低优先级 */
		xReturn = htTaskCreate(htprvIdleTask, "IDLE", configMINIMAL_STACK_SIZE, NULL,
				HT_IDLE_TASK, /* 使用优先级0(最低) */
				(TaskHandle_t *)&pxIdleTaskTCB);
#else
		/* 没有堆：改用内核自己提供的静态存储 */
		pxIdleTaskTCB = (htTCB_t *)htTaskCreateStatic(
				htprvIdleTask, "IDLE", configMINIMAL_STACK_SIZE, NULL, HT_IDLE_TASK, xIdleStackBuffer, &xIdleTaskTCB);
		xReturn = (pxIdleTaskTCB != NULL) ? htPASS : htFAIL;
#endif
	} else {
		/* 空闲任务已经存在 */
		xReturn = htPASS;
	}

#if (configGENERATE_RUN_TIME_STATS == 1)
	s_xIdleTaskHandle = (TaskHandle_t)pxIdleTaskTCB;
#endif

	return xReturn;
}

#if (configGENERATE_RUN_TIME_STATS == 1)
TaskHandle_t htTaskGetIdleHandle(void)
{
	return s_xIdleTaskHandle;
}
#endif

#if (configCHECK_FOR_STACK_OVERFLOW > 0)
/**
 * @brief 栈溢出钩子：弱符号，默认直接停机。
 *
 * @param[in] xTask 发生溢出的任务句柄。
 * @param[in] pcTaskName 任务名，便于调试时定位。
 *
 * @note 继续运行只会写坏相邻内存，让故障离现场越来越远；停机能让调试器直接
 *       停在写崩栈的那个任务上。应用提供同名强符号即可接管。
 */
__attribute__((weak)) void htStackOverflowHook(TaskHandle_t xTask, const char *pcTaskName)
{
	(void)xTask;
	(void)pcTaskName;
	for (;;) {
		/* 停机 */
	}
}

/**
 * @brief 检查一个任务是否写穿了自己的栈。
 *
 * 栈底 4 个字在创建任务时被填成 htTASK_STACK_FILL_VALUE，正常执行不会碰到；
 * 只要它们变了，就说明栈指针已经越过边界。只查 4 个字是为了让开销恒定在几个
 * 周期量级，可以留在每次任务切换里。
 *
 * @param[in] xTask 要检查的任务，NULL 表示当前任务。
 *
 * @note 任务尚未初始化（栈指针或深度非法）时直接返回，不做误判。
 */
void htTaskCheckStackSentinel(TaskHandle_t xTask)
{
	htTCB_t *pxTCB = (xTask == NULL) ? pxCurrentTCB : (htTCB_t *)xTask;
	if (pxTCB == NULL || pxTCB->pxStack == NULL || pxTCB->uxStackDepth < 4U) {
		return;
	}

	for (UBaseType_t uxIndex = 0U; uxIndex < 4U; uxIndex++) {
		if (pxTCB->pxStack[uxIndex] != htTASK_STACK_FILL_VALUE) {
			htStackOverflowHook((TaskHandle_t)pxTCB, pxTCB->pcTaskName);
			return;
		}
	}
}
#endif /* configCHECK_FOR_STACK_OVERFLOW */

/**
 * @brief 将当前任务按绝对唤醒时刻挂入延时链表（调用方需持有临界区）。
 */
static BaseType_t prvInsertCurrentTaskIntoDelayedList(TickType_t xTimeToWake)
{
	if (pxDelayedTaskList == NULL || pxOverflowDelayedTaskList == NULL || pxCurrentTCB == NULL) {
		return htFAIL;
	}

	if (pxCurrentTCB->xStateListItem.pxContainer != NULL) {
		htListRemove(&(pxCurrentTCB->xStateListItem));
		htSchedClearReadyPriorityIfEmpty(pxCurrentTCB->uxPriority);
	}

	htListSetItemValue(&(pxCurrentTCB->xStateListItem), xTimeToWake);
	if (xTimeToWake > xTickCount) {
		htListInsert(pxDelayedTaskList, &(pxCurrentTCB->xStateListItem));
		if (xTimeToWake < xNextTaskUnblockTime) {
			xNextTaskUnblockTime = xTimeToWake;
		}
	} else {
		htListInsert(pxOverflowDelayedTaskList, &(pxCurrentTCB->xStateListItem));
	}

	pxCurrentTCB->uxTaskState = HT_TASK_BLOCKED;
	return htPASS;
}

/**
 * @brief 相对延时：让当前任务阻塞指定 tick 数，到期后重新就绪。
 *
 * @param[in] xTicksToDelay 延时 tick 数；为 0 时只让出 CPU、不进入阻塞态。
 */
void htTaskDelay(TickType_t xTicksToDelay)
{
	/* 延时为 0：只让出 CPU，不进入阻塞态。 */
	if (xTicksToDelay == 0U) {
		htTaskYield();
		return;
	}

	htEnterCritical();
	const BaseType_t xBlocked = prvInsertCurrentTaskIntoDelayedList(xTickCount + xTicksToDelay);
	htExitCritical();

	if (xBlocked == htPASS) {
		htTaskYield();
	}
}

/**
 * @brief 按绝对周期延时，避免周期任务逐次累积漂移。
 *
 * @param[in,out] pxPreviousWakeTime 上一次唤醒时刻；返回时被更新为下一次唤醒时刻。
 * @param[in] xTimeIncrement 周期长度（tick）。
 *
 * @return 正常进入延时返回 htPASS；已经错过唤醒时刻、无需等待时返回 htFAIL。
 *
 * @note 差值判定与绝对唤醒时刻入表在同一临界区内完成，消除读 xTickCount 后的 TOCTOU 漂移。
 */
BaseType_t htTaskDelayUntil(TickType_t *pxPreviousWakeTime, TickType_t xTimeIncrement)
{
	if (pxPreviousWakeTime == NULL || xTimeIncrement == 0U || xTimeIncrement > (TickType_t)INT32_MAX) {
		return htFAIL;
	}

	const TickType_t xNextWakeTime = *pxPreviousWakeTime + xTimeIncrement;
	*pxPreviousWakeTime = xNextWakeTime;

	htEnterCritical();
	const int32_t xTicksRemaining = (int32_t)(xNextWakeTime - xTickCount);
	if (xTicksRemaining <= 0) {
		htExitCritical();
		return htFAIL;
	}
	const BaseType_t xBlocked = prvInsertCurrentTaskIntoDelayedList(xNextWakeTime);
	htExitCritical();

	if (xBlocked != htPASS) {
		return htFAIL;
	}
	htTaskYield();
	return htPASS;
}

/**
 * @brief 查询任务优先级。
 *
 * @param[in] xTask 任务句柄；NULL 表示当前任务。
 *
 * @return 优先级；句柄与当前任务都无效时返回 0。
 */
UBaseType_t htTaskPriorityGet(TaskHandle_t xTask)
{
	htTCB_t *pxTCB = xTask == NULL ? pxCurrentTCB : (htTCB_t *)xTask;
	return pxTCB == NULL ? 0U : pxTCB->uxPriority;
}

/**
 * @brief 修改任务优先级，并按需要把它在链表间重新排队。
 *
 * @param[in] xTask 任务句柄；NULL 表示当前任务。
 * @param[in] uxNewPriority 新优先级，越界时钳到 configMAX_PRIORITIES - 1。
 *
 * @note 优先级变化后要同步状态项、事件项、跟踪项三者的值；若需要切换，则
 *       在退出临界区之后再让出 CPU。
 */
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
			htSchedClearReadyPriorityIfEmpty(uxOldPriority);
		}

		/* 若正挂在某个事件等待链表上，先摘下来，稍后按新优先级重新插入。 */
		htList_t *pxEventList = (htList_t *)pxTCB->xEventListItem.pxContainer;
		if (pxEventList != NULL) {
			htListRemove(&(pxTCB->xEventListItem));
		}

		/* 更新优先级与基优先级，并同步链表项的值。 */
		pxTCB->uxPriority = (uint8_t)uxNewPriority;
		pxTCB->uxBasePriority = (uint8_t)uxNewPriority;
#if (configUSE_TRACE_FACILITY == 1)
		htListSetItemValue(&(pxTCB->xAllListItem), configMAX_PRIORITIES - uxNewPriority);
#endif
		htListSetItemValue(&(pxTCB->xEventListItem), configMAX_PRIORITIES - uxNewPriority);

		if (pxTCB->uxTaskState == HT_TASK_READY || pxTCB->uxTaskState == HT_TASK_RUNNING) {
			htListSetItemValue(&(pxTCB->xStateListItem), configMAX_PRIORITIES - uxNewPriority);
			htListInsertEnd(&(pxReadyTasksLists[uxNewPriority]), &(pxTCB->xStateListItem));
			htSchedRecordReadyPriority(uxNewPriority);
		}
		if (pxEventList != NULL) {
			htListInsert(pxEventList, &(pxTCB->xEventListItem));
		}

		/* 降低了当前任务优先级，或有更高优先级任务变为就绪，都需要切换。 */
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

/**
 * @brief 挂起任务：把任务从就绪/等待链表摘下，改挂到挂起链表。
 *
 * @param[in] xTaskToSuspend 任务句柄；NULL 表示当前任务。
 *
 * @note 挂起只影响能否被调度，不撤销任务的等待超时；对已挂起任务重复调用无副作用。
 * @note 挂起自己之后必须让出 CPU，否则会原地空转。
 */
void htTaskSuspend(TaskHandle_t xTaskToSuspend)
{
	htTCB_t *pxTCB = xTaskToSuspend == NULL ? pxCurrentTCB : (htTCB_t *)xTaskToSuspend;
	if (pxTCB == NULL || pxTCB->uxTaskState == HT_TASK_SUSPENDED) {
		return;
	}

	/* 记录是否挂起的是自己：只有挂起自己才需要主动让出 CPU。 */
	const BaseType_t xSuspendCurrent = pxTCB == pxCurrentTCB ? htTRUE : htFALSE;
	htEnterCritical();
	htListRemove(&(pxTCB->xStateListItem));
	htSchedClearReadyPriorityIfEmpty(pxTCB->uxPriority);
	htListRemove(&(pxTCB->xEventListItem));
	pxTCB->uxTaskState = HT_TASK_SUSPENDED;
	htListSetItemValue(&(pxTCB->xStateListItem), 0U);
	htListInsertEnd(&xSuspendedTaskList, &(pxTCB->xStateListItem));
	htExitCritical();

	if (xSuspendCurrent == htTRUE) {
		htTaskYield();
	}
}

/**
 * @brief 把任务从挂起链表恢复成就绪态。
 *
 * @param[in] pxTCB 目标任务。
 *
 * @return 需要立即切换返回 htTRUE；任务不在挂起链表或参数非法返回 htFALSE。
 *
 * @warning 调用者需自行保证临界区；htTaskResumeFromISR 靠中断上下文天然互斥。
 */
static BaseType_t prvResumeTask(htTCB_t *pxTCB)
{
	if (pxTCB == NULL || pxTCB->uxTaskState != HT_TASK_SUSPENDED ||
			pxTCB->xStateListItem.pxContainer != &xSuspendedTaskList) {
		return htFALSE;
	}

	/* 从挂起链表摘下，重置状态项的值后挂回对应优先级的就绪链表。 */
	htListRemove(&(pxTCB->xStateListItem));
	htListSetItemValue(&(pxTCB->xStateListItem), configMAX_PRIORITIES - pxTCB->uxPriority);
	pxTCB->uxTaskState = HT_TASK_READY;
	htListInsertEnd(&(pxReadyTasksLists[pxTCB->uxPriority]), &(pxTCB->xStateListItem));
	htSchedRecordReadyPriority(pxTCB->uxPriority);
	htSchedNotifyTaskEnqueued(pxTCB);
	/* 新任务优先级高于当前任务时才需要切换。 */
	return pxCurrentTCB == NULL || pxTCB->uxPriority > pxCurrentTCB->uxPriority ? htTRUE : htFALSE;
}

#if (configUSE_TASK_NOTIFICATIONS == 1)
/**
 * @brief 把任务阻塞在"等待任务通知"上，必要时同时挂入延时链表。
 */
static BaseType_t prvBlockCurrentTaskOnNotification(htTCB_t *pxTCB, TickType_t xTicksToWait)
{
	if (pxTCB == NULL) {
		return htFAIL;
	}
	if (xTicksToWait != htBLOCKED_INDEFINITELY && (pxDelayedTaskList == NULL || pxOverflowDelayedTaskList == NULL)) {
		return htFAIL;
	}

	htListRemove(&(pxTCB->xStateListItem));
	htSchedClearReadyPriorityIfEmpty(pxTCB->uxPriority);
	htListRemove(&(pxTCB->xEventListItem));

	if (xTicksToWait != htBLOCKED_INDEFINITELY) {
		const TickType_t xTimeToWake = xTickCount + xTicksToWait;
		htListSetItemValue(&(pxTCB->xStateListItem), xTimeToWake);
		if (xTimeToWake > xTickCount) {
			htListInsert(pxDelayedTaskList, &(pxTCB->xStateListItem));
			if (xTimeToWake < xNextTaskUnblockTime) {
				xNextTaskUnblockTime = xTimeToWake;
			}
		} else {
			htListInsert(pxOverflowDelayedTaskList, &(pxTCB->xStateListItem));
		}
	}

	pxTCB->uxNotificationStatus = HT_NOTIFY_WAITING;
	pxTCB->uxTaskState = HT_TASK_BLOCKED;
	return htPASS;
}

/**
 * @brief 把因等待任务通知而阻塞的任务唤醒成就绪态。
 */
static BaseType_t prvUnblockNotificationTask(htTCB_t *pxTCB)
{
	if (pxTCB == NULL || pxTCB->uxPriority >= configMAX_PRIORITIES || pxTCB->uxTaskState != HT_TASK_BLOCKED) {
		return htFALSE;
	}
	htListRemove(&(pxTCB->xEventListItem));
	htListRemove(&(pxTCB->xStateListItem));

	htListSetItemValue(&(pxTCB->xStateListItem), configMAX_PRIORITIES - pxTCB->uxPriority);
	pxTCB->uxTaskState = HT_TASK_READY;
	htListInsertEnd(&(pxReadyTasksLists[pxTCB->uxPriority]), &(pxTCB->xStateListItem));
	htSchedRecordReadyPriority(pxTCB->uxPriority);
	htSchedNotifyTaskEnqueued(pxTCB);
	return pxCurrentTCB == NULL || pxTCB->uxPriority > pxCurrentTCB->uxPriority ? htTRUE : htFALSE;
}

/**
 * @brief 按指定动作修改目标任务的通知值，并在需要时唤醒它。
 */
static BaseType_t prvNotifyTask(htTCB_t *pxTCB, uint32_t ulValue, htNotifyAction_t xAction, BaseType_t *pxYieldRequired)
{
	if (pxTCB == NULL || xAction > HT_NOTIFY_SET_VALUE_WITHOUT_OVERWRITE) {
		return htFAIL;
	}
	if (xAction == HT_NOTIFY_SET_VALUE_WITHOUT_OVERWRITE && pxTCB->uxNotificationStatus == HT_NOTIFY_RECEIVED) {
		return htFAIL;
	}

	/* 记下进入时的状态：只有它本来在等通知，才需要唤醒。 */
	const UBaseType_t uxPreviousStatus = pxTCB->uxNotificationStatus;
	switch (xAction) {
		case HT_NOTIFY_NO_ACTION:
			break;
		case HT_NOTIFY_SET_BITS:
			pxTCB->ulNotifiedValue |= ulValue;
			break;
		case HT_NOTIFY_INCREMENT:
			if (pxTCB->ulNotifiedValue != UINT32_MAX) {
				pxTCB->ulNotifiedValue++;
			}
			break;
		case HT_NOTIFY_SET_VALUE_WITH_OVERWRITE:
		case HT_NOTIFY_SET_VALUE_WITHOUT_OVERWRITE:
			pxTCB->ulNotifiedValue = ulValue;
			break;
		default:
			return htFAIL;
	}

	/* 值已更新，状态置为"已收到"；若它正在等，就顺带唤醒。 */
	pxTCB->uxNotificationStatus = HT_NOTIFY_RECEIVED;
	if (uxPreviousStatus == HT_NOTIFY_WAITING && prvUnblockNotificationTask(pxTCB) == htTRUE &&
			pxYieldRequired != NULL) {
		*pxYieldRequired = htTRUE;
	}
	return htPASS;
}
#endif /* configUSE_TASK_NOTIFICATIONS */

/**
 * @brief 恢复被挂起的任务。
 *
 * @param[in] xTaskToResume 任务句柄。
 *
 * @note 若被恢复任务的优先级高于当前任务，会立即让出 CPU。
 */
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

/**
 * @brief 在中断中恢复被挂起的任务。
 *
 * @param[in] xTaskToResume 任务句柄。
 *
 * @return 需要中断退出后切换返回 htTRUE，否则返回 htFALSE。
 *
 * @warning 只能在中断上下文调用；不阻塞，也不进入临界区。
 */
BaseType_t htTaskResumeFromISR(TaskHandle_t xTaskToResume)
{
	const UBaseType_t uxSavedStatus = htEnterCriticalFromISR();
	const BaseType_t xYieldRequired = prvResumeTask((htTCB_t *)xTaskToResume);
	htExitCriticalFromISR(uxSavedStatus);
	return xYieldRequired;
}

/**
 * @brief 向指定任务投递一次任务通知。
 *
 * @param[in] xTaskToNotify 目标任务句柄。
 * @param[in] ulValue 参与运算的值，含义由 xAction 决定。
 * @param[in] xAction 通知动作。
 *
 * @return 投递成功返回 htPASS；动作非法、目标为空或功能未开启返回 htFAIL。
 *
 * @note configUSE_TASK_NOTIFICATIONS 为 0 时只吞掉参数并返回 htFAIL。
 */
BaseType_t htTaskNotify(TaskHandle_t xTaskToNotify, uint32_t ulValue, htNotifyAction_t xAction)
{
#if configUSE_TASK_NOTIFICATIONS == 1
	htTCB_t *pxTCB = (htTCB_t *)xTaskToNotify;
	BaseType_t xYieldRequired = htFALSE;
	BaseType_t xReturn;

	htEnterCritical();
	xReturn = prvNotifyTask(pxTCB, ulValue, xAction, &xYieldRequired);
	htExitCritical();

	if (xReturn == htPASS && xYieldRequired == htTRUE) {
		htTaskYield();
	}
	return xReturn;
#else
	(void)xTaskToNotify;
	(void)ulValue;
	(void)xAction;
	return htFAIL;
#endif
}

/**
 * @brief 在中断中向任务投递通知，不阻塞。
 *
 * @param[in] xTaskToNotify 目标任务句柄。
 * @param[in] ulValue 参与运算的值。
 * @param[in] xAction 通知动作。
 * @param[out] pxHigherPriorityTaskWoken 若唤醒了更高优先级任务则置 htTRUE；可为 NULL。
 *
 * @return 投递成功返回 htPASS，失败返回 htFAIL。
 *
 * @warning 只能在中断上下文调用。
 */
BaseType_t htTaskNotifyFromISR(
		TaskHandle_t xTaskToNotify, uint32_t ulValue, htNotifyAction_t xAction, BaseType_t *pxHigherPriorityTaskWoken)
{
#if configUSE_TASK_NOTIFICATIONS == 1
	if (pxHigherPriorityTaskWoken != NULL) {
		*pxHigherPriorityTaskWoken = htFALSE;
	}
	const UBaseType_t uxSavedStatus = htEnterCriticalFromISR();
	const BaseType_t xReturn = prvNotifyTask((htTCB_t *)xTaskToNotify, ulValue, xAction, pxHigherPriorityTaskWoken);
	htExitCriticalFromISR(uxSavedStatus);
	return xReturn;
#else
	(void)xTaskToNotify;
	(void)ulValue;
	(void)xAction;
	if (pxHigherPriorityTaskWoken != NULL) {
		*pxHigherPriorityTaskWoken = htFALSE;
	}
	return htFAIL;
#endif
}

/**
 * @brief 以"计数信号量"方式给目标任务一次通知（通知值加一）。
 *
 * @param[in] xTaskToNotify 目标任务句柄。
 *
 * @return 成功返回 htPASS，失败返回 htFAIL。
 */
BaseType_t htTaskNotifyGive(TaskHandle_t xTaskToNotify)
{
	return htTaskNotify(xTaskToNotify, 0U, HT_NOTIFY_INCREMENT);
}

/**
 * @brief 在中断中以"计数信号量"方式给任务通知。
 *
 * @param[in] xTaskToNotify 目标任务句柄。
 * @param[out] pxHigherPriorityTaskWoken 若唤醒了更高优先级任务则置 htTRUE；可为 NULL。
 *
 * @return 成功返回 htPASS，失败返回 htFAIL。
 */
BaseType_t htTaskNotifyGiveFromISR(TaskHandle_t xTaskToNotify, BaseType_t *pxHigherPriorityTaskWoken)
{
	return htTaskNotifyFromISR(xTaskToNotify, 0U, HT_NOTIFY_INCREMENT, pxHigherPriorityTaskWoken);
}

/**
 * @brief 等待任务通知，并按掩码在进入等待前/取到通知后清除通知值中的位。
 *
 * @param[in] ulBitsToClearOnEntry 进入等待前要从通知值中清掉的位。
 * @param[in] ulBitsToClearOnExit 取到通知后要从通知值中清掉的位。
 * @param[out] pulNotificationValue 返回本次拿到的通知值，可为 NULL。
 * @param[in] xTicksToWait 最长等待 tick 数，0 表示不等待。
 *
 * @return 拿到通知返回 htPASS，超时返回 htFAIL。
 *
 * @note 只有"尚未收到通知"时才进入等待；等待期间投递的通知不会被丢弃。
 */
BaseType_t htTaskNotifyWait(uint32_t ulBitsToClearOnEntry, uint32_t ulBitsToClearOnExit, uint32_t *pulNotificationValue,
		TickType_t xTicksToWait)
{
#if configUSE_TASK_NOTIFICATIONS == 1
	htTCB_t *pxTCB = pxCurrentTCB;
	if (pxTCB == NULL) {
		return htFAIL;
	}

	htEnterCritical();
	if (pxTCB->uxNotificationStatus != HT_NOTIFY_RECEIVED) {
		pxTCB->ulNotifiedValue &= ~ulBitsToClearOnEntry;
		if (xTicksToWait == 0U || prvBlockCurrentTaskOnNotification(pxTCB, xTicksToWait) != htPASS) {
			if (pulNotificationValue != NULL) {
				*pulNotificationValue = 0U;
			}
			pxTCB->uxNotificationStatus = HT_NOTIFY_NOT_WAITING;
			htExitCritical();
			return htFAIL;
		}
		htExitCritical();
		htTaskYield();
		htEnterCritical();
	}

	if (pxTCB->uxNotificationStatus != HT_NOTIFY_RECEIVED) {
		pxTCB->uxNotificationStatus = HT_NOTIFY_NOT_WAITING;
		if (pulNotificationValue != NULL) {
			*pulNotificationValue = 0U;
		}
		htExitCritical();
		return htFAIL;
	}

	if (pulNotificationValue != NULL) {
		*pulNotificationValue = pxTCB->ulNotifiedValue;
	}
	pxTCB->ulNotifiedValue &= ~ulBitsToClearOnExit;
	pxTCB->uxNotificationStatus = HT_NOTIFY_NOT_WAITING;
	htExitCritical();
	return htPASS;
#else
	(void)ulBitsToClearOnEntry;
	(void)ulBitsToClearOnExit;
	(void)pulNotificationValue;
	(void)xTicksToWait;
	return htFAIL;
#endif
}

/**
 * @brief 以"计数信号量"方式取出通知值。
 *
 * @param[in] xClearCountOnExit 非 htFALSE 时取完后把通知值清零，否则只减一。
 * @param[in] xTicksToWait 通知值为 0 时的最长等待 tick 数。
 *
 * @return 取到的通知值；超时或功能未开启返回 0。
 */
uint32_t htTaskNotifyTake(BaseType_t xClearCountOnExit, TickType_t xTicksToWait)
{
#if configUSE_TASK_NOTIFICATIONS == 1
	htTCB_t *pxTCB = pxCurrentTCB;
	if (pxTCB == NULL) {
		return 0U;
	}

	htEnterCritical();
	if (pxTCB->ulNotifiedValue == 0U) {
		if (xTicksToWait == 0U || prvBlockCurrentTaskOnNotification(pxTCB, xTicksToWait) != htPASS) {
			pxTCB->uxNotificationStatus = HT_NOTIFY_NOT_WAITING;
			htExitCritical();
			return 0U;
		}
		htExitCritical();
		htTaskYield();
		htEnterCritical();
	}

	const uint32_t ulReturn = pxTCB->ulNotifiedValue;
	if (ulReturn != 0U) {
		if (xClearCountOnExit != htFALSE) {
			pxTCB->ulNotifiedValue = 0U;
		} else {
			pxTCB->ulNotifiedValue--;
		}
	}
	pxTCB->uxNotificationStatus = HT_NOTIFY_NOT_WAITING;
	htExitCritical();
	return ulReturn;
#else
	(void)xClearCountOnExit;
	(void)xTicksToWait;
	return 0U;
#endif
}

/**
 * @brief 取当前正在运行的任务句柄。
 *
 * @return 当前任务 TCB；调度器尚未启动时返回 NULL。
 */
TaskHandle_t htTaskGetCurrentTaskHandle(void)
{
	return (TaskHandle_t)pxCurrentTCB;
}

/**
 * @brief 查询任务名。
 *
 * @param[in] xTaskToQuery 任务句柄；NULL 表示当前任务。
 *
 * @return 指向 TCB 内任务名缓冲区的指针；句柄无效时返回 NULL。
 *
 * @note 返回的是 TCB 内部缓冲区，不能修改也不能释放。
 */
char *htTaskGetName(TaskHandle_t xTaskToQuery)
{
	htTCB_t *pxTCB = xTaskToQuery == NULL ? pxCurrentTCB : (htTCB_t *)xTaskToQuery;
	return pxTCB == NULL ? NULL : pxTCB->pcTaskName;
}

/**
 * @brief 查询任务当前状态。
 *
 * @param[in] xTask 任务句柄；NULL 表示当前任务。
 *
 * @return 状态枚举值；句柄与当前任务都无效时返回 HT_TASK_INVALID。
 */
htTaskStatus_t htTaskGetState(TaskHandle_t xTask)
{
	htTCB_t *pxTCB = xTask == NULL ? pxCurrentTCB : (htTCB_t *)xTask;
	return pxTCB == NULL ? HT_TASK_INVALID : (htTaskStatus_t)pxTCB->uxTaskState;
}

/**
 * @brief 启动调度器的别名入口，直接转调 htStartScheduler()。
 *
 * @note 保留该名字是为了兼容 FreeRTOS 风格的调用习惯。
 */
void htTaskStartScheduler(void)
{
	htStartScheduler();
}

/**
 * @brief 请求一次上下文切换：通过移植层 htPortYield() 挂起 PendSV 让出 CPU。
 */
void htTaskYield(void)
{
	htPortYield();
}

void htTaskCheckDelayedTasks(void)
{
	/* 防御：延时链表尚未初始化时直接返回。 */
	if (pxDelayedTaskList == NULL) {
		return;
	}

	/* 先取一次 tick 快照，整轮判断都用它，避免中途 tick 变化导致误判。 */
	TickType_t xCurrentTickCount = xTickCount;

	/* O(1) 快速路径：最早唤醒时刻尚未到达时直接跳过链表检查。 */
	if (xCurrentTickCount < xNextTaskUnblockTime) {
		return;
	}

	/* 溢出列表要等 tick 回绕交换后才能检查，否则会被提前唤醒。 */
	if (pxDelayedTaskList->uxNumberOfItems == 0U) {
		xNextTaskUnblockTime = (TickType_t)htBLOCKED_INDEFINITELY;
		return;
	}
	/* 循环处理所有到期任务：一次 tick 可能唤醒多个。 */
	htListItem_t *pxItem;
	htTCB_t *pxTCB;
	TickType_t xItemValue;
	BaseType_t xListItemRemoved = htFALSE; /* 本轮是否摘过节点，决定要不要继续循环 */

	/* 逐个检查表头，直到遇到尚未到期的任务。 */
	do {
		xListItemRemoved = htFALSE;

		/* 取表头：它是最早到期的任务。 */
		pxItem = htListGetHead(pxDelayedTaskList);
		if (pxItem == NULL) {
			/* 链表为空，结束。 */
			break;
		}

		/* 该链表项的值就是唤醒时刻。 */
		xItemValue = htListGetItemValue(pxItem);

		/* 已到期才处理；未到期说明后面的都还没到期。 */
		if (xItemValue <= xCurrentTickCount) {
			/* 由链表项反查所属 TCB。 */
			pxTCB = (htTCB_t *)htListGetItemOwner(pxItem);
			if (pxTCB != NULL) {
				/* 先从延时链表摘下。 */
				htListRemove(pxItem);
				/* 超时唤醒时也必须从队列/信号量等待列表中摘除。 */
				htListRemove(&(pxTCB->xEventListItem));
				xListItemRemoved = htTRUE;

				/* 再挂回对应优先级的就绪链表尾部。 */
				if (pxTCB->uxPriority < configMAX_PRIORITIES) {
					/* 状态改回就绪，并同步重置排序键与 O(1) 位图。 */
					pxTCB->uxTaskState = HT_TASK_READY;
					htListSetItemValue(pxItem, configMAX_PRIORITIES - pxTCB->uxPriority);
					htListInsertEnd(&(pxReadyTasksLists[pxTCB->uxPriority]), pxItem);
					htSchedRecordReadyPriority(pxTCB->uxPriority);
					htSchedNotifyTaskEnqueued(pxTCB);
				}
			} else {
				/* 属主非法：丢掉这个坏节点，继续处理下一个。 */
				htListRemove(pxItem);
				xListItemRemoved = htTRUE;
			}
		}

		/* 只要本轮摘过节点就再查一次：可能还有同一时刻到期的任务。 */
	} while (xListItemRemoved == htTRUE);

	/* 刷新下一最早唤醒时刻缓存。 */
	prvResetNextTaskUnblockTime();
}

/**
 * @brief 系统滴答处理：递增 tick、处理回绕、扫描延时任务并按需请求切换。
 *
 * @note 全程关中断执行，返回前才重新开中断；由 SysTick 中断调用。
 * @note tick 回绕时交换两条延时链表，使跨越回绕点的任务成为当前链表成员。
 */
#if (configUSE_TICK_HOOK == 1)
/**
 * @brief 滴答钩子的弱默认实现：什么也不做。
 *
 * @note 这里不写成停机循环，因为钩子每毫秒都会走一次；应用提供同名强符号即可接管。
 */
__attribute__((weak)) void htTickHook(void) {}
#endif

void htTaskTickInc(void)
{
	/* 保存/恢复中断屏蔽状态，防止高优先级 ISR 抢占破坏 tick 与延时链表。 */
	const UBaseType_t uxSavedStatus = htEnterCriticalFromISR();

	/* 计量本次滴答中断本身的开销：起点在屏蔽中断之后，止点在恢复中断之前。 */
	HT_LATENCY_TICK_BEGIN();

	/* 递增系统滴答计数。 */
	xTickCount++;
	if (xTickCount == 0U) {
		/* tick 回绕后，之前跨越回绕点的任务成为当前延时任务，并立刻刷新最早唤醒时刻缓存。 */
		htList_t *pxTemporary = pxDelayedTaskList;
		pxDelayedTaskList = pxOverflowDelayedTaskList;
		pxOverflowDelayedTaskList = pxTemporary;
		prvResetNextTaskUnblockTime();
	}

#if (configUSE_TICK_HOOK == 1)
	/*
	 * 钩子在扫描延时链表之前调用：它看到的是"tick 已经加一、但还没有任何任务
	 * 因到期而就绪"的时刻，与 FreeRTOS 的 vApplicationTickHook() 次序一致。
	 * 此处中断仍是关闭的，钩子不得阻塞。
	 */
	htTickHook();
#endif

	/*
	 * 把刚刚过去的这一个 tick 记到正在运行的任务头上。采样放在切换之前，
	 * 因此记的正是"过去 1 ms 里真正占用 CPU 的那个任务"。
	 */
#if (configGENERATE_RUN_TIME_STATS == 1)
	htRunTimeSample();
#endif

	/* 让当前策略给刚过去的这个 tick 记账：公平策略累加虚拟时间，最早死期策略查漏期。 */
	htSchedNotifyTick(pxCurrentTCB);

	/* 把所有到期任务搬回就绪链表。 */
	htTaskCheckDelayedTasks();

	/* 有任务在跑且确实需要切换时，置位 PendSV。 */
	if (pxCurrentTCB != NULL) {
		if (htSchedulerNeedsSwitch()) {
			/* 置 PENDSVSET，等中断退出后执行切换。 */
			htPortRequestSwitchFromISR();
		}
	}
	/* 结算本次滴答中断的耗时，然后恢复中断屏蔽。 */
	HT_LATENCY_TICK_END();

	htExitCriticalFromISR(uxSavedStatus);
}

/**
 * @brief 删除任务并释放其占用的资源。
 *
 * @param[in] xTaskToDelete 任务句柄；NULL 表示删除当前任务自己。
 *
 * @note 删除别的任务可以立即释放；删除自己时不能释放正在使用的栈，只能挂到
 *       xTasksWaitingTermination，等空闲任务切换后回收。
 * @warning 删除后句柄立即失效；静态分配的任务其 TCB 与栈不会被释放。
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

	/* 防重复删除：仅当任务仍在已分配总表或活动链表内时才递减计数。 */
#if (configUSE_TRACE_FACILITY == 1)
	const BaseType_t xWasAllocated = (pxTCB->xAllListItem.pxContainer == &pxAllocatedTasksList) ? htTRUE : htFALSE;
#else
	const BaseType_t xWasAllocated =
			(pxTCB->xStateListItem.pxContainer != NULL || pxTCB->xEventListItem.pxContainer != NULL ||
					pxTCB->uxTaskState != HT_TASK_SUSPENDED)
					? htTRUE
					: htFALSE;
#endif

	/* 从状态、事件和全局跟踪列表中移除任务。 */
	htListRemove(&(pxTCB->xStateListItem));
	htSchedClearReadyPriorityIfEmpty(pxTCB->uxPriority);
	htListRemove(&(pxTCB->xEventListItem));
#if (configUSE_TRACE_FACILITY == 1)
	htListRemove(&(pxTCB->xAllListItem));
#endif

	if (xDeleteCurrentTask == htTRUE) {
		pxTCB->uxTaskState = HT_TASK_SUSPENDED;
		htListSetItemValue(&(pxTCB->xStateListItem), 0U);
		htListInsertEnd(&xTasksWaitingTermination, &(pxTCB->xStateListItem));
	}

	/* 仅在任务原本在册时递减任务计数器 */
	if (xWasAllocated == htTRUE && uxCurrentNumberOfTasks > 0U) {
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
	pxTCB->uxTaskState = HT_TASK_SUSPENDED;
	if (pxTCB->ucStaticallyAllocated == 0U) {
		htPortFree(pxTCB->pxStack);
		htPortFree(pxTCB);
	}
}

/* ------------------------------------------------------------------ */
/* 运行时间统计                                                         */
/* ------------------------------------------------------------------ */
/*
 * 统计必须跟着滴答走，也要直接读当前任务的 TCB，因此实现在本文件而不是
 * kernel/htutils.c；include/htutils.h 只负责声明。
 */

#if (configGENERATE_RUN_TIME_STATS == 1)

/** @brief 上一次采样时周期计数器的读数。 */
static uint32_t s_ulLastSampleCycles;
/** @brief 自启动以来累计的周期数，是所有任务运行时间之和。 */
static uint64_t s_ullTotalCycles;
/** @brief 其中记在空闲任务头上的周期数。 */
static uint64_t s_ullIdleCycles;
/** @brief 时间基准是否就绪；0 表示下一次采样只用来建立基准。 */
static uint8_t s_ucTimerReady;

/**
 * @brief 采样一次运行时间，把刚过去的一个 tick 记到当前任务头上。
 *
 * @note 由 htTaskTickInc() 在关中断状态下每毫秒调用一次，应用不要直接调用。
 * @note 第一次调用只打开周期计数器并记下基准，启动阶段的时间不计入任何任务。
 * @note 两次采样之间 32 位计数器的回绕是安全的：无符号减法自动得到正确增量，
 *       前提是采样间隔远小于回绕周期（72 MHz 下约 59.6 s）。
 */
void htRunTimeSample(void)
{
	uint32_t ulNow;
	uint32_t ulDelta;

	if (s_ucTimerReady == 0U) {
		portCONFIGURE_TIMER_FOR_RUN_TIME_STATS();
		s_ulLastSampleCycles = portGET_RUN_TIME_COUNTER_VALUE();
		s_ucTimerReady = 1U;
		return;
	}

	ulNow = portGET_RUN_TIME_COUNTER_VALUE();
	/* 无符号相减，计数器回绕时结果依然正确。 */
	ulDelta = ulNow - s_ulLastSampleCycles;
	s_ulLastSampleCycles = ulNow;

	s_ullTotalCycles += ulDelta;
	if (pxCurrentTCB != NULL) {
		pxCurrentTCB->ullRunTimeCounter += ulDelta;
		if ((TaskHandle_t)pxCurrentTCB == htTaskGetIdleHandle()) {
			s_ullIdleCycles += ulDelta;
		}
	}
}

/**
 * @brief 取一份总时长与空闲时长的快照。
 *
 * @param[out] pullTotal 自启动以来的累计周期数。
 * @param[out] pullIdle  其中空闲任务占用的周期数。
 * @note 两个 64 位量在 32 位机上是分几次读出的，中断是毫秒级的采样点，
 *       这里先存下 PRIMASK 再关中断，读完按原值恢复——即便调用方本身就处在
 *       临界区内，也不会被提前打开中断。
 */
static void prvReadRunTimeCounters(uint64_t *pullTotal, uint64_t *pullIdle)
{
	const uint32_t ulSaved = htPortEnterAtomicRaw();
	*pullTotal = s_ullTotalCycles;
	*pullIdle = s_ullIdleCycles;
	htPortExitAtomicRaw(ulSaved);
}

#endif /* configGENERATE_RUN_TIME_STATS */

/**
 * @brief 获取空闲任务占用 CPU 时间的百分比。
 *
 * @return 0~100 的百分比；还没有样本时返回 0。
 * @note 返回值是"自启动以来的平均值"，不是最近一段时间的使用率。
 * @note configGENERATE_RUN_TIME_STATS 为 0 时恒返回 0，表示没有这项数据，
 *       而不是表示系统一直空闲。
 */
#if (configGENERATE_RUN_TIME_STATS == 1) || (configUSE_STATS_FORMATTING_FUNCTIONS == 1)
/**
 * @brief 用等比右移 + 32 位硬件 UDIV 计算 64 位占比（0..100），消除 __udivmoddi4（省 ~754 B Flash）。
 */
static uint32_t prvCalcPercent64(uint64_t ullPart, uint64_t ullTotal)
{
	if (ullTotal == 0U) {
		return 0U;
	}
	if (ullPart >= ullTotal) {
		return 100U;
	}
	while (ullTotal > (uint64_t)(UINT32_MAX / 100U)) {
		ullPart >>= 1U;
		ullTotal >>= 1U;
	}
	return (uint32_t)(((uint32_t)ullPart * 100U) / (uint32_t)ullTotal);
}
#endif

uint32_t htGetIdleRunTimePercent(void)
{
#if (configGENERATE_RUN_TIME_STATS == 1)
	uint64_t ullTotal;
	uint64_t ullIdle;

	prvReadRunTimeCounters(&ullTotal, &ullIdle);
	return prvCalcPercent64(ullIdle, ullTotal);
#else
	return 0U;
#endif
}

/**
 * @brief 获取 CPU 使用率。
 *
 * @return 0~100 的百分比，等于 100 减去空闲占比。
 * @note 与空闲占比一样，是自启动以来的平均值；统计关闭时返回 0。
 */
uint32_t htGetCPUUsage(void)
{
#if (configGENERATE_RUN_TIME_STATS == 1)
	uint64_t ullTotal;
	uint64_t ullIdle;
	uint32_t ulIdlePercent;

	prvReadRunTimeCounters(&ullTotal, &ullIdle);
	if (ullTotal == 0U) {
		/* 还没有样本：与空闲占比一致地返回 0，而不是 100% 满载。 */
		return 0U;
	}
	ulIdlePercent = prvCalcPercent64(ullIdle, ullTotal);
	return (ulIdlePercent >= 100U) ? 0U : (100U - ulIdlePercent);
#else
	return 0U;
#endif
}

/**
 * @brief 获取自启动以来累计的运行时间计数。
 *
 * @return 累计的周期数；72 MHz 下除以 72000 即得毫秒数。
 * @note 统计关闭时返回 0。需要 tick 级时间时请改用 xTickCount 或 htTicksToTime()。
 */
uint64_t htGetRunTimeCounter(void)
{
#if (configGENERATE_RUN_TIME_STATS == 1)
	uint64_t ullTotal;
	uint64_t ullIdle;

	prvReadRunTimeCounters(&ullTotal, &ullIdle);
	(void)ullIdle;
	return ullTotal;
#else
	return 0U;
#endif
}

#if (configUSE_STATS_FORMATTING_FUNCTIONS == 1)
/* 任务名在表格里占的列宽（字符数），超出部分截断。 */
#define HT_STATS_NAME_WIDTH 8U

/*
 * 下面四个追加函数共同维护一个“有界写入”的游标：缓冲区最后一个字节永远留给结束符，
 * 写满之后的追加请求被静默丢弃，因此调用方拿到的永远是合法 C 字符串。之所以自己拼接
 * 而不用 snprintf，是因为固件侧已经把 printf 家族整个裁掉，再把它们链回来等于白裁。
 */
typedef struct {
	char *pcBuffer;
	size_t uxCapacity;
	size_t uxLength;
} htStatsWriter_t;

static void prvStatsPutChar(htStatsWriter_t *pxWriter, char cChar)
{
	if ((pxWriter->uxLength + 1U) < pxWriter->uxCapacity) {
		pxWriter->pcBuffer[pxWriter->uxLength] = cChar;
		pxWriter->uxLength++;
		pxWriter->pcBuffer[pxWriter->uxLength] = '\0';
	}
}

static void prvStatsPutString(htStatsWriter_t *pxWriter, const char *pcText)
{
	while (*pcText != '\0') {
		prvStatsPutChar(pxWriter, *pcText);
		pcText++;
	}
}

static void prvStatsPutUint(htStatsWriter_t *pxWriter, uint32_t ulValue)
{
	char cDigits[10];
	size_t uxCount = 0U;

	do {
		cDigits[uxCount] = (char)('0' + (char)(ulValue % 10U));
		uxCount++;
		ulValue /= 10U;
	} while (ulValue != 0U);

	while (uxCount > 0U) {
		uxCount--;
		prvStatsPutChar(pxWriter, cDigits[uxCount]);
	}
}

/* 任务名按固定宽度左对齐输出，超长截断，保证各行还能对齐着看。 */
static void prvStatsPutName(htStatsWriter_t *pxWriter, const char *pcName)
{
	size_t uxIndex = 0U;

	while ((uxIndex < HT_STATS_NAME_WIDTH) && (pcName[uxIndex] != '\0')) {
		prvStatsPutChar(pxWriter, pcName[uxIndex]);
		uxIndex++;
	}
	while (uxIndex < HT_STATS_NAME_WIDTH) {
		prvStatsPutChar(pxWriter, ' ');
		uxIndex++;
	}
}

static char prvStatsStateChar(htTaskStatus_t xState)
{
	switch (xState) {
		case HT_TASK_RUNNING:
			return 'R';
		case HT_TASK_READY:
			return 'r';
		case HT_TASK_BLOCKED:
			return 'B';
		case HT_TASK_SUSPENDED:
			return 'S';
		default:
			return '?';
	}
}

size_t htTaskList(char *pcWriteBuffer, size_t uxBufferSize)
{
	htStatsWriter_t xWriter;
	htListItem_t *pxItem;
	htTCB_t *pxTask;
	uint64_t ullTotal;
	uint32_t ulPercent;
	UBaseType_t uxRemaining;

	if ((pcWriteBuffer == NULL) || (uxBufferSize < 2U)) {
		return 0U;
	}

	xWriter.pcBuffer = pcWriteBuffer;
	xWriter.uxCapacity = uxBufferSize;
	xWriter.uxLength = 0U;
	pcWriteBuffer[0] = '\0';

	prvStatsPutString(&xWriter, "name     s prio stk rt%\r\n");

	/*
	 * 仅挂起调度器而非全程关中断：防止空闲任务回收或其它任务删除 TCB，同时
	 * 保证全任务栈水位扫描与字符串格式化期间硬件中断完全不被屏蔽。
	 */
	htSuspendScheduler();
	ullTotal = htGetRunTimeCounter();
	uxRemaining = htListGetCurrentNumberOfItems(&pxAllocatedTasksList);
	pxItem = htListGetHead(&pxAllocatedTasksList);
	while ((pxItem != NULL) && (uxRemaining > 0U)) {
		uint64_t ullTaskCycles;
		uint32_t ulSaved;

		pxTask = (htTCB_t *)htListGetItemOwner(pxItem);
		pxItem = htListGetNext(pxItem);
		uxRemaining--;
		if (pxTask == NULL) {
			continue;
		}

		/* 仅在读取 64 位任务周期计数的一瞬间做原子保护（约数个时钟周期）。 */
#if (configGENERATE_RUN_TIME_STATS == 1)
		ulSaved = htPortEnterAtomicRaw();
		ullTaskCycles = pxTask->ullRunTimeCounter;
		htPortExitAtomicRaw(ulSaved);
#else
		(void)ulSaved;
		ullTaskCycles = 0U;
#endif

		prvStatsPutName(&xWriter, pxTask->pcTaskName);
		prvStatsPutChar(&xWriter, ' ');
		prvStatsPutChar(&xWriter, prvStatsStateChar(htTaskGetState((TaskHandle_t)pxTask)));
		prvStatsPutChar(&xWriter, ' ');
		prvStatsPutUint(&xWriter, (uint32_t)pxTask->uxPriority);
		prvStatsPutChar(&xWriter, ' ');
		prvStatsPutUint(&xWriter, (uint32_t)htGetTaskStackHighWaterMark((TaskHandle_t)pxTask));
		prvStatsPutChar(&xWriter, ' ');
		ulPercent = prvCalcPercent64(ullTaskCycles, ullTotal);
		prvStatsPutUint(&xWriter, ulPercent);
		prvStatsPutString(&xWriter, "%\r\n");
	}
	htResumeScheduler();

	return xWriter.uxLength;
}
#endif /* configUSE_STATS_FORMATTING_FUNCTIONS */
