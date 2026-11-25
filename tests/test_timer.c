/**
 * @file tests/test_timer.c
 * @brief 软件定时器（kernel/httimer.c）的主机侧单元测试。
 *
 * 覆盖：单次触发与周期自动重载、静态与动态创建、Start/Stop/Reset/ChangePeriod/Delete、
 * FromISR 变体、32 位 tick 回绕安全、回调内自停以及临界区嵌套归零。
 */
#include "unity.h"

#include <stdlib.h>
#include <string.h>

#include "htkernel_internal.h"
#include "htlist.h"
#include "htqueue.h"
#include "htschedpolicy.h"
#include "htscheduler.h"
#include "httask.h"
#include "httimer.h"

htTCB_t *pxCurrentTCB = NULL;
TickType_t xTickCount = 0U;
htList_t pxReadyTasksLists[configMAX_PRIORITIES];
static htList_t s_delayed_list;
static htList_t s_overflow_delayed_list;
htList_t *pxDelayedTaskList = &s_delayed_list;
htList_t *pxOverflowDelayedTaskList = &s_overflow_delayed_list;
TickType_t xNextTaskUnblockTime = htBLOCKED_INDEFINITELY;
volatile UBaseType_t uxCriticalNesting = 0U;

static UBaseType_t s_isr_critical_nesting = 0U;
static size_t s_alloc_count = 0U;
static size_t s_free_count = 0U;
static UBaseType_t s_yield_count = 0U;
static htTCB_t s_daemon_stub_tcb;

void *htPortMalloc(size_t size)
{
	s_alloc_count++;
	return malloc(size);
}

