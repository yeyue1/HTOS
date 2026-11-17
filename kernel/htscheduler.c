#include "htscheduler.h"
#include "httask.h"
#include "htlist.h"
#include "stm32f1xx_hal.h"
#include <stddef.h> // 提供NULL定义
#include <stdio.h> // 提供printf定义
#include "core_cm3.h" // 添加此头文件以提供NVIC函数和中断相关定义

/* 外部函数声明 */
extern void prvInitialiseTaskLists(void);
extern void htTaskYield(void);

/* 外部变量声明 */
extern htTCB_t *pxCurrentTCB;
extern htList_t pxReadyTasksLists[];
extern htList_t xDelayedTaskList1;
extern htList_t xDelayedTaskList2;
extern htList_t *pxDelayedTaskList;
extern htList_t *pxOverflowDelayedTaskList;
extern TickType_t xTickCount;

/* 调度器变量 */
static htSchedulerState_t xSchedulerState = HT_SCHEDULER_NOT_STARTED;
static UBaseType_t uxSchedulerSuspended = 0; /* 调度器挂起计数 */
static BaseType_t xYieldPending = htFALSE; /* 任务切换挂起标志 */

/* 临界区嵌套计数器 */
volatile UBaseType_t uxCriticalNesting = 0U;

BaseType_t htPortStartScheduler(void)
{
	/* 重置临界区嵌套计数器 */
	uxCriticalNesting = 0;

	/* 启动第一个任务 - 这个函数通常不会返回 */
	htPortStartFirstTask();

	/* 如果到达这里，说明启动失败 */
	return htFAIL;
}

/**
 * 初始化调度器
 */
void htSchedulerInit(void)
{
	/* 初始化任务列表 */
	prvInitialiseTaskLists();

	/* 将调度器设为未启动状态 */
	xSchedulerState = HT_SCHEDULER_NOT_STARTED;
	uxSchedulerSuspended = 0;
	xYieldPending = htFALSE;
	uxCriticalNesting = 0;
}

/**
 * 启动调度器
 */
void htStartScheduler(void)
{
	// 关键安全检查
	if (pxCurrentTCB == NULL) {
		printf("FATAL: No tasks available to run!\r\n");
		return;
	}

	// 确保延时列表被初始化
	htListInit(&xDelayedTaskList1);
	htListInit(&xDelayedTaskList2);
	pxDelayedTaskList = &xDelayedTaskList1;
	pxOverflowDelayedTaskList = &xDelayedTaskList2;

	// 创建空闲任务
	if (htprvCreateIdleTask() != htPASS) {
		printf("FATAL: Failed to create idle task!\r\n");
		return;
	}

	// 设置关键中断优先级
	NVIC_SetPriority(PendSV_IRQn, 0xFF); // 最低优先级
	NVIC_SetPriority(SysTick_IRQn, 0x70); // 较高优先级

	// 确保所有中断已正确配置
	__enable_irq();
	__DSB();
	__ISB();

	// 更新系统状态
	xSchedulerState = HT_SCHEDULER_RUNNING;

	/* 必须先按优先级选出真正的第一个任务：htTaskCreate() 只在创建第一个任务时
	   顺便把 pxCurrentTCB 赋成它，之后创建的高优先级任务不会回填。若此时直接启动，
	   实际运行的是"最先创建的任务"，却与 PendSV 里保存 SP 用的 pxCurrentTCB
	   不是同一个对象，会把这个任务的栈指针写进另一个 TCB，造成栈交叉污染
	   （实板现象：HardFault + CFSR=INVPC）。 */
	htTaskSwitchContext();

	// 启动第一个任务
	vPortStartFirstTask();
}

/**
 * 暂停调度器
 */
void htSuspendScheduler(void)
{
	/* 进入临界区 */
	htEnterCritical();

	/* 设置调度器状态为暂停 */
	if (xSchedulerState == HT_SCHEDULER_RUNNING) {
		xSchedulerState = HT_SCHEDULER_SUSPENDED;
	}

	/* 退出临界区 */
	htExitCritical();
}

/**
 * 恢复调度器
 */
void htResumeScheduler(void)
{
	/* 进入临界区 */
	htEnterCritical();

	/* 恢复调度器状态 */
	if (xSchedulerState == HT_SCHEDULER_SUSPENDED) {
		xSchedulerState = HT_SCHEDULER_RUNNING;

		/* 检查是否需要进行上下文切换 */
		/* 在实际实现中，可能需要调用函数检查是否需要调度 */
	}

	/* 退出临界区 */
	htExitCritical();
}

/**
 * 系统滴答处理函数
 */
void htSchedulerTickHandler(void)
{
	htTaskTickInc();
}

/**
 * 内核进入临界区
 */
void htEnterCritical(void)
{
	/* 增加嵌套计数 */
	uxCriticalNesting++;

	/* 禁用中断 - 实际实现将使用Cortex-M的PRIMASK或BASEPRI寄存器 */
	/* 这里简化处理，实际需要根据目标MCU具体实现 */
	__asm volatile("cpsid i");
}

/**
 * 退出临界区
 * 在Cortex-M上，根据嵌套计数可能重新启用中断
 */
void htExitCritical(void)
{
	/* 确保嵌套计数有效 */
	if (uxCriticalNesting > 0) {
		uxCriticalNesting--;

		/* 如果计数为0，重新启用中断 */
		if (uxCriticalNesting == 0) {
			__asm volatile("cpsie i");
		}
	}
}

