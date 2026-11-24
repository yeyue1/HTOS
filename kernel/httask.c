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
#include "httrace.h"
#include "htutils.h"
#include <string.h>
#include "stm32f1xx_hal.h"
#include "core_cm3.h" /* 提供 NVIC 与 ICSR 等内核寄存器定义 */

/* 全局变量 */
htTCB_t *pxCurrentTCB = NULL; /* 当前运行的任务 TCB */
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
/* 所有任务链表：调试时可遍历全部在册任务。 */
htList_t pxAllocatedTasksList;

/**
 * @brief 从 htscheduler.c 导入的"是否需要切换"判定。
 *
 * @return 需要切换返回 htTRUE，否则返回 htFALSE。
 */
extern BaseType_t htSchedulerNeedsSwitch(void);

/**
 * @brief 初始化全部任务链表：各优先级就绪链表、延时/溢出链表、挂起与终止链表。
 *
 * @note 由 htSchedulerInit() 调用；去掉 static 只是为了让调度器初始化能调用
 *       它，并不属于对外 API。
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
 * @brief 任务函数返回后的最终归宿：永久停机。
 *
 * 任务函数禁止返回；一旦返回，bx lr 必须落到一条永久停机指令上。若把 LR 设为
 * 0xFFFFFFFE 这类非法地址，Cortex-M3 只会报 HardFault，CFSR 的 UFSR 段给出
 * 0x00040000（INVPC，非法 PC 装载），现象离原因很远，极难定位。
 */
static void prvTaskExitTrap(void)
{
	__disable_irq();
	for (;;) {
	}
}

/**
 * @brief 在任务栈顶构造一个"异常返回帧"，让任务首次被调度时像从中断返回一样起跑。
 *
 * @param[in] pxTopOfStack 栈顶指针（由调用者给出，通常为 栈底 + 深度 - 1）。
 * @param[in] pxCode 任务入口函数。
 * @param[in] pvParameters 传给任务的唯一参数，异常返回时装进 R0。
 *
 * @return 压好帧之后的新栈顶，写入 htTCB_t.pxTopOfStack。
 *
 * @warning 栈顶必须先做 8 字节对齐；xPSR 的 T 位必须为 1，而 PC 槽的 bit0 必须为 0。
 */