void htPortFree(void *ptr)
{
	if (ptr != NULL) {
		s_free_count++;
		free(ptr);
	}
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

UBaseType_t htEnterCriticalFromISR(void)
{
	const UBaseType_t uxPrev = s_isr_critical_nesting;
	s_isr_critical_nesting++;
	return uxPrev;
}

void htExitCriticalFromISR(UBaseType_t uxSavedStatus)
{
	s_isr_critical_nesting = uxSavedStatus;
}

void htTaskYield(void)
{
	s_yield_count++;
}

void htSchedRecordReadyPriority(UBaseType_t uxPriority)
{
	(void)uxPriority;
}

void htSchedClearReadyPriorityIfEmpty(UBaseType_t uxPriority)
{
	(void)uxPriority;
}

void htSchedNotifyTaskEnqueued(htTCB_t *pxTask)
{
	(void)pxTask;
}

BaseType_t htTaskCreate(TaskFunction_t pxTaskCode, const char *const pcName, const uint16_t usStackDepth,
		void *const pvParameters, UBaseType_t uxPriority, void **const pxCreatedTask)
{
	(void)pxTaskCode;
	(void)pcName;
	(void)usStackDepth;
	(void)pvParameters;
	(void)uxPriority;
	if (pxCreatedTask != NULL) {
		*pxCreatedTask = &s_daemon_stub_tcb;
	}
	return htPASS;
}

TaskHandle_t htTaskCreateStatic(TaskFunction_t pxTaskCode, const char *const pcName, uint16_t usStackDepth,
		void *const pvParameters, UBaseType_t uxPriority, StackType_t *puxStackBuffer, htTCB_t *pxTaskBuffer)
{
	(void)pxTaskCode;
	(void)pcName;
	(void)usStackDepth;
	(void)pvParameters;
	(void)uxPriority;
	(void)puxStackBuffer;
	return (TaskHandle_t)pxTaskBuffer;
}

static UBaseType_t s_oneshot_hits = 0U;
static UBaseType_t s_periodic_hits = 0U;
static TimerHandle_t s_last_timer = NULL;
static void *s_last_id = NULL;
static UBaseType_t s_self_stop_hits = 0U;

static void prvOneshotCallback(TimerHandle_t xTimer)
{
	s_oneshot_hits++;
	s_last_timer = xTimer;
	s_last_id = htTimerGetTimerID(xTimer);
}

static void prvPeriodicCallback(TimerHandle_t xTimer)
{
	s_periodic_hits++;
	s_last_timer = xTimer;
	s_last_id = htTimerGetTimerID(xTimer);
}

static void prvSelfStopCallback(TimerHandle_t xTimer)
{
	s_self_stop_hits++;
	if (s_self_stop_hits >= 2U) {
		(void)htTimerStop(xTimer, 0U);
	}
}

static void prvResetEnv(TickType_t xStartTick)
{
	for (UBaseType_t i = 0U; i < (UBaseType_t)configMAX_PRIORITIES; i++) {
		htListInit(&(pxReadyTasksLists[i]));
	}
	htListInit(&s_delayed_list);
	htListInit(&s_overflow_delayed_list);
	pxCurrentTCB = NULL;
	xTickCount = xStartTick;
	uxCriticalNesting = 0U;
	s_isr_critical_nesting = 0U;
	s_alloc_count = 0U;
	s_free_count = 0U;
	s_yield_count = 0U;
	s_oneshot_hits = 0U;
	s_periodic_hits = 0U;
	s_self_stop_hits = 0U;
	s_last_timer = NULL;
	s_last_id = NULL;
	TEST_ASSERT_EQUAL(htPASS, htTimerInit());
}

static void test_oneshot_and_periodic_timers_fire_on_schedule(void)
{
	prvResetEnv(100U);
	htTimer_t xOneshotBuf;
	htTimer_t xPeriodicBuf;
	int xId1 = 11;
	int xId2 = 22;

	TimerHandle_t xOneshot = htTimerCreateStatic("one", 10U, htFALSE, &xId1, prvOneshotCallback, &xOneshotBuf);
	TimerHandle_t xPeriodic = htTimerCreateStatic("per", 5U, htTRUE, &xId2, prvPeriodicCallback, &xPeriodicBuf);
	TEST_ASSERT_NOT_NULL(xOneshot);
	TEST_ASSERT_NOT_NULL(xPeriodic);
	TEST_ASSERT_EQUAL(0U, s_alloc_count);
	TEST_ASSERT_EQUAL(htFALSE, htTimerIsActive(xOneshot));
	TEST_ASSERT_EQUAL(10U, htTimerGetPeriod(xOneshot));
	TEST_ASSERT(strcmp(htTimerGetName(xOneshot), "one") == 0);

	TEST_ASSERT_EQUAL(htPASS, htTimerStart(xOneshot, 0U));
	TEST_ASSERT_EQUAL(htPASS, htTimerStart(xPeriodic, 0U));
	TEST_ASSERT_EQUAL(5U, htTimerProcessStep(100U));
	TEST_ASSERT_EQUAL(htTRUE, htTimerIsActive(xOneshot));
	TEST_ASSERT_EQUAL(htTRUE, htTimerIsActive(xPeriodic));

	/* t = 105: 周期定时器首次到期，单次尚未到期。 */
	TEST_ASSERT_EQUAL(5U, htTimerProcessStep(105U));
	TEST_ASSERT_EQUAL(1U, s_periodic_hits);
	TEST_ASSERT_EQUAL(0U, s_oneshot_hits);
	TEST_ASSERT(s_last_id == &xId2);

	/* t = 110: 周期第二次到期，单次到期后自动停用。 */
	TEST_ASSERT_EQUAL(5U, htTimerProcessStep(110U));
	TEST_ASSERT_EQUAL(2U, s_periodic_hits);
	TEST_ASSERT_EQUAL(1U, s_oneshot_hits);
	TEST_ASSERT_EQUAL(htFALSE, htTimerIsActive(xOneshot));
	TEST_ASSERT_EQUAL(htTRUE, htTimerIsActive(xPeriodic));

	/* 停止周期定时器并确认后续不再触发。 */
	TEST_ASSERT_EQUAL(htPASS, htTimerStop(xPeriodic, 0U));
	TEST_ASSERT_EQUAL(htBLOCKED_INDEFINITELY, htTimerProcessStep(120U));
	TEST_ASSERT_EQUAL(2U, s_periodic_hits);
	TEST_ASSERT_EQUAL(htFALSE, htTimerIsActive(xPeriodic));
	TEST_ASSERT_EQUAL(0U, uxCriticalNesting);
}

static void test_timer_reset_change_period_and_dynamic_delete(void)
{
	prvResetEnv(200U);
	TimerHandle_t xTimer = htTimerCreate("dyn", 20U, htTRUE, NULL, prvSelfStopCallback);
	TEST_ASSERT_NOT_NULL(xTimer);
	TEST_ASSERT_EQUAL(1U, s_alloc_count);

	/* 启动后在 t=210 复位，新到期时刻推迟到 230。 */
	TEST_ASSERT_EQUAL(htPASS, htTimerStart(xTimer, 0U));
	TEST_ASSERT_EQUAL(20U, htTimerProcessStep(200U));
	xTickCount = 210U;
	TEST_ASSERT_EQUAL(htPASS, htTimerReset(xTimer, 0U));
	TEST_ASSERT_EQUAL(20U, htTimerProcessStep(210U));
	TEST_ASSERT_EQUAL(10U, htTimerProcessStep(220U));
	TEST_ASSERT_EQUAL(0U, s_self_stop_hits);

	/* 改周期为 4 tick，新到期时刻为 224；t=224 首次触发，t=228 第二次触发并在回调内自停。 */
	xTickCount = 220U;
	TEST_ASSERT_EQUAL(htPASS, htTimerChangePeriod(xTimer, 4U, 0U));
	TEST_ASSERT_EQUAL(4U, htTimerProcessStep(220U));
	TEST_ASSERT_EQUAL(4U, htTimerGetPeriod(xTimer));

	TEST_ASSERT_EQUAL(4U, htTimerProcessStep(224U));
	TEST_ASSERT_EQUAL(1U, s_self_stop_hits);
	TEST_ASSERT_EQUAL(4U, htTimerProcessStep(228U));
	TEST_ASSERT_EQUAL(2U, s_self_stop_hits);
	/* 回调内入队的 Stop 命令在下一轮步进中生效。 */
	TEST_ASSERT_EQUAL(htBLOCKED_INDEFINITELY, htTimerProcessStep(229U));
	TEST_ASSERT_EQUAL(htFALSE, htTimerIsActive(xTimer));

	/* 动态删除必须释放控制块。 */
	TEST_ASSERT_EQUAL(htPASS, htTimerDelete(xTimer, 0U));
	(void)htTimerProcessStep(230U);
	TEST_ASSERT_EQUAL(1U, s_free_count);
	TEST_ASSERT_EQUAL(0U, uxCriticalNesting);
}

static void test_timer_survives_32bit_tick_wraparound_and_isr_commands(void)
{
	prvResetEnv(UINT32_MAX - 3U);
	htTimer_t xWrapBuf;
	int xId = 99;
	BaseType_t xWoken = htTRUE;

	TimerHandle_t xWrap = htTimerCreateStatic("wrap", 10U, htFALSE, &xId, prvOneshotCallback, &xWrapBuf);
	TEST_ASSERT_NOT_NULL(xWrap);
	TEST_ASSERT_EQUAL(htPASS, htTimerStartFromISR(xWrap, &xWoken));
	TEST_ASSERT_EQUAL(htFALSE, xWoken);
	TEST_ASSERT_EQUAL(0U, s_isr_critical_nesting);

	/* 步进至回绕前：到期时刻 = (UINT32_MAX - 3) + 10 = 6，应落入溢出链表。 */
	(void)htTimerProcessStep(UINT32_MAX - 3U);
	TEST_ASSERT_EQUAL(0U, s_oneshot_hits);
	(void)htTimerProcessStep(UINT32_MAX);
	TEST_ASSERT_EQUAL(0U, s_oneshot_hits);

	/* 回绕到 0..5 仍未到期，t=6 精确触发！ */
	TEST_ASSERT_EQUAL(6U, htTimerProcessStep(0U));
	TEST_ASSERT_EQUAL(0U, s_oneshot_hits);
	TEST_ASSERT_EQUAL(1U, htTimerProcessStep(5U));
	TEST_ASSERT_EQUAL(0U, s_oneshot_hits);
	TEST_ASSERT_EQUAL(htBLOCKED_INDEFINITELY, htTimerProcessStep(6U));
	TEST_ASSERT_EQUAL(1U, s_oneshot_hits);
	TEST_ASSERT(s_last_timer == xWrap);
	TEST_ASSERT(s_last_id == &xId);

	/* 校验守护任务创建幂等性与非法参数拦截。 */
	TEST_ASSERT_EQUAL(htPASS, htTimerCreateDaemonTask());
	TEST_ASSERT_NOT_NULL(htTimerGetDaemonTaskHandle());
	TEST_ASSERT_NULL(htTimerCreate("bad", 0U, htFALSE, NULL, prvOneshotCallback));
	TEST_ASSERT_NULL(htTimerCreate("bad", 10U, htFALSE, NULL, NULL));
	TEST_ASSERT_EQUAL(htFAIL, htTimerChangePeriod(xWrap, 0U, 0U));
	TEST_ASSERT_EQUAL(0U, uxCriticalNesting);
}

int main(void)
{
	UnityBegin("test_timer.c");
	RUN_TEST(test_oneshot_and_periodic_timers_fire_on_schedule);
	RUN_TEST(test_timer_reset_change_period_and_dynamic_delete);
	RUN_TEST(test_timer_survives_32bit_tick_wraparound_and_isr_commands);
	return UnityEnd();
}
