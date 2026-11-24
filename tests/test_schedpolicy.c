/**
 * @file tests/test_schedpolicy.c
 * @brief 可插拔调度策略的主机侧单元测试：注册表、固定优先级、公平分享与最早死期。
 *
 * 本套件不编译 htscheduler.c，而是自己提供就绪表、当前任务与节拍计数这三个
 * 内核全局量，直接驱动策略对象的五个操作，因此可以在 PC 上精确复现"跑 N 个
 * tick 后谁拿到多少 CPU"这类时序场景。
 */
#include "unity.h"

#include <string.h>

#include "htlist.h"
#include "htschedpolicy.h"
#include "httask.h"

/* ------------------------------------------------------------------ */
/* 1. 内核全局量与依赖桩                                              */
/* ------------------------------------------------------------------ */

/** 各优先级的就绪链表，模拟内核里的同名变量。 */
htList_t pxReadyTasksLists[configMAX_PRIORITIES];
/** 全局任务总表，公平/死期策略的 vInitialise 会遍历它。 */
htList_t pxAllocatedTasksList;
/** 当前运行的任务。 */
htTCB_t *pxCurrentTCB;
/** 系统节拍计数。 */
TickType_t xTickCount;

/** 调度器是否已启动，由用例控制。 */
static BaseType_t xSchedulerStarted;
/** 临界区嵌套计数，用例末尾断言归零。 */
static UBaseType_t uxCriticalNesting;

/** 三个假任务的 TCB。 */
static htTCB_t xTasks[3];

BaseType_t htSchedulerIsStarted(void)
{
	return xSchedulerStarted;
}

void htEnterCritical(void)
{
	uxCriticalNesting++;
}

void htExitCritical(void)
{
	if (uxCriticalNesting > 0U) {
		uxCriticalNesting--;
	}
}

/* ------------------------------------------------------------------ */
/* 2. 夹具                                                             */
/* ------------------------------------------------------------------ */

#define TEST_PRIO_LOW 1U
#define TEST_PRIO_HIGH 2U

/**
 * @brief 复位全部全局量与链表，并把三个假任务初始化成"未挂入任何链表"。
 */
static void setup(void)
{
	for (UBaseType_t uxPriority = 0U; uxPriority < (UBaseType_t)configMAX_PRIORITIES; uxPriority++) {
		htListInit(&(pxReadyTasksLists[uxPriority]));
	}
	htListInit(&pxAllocatedTasksList);

	pxCurrentTCB = NULL;
	xTickCount = 0U;
	xSchedulerStarted = htFALSE;
	uxCriticalNesting = 0U;

	memset(xTasks, 0, sizeof(xTasks));
	for (UBaseType_t uxIndex = 0U; uxIndex < 3U; uxIndex++) {
		htListItemInit(&(xTasks[uxIndex].xStateListItem));
		htListItemInit(&(xTasks[uxIndex].xAllListItem));
		htListSetItemOwner(&(xTasks[uxIndex].xStateListItem), &xTasks[uxIndex]);
		htListSetItemOwner(&(xTasks[uxIndex].xAllListItem), &xTasks[uxIndex]);
		xTasks[uxIndex].uxPriority = TEST_PRIO_LOW;
		xTasks[uxIndex].uxTaskState = HT_TASK_READY;
	}
}

/**
 * @brief 把任务挂进就绪表与全局任务表，并通知策略"新任务已创建"。
 */
static void prvAddReady(htTCB_t *pxTask)
{
	htListInsertEnd(&(pxReadyTasksLists[pxTask->uxPriority]), &(pxTask->xStateListItem));
	htListInsertEnd(&pxAllocatedTasksList, &(pxTask->xAllListItem));
	htSchedNotifyTaskCreated(pxTask);
}

/**
 * @brief 模拟调度器跑 uxTicks 个节拍，统计两个任务各拿到多少 tick。
 *
 * @note 顺序与内核一致：先给当前任务记账，再判断是否抢占，最后才换人。
 */
