/**
 * @file htschededf.c
 * @brief 最早死期优先策略（Earliest Deadline First）。
 *
 * 每个任务可以登记一个绝对死期（单位 tick）。先按优先级选就绪任务的最高一档，
 * 再在这一档里挑死期最近的那个跑，因此优先级语义不变、EDF 只在同档内生效；
 * 这一档里没有登记死期的任务时退化为固定优先级轮转。
 * 当前任务跑过自己的死期时记一次漏期，同一死期只记一次。
 *
 * 死期比较统一用有符号差值 (int32_t)(a - b)，tick 回绕时仍然正确。
 */
#include "htschedpolicy.h"
#include "htscheduler.h"
#include "htlist.h"
#include "htkernel_internal.h"

#if (configUSE_SCHED_POLICY_EDF == 1)

/** 累计漏期次数，只为诊断服务。 */
static volatile uint32_t s_ulDeadlineMisses = 0U;

/**
 * @brief 新任务刚创建时的初始化：还没有登记死期。
 *
 * @note 新建任务的 TCB 在 prvInitialiseNewTask() 里已经整块清零，这里显式赋值是为了
 *       让"新任务 = 无死期"这条契约写在策略里，而不是依赖 TCB 的构造细节。
 */
static void prvEdfInitTask(htTCB_t *pxTask)
{
	if (pxTask != NULL) {
		pxTask->ulDeadline = HT_SCHED_NO_DEADLINE;
		pxTask->ucHasDeadline = 0U;
		pxTask->ucDeadlineMissed = 0U;
	}
}

static void prvEdfResetTask(htTCB_t *pxTask)
{
	if (pxTask != NULL) {
		pxTask->ucDeadlineMissed = 0U;
	}
}

static void prvEdfInitialise(void)
{
	UBaseType_t uxCount = htListGetCurrentNumberOfItems(&pxAllocatedTasksList);
	htListItem_t *pxItem = htListGetHead(&pxAllocatedTasksList);

	s_ulDeadlineMisses = 0U;

	for (UBaseType_t uxIndex = 0U; (uxIndex < uxCount) && (pxItem != NULL); uxIndex++) {
		prvEdfResetTask((htTCB_t *)htListGetItemOwner(pxItem));
		pxItem = htListGetNext(pxItem);
	}
}

/**
 * @brief 无副作用扫描最高优先级就绪链表中死期最近的任务。
 *
 * @return 找到的带死期任务；若该档无带死期任务返回 NULL。
 */
static htTCB_t *prvEdfFindBestDeadlineTask(void)
{
	UBaseType_t uxPriority = htSchedGetTopReadyPriority();
	htList_t *pxList = &(pxReadyTasksLists[uxPriority]);
	UBaseType_t uxCount = htListGetCurrentNumberOfItems(pxList);
	htListItem_t *pxItem = htListGetHead(pxList);
	htTCB_t *pxBest = NULL;

	for (UBaseType_t uxIndex = 0U; (uxIndex < uxCount) && (pxItem != NULL); uxIndex++) {
		htTCB_t *pxTask = (htTCB_t *)htListGetItemOwner(pxItem);
		if (pxTask != NULL && pxTask->ucHasDeadline != 0U) {
			if (pxBest == NULL || (int32_t)(pxTask->ulDeadline - pxBest->ulDeadline) < 0 ||
					(pxTask->ulDeadline == pxBest->ulDeadline && pxTask == pxCurrentTCB)) {
				pxBest = pxTask;
			}
		}
		pxItem = htListGetNext(pxItem);
	}

	return pxBest;
}

static htTCB_t *prvEdfSelectNext(void)
{
	htTCB_t *pxBest = prvEdfFindBestDeadlineTask();

	if (pxBest == NULL) {
		pxBest = htSchedPolicyFixedPriority.pxSelectNext();
	}

	return pxBest;
}

/**
 * @brief 无副作用抢占判断：绝不推进固定优先级轮转指针 pxIndex。
 */
