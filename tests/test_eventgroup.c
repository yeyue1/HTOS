/**
 * @file tests/test_eventgroup.c
 * @brief 24 位事件标志组（kernel/hteventgroup.c）的主机侧单元测试。
 *
 * 覆盖：动态/静态创建、WAIT_ANY 与 WAIT_ALL、ClearOnExit、多任务广播原子唤醒、
 * 超时清理、SetBitsFromISR 抢占标记、越界高 8 位参数拦截以及临界区配对。
 */
#include "unity.h"

#include <stdlib.h>
#include <string.h>

#include "hteventgroup.h"
#include "htkernel_internal.h"
#include "htlist.h"
#include "htschedpolicy.h"
#include "htscheduler.h"
#include "httask.h"

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
static EventGroupHandle_t s_yield_set_group = NULL;
static EventBits_t s_yield_set_bits = 0U;

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

void htTaskYield(void)
{
	s_yield_count++;
	if (s_yield_set_group != NULL && s_yield_set_bits != 0U) {
		EventGroupHandle_t xGroup = s_yield_set_group;
		const EventBits_t uxBits = s_yield_set_bits;
		s_yield_set_group = NULL;
		s_yield_set_bits = 0U;
		(void)htEventGroupSetBits(xGroup, uxBits);
	}
}

static void prvInitFakeTask(htTCB_t *pxTask, UBaseType_t uxPrio)
{
	memset(pxTask, 0, sizeof(*pxTask));
	pxTask->uxPriority = (uint8_t)uxPrio;
	pxTask->uxBasePriority = (uint8_t)uxPrio;
	pxTask->uxTaskState = HT_TASK_READY;
	htListItemInit(&(pxTask->xStateListItem));
	htListItemInit(&(pxTask->xEventListItem));
	htListSetItemOwner(&(pxTask->xStateListItem), pxTask);
	htListSetItemOwner(&(pxTask->xEventListItem), pxTask);
	htListInsertEnd(&(pxReadyTasksLists[uxPrio]), &(pxTask->xStateListItem));
}

static void prvResetEnv(void)
{
	for (UBaseType_t i = 0U; i < (UBaseType_t)configMAX_PRIORITIES; i++) {
		htListInit(&(pxReadyTasksLists[i]));
	}
	htListInit(&s_delayed_list);
	htListInit(&s_overflow_delayed_list);
	pxCurrentTCB = NULL;
	xTickCount = 0U;
	xNextTaskUnblockTime = htBLOCKED_INDEFINITELY;
	uxCriticalNesting = 0U;
	s_isr_critical_nesting = 0U;
	s_alloc_count = 0U;
	s_free_count = 0U;
	s_yield_count = 0U;
	s_yield_set_group = NULL;
	s_yield_set_bits = 0U;
}

static void test_immediate_wait_any_all_and_clear_on_exit(void)
{
	prvResetEnv();
	htEventGroup_t xStaticBuf;
	EventGroupHandle_t xGroup = htEventGroupCreateStatic(&xStaticBuf);
	TEST_ASSERT_NOT_NULL(xGroup);
	TEST_ASSERT_EQUAL(0U, s_alloc_count);

	TEST_ASSERT_EQUAL(0x03U, htEventGroupSetBits(xGroup, 0x03U));
	TEST_ASSERT_EQUAL(0x03U, htEventGroupGetBits(xGroup));

	/* WAIT_ANY + 不清零。 */
	TEST_ASSERT_EQUAL(0x03U, htEventGroupWaitBits(xGroup, 0x02U, htFALSE, htFALSE, 0U));
	TEST_ASSERT_EQUAL(0x03U, htEventGroupGetBits(xGroup));

	/* WAIT_ALL 未满足时非阻塞返回当前位且不清零。 */
	TEST_ASSERT_EQUAL(0x03U, htEventGroupWaitBits(xGroup, 0x07U, htTRUE, htTRUE, 0U));
	TEST_ASSERT_EQUAL(0x03U, htEventGroupGetBits(xGroup));

	/* 补齐 bit 2 后 WAIT_ALL + ClearOnExit 返回清零前快照，且对应位已清除。 */
	TEST_ASSERT_EQUAL(0x0FU, htEventGroupSetBits(xGroup, 0x0CU));
	TEST_ASSERT_EQUAL(0x0FU, htEventGroupWaitBits(xGroup, 0x07U, htTRUE, htTRUE, 0U));
	TEST_ASSERT_EQUAL(0x08U, htEventGroupGetBits(xGroup));

	/* ClearBits 返回清除前的旧值。 */
	TEST_ASSERT_EQUAL(0x08U, htEventGroupClearBits(xGroup, 0x08U));
	TEST_ASSERT_EQUAL(0U, htEventGroupGetBits(xGroup));

	/* 越界高 8 位被拒绝。 */
	TEST_ASSERT_EQUAL(0U, htEventGroupSetBits(xGroup, 0x01000000U));
	TEST_ASSERT_EQUAL(0U, htEventGroupWaitBits(xGroup, 0x80000000U, htFALSE, htFALSE, 0U));

	htEventGroupDelete(xGroup);
	TEST_ASSERT_EQUAL(0U, s_free_count);
	TEST_ASSERT_EQUAL(0U, uxCriticalNesting);
}