static void prvRunTicks(
		const htSchedPolicy_t *pxPolicy, UBaseType_t uxTicks, UBaseType_t *puxFirst, UBaseType_t *puxSecond)
{
	*puxFirst = 0U;
	*puxSecond = 0U;

	for (UBaseType_t uxTick = 0U; uxTick < uxTicks; uxTick++) {
		pxPolicy->vTick(pxCurrentTCB);

		if (pxCurrentTCB == &xTasks[0]) {
			(*puxFirst)++;
		} else {
			(*puxSecond)++;
		}

		if (pxPolicy->xShouldPreempt(pxCurrentTCB) == htTRUE) {
			pxCurrentTCB = pxPolicy->pxSelectNext();
		}
	}
}

/* ------------------------------------------------------------------ */
/* 3. 注册表                                                           */
/* ------------------------------------------------------------------ */

static void test_registry_rejects_invalid_policy(void)
{
	htSchedPolicy_t xIncomplete;

	setup();
	memset(&xIncomplete, 0, sizeof(xIncomplete));

	TEST_ASSERT_EQUAL(htFAIL, htSchedSetPolicy(NULL));
	TEST_ASSERT_EQUAL(htFAIL, htSchedSetPolicy(&xIncomplete));
	TEST_ASSERT_EQUAL(htPASS, htSchedSetPolicy(&htSchedPolicyFixedPriority));
	TEST_ASSERT(htSchedGetPolicy() == &htSchedPolicyFixedPriority);
	TEST_ASSERT(0 == strcmp(htSchedGetPolicy()->pcName, "fixed-priority"));

	/* 启动之后不允许换策略。 */
	xSchedulerStarted = htTRUE;
	TEST_ASSERT_EQUAL(htFAIL, htSchedSetPolicy(&htSchedPolicyFixedPriority));
	TEST_ASSERT_EQUAL(0, (int)uxCriticalNesting);
}

/* ------------------------------------------------------------------ */
/* 4. 固定优先级策略（行为必须与改动前完全一致）                        */
/* ------------------------------------------------------------------ */

static void test_fixed_policy_rotates_equal_priority(void)
{
	const htSchedPolicy_t *pxPolicy = &htSchedPolicyFixedPriority;

	setup();
	prvAddReady(&xTasks[0]);
	prvAddReady(&xTasks[1]);

	TEST_ASSERT(pxPolicy->pxSelectNext() == &xTasks[0]);
	TEST_ASSERT(pxPolicy->pxSelectNext() == &xTasks[1]);
	TEST_ASSERT(pxPolicy->pxSelectNext() == &xTasks[0]);
	TEST_ASSERT_EQUAL(0, (int)uxCriticalNesting);
}

static void test_fixed_policy_prefers_higher_priority(void)
{
	const htSchedPolicy_t *pxPolicy = &htSchedPolicyFixedPriority;

	setup();
	xTasks[2].uxPriority = TEST_PRIO_HIGH;
	prvAddReady(&xTasks[0]);
	prvAddReady(&xTasks[2]);

	TEST_ASSERT(pxPolicy->pxSelectNext() == &xTasks[2]);
	TEST_ASSERT(pxPolicy->pxSelectNext() == &xTasks[2]);
	TEST_ASSERT_EQUAL(0, (int)uxCriticalNesting);
}

static void test_fixed_policy_preemption_conditions(void)
{
	const htSchedPolicy_t *pxPolicy = &htSchedPolicyFixedPriority;

	setup();
	xTasks[2].uxPriority = TEST_PRIO_HIGH;
	prvAddReady(&xTasks[0]);
	pxCurrentTCB = &xTasks[0];

	/* 只有一个任务：不切换。 */
	TEST_ASSERT_EQUAL(htFALSE, pxPolicy->xShouldPreempt(&xTasks[0]));

	/* 更高优先级就绪：切换。 */
	prvAddReady(&xTasks[2]);
	TEST_ASSERT_EQUAL(htTRUE, pxPolicy->xShouldPreempt(&xTasks[0]));

	/* 同优先级兄弟就绪：时间片轮转也要切换。 */
	prvAddReady(&xTasks[1]);
	TEST_ASSERT_EQUAL(htTRUE, pxPolicy->xShouldPreempt(&xTasks[0]));
	TEST_ASSERT_EQUAL(0, (int)uxCriticalNesting);
}

/* ------------------------------------------------------------------ */
/* 5. 公平分享策略                                                     */
/* ------------------------------------------------------------------ */