static StackType_t *prvInitialiseTaskStack(StackType_t *pxTopOfStack, TaskFunction_t pxCode, void *pvParameters)
{
	/* Cortex-M 异常入口要求栈顶 8 字节对齐；uintptr_t 同时兼容 64 位主机测试。 */
	pxTopOfStack = (StackType_t *)((uintptr_t)pxTopOfStack & ~(uintptr_t)0x7U);

	/* 先构造硬件异常返回帧（从高地址往低地址压）。 */
	*(--pxTopOfStack) = 0x01000000; /* xPSR - T位置位 */
	/* PC 槽由硬件异常返回装载：必须存偶数地址（bit0=0）。Thumb 状态由同一帧中
	   xPSR 的 T 位给出，把 bit0 置 1 反而会让返回失败：Cortex-M3 报 INVPC
	   （非法 PC 装载）。FreeRTOS 的 portSTART_ADDRESS_MASK 做的正是这件事。 */
	*(--pxTopOfStack) = ((uint32_t)(uintptr_t)pxCode) & ~(uint32_t)0x1U;
	*(--pxTopOfStack) = (uint32_t)(uintptr_t)prvTaskExitTrap; /* LR - 任务返回即停机 */
	*(--pxTopOfStack) = 0; /* R12 */
	*(--pxTopOfStack) = 0; /* R3 */
	*(--pxTopOfStack) = 0; /* R2 */
	*(--pxTopOfStack) = 0; /* R1 */
	*(--pxTopOfStack) = (uint32_t)pvParameters; /* R0 - 任务参数 */

	/* 再压入软件保存的 r4-r11，与 PendSV 里的 stmdb r0!,{r4-r11} 对应。 */
	*(--pxTopOfStack) = 0; /* R11 */
	*(--pxTopOfStack) = 0; /* R10 */
	*(--pxTopOfStack) = 0; /* R9 */
	*(--pxTopOfStack) = 0; /* R8 */
	*(--pxTopOfStack) = 0; /* R7 */
	*(--pxTopOfStack) = 0; /* R6 */
	*(--pxTopOfStack) = 0; /* R5 */
	*(--pxTopOfStack) = 0; /* R4 */

	return pxTopOfStack;
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

	pxNewTCB->pxTopOfStack = prvInitialiseTaskStack(pxStack + usStackDepth - 1U, pxTaskCode, pvParameters);

	pxNewTCB->uxPriority = uxPriority;
	pxNewTCB->uxBasePriority = uxPriority;
	pxNewTCB->uxNotificationStatus = HT_NOTIFY_NOT_WAITING;

	/* 三个链表项：状态项挂就绪/延时链表，事件项挂等待链表，跟踪项挂全局链表。 */
	htListItemInit(&(pxNewTCB->xStateListItem));
	htListItemInit(&(pxNewTCB->xEventListItem));
	htListItemInit(&(pxNewTCB->xAllListItem));
	htListSetItemOwner(&(pxNewTCB->xStateListItem), pxNewTCB);
	htListSetItemOwner(&(pxNewTCB->xEventListItem), pxNewTCB);
	htListSetItemOwner(&(pxNewTCB->xAllListItem), pxNewTCB);
	htListSetItemValue(&(pxNewTCB->xStateListItem), configMAX_PRIORITIES - uxPriority);
	htListSetItemValue(&(pxNewTCB->xAllListItem), configMAX_PRIORITIES - uxPriority);
	htListInsertEnd(&pxAllocatedTasksList, &(pxNewTCB->xAllListItem));

	/* 初始为就绪态，直接挂到对应优先级的就绪链表尾部。 */
	pxNewTCB->uxTaskState = HT_TASK_READY;
	htListInsertEnd(&(pxReadyTasksLists[uxPriority]), &(pxNewTCB->xStateListItem));
	uxCurrentNumberOfTasks++;

	/* 通知当前策略：新任务已经进入就绪表，策略可以初始化自己的私有字段。 */
	htSchedNotifyTaskCreated(pxNewTCB);
	if (pxCurrentTCB == NULL) {
		pxCurrentTCB = pxNewTCB;
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
 * @brief 相对延时：让当前任务阻塞指定 tick 数，到期后重新就绪。
 *
 * @param[in] xTicksToDelay 延时 tick 数；为 0 时只让出 CPU、不进入阻塞态。
 *
 * @note 这里用 __disable_irq()/__enable_irq() 直接开关中断，而不是可嵌套的
 *       htEnterCritical()，因此不能与临界区嵌套使用。
 */
void htTaskDelay(TickType_t xTicksToDelay)
{
	/* 延时为 0：只让出 CPU，不进入阻塞态。 */
	if (xTicksToDelay == 0) {
		htTaskYield();
		return;
	}

	/* 关中断，保证"摘链表 + 挂延时链表"这一串操作是原子的。 */
	__disable_irq();

	/* 防御：延时链表或当前任务尚未就绪时直接返回。 */
	if (pxDelayedTaskList == NULL || pxCurrentTCB == NULL) {
		__enable_irq();
		return;
	}

	/* 算出唤醒时刻；tick 回绕时下面的比较会把它归入溢出链表。 */
	TickType_t xTimeToWake = xTickCount + xTicksToDelay;

	/* 若当前任务还挂在就绪链表里，先摘出来。 */
	if (pxCurrentTCB->xStateListItem.pxContainer) {
		htListRemove(&(pxCurrentTCB->xStateListItem));
	}

	/* 把唤醒时刻写进状态项，按值升序插入延时链表。 */
	htListSetItemValue(&(pxCurrentTCB->xStateListItem), xTimeToWake);

	if (xTimeToWake > xTickCount) {
		pxCurrentTCB->xStateListItem.pxContainer = pxDelayedTaskList;
		htListInsert(pxDelayedTaskList, &(pxCurrentTCB->xStateListItem));
	} else {
		pxCurrentTCB->xStateListItem.pxContainer = pxOverflowDelayedTaskList;
		htListInsert(pxOverflowDelayedTaskList, &(pxCurrentTCB->xStateListItem));
	}

	/* 标记为阻塞态。 */
	pxCurrentTCB->uxTaskState = HT_TASK_BLOCKED;

	/* 重新开中断，随后请求一次切换让出 CPU。 */
	__enable_irq();
	htTaskYield();
}

/**
 * @brief 按绝对周期延时，避免周期任务逐次累积漂移。
 *
 * @param[in,out] pxPreviousWakeTime 上一次唤醒时刻；返回时被更新为下一次唤醒时刻。
 * @param[in] xTimeIncrement 周期长度（tick）。
 *
 * @return 正常进入延时返回 htPASS；已经错过唤醒时刻、无需等待时返回 htFAIL。
 *
 * @note 差值用 int32_t 计算，天然处理 tick 回绕；xTimeIncrement 超过 INT32_MAX
 *       会被拒绝，否则差值的符号位失去意义。
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
		}

		/* 若正挂在某个事件等待链表上，先摘下来，稍后按新优先级重新插入。 */
		htList_t *pxEventList = (htList_t *)pxTCB->xEventListItem.pxContainer;
		if (pxEventList != NULL) {
			htListRemove(&(pxTCB->xEventListItem));
		}

		/* 更新优先级与基优先级，并同步三个链表项的值。 */
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
	/* 新任务优先级高于当前任务时才需要切换。 */
	return pxCurrentTCB == NULL || pxTCB->uxPriority > pxCurrentTCB->uxPriority ? htTRUE : htFALSE;
}

/**
 * @brief 把任务阻塞在"等待任务通知"上，必要时同时挂入延时链表。
 *
 * @param[in,out] pxTCB 要阻塞的任务，通常是当前任务。
 * @param[in] xTicksToWait 最长等待 tick 数；htBLOCKED_INDEFINITELY 表示无限等待。
 *
 * @return 阻塞成功返回 htPASS；参数非法或延时链表未就绪返回 htFAIL。
 *
 * @warning 必须在临界区内调用。
 * @note 任务通知没有独立的事件链表，所以只动状态项，等待标志存放在
 *       uxNotificationStatus 里。
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
	htListRemove(&(pxTCB->xEventListItem));

	if (xTicksToWait != htBLOCKED_INDEFINITELY) {
		const TickType_t xTimeToWake = xTickCount + xTicksToWait;
		htListSetItemValue(&(pxTCB->xStateListItem), xTimeToWake);
		if (xTimeToWake > xTickCount) {
			htListInsert(pxDelayedTaskList, &(pxTCB->xStateListItem));
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
 *
 * @param[in] pxTCB 目标任务。
 *
 * @return 需要立即切换返回 htTRUE；任务状态不符或参数非法返回 htFALSE。
 *
 * @warning 必须在临界区内调用。
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
	return pxCurrentTCB == NULL || pxTCB->uxPriority > pxCurrentTCB->uxPriority ? htTRUE : htFALSE;
}

/**
 * @brief 按指定动作修改目标任务的通知值，并在需要时唤醒它。
 *
 * @param[in,out] pxTCB 目标任务。
 * @param[in] ulValue 参与运算的值，具体含义由 xAction 决定。
 * @param[in] xAction 通知动作，取值见 htNotifyAction_t。
 * @param[out] pxYieldRequired 若唤醒的任务优先级更高则置 htTRUE，可为 NULL。
 *
 * @return 通知已投递返回 htPASS；动作非法或"不覆盖已有值"检查失败返回 htFAIL。
 *
 * @note 目标不在等待状态时通知值照样更新，只是不会唤醒它。
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
	return prvResumeTask((htTCB_t *)xTaskToResume);
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
	return prvNotifyTask((htTCB_t *)xTaskToNotify, ulValue, xAction, pxHigherPriorityTaskWoken);
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
 * @brief 请求一次上下文切换：置位 PendSV，让出 CPU。
 *
 * @note 写 0xE000ED04（ICSR）的 PENDSVSET 位；PendSV 被设为最低优先级，
 *       因此总在当前所有中断退出后才执行切换。若已在中断里，则等中断返回时切换。
 * @note 主机单元测试下改调 htHostTestYieldHook()，不触碰硬件寄存器。
 */
void htTaskYield(void)
{
#if defined(HTOS_HOST_TEST)
	extern void htHostTestYieldHook(void);
	htHostTestYieldHook();
#else
	/* 触发 PendSV：向 ICSR 的 PENDSVSET 位写 1，请求一次上下文切换。 */
	*((volatile uint32_t *)0xE000ED04) = 0x10000000; /* ICSR 寄存器，置位 PENDSVSET */
#endif

	/*
	 * 早期版本在这里还补了两条内存屏障，实测非必需，已注释掉：
	 *   __asm volatile ("dsb" ::: "memory");
	 *   __asm volatile ("isb");
	 */
}

/**
 * @brief 扫描当前延时链表，把所有已到期的任务搬回就绪链表。
 *
 * @note 链表按唤醒时刻升序排列，所以一旦表头都没到期就可以停止。
 * @note 一次 tick 可能同时叫醒多个任务，因此用 do/while 逐个处理。
 */
void htTaskCheckDelayedTasks(void)
{
	/* 防御：延时链表尚未初始化时直接返回。 */
	if (pxDelayedTaskList == NULL) {
		return;
	}

	/* 先取一次 tick 快照，整轮判断都用它，避免中途 tick 变化导致误判。 */
	TickType_t xCurrentTickCount = xTickCount;

	/* 溢出列表要等 tick 回绕交换后才能检查，否则会被提前唤醒。 */
	if (pxDelayedTaskList->uxNumberOfItems == 0) {
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
					/* 状态改回就绪。 */
					pxTCB->uxTaskState = HT_TASK_READY;

					/* 把容器指针指向就绪链表，再插入链表尾部。 */
					pxItem->pxContainer = &(pxReadyTasksLists[pxTCB->uxPriority]);
					htListInsertEnd(&(pxReadyTasksLists[pxTCB->uxPriority]), pxItem);
				}
			} else {
				/* 属主非法：丢掉这个坏节点，继续处理下一个。 */
				htListRemove(pxItem);
				xListItemRemoved = htTRUE;
			}
		}

		/* 只要本轮摘过节点就再查一次：可能还有同一时刻到期的任务。 */
	} while (xListItemRemoved == htTRUE);
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
	/* 整段处理在关中断下完成，保证 tick 递增与延时时序一致。 */
	__disable_irq();

	/* 递增系统滴答计数。 */
	xTickCount++;
	if (xTickCount == 0U) {
		/* tick 回绕后，之前跨越回绕点的任务成为当前延时任务。 */
		htList_t *pxTemporary = pxDelayedTaskList;
		pxDelayedTaskList = pxOverflowDelayedTaskList;
		pxOverflowDelayedTaskList = pxTemporary;
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
			SCB->ICSR = SCB_ICSR_PENDSVSET_Msk;
		}
	}
	/* 重新开中断。 */
	__enable_irq();
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
		pxCurrentTCB->ulRunTimeCounter += ulDelta;
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
	const uint32_t ulPrimask = __get_PRIMASK();

	__disable_irq();
	*pullTotal = s_ullTotalCycles;
	*pullIdle = s_ullIdleCycles;
	__set_PRIMASK(ulPrimask);
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
uint32_t htGetIdleRunTimePercent(void)
{
	uint64_t ullTotal;
	uint64_t ullIdle;

#if (configGENERATE_RUN_TIME_STATS == 1)
	prvReadRunTimeCounters(&ullTotal, &ullIdle);
#else
	ullTotal = 0U;
	ullIdle = 0U;
#endif

	if (ullTotal == 0U) {
		/* 还没有样本：返回 0 而不是 100%，避免把"没有数据"误报成"一直空闲"。 */
		return 0U;
	}
	return (uint32_t)((ullIdle * 100U) / ullTotal);
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
	ulIdlePercent = (uint32_t)((ullIdle * 100U) / ullTotal);
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
	uint64_t ullTotal;
	uint64_t ullIdle;

#if (configGENERATE_RUN_TIME_STATS == 1)
	prvReadRunTimeCounters(&ullTotal, &ullIdle);
	(void)ullIdle;
#else
	ullTotal = 0U;
	ullIdle = 0U;
#endif

	return ullTotal;
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

	/* 遍历中 tick 处理可能改动链表，整段关中断；因此本函数只适合调试输出。 */
	htEnterCritical();
	ullTotal = htGetRunTimeCounter();
	/* 用「剩余项数」而不是「下一个是否为空」作为循环条件：pxAllocatedTasksList
	   是自身成环的双向链表，走到表尾后 htListGetNext() 会回到表头，靠空指针
	   判断会一直转下去。 */
	uxRemaining = htListGetCurrentNumberOfItems(&pxAllocatedTasksList);
	pxItem = htListGetHead(&pxAllocatedTasksList);
	while ((pxItem != NULL) && (uxRemaining > 0U)) {
		pxTask = (htTCB_t *)htListGetItemOwner(pxItem);
		pxItem = htListGetNext(pxItem);
		uxRemaining--;
		if (pxTask == NULL) {
			continue;
		}

		prvStatsPutName(&xWriter, pxTask->pcTaskName);
		prvStatsPutChar(&xWriter, ' ');
		prvStatsPutChar(&xWriter, prvStatsStateChar(htTaskGetState((TaskHandle_t)pxTask)));
		prvStatsPutChar(&xWriter, ' ');
		prvStatsPutUint(&xWriter, (uint32_t)pxTask->uxPriority);
		prvStatsPutChar(&xWriter, ' ');
		prvStatsPutUint(&xWriter, (uint32_t)htGetTaskStackHighWaterMark((TaskHandle_t)pxTask));
		prvStatsPutChar(&xWriter, ' ');
		if (ullTotal == 0U) {
			ulPercent = 0U;
		} else {
			ulPercent = (uint32_t)(((uint64_t)pxTask->ulRunTimeCounter * 100U) / ullTotal);
		}
		prvStatsPutUint(&xWriter, ulPercent);
		prvStatsPutString(&xWriter, "%\r\n");
	}
	htExitCritical();

	return xWriter.uxLength;
}
#endif /* configUSE_STATS_FORMATTING_FUNCTIONS */
