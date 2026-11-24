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

#if (configUSE_SCHED_POLICY_FAIR_SHARE == 1)

/** 各优先级的就绪链表。 */
extern htList_t pxReadyTasksLists[];
/** 当前运行的任务。 */
extern htTCB_t *pxCurrentTCB;

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

/** 虚拟时间下界（单调不减）：唤醒保护以它为基准，对应 Linux 的 min_vruntime。 */
static uint32_t s_ulMinVruntime = 0U;

/**
 * @brief 把任务恢复到"刚加入公平队列"的状态。
 *
 * @note 步长由用户通过 htSchedSetWeight() 设置，是任务参数，热切换时必须保留；
 *       只有从未设置过（步长为 0）的任务才落回标准份额。虚拟时间是策略的记账
 *       状态，每次重新接管都清零，避免带着上一轮的差距进入新一轮比较。
 */
static void prvFairInitTask(htTCB_t *pxTask)
{
	if (pxTask != NULL) {
		if (pxTask->ulStride == 0U) {
			pxTask->ulStride = HT_FAIR_DEFAULT_STRIDE;
		}
		pxTask->ulVruntime = 0U;
	}
}

/**
 * @brief 策略初始化：把当前已存在的任务全部纳入公平队列。
 */
static void prvFairInitialise(void)
{
	UBaseType_t uxCount = htListGetCurrentNumberOfItems(&pxAllocatedTasksList);
	htListItem_t *pxItem = htListGetHead(&pxAllocatedTasksList);

	s_ulMinVruntime = 0U;

	for (UBaseType_t uxIndex = 0U; (uxIndex < uxCount) && (pxItem != NULL); uxIndex++) {
		prvFairInitTask((htTCB_t *)htListGetItemOwner(pxItem));
		pxItem = htListGetNext(pxItem);
	}
}

/**
 * @brief 滴答记账：正在运行的任务虚拟时间加一个步长。
 */
static void prvFairTick(htTCB_t *pxCurrent)
{
	if (pxCurrent != NULL) {
		/* 防御：策略切换前创建、还没走到 vTaskCreated 的任务。 */
		if (pxCurrent->ulStride == 0U) {
			pxCurrent->ulStride = HT_FAIR_DEFAULT_STRIDE;
		}
		pxCurrent->ulVruntime += pxCurrent->ulStride;

		/* 没有同优先级竞争者时，把下界抬到当前值，供唤醒保护使用。 */
		if (htListGetCurrentNumberOfItems(&(pxReadyTasksLists[pxCurrent->uxPriority])) <= 1U) {
			s_ulMinVruntime = pxCurrent->ulVruntime;
		}
	}
}

/**
 * @brief 从最高优先级就绪表里挑虚拟时间最小的任务。
 *
 * @note 先扫一遍求最小值，把落后过多的任务拉回窗口内，再扫一遍选最小。
 *       同优先级任务数很少（个位数），两次线性扫描的代价可以忽略。
 */
static htTCB_t *prvFairSelectNext(void)
{
	htList_t *pxList = &(pxReadyTasksLists[htSchedGetTopReadyPriority()]);
	UBaseType_t uxCount = htListGetCurrentNumberOfItems(pxList);
	htListItem_t *pxItem = htListGetHead(pxList);
	uint32_t ulMinVruntime = UINT32_MAX;

	/* 第一遍：求当前最小虚拟时间。 */
	for (UBaseType_t uxIndex = 0U; (uxIndex < uxCount) && (pxItem != NULL); uxIndex++) {
		htTCB_t *pxTask = (htTCB_t *)htListGetItemOwner(pxItem);
		if (pxTask != NULL && pxTask->ulVruntime < ulMinVruntime) {
			ulMinVruntime = pxTask->ulVruntime;
		}
		pxItem = htListGetNext(pxItem);
	}
	if (ulMinVruntime == UINT32_MAX) {
		return NULL;
	}

	/* 下界只升不降：睡醒的任务带着旧虚拟时间回来时，用历史下界而不是
	   它自己的旧值做基准，才认得出"落后太多"。 */
	if ((int32_t)(ulMinVruntime - s_ulMinVruntime) > 0) {
		s_ulMinVruntime = ulMinVruntime;
	}

	/* 第二遍：唤醒保护 + 选最小。 */
	htTCB_t *pxBest = NULL;
	pxItem = htListGetHead(pxList);
	for (UBaseType_t uxIndex = 0U; (uxIndex < uxCount) && (pxItem != NULL); uxIndex++) {
		htTCB_t *pxTask = (htTCB_t *)htListGetItemOwner(pxItem);
		if (pxTask != NULL) {
			/* 落后超过唤醒窗口的任务，把虚拟时间抬到窗口边缘。 */
			if ((int32_t)(pxTask->ulVruntime - s_ulMinVruntime) < -(int32_t)HT_FAIR_WAKE_MARGIN) {
				pxTask->ulVruntime = s_ulMinVruntime - HT_FAIR_WAKE_MARGIN;
			}
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
		UBaseType_t uxCurrentPriority = pxCurrent->uxPriority;

		/* 高优先级照旧绝对抢占，优先级语义不变。 */
		for (UBaseType_t uxPriority = configMAX_PRIORITIES - 1U; uxPriority > uxCurrentPriority; uxPriority--) {
			if (htListGetCurrentNumberOfItems(&(pxReadyTasksLists[uxPriority])) > 0U) {
				xSwitchRequired = htTRUE;
				break;
			}
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
};

#endif /* configUSE_SCHED_POLICY_FAIR_SHARE */