static void test_blocking_wait_woken_by_set_bits_and_timeout_cleanup(void)
{
	prvResetEnv();
	EventGroupHandle_t xGroup = htEventGroupCreate();
	TEST_ASSERT_NOT_NULL(xGroup);
	TEST_ASSERT_EQUAL(1U, s_alloc_count);

	htTCB_t xTask;
	prvInitFakeTask(&xTask, 2U);
	pxCurrentTCB = &xTask;

	/* 让出时模拟另一任务置位 0x05，满足 WAIT_ALL(0x05) 并 ClearOnExit。 */
	s_yield_set_group = xGroup;
	s_yield_set_bits = 0x05U;
	const EventBits_t uxMatched = htEventGroupWaitBits(xGroup, 0x05U, htTRUE, htTRUE, 10U);
	TEST_ASSERT_EQUAL(0x05U, uxMatched);
	TEST_ASSERT_EQUAL(0U, htEventGroupGetBits(xGroup));
	TEST_ASSERT_EQUAL(HT_TASK_READY, xTask.uxTaskState);
	TEST_ASSERT_NULL(xTask.xEventListItem.pxContainer);

	/* 模拟超时返回：yield 后没有任何人置位，必须自动从等待链表摘除。 */
	const EventBits_t uxTimeoutBits = htEventGroupWaitBits(xGroup, 0x10U, htTRUE, htFALSE, 5U);
	TEST_ASSERT_EQUAL(0U, uxTimeoutBits);
	TEST_ASSERT_NULL(xTask.xEventListItem.pxContainer);
	TEST_ASSERT_EQUAL(0U, htListGetCurrentNumberOfItems(&(xGroup->xTasksWaitingForBits)));

	htEventGroupDelete(xGroup);
	TEST_ASSERT_EQUAL(1U, s_free_count);
	TEST_ASSERT_EQUAL(0U, uxCriticalNesting);
}

static void test_broadcast_wakeup_and_from_isr_preemption_flag(void)
{
	prvResetEnv();
	htEventGroup_t xStaticBuf;
	EventGroupHandle_t xGroup = htEventGroupCreateStatic(&xStaticBuf);
	TEST_ASSERT_NOT_NULL(xGroup);

	htTCB_t xCurrent;
	htTCB_t xWaiterLow;
	htTCB_t xWaiterHigh;
	prvInitFakeTask(&xCurrent, 2U);
	prvInitFakeTask(&xWaiterLow, 1U);
	prvInitFakeTask(&xWaiterHigh, 5U);

	/* 手工把两个任务挂到同一事件组：Low 等 bit0(ANY+Clear)，High 等 bit0|bit1(ALL+Clear)。 */
	htListRemove(&(xWaiterLow.xStateListItem));
	xWaiterLow.uxTaskState = HT_TASK_BLOCKED;
	htListSetItemValue(&(xWaiterLow.xEventListItem), 0x01U | 0x01000000U);
	htListInsertEnd(&(xGroup->xTasksWaitingForBits), &(xWaiterLow.xEventListItem));

	htListRemove(&(xWaiterHigh.xStateListItem));
	xWaiterHigh.uxTaskState = HT_TASK_BLOCKED;
	htListSetItemValue(&(xWaiterHigh.xEventListItem), 0x03U | 0x01000000U | 0x02000000U);
	htListInsertEnd(&(xGroup->xTasksWaitingForBits), &(xWaiterHigh.xEventListItem));

	pxCurrentTCB = &xCurrent;
	BaseType_t xWoken = htFALSE;
	TEST_ASSERT_EQUAL(htPASS, htEventGroupSetBitsFromISR(xGroup, 0x03U, &xWoken));

	/* 两个等待者在同一 SetBitsFromISR 中原子广播唤醒，且清零在两者匹配完成后统一执行。 */
	TEST_ASSERT_EQUAL(htTRUE, xWoken);
	TEST_ASSERT_EQUAL(HT_TASK_READY, xWaiterLow.uxTaskState);
	TEST_ASSERT_EQUAL(HT_TASK_READY, xWaiterHigh.uxTaskState);
	TEST_ASSERT_EQUAL(0U, htEventGroupGetBitsFromISR(xGroup));
	TEST_ASSERT_EQUAL(0U, htListGetCurrentNumberOfItems(&(xGroup->xTasksWaitingForBits)));
	TEST_ASSERT_EQUAL(0U, s_isr_critical_nesting);
	TEST_ASSERT_EQUAL(0U, uxCriticalNesting);
}

int main(void)
{
	UnityBegin("test_eventgroup.c");
	RUN_TEST(test_immediate_wait_any_all_and_clear_on_exit);
	RUN_TEST(test_blocking_wait_woken_by_set_bits_and_timeout_cleanup);
	RUN_TEST(test_broadcast_wakeup_and_from_isr_preemption_flag);
	return UnityEnd();
}
