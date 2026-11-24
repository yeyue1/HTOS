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

#if (configUSE_SCHED_POLICY_EDF == 1)

/** 各优先级的就绪链表。 */
extern htList_t pxReadyTasksLists[];
/** 当前运行的任务。 */
extern htTCB_t *pxCurrentTCB;
/** 系统滴答计数。 */
extern TickType_t xTickCount;

/** 累计漏期次数，只为诊断服务。 */
static volatile uint32_t s_ulDeadlineMisses = 0U;

/**
 * @brief 把任务恢复为"没有死期"的状态。
 */
static void prvEdfInitTask(htTCB_t *pxTask)
{
	if (pxTask != NULL) {
		pxTask->ulDeadline = HT_SCHED_NO_DEADLINE;
		pxTask->ucDeadlineMissed = 0U;
	}
}

/**
 * @brief 策略初始化：清空漏期计数，并把已有任务都标成无死期。
 */
static void prvEdfInitialise(void)
{
	UBaseType_t uxCount = htListGetCurrentNumberOfItems(&pxAllocatedTasksList);
	htListItem_t *pxItem = htListGetHead(&pxAllocatedTasksList);

	s_ulDeadlineMisses = 0U;

	for (UBaseType_t uxIndex = 0U; (uxIndex < uxCount) && (pxItem != NULL); uxIndex++) {
		prvEdfInitTask((htTCB_t *)htListGetItemOwner(pxItem));
		pxItem = htListGetNext(pxItem);
	}
}

/**
 * @brief 选择死期最近的就绪任务。
 *
 * @note 只考察最高优先级的就绪链表：优先级仍是硬边界，EDF 只在同一档内排序。
 *       这一档里没有登记死期的任务时，退化为固定优先级选择（合成复用默认策略），
 *       保证空闲任务等普通任务仍然能被选中。
 */
static htTCB_t *prvEdfSelectNext(void)
{
	UBaseType_t uxPriority = htSchedGetTopReadyPriority();
	htList_t *pxList = &(pxReadyTasksLists[uxPriority]);
	UBaseType_t uxCount = htListGetCurrentNumberOfItems(pxList);
	htListItem_t *pxItem = htListGetHead(pxList);
	htTCB_t *pxBest = NULL;

	for (UBaseType_t uxIndex = 0U; (uxIndex < uxCount) && (pxItem != NULL); uxIndex++) {
		htTCB_t *pxTask = (htTCB_t *)htListGetItemOwner(pxItem);
		if (pxTask != NULL && pxTask->ulDeadline != HT_SCHED_NO_DEADLINE) {
			if (pxBest == NULL || (int32_t)(pxTask->ulDeadline - pxBest->ulDeadline) < 0) {
				pxBest = pxTask;
			} else if (pxTask->ulDeadline == pxBest->ulDeadline && pxTask == pxCurrentTCB) {
				/* 死期相同优先保持当前任务，避免两个同死期任务每 tick 互踢。 */
				pxBest = pxTask;
			}
		}
		pxItem = htListGetNext(pxItem);
	}

	if (pxBest == NULL) {
		pxBest = htSchedPolicyFixedPriority.pxSelectNext();
	}

	return pxBest;
}

/**
 * @brief 抢占判断：死期最近的就绪任务不是当前任务时切换。
 */
static BaseType_t prvEdfShouldPreempt(htTCB_t *pxCurrent)
{
	BaseType_t xSwitchRequired = htFALSE;

#if (configUSE_PREEMPTION == 1)
	htTCB_t *pxBest = prvEdfSelectNext();
	if (pxBest != NULL && pxBest != pxCurrent) {
		xSwitchRequired = htTRUE;
	}
#else
	(void)pxCurrent;
#endif /* configUSE_PREEMPTION */

	return xSwitchRequired;
}

/**
 * @brief 滴答记账：检查当前任务是否已经跑过自己的死期。
 */
static void prvEdfTick(htTCB_t *pxCurrent)
{
	if (pxCurrent == NULL || pxCurrent->ulDeadline == HT_SCHED_NO_DEADLINE) {
		return;
	}

	if ((int32_t)(pxCurrent->ulDeadline - xTickCount) < 0 && pxCurrent->ucDeadlineMissed == 0U) {
		pxCurrent->ucDeadlineMissed = 1U;
		s_ulDeadlineMisses++;
	}
}

/**
 * @brief 设置任务死期；传 HT_SCHED_NO_DEADLINE 表示取消。
 */
BaseType_t htSchedSetDeadline(TaskHandle_t xTask, TickType_t xDeadline)
{
	if (xTask == NULL) {
		return htFAIL;
	}

	htTCB_t *pxTask = (htTCB_t *)xTask;

	htEnterCritical();
	pxTask->ulDeadline = xDeadline;
	pxTask->ucDeadlineMissed = 0U;
	htExitCritical();

	return htPASS;
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
};

#endif /* configUSE_SCHED_POLICY_EDF */