static BaseType_t prvEdfShouldPreempt(htTCB_t *pxCurrent)
{
	BaseType_t xSwitchRequired = htFALSE;

#if (configUSE_PREEMPTION == 1)
	htTCB_t *pxBest = prvEdfFindBestDeadlineTask();
	if (pxBest != NULL) {
		if (pxBest != pxCurrent) {
			xSwitchRequired = htTRUE;
		}
	} else {
		/* 最高优先级档没有带死期任务时，走无副作用的固定优先级抢占判定，防止双倍推进 pxIndex。 */
		xSwitchRequired = htSchedPolicyFixedPriority.xShouldPreempt(pxCurrent);
	}
#else
	(void)pxCurrent;
#endif /* configUSE_PREEMPTION */

	return xSwitchRequired;
}

static void prvEdfCheckTaskMiss(htTCB_t *pxTask)
{
	if (pxTask != NULL && pxTask->ucHasDeadline != 0U && pxTask->ucDeadlineMissed == 0U &&
			(int32_t)(pxTask->ulDeadline - xTickCount) < 0) {
		pxTask->ucDeadlineMissed = 1U;
		s_ulDeadlineMisses++;
	}
}

/**
 * @brief 滴答记账：检查当前任务以及所有就绪态带死期任务是否错过死期。
 */
static void prvEdfTick(htTCB_t *pxCurrent)
{
	UBaseType_t uxCount = htListGetCurrentNumberOfItems(&pxAllocatedTasksList);
	htListItem_t *pxItem = htListGetHead(&pxAllocatedTasksList);

	for (UBaseType_t uxIndex = 0U; (uxIndex < uxCount) && (pxItem != NULL); uxIndex++) {
		htTCB_t *pxTask = (htTCB_t *)htListGetItemOwner(pxItem);
		if (pxTask != NULL && (pxTask == pxCurrent || pxTask->uxTaskState == HT_TASK_READY ||
									  pxTask->uxTaskState == HT_TASK_RUNNING)) {
			prvEdfCheckTaskMiss(pxTask);
		}
		pxItem = htListGetNext(pxItem);
	}

	/* 兜底：若调用方测试未将 pxCurrent 挂进 pxAllocatedTasksList，仍确保检查它。 */
	prvEdfCheckTaskMiss(pxCurrent);
}

static BaseType_t prvEdfUpdateDeadline(TaskHandle_t xTask, TickType_t xDeadline, uint8_t ucHasDeadline)
{
	BaseType_t xYieldRequired = htFALSE;

	if (xTask == NULL) {
		return htFAIL;
	}

	htTCB_t *pxTask = (htTCB_t *)xTask;

	htEnterCritical();
	pxTask->ulDeadline = xDeadline;
	pxTask->ucHasDeadline = ucHasDeadline;
	pxTask->ucDeadlineMissed = 0U;
	if (htSchedulerIsStarted() == htTRUE && htSchedGetPolicy() == &htSchedPolicyEdf &&
			prvEdfShouldPreempt(pxCurrentTCB) == htTRUE) {
		xYieldRequired = htTRUE;
	}
	htExitCritical();

	if (xYieldRequired == htTRUE) {
		htTaskYield();
	}
	return htPASS;
}

BaseType_t htSchedClearDeadline(TaskHandle_t xTask)
{
	return prvEdfUpdateDeadline(xTask, HT_SCHED_NO_DEADLINE, 0U);
}

BaseType_t htSchedSetDeadline(TaskHandle_t xTask, TickType_t xDeadline)
{
	return prvEdfUpdateDeadline(xTask, xDeadline, (xDeadline == HT_SCHED_NO_DEADLINE) ? 0U : 1U);
}

BaseType_t htSchedSetDeadlineAbsolute(TaskHandle_t xTask, TickType_t xDeadline)
{
	return prvEdfUpdateDeadline(xTask, xDeadline, 1U);
}

uint32_t htSchedEdfGetDeadlineMisses(void)
{
	return s_ulDeadlineMisses;
}

void htSchedEdfClearDeadlineMisses(void)
{
	htEnterCritical();
	s_ulDeadlineMisses = 0U;
	htExitCritical();
}

/** @brief 最早死期优先策略对象。 */
const htSchedPolicy_t htSchedPolicyEdf = {
		"edf", /* pcName */
		prvEdfInitialise, /* vInitialise */
		prvEdfInitTask, /* vTaskCreated */
		prvEdfTick, /* vTick */
		prvEdfSelectNext, /* pxSelectNext */
		prvEdfShouldPreempt, /* xShouldPreempt */
		NULL, /* vTaskEnqueued */
};

#endif /* configUSE_SCHED_POLICY_EDF */
