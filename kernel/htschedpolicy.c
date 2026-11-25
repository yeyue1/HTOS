/**
 * @file htschedpolicy.c
 * @brief 调度策略注册表 + 默认的固定优先级策略。
 *
 * 本文件是"策略"这一侧的入口：内核通过 htSchedGetPolicy() 拿到抽象接口，
 * 由具体策略对象提供选择与抢占判断。固定优先级策略就实现在本文件末尾，
 * 它复刻了内核原本的行为，因此默认配置下调度结果与之前完全一致。
 */
#include "htschedpolicy.h"
#include "htscheduler.h"
#include "htlist.h"
#include "htPort.h"
#include "htkernel_internal.h"

_Static_assert((configMAX_PRIORITIES > 0) && (configMAX_PRIORITIES <= 32),
		"O(1) ready priority bitmap requires 1 <= configMAX_PRIORITIES <= 32");

/** @brief 32 位就绪优先级位图：bit p 为 1 表示 pxReadyTasksLists[p] 非空。 */
static uint32_t s_ulReadyPrioritiesBitmap = 0U;

/** @brief 当前生效的策略对象；默认固定优先级，启动前可整体替换。 */
static const htSchedPolicy_t *pxSchedulerPolicy = &htSchedPolicyFixedPriority;

void htSchedRecordReadyPriority(UBaseType_t uxPriority)
{
	if (uxPriority < (UBaseType_t)configMAX_PRIORITIES) {
		s_ulReadyPrioritiesBitmap |= (1UL << (uint32_t)uxPriority);
	}
}

void htSchedClearReadyPriorityIfEmpty(UBaseType_t uxPriority)
{
	if (uxPriority < (UBaseType_t)configMAX_PRIORITIES &&
			htListGetCurrentNumberOfItems(&(pxReadyTasksLists[uxPriority])) == 0U) {
		s_ulReadyPrioritiesBitmap &= ~(1UL << (uint32_t)uxPriority);
	}
}

void htSchedResetReadyBitmap(void)
{
	s_ulReadyPrioritiesBitmap = 0U;
}

/**
 * @brief O(1) 查找最高非空就绪优先级（Cortex-M3 单周期 CLZ 指令）。
 */
UBaseType_t htSchedGetTopReadyPriority(void)
{
	if (s_ulReadyPrioritiesBitmap == 0U) {
		return 0U;
	}
	return (UBaseType_t)(31U - htPortCountLeadingZeros(s_ulReadyPrioritiesBitmap));
}

/**
 * @brief 固定优先级选择：最高优先级就绪表内按 pxIndex 轮转，取下一项。
 */
static htTCB_t *prvFixedSelectNext(void)
{
	htList_t *pxList = &(pxReadyTasksLists[htSchedGetTopReadyPriority()]);
	htListItem_t *const pxEnd = htListGetEndMarker(pxList);

	if (htListGetCurrentNumberOfItems(pxList) == 0U) {
		return NULL;
	}

	/* 从当前索引向后轮转，严禁经由 htListItem_t* 解引用精简哨兵。 */
	htListItem_t *pxNextTCB = (pxList->pxIndex == pxEnd) ? pxList->xListEnd.pxNext : htListGetNext(pxList->pxIndex);
	if (pxNextTCB == pxEnd) {
		pxNextTCB = pxList->xListEnd.pxNext;
	}

	/* 推进轮转索引，使同优先级任务下次从下一个开始。 */
	if (pxNextTCB != NULL && pxNextTCB != pxEnd) {
		pxList->pxIndex = pxNextTCB;

		/* 从链表项取回 TCB；属主非法时返回 NULL，让调用方保持原任务。 */
		return (htTCB_t *)htListGetItemOwner(pxNextTCB);
	}

	return NULL;
}

/**
 * @brief 固定优先级抢占判断：更高优先级就绪即切换；同优先级按时间片轮转。
 */
