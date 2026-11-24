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

/** 各优先级的就绪链表，下标即优先级。 */
extern htList_t pxReadyTasksLists[];
/** 当前运行的任务。 */
extern htTCB_t *pxCurrentTCB;

/** @brief 当前生效的策略对象；默认固定优先级，启动前可整体替换。 */
static const htSchedPolicy_t *pxSchedulerPolicy = &htSchedPolicyFixedPriority;

/**
 * @brief 找最高优先级的非空就绪链表。
 *
 * @note 循环上限 32 只是防御：链表被踩坏时不至于死循环在调度器里。
 */
UBaseType_t htSchedGetTopReadyPriority(void)
{
	UBaseType_t uxTopPriority = 0U;
	uint32_t ulSafetyCounter = 0U;

	for (uxTopPriority = configMAX_PRIORITIES - 1U; uxTopPriority > 0U && ulSafetyCounter < 32U; uxTopPriority--) {
		ulSafetyCounter++;
		if (htListGetCurrentNumberOfItems(&(pxReadyTasksLists[uxTopPriority])) > 0U) {
			break;
		}
	}

	return uxTopPriority;
}

/**
 * @brief 固定优先级选择：最高优先级就绪表内按 pxIndex 轮转，取下一项。
 */
static htTCB_t *prvFixedSelectNext(void)
{
	htList_t *pxList = &(pxReadyTasksLists[htSchedGetTopReadyPriority()]);

	/* 从当前索引向后轮转，跳过列表尾哨兵。 */
	htListItem_t *pxNextTCB = htListGetNext(pxList->pxIndex);
	if (pxNextTCB == &(pxList->xListEnd)) {
		pxNextTCB = htListGetNext(pxNextTCB);
	}

	/* 推进轮转索引，使同优先级任务下次从下一个开始。 */
	if (pxNextTCB != NULL) {
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
		UBaseType_t uxCurrentPriority = pxCurrent->uxPriority;

		/* 有更高优先级任务就绪就需要切换。 */
		for (UBaseType_t uxPriority = configMAX_PRIORITIES - 1U; uxPriority > uxCurrentPriority; uxPriority--) {
			if (htListGetCurrentNumberOfItems(&(pxReadyTasksLists[uxPriority])) > 0U) {
				xSwitchRequired = htTRUE;
				break;
			}
		}

#if (configUSE_TIME_SLICING == 1)
		/*
		 * 时间片轮转：同优先级存在兄弟任务时同样要请求切换。
		 *
		 * 只判断"更高优先级"会让同优先级的多任务退化为严格先来先服务——先创建的
		 * 那个除非自己阻塞，否则后创建者永远拿不到 CPU。
		 */
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
};

/**
 * @brief 切换调度策略，只允许在调度器启动之前调用。
 */
BaseType_t htSchedSetPolicy(const htSchedPolicy_t *pxPolicy)
{
	/* 策略必须提供两个必需操作，否则内核无法工作。 */
	if (pxPolicy == NULL || pxPolicy->pxSelectNext == NULL || pxPolicy->xShouldPreempt == NULL) {
		return htFAIL;
	}

	/* 启动之后换策略相当于在飞行中换发动机，直接拒绝。 */
	if (htSchedulerIsStarted() != htFALSE) {
		return htFAIL;
	}

	pxSchedulerPolicy = pxPolicy;

	/* 让新策略清空自己的私有状态，并把已有任务纳入管理。 */
	if (pxPolicy->vInitialise != NULL) {
		pxPolicy->vInitialise();
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
