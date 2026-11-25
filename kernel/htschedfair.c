/**
 * @file htschedfair.c
 * @brief 公平分享策略（方案 A）：优先级语义不变，同一优先级内按权重分配 CPU。
 *
 * 算法是 Stride Scheduling —— Linux CFS 的整数近似：
 *   - 每个任务有一个步长 ulStride = 1048576 / 权重，权重越大步长越小；
 *   - 任务每跑一个 tick，虚拟时间 ulVruntime 加上自己的步长；
 *   - 同一优先级内永远选虚拟时间最小的任务。
 * 于是权重 2048 的任务拿到的 CPU 是权重 1024 的两倍。
 *
 * 全程只用 32 位整数加法与比较：没有除法（除法只在设权重时做一次），
 * 没有 64 位运算，适合没有 FPU 与硬件除法器的 Cortex-M3。
 * 虚拟时间按 tick 计，步长 1024 时约 70 分钟回绕一次；比较统一用有符号
 * 差值 (int32_t)(a - b) < 0，可以自然跨越回绕点。
 */
#include "htschedpolicy.h"
#include "htscheduler.h"
#include "htlist.h"
#include "htkernel_internal.h"

#if (configUSE_SCHED_POLICY_FAIR_SHARE == 1)

/** 步长计算基数：基数 / 权重 = 步长；权重 1024 时步长正好 1024。 */
#define HT_FAIR_STRIDE_SCALE 1048576U
/** 默认步长，即权重 HT_SCHED_DEFAULT_WEIGHT 对应的步长。 */
#define HT_FAIR_DEFAULT_STRIDE (HT_FAIR_STRIDE_SCALE / HT_SCHED_DEFAULT_WEIGHT)
/**
 * 唤醒保护窗口，单位虚拟时间。
 *
 * 任务睡了很久再醒来时虚拟时间会远远落后，若直接参与比较会独占 CPU 直到
 * 追平。这里把它的虚拟时间抬到"当前最小值 - 本窗口"，最多只补回半个窗口，
 * 对应 Linux CFS 的 place_entity()。
 */
#define HT_FAIR_WAKE_MARGIN 4096U

/** 各优先级独立的虚拟时间单调下界，杜绝高优先级任务污染低优先级公平份额。 */
static uint32_t s_aulMinVruntime[configMAX_PRIORITIES];
static uint8_t s_aucMinVruntimeValid[configMAX_PRIORITIES];

static void prvFairAdvanceFloor(UBaseType_t uxPriority, uint32_t ulCandidate)
{
	if (uxPriority >= configMAX_PRIORITIES) {
		return;
	}
	if (s_aucMinVruntimeValid[uxPriority] == 0U) {
		s_aulMinVruntime[uxPriority] = ulCandidate;
		s_aucMinVruntimeValid[uxPriority] = 1U;
	} else if ((int32_t)(ulCandidate - s_aulMinVruntime[uxPriority]) > 0) {
		s_aulMinVruntime[uxPriority] = ulCandidate;
	}
}

static void prvFairClampWokenTask(htTCB_t *pxTask)
{
	if (pxTask == NULL || pxTask->uxPriority >= configMAX_PRIORITIES) {
		return;
	}
	if (s_aucMinVruntimeValid[pxTask->uxPriority] == 0U) {
		return;
	}
	const uint32_t ulFloor = s_aulMinVruntime[pxTask->uxPriority];
	if ((int32_t)(pxTask->ulVruntime - ulFloor) < -(int32_t)HT_FAIR_WAKE_MARGIN) {
		pxTask->ulVruntime = ulFloor - HT_FAIR_WAKE_MARGIN;
	}
}

static void prvFairInitTask(htTCB_t *pxTask)
{
	if (pxTask != NULL) {
		if (pxTask->ulStride == 0U) {
			pxTask->ulStride = HT_FAIR_DEFAULT_STRIDE;
		}
		pxTask->ulVruntime = 0U;
	}
}

static void prvFairInitialise(void)
{
	UBaseType_t uxCount = htListGetCurrentNumberOfItems(&pxAllocatedTasksList);
	htListItem_t *pxItem = htListGetHead(&pxAllocatedTasksList);

	for (UBaseType_t uxPriority = 0U; uxPriority < (UBaseType_t)configMAX_PRIORITIES; uxPriority++) {
		s_aulMinVruntime[uxPriority] = 0U;
		s_aucMinVruntimeValid[uxPriority] = 0U;
	}

	for (UBaseType_t uxIndex = 0U; (uxIndex < uxCount) && (pxItem != NULL); uxIndex++) {
		prvFairInitTask((htTCB_t *)htListGetItemOwner(pxItem));
		pxItem = htListGetNext(pxItem);
	}
}

static void prvFairTick(htTCB_t *pxCurrent)
{
	if (pxCurrent != NULL) {
		/* 防御：策略切换前创建、还没走到 vTaskCreated 的任务。 */
		if (pxCurrent->ulStride == 0U) {
			pxCurrent->ulStride = HT_FAIR_DEFAULT_STRIDE;
		}
		pxCurrent->ulVruntime += pxCurrent->ulStride;

		/* 没有同优先级竞争者时单调抬升本优先级下界，不影响其他优先级。 */
		if (pxCurrent->uxPriority < configMAX_PRIORITIES &&
				htListGetCurrentNumberOfItems(&(pxReadyTasksLists[pxCurrent->uxPriority])) <= 1U) {
			prvFairAdvanceFloor(pxCurrent->uxPriority, pxCurrent->ulVruntime);
		}
	}
}