static void test_fair_equal_weights_split_evenly(void)
{
	const htSchedPolicy_t *pxPolicy = &htSchedPolicyFairShare;
	UBaseType_t uxFirst = 0U;
	UBaseType_t uxSecond = 0U;

	setup();
	prvAddReady(&xTasks[0]);
	prvAddReady(&xTasks[1]);
	TEST_ASSERT_EQUAL(htPASS, htSchedSetPolicy(pxPolicy));

	pxCurrentTCB = &xTasks[0];
	prvRunTicks(pxPolicy, 2000U, &uxFirst, &uxSecond);

	/* 等权：两份 CPU 份额相差不超过 5%。 */
	TEST_ASSERT(uxFirst + uxSecond == 2000U);
	TEST_ASSERT(uxFirst > 900U);
	TEST_ASSERT(uxSecond > 900U);
	TEST_ASSERT_EQUAL(0, (int)uxCriticalNesting);
}

static void test_fair_weight_doubles_share(void)
{
	const htSchedPolicy_t *pxPolicy = &htSchedPolicyFairShare;
	UBaseType_t uxFirst = 0U;
	UBaseType_t uxSecond = 0U;

	setup();
	prvAddReady(&xTasks[0]);
	prvAddReady(&xTasks[1]);
	TEST_ASSERT_EQUAL(htPASS, htSchedSetPolicy(pxPolicy));
	TEST_ASSERT_EQUAL(htFAIL, htSchedSetWeight(&xTasks[1], 0U));
	TEST_ASSERT_EQUAL(htFAIL, htSchedSetWeight(NULL, 1024U));
	TEST_ASSERT_EQUAL(htPASS, htSchedSetWeight(&xTasks[1], 2048U));

	pxCurrentTCB = &xTasks[0];
	prvRunTicks(pxPolicy, 3000U, &uxFirst, &uxSecond);

	/* 权重 2:1：第二个任务应拿到约三分之二 CPU（允许 ±10%）。 */
	TEST_ASSERT(uxSecond * 100U >= uxFirst * 180U);
	TEST_ASSERT(uxSecond * 100U <= uxFirst * 220U);
	TEST_ASSERT_EQUAL(0, (int)uxCriticalNesting);
}

static void test_fair_pulls_woken_task_back_to_floor(void)
{
	const htSchedPolicy_t *pxPolicy = &htSchedPolicyFairShare;

	setup();
	prvAddReady(&xTasks[0]);
	TEST_ASSERT_EQUAL(htPASS, htSchedSetPolicy(pxPolicy));

	/* 只有一个任务时，虚拟时间下界跟着它单调抬升。 */
	pxCurrentTCB = &xTasks[0];
	for (UBaseType_t uxTick = 0U; uxTick < 200U; uxTick++) {
		pxPolicy->vTick(pxCurrentTCB);
	}
	TEST_ASSERT_EQUAL((int)(200U * HT_SCHED_DEFAULT_WEIGHT), (int)xTasks[0].ulVruntime);

	/* 睡醒的第二个任务虚拟时间仍是 0，必须被抬到下界减窗口的位置。 */
	prvAddReady(&xTasks[1]);
	TEST_ASSERT(pxPolicy->pxSelectNext() == &xTasks[1]);
	TEST_ASSERT_EQUAL((int)((200U * HT_SCHED_DEFAULT_WEIGHT) - 4096U), (int)xTasks[1].ulVruntime);
	TEST_ASSERT_EQUAL(0, (int)uxCriticalNesting);
}

/* ------------------------------------------------------------------ */
/* 6. 最早死期优先策略                                                 */
/* ------------------------------------------------------------------ */

static void test_edf_picks_earliest_deadline(void)
{
	const htSchedPolicy_t *pxPolicy = &htSchedPolicyEdf;

	setup();
	prvAddReady(&xTasks[0]);
	prvAddReady(&xTasks[1]);
	TEST_ASSERT_EQUAL(htPASS, htSchedSetPolicy(pxPolicy));
	TEST_ASSERT_EQUAL(htFAIL, htSchedSetDeadline(NULL, 10U));
	TEST_ASSERT_EQUAL(htPASS, htSchedSetDeadline(&xTasks[0], 100U));
	TEST_ASSERT_EQUAL(htPASS, htSchedSetDeadline(&xTasks[1], 50U));

	pxCurrentTCB = &xTasks[0];
	TEST_ASSERT(pxPolicy->pxSelectNext() == &xTasks[1]);
	TEST_ASSERT_EQUAL(0, (int)uxCriticalNesting);
}