static BaseType_t prvFixedShouldPreempt(htTCB_t *pxCurrent)
{
	BaseType_t xSwitchRequired = htFALSE;

#if (configUSE_PREEMPTION == 1)
	if (pxCurrent != NULL) {
		const UBaseType_t uxCurrentPriority = pxCurrent->uxPriority;

		/* O(1) 位图查询：有更高优先级任务就绪即切换。 */
		if (htSchedGetTopReadyPriority() > uxCurrentPriority) {
			xSwitchRequired = htTRUE;
		}

#if (configUSE_TIME_SLICING == 1)
		/* 时间片轮转：同优先级存在兄弟任务时同样请求切换。 */
		if (xSwitchRequired == htFALSE && htListGetCurrentNumberOfItems(&(pxReadyTasksLists[uxCurrentPriority])) > 1U) {
			xSwitchRequired = htTRUE;
		}
#endif /* configUSE_TIME_SLICING */
	}
#else
	(void)pxCurrent;
#endif /* configUSE_PREEMPTION */

	return xSwitchRequired;
}

/** @brief 固定优先级策略对象：唯一的内置默认策略。 */
const htSchedPolicy_t htSchedPolicyFixedPriority = {
		"fixed-priority", /* pcName */
		NULL, /* vInitialise：无策略私有状态 */
		NULL, /* vTaskCreated：无私有字段 */
		NULL, /* vTick：时间片由抢占判断负责，无需记账 */
		prvFixedSelectNext, /* pxSelectNext */
		prvFixedShouldPreempt, /* xShouldPreempt */
		NULL, /* vTaskEnqueued：无需唤醒记账 */
};

/**
 * @brief 判断策略对象是否满足内核的最低要求：两个必需操作都必须存在。
 */
static BaseType_t prvPolicyIsUsable(const htSchedPolicy_t *pxPolicy)
{
	if (pxPolicy == NULL) {
		return htFALSE;
	}

	return (pxPolicy->pxSelectNext != NULL && pxPolicy->xShouldPreempt != NULL) ? htTRUE : htFALSE;
}

/**
 * @brief 切换调度策略：启动前登记，启动后热切换。
 *
 * @note 三步走，顺序不可颠倒：
 *       1) 换指针——此后 tick 记账与任务创建回调都落到新策略上；
 *       2) 调 vInitialise()——让新策略接管已经存在的任务；
 *       3) 已启动则 htTaskYield()——立刻按新规则重选一次，而不是等下一个滴答。
 *       前两步在临界区内完成，因此滴答中断永远看不到"指针已换、状态未接管"的中间态。
 */
BaseType_t htSchedSetPolicy(const htSchedPolicy_t *pxPolicy)
{
	/* 策略必须提供两个必需操作，否则内核无法工作。 */
	if (prvPolicyIsUsable(pxPolicy) == htFALSE) {
		return htFAIL;
	}

	/* 进来先关中断：读当前策略、换指针、重置状态必须是一个原子动作。 */
	htEnterCritical();

	/* 重复登记同一个对象是空操作：不要因此清掉策略已经积累的运行状态。 */
	if (pxSchedulerPolicy == pxPolicy) {
		htExitCritical();
		return htPASS;
	}

	/* 是否已启动决定切换后要不要主动让出 CPU，必须在换指针前读。 */
	BaseType_t xSchedulerRunning = htSchedulerIsStarted();

	/* 第一步：换指针。 */
	pxSchedulerPolicy = pxPolicy;

	/* 第二步：让新策略按自己的规则接管已有任务。 */
	if (pxPolicy->vInitialise != NULL) {
		pxPolicy->vInitialise();
	}

	htExitCritical();

	/* 第三步：运行中切换时立刻重选，否则新策略要等到下一个滴答才生效。 */
	if (xSchedulerRunning != htFALSE) {
		htTaskYield();
	}

	return htPASS;
}

const htSchedPolicy_t *htSchedGetPolicy(void)
{
	return pxSchedulerPolicy;
}

void htSchedNotifyTaskCreated(htTCB_t *pxTask)
{
	if (pxSchedulerPolicy->vTaskCreated != NULL) {
		pxSchedulerPolicy->vTaskCreated(pxTask);
	}
}

void htSchedNotifyTick(htTCB_t *pxCurrent)
{
	if (pxSchedulerPolicy->vTick != NULL) {
		pxSchedulerPolicy->vTick(pxCurrent);
	}
}

void htSchedNotifyTaskEnqueued(htTCB_t *pxTask)
{
	if (pxTask != NULL && pxSchedulerPolicy->vTaskEnqueued != NULL) {
		pxSchedulerPolicy->vTaskEnqueued(pxTask);
	}
}