/**
 * 上下文切换函数 - 增强健壮性
 * 选择下一个要运行的任务
 */
void htTaskSwitchContext(void)
{
	/* 检查调度器是否被挂起 */
	if (uxSchedulerSuspended != 0) {
		/* 调度器被挂起，设置挂起标志 */
		xYieldPending = htTRUE;
		return;
	}

	/* 重置挂起标志 */
	xYieldPending = htFALSE;

	/* 验证当前TCB有效 */
	if (pxCurrentTCB == NULL) {
		return;
	}

	/* 选择最高优先级的就绪任务 */
	UBaseType_t uxTopPriority = 0;
	uint32_t safetyCounter = 0;
	const uint32_t MAX_ITERATIONS = 32;

	/* 找到最高优先级的非空就绪列表 */
	for (uxTopPriority = configMAX_PRIORITIES - 1; uxTopPriority > 0 && safetyCounter < MAX_ITERATIONS;
			uxTopPriority--) {
		safetyCounter++;
		if (htListGetCurrentNumberOfItems(&(pxReadyTasksLists[uxTopPriority])) > 0) {
			break;
		}
	}

	/* 获取就绪列表 */
	htList_t *pxList = &(pxReadyTasksLists[uxTopPriority]);

	/* 从当前索引向后轮转，跳过列表尾哨兵。 */
	htListItem_t *pxNextTCB = htListGetNext(pxList->pxIndex);
	if (pxNextTCB == &(pxList->xListEnd)) {
		pxNextTCB = htListGetNext(pxNextTCB);
	}

	/* 如果找到就绪任务 */
	if (pxNextTCB != NULL) {
		/* 将列表索引移动到下一个项目 */
		pxList->pxIndex = pxNextTCB;

		/* 获取任务控制块并验证 */
		htTCB_t *pxNewTCB = (htTCB_t *)htListGetItemOwner(pxNextTCB);
		if (pxNewTCB != NULL) {
			/* 更新任务状态并设置当前TCB */
			pxNewTCB->uxTaskState = HT_TASK_RUNNING;
			pxCurrentTCB = pxNewTCB;
		}
	}
}

/**
 * 挂起调度器
 *
 * @return 当前挂起嵌套计数
 */
UBaseType_t htSchedulerSuspend(void)
{
	/* 增加挂起计数 */
	uxSchedulerSuspended++;

	return uxSchedulerSuspended;
}

/**
 * 恢复调度器
 */
void htSchedulerResume(void)
{
	/* 检查调度器是否被挂起 */
	if (uxSchedulerSuspended > 0) {
		/* 减少挂起计数 */
		uxSchedulerSuspended--;

		/* 如果完全恢复且有挂起的上下文切换请求 */
		if (uxSchedulerSuspended == 0 && xYieldPending == htTRUE) {
			/* 触发上下文切换 */
			htTaskYield();
		}
	}
}

/**
 * 检查是否需要进行任务调度
 *
 * @return 如果需要任务切换则返回htTRUE，否则返回htFALSE
 */
BaseType_t htSchedulerNeedsSwitch(void)
{
	BaseType_t xSwitchRequired = htFALSE;

	/*
	 * 调度器尚未运行之前绝不请求上下文切换。
	 *
	 * board_clock_init() 里的 HAL_Init() 一开机就使能了 SysTick，而第一个任务
	 * 创建完成后 pxCurrentTCB 已经非空，二者叠加会在"任务已建好、第一个任务
	 * 上下文还没建立"之间开出一个窗口：此时若置位 PendSV，PendSV 会在
	 * PSP 仍为 0 的情况下执行保存动作，把 r4-r11 写到 0xFFFFFFE0，直接进
	 * HardFault。滴答计数照常累加，只是暂不请求切换。
	 */
	if (xSchedulerState != HT_SCHEDULER_RUNNING) {
		return htFALSE;
	}

	/* 检查是否有高优先级任务就绪 */
	if (pxCurrentTCB != NULL) {
		UBaseType_t uxCurrentPriority = pxCurrentTCB->uxPriority;

		/* 检查是否有更高优先级的任务就绪 */
		for (UBaseType_t uxPriority = configMAX_PRIORITIES - 1; uxPriority > uxCurrentPriority; uxPriority--) {
			if (htListGetCurrentNumberOfItems(&(pxReadyTasksLists[uxPriority])) > 0) {
				xSwitchRequired = htTRUE;
				break;
			}
		}

#if (configUSE_TIME_SLICING == 1)
		/*
		 * 时间片轮转：同优先级存在兄弟任务时同样要请求切换。
		 *
		 * 只判断"更高优先级"会让同优先级的多任务退化为严格先来先服务——先创建的
		 * 那个除非自己阻塞，否则后创建者永远拿不到 CPU。htTaskSwitchContext()
		 * 本来就会推进 pxList->pxIndex 完成列表内轮转，缺的只是这个触发条件。
		 */
		if (xSwitchRequired == htFALSE && htListGetCurrentNumberOfItems(&(pxReadyTasksLists[uxCurrentPriority])) > 1U) {
			xSwitchRequired = htTRUE;
		}
#endif
	}

	return xSwitchRequired;
}