static void test_edf_tie_keeps_current_task(void)
{
	const htSchedPolicy_t *pxPolicy = &htSchedPolicyEdf;

	setup();
	prvAddReady(&xTasks[0]);
	prvAddReady(&xTasks[1]);
	TEST_ASSERT_EQUAL(htPASS, htSchedSetPolicy(pxPolicy));
	TEST_ASSERT_EQUAL(htPASS, htSchedSetDeadline(&xTasks[0], 100U));
	TEST_ASSERT_EQUAL(htPASS, htSchedSetDeadline(&xTasks[1], 100U));

	pxCurrentTCB = &xTasks[0];
	TEST_ASSERT(pxPolicy->pxSelectNext() == &xTasks[0]);

	pxCurrentTCB = &xTasks[1];
	TEST_ASSERT(pxPolicy->pxSelectNext() == &xTasks[1]);
	TEST_ASSERT_EQUAL(0, (int)uxCriticalNesting);
}

static void test_edf_falls_back_to_priority(void)
{
	const htSchedPolicy_t *pxPolicy = &htSchedPolicyEdf;

	setup();
	xTasks[2].uxPriority = TEST_PRIO_HIGH;
	prvAddReady(&xTasks[0]);
	prvAddReady(&xTasks[2]);
	TEST_ASSERT_EQUAL(htPASS, htSchedSetPolicy(pxPolicy));

	/* 两个任务都没有死期：退化为固定优先级，高优先级胜出。 */
	TEST_ASSERT(pxPolicy->pxSelectNext() == &xTasks[2]);
	TEST_ASSERT_EQUAL(0, (int)uxCriticalNesting);
}

static void test_edf_counts_each_miss_once(void)
{
	const htSchedPolicy_t *pxPolicy = &htSchedPolicyEdf;

	setup();
	prvAddReady(&xTasks[0]);
	TEST_ASSERT_EQUAL(htPASS, htSchedSetPolicy(pxPolicy));
	TEST_ASSERT_EQUAL(htPASS, htSchedSetDeadline(&xTasks[0], 10U));
	pxCurrentTCB = &xTasks[0];

	/* 连跑 30 个 tick，死期只漏一次。 */
	for (UBaseType_t uxTick = 0U; uxTick < 30U; uxTick++) {
		xTickCount++;
		pxPolicy->vTick(pxCurrentTCB);
	}
	TEST_ASSERT_EQUAL(1, (int)htSchedEdfGetDeadlineMisses());

	/* 重新登记死期（同时清漏期标志）后，再漏一次才计第二次。 */
	TEST_ASSERT_EQUAL(htPASS, htSchedSetDeadline(&xTasks[0], 5U));
	pxPolicy->vTick(pxCurrentTCB);
	TEST_ASSERT_EQUAL(2, (int)htSchedEdfGetDeadlineMisses());

	htSchedEdfClearDeadlineMisses();
	TEST_ASSERT_EQUAL(0, (int)htSchedEdfGetDeadlineMisses());
	TEST_ASSERT_EQUAL(0, (int)uxCriticalNesting);
}

int main(void)
{
	UnityBegin("test_schedpolicy.c");

	RUN_TEST(test_registry_rejects_invalid_policy);
	RUN_TEST(test_fixed_policy_rotates_equal_priority);
	RUN_TEST(test_fixed_policy_prefers_higher_priority);
	RUN_TEST(test_fixed_policy_preemption_conditions);
	RUN_TEST(test_fair_equal_weights_split_evenly);
	RUN_TEST(test_fair_weight_doubles_share);
	RUN_TEST(test_fair_pulls_woken_task_back_to_floor);
	RUN_TEST(test_edf_picks_earliest_deadline);
	RUN_TEST(test_edf_tie_keeps_current_task);
	RUN_TEST(test_edf_falls_back_to_priority);
	RUN_TEST(test_edf_counts_each_miss_once);

	return UnityEnd();
}