static htTCB_t *prvFairSelectNext(void)
{
	const UBaseType_t uxTopPriority = htSchedGetTopReadyPriority();
	htList_t *pxList = &(pxReadyTasksLists[uxTopPriority]);
	UBaseType_t uxCount = htListGetCurrentNumberOfItems(pxList);
	htListItem_t *pxItem = htListGetHead(pxList);
	uint32_t ulMinVruntime = 0U;
	BaseType_t xFoundAny = htFALSE;

	/* 第一遍：用有符号差值求当前最小虚拟时间，跨越 32 位回绕点依然正确。 */
	for (UBaseType_t uxIndex = 0U; (uxIndex < uxCount) && (pxItem != NULL); uxIndex++) {
		htTCB_t *pxTask = (htTCB_t *)htListGetItemOwner(pxItem);
		if (pxTask != NULL) {
			if (xFoundAny == htFALSE || (int32_t)(pxTask->ulVruntime - ulMinVruntime) < 0) {
				ulMinVruntime = pxTask->ulVruntime;
				xFoundAny = htTRUE;
			}
		}
		pxItem = htListGetNext(pxItem);
	}
	if (xFoundAny == htFALSE) {
		return NULL;
	}

	/* 本优先级下界单调不减。 */
	prvFairAdvanceFloor(uxTopPriority, ulMinVruntime);

	/* 第二遍：唤醒保护 + 选最小。 */
	htTCB_t *pxBest = NULL;
	pxItem = htListGetHead(pxList);
	for (UBaseType_t uxIndex = 0U; (uxIndex < uxCount) && (pxItem != NULL); uxIndex++) {
		htTCB_t *pxTask = (htTCB_t *)htListGetItemOwner(pxItem);
		if (pxTask != NULL) {
			prvFairClampWokenTask(pxTask);
			if (pxBest == NULL || (int32_t)(pxTask->ulVruntime - pxBest->ulVruntime) < 0) {
				pxBest = pxTask;
			}
		}
		pxItem = htListGetNext(pxItem);
	}

	return pxBest;
}

/**
 * @brief 抢占判断：更高优先级就绪即切换；同优先级里有人虚拟时间更小也切换。
 */
static BaseType_t prvFairShouldPreempt(htTCB_t *pxCurrent)
{
	BaseType_t xSwitchRequired = htFALSE;

#if (configUSE_PREEMPTION == 1)
	if (pxCurrent != NULL) {
		const UBaseType_t uxCurrentPriority = pxCurrent->uxPriority;

		/* O(1) 位图查询：高优先级照旧绝对抢占，优先级语义不变。 */
		if (htSchedGetTopReadyPriority() > uxCurrentPriority) {
			xSwitchRequired = htTRUE;
		}

#if (configUSE_TIME_SLICING == 1)
		/* 同优先级：只要有兄弟的虚拟时间更小，就该让它上。 */
		if (xSwitchRequired == htFALSE) {
			htList_t *pxList = &(pxReadyTasksLists[uxCurrentPriority]);
			UBaseType_t uxCount = htListGetCurrentNumberOfItems(pxList);
			htListItem_t *pxItem = htListGetHead(pxList);

			for (UBaseType_t uxIndex = 0U; (uxIndex < uxCount) && (pxItem != NULL); uxIndex++) {
				htTCB_t *pxTask = (htTCB_t *)htListGetItemOwner(pxItem);
				if (pxTask != NULL && pxTask != pxCurrent &&
						(int32_t)(pxTask->ulVruntime - pxCurrent->ulVruntime) < 0) {
					xSwitchRequired = htTRUE;
					break;
				}
				pxItem = htListGetNext(pxItem);
			}
		}
#endif /* configUSE_TIME_SLICING */
	}
#else
	(void)pxCurrent;
#endif /* configUSE_PREEMPTION */

	return xSwitchRequired;
}

/**
 * @brief 设置任务权重。
 */
BaseType_t htSchedSetWeight(TaskHandle_t xTask, UBaseType_t uxWeight)
{
	if (xTask == NULL || uxWeight == 0U || uxWeight > HT_SCHED_MAX_WEIGHT) {
		return htFAIL;
	}

	htTCB_t *pxTask = (htTCB_t *)xTask;

	htEnterCritical();
	pxTask->ulStride = HT_FAIR_STRIDE_SCALE / uxWeight;
	htExitCritical();

	return htPASS;
}

/** @brief 公平分享策略对象。 */
const htSchedPolicy_t htSchedPolicyFairShare = {
		"fair-share", /* pcName */
		prvFairInitialise, /* vInitialise */
		prvFairInitTask, /* vTaskCreated */
		prvFairTick, /* vTick */
		prvFairSelectNext, /* pxSelectNext */
		prvFairShouldPreempt, /* xShouldPreempt */
		prvFairClampWokenTask, /* vTaskEnqueued */
};

#endif /* configUSE_SCHED_POLICY_FAIR_SHARE */
