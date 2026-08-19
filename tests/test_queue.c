#include "unity.h"

#include <stdlib.h>

#include "htqueue.h"
#include "htscheduler.h"
#include "htsemaphore.h"

htTCB_t *pxCurrentTCB = NULL;
TickType_t xTickCount = 0U;
htList_t pxReadyTasksLists[configMAX_PRIORITIES];
static htList_t delayed_task_list;
static htList_t overflow_delayed_task_list;
htList_t *pxDelayedTaskList = &delayed_task_list;
htList_t *pxOverflowDelayedTaskList = &overflow_delayed_task_list;
volatile UBaseType_t uxCriticalNesting = 0U;

static size_t allocation_count;
static size_t fail_allocation_at;
static TickType_t ticks_advanced_on_yield;

void *htPortMalloc(size_t size)
{
	allocation_count++;
	if (fail_allocation_at != 0U && allocation_count == fail_allocation_at) {
		return NULL;
	}
	return malloc(size);
}

void htPortFree(void *pointer)
{
	free(pointer);
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

void __disable_irq(void)
{
	uxCriticalNesting++;
}

void __enable_irq(void)
{
	if (uxCriticalNesting > 0U) {
		uxCriticalNesting--;
	}
}

void htTaskYield(void)
{
	if (pxCurrentTCB == NULL || ticks_advanced_on_yield == 0U) {
		return;
	}

	xTickCount += ticks_advanced_on_yield;
	ticks_advanced_on_yield = 0U;
	htListRemove(&(pxCurrentTCB->xStateListItem));
	htListRemove(&(pxCurrentTCB->xEventListItem));
	pxCurrentTCB->uxTaskState = HT_TASK_READY;
	htListInsertEnd(&(pxReadyTasksLists[pxCurrentTCB->uxPriority]), &(pxCurrentTCB->xStateListItem));
}

static void reset_test_state(void)
{
	allocation_count = 0U;
	fail_allocation_at = 0U;
	ticks_advanced_on_yield = 0U;
	uxCriticalNesting = 0U;
	xTickCount = 0U;
	pxCurrentTCB = NULL;
	htListInit(pxDelayedTaskList);
	htListInit(pxOverflowDelayedTaskList);
	for (UBaseType_t priority = 0U; priority < configMAX_PRIORITIES; priority++) {
		htListInit(&pxReadyTasksLists[priority]);
	}
}

static void initialise_current_task(htTCB_t *task, UBaseType_t priority)
{
	htListItemInit(&(task->xStateListItem));
	htListItemInit(&(task->xEventListItem));
	htListSetItemOwner(&(task->xStateListItem), task);
	htListSetItemOwner(&(task->xEventListItem), task);
	task->uxPriority = priority;
	task->uxTaskState = HT_TASK_READY;
	htListInsertEnd(&(pxReadyTasksLists[priority]), &(task->xStateListItem));
	pxCurrentTCB = task;
}

static void test_queue_is_fifo_and_full_send_balances_critical_section(void)
{
	reset_test_state();
	QueueHandle_t queue = htQueueCreate(2U, sizeof(int));
	int first = 11;
	int second = 22;
	int third = 33;
	int received = 0;

	TEST_ASSERT_NOT_NULL(queue);
	TEST_ASSERT_EQUAL(htPASS, htQueueSend(queue, &first, 0U));
	TEST_ASSERT_EQUAL(htPASS, htQueueSend(queue, &second, 0U));
	TEST_ASSERT_EQUAL(htFAIL, htQueueSend(queue, &third, 0U));
	TEST_ASSERT_EQUAL(0, uxCriticalNesting);
	TEST_ASSERT_EQUAL(2, htQueueMessagesWaiting(queue));
	TEST_ASSERT_EQUAL(htPASS, htQueueReceive(queue, &received, 0U));
	TEST_ASSERT_EQUAL(first, received);
	TEST_ASSERT_EQUAL(htPASS, htQueueReceive(queue, &received, 0U));
	TEST_ASSERT_EQUAL(second, received);
	htQueueDelete(queue);
}

static void test_zero_sized_queue_honors_maximum_count(void)
{
	reset_test_state();
	QueueHandle_t semaphore = htQueueCreate(1U, 0U);
	unsigned char dummy = 0U;

	TEST_ASSERT_NOT_NULL(semaphore);
	TEST_ASSERT_EQUAL(htPASS, htQueueSend(semaphore, &dummy, 0U));
	TEST_ASSERT_EQUAL(htFAIL, htQueueSend(semaphore, &dummy, 0U));
	TEST_ASSERT_EQUAL(1, htQueueMessagesWaiting(semaphore));
	TEST_ASSERT_EQUAL(0, uxCriticalNesting);
	htQueueDelete(semaphore);
}

static void test_queue_create_returns_null_when_storage_allocation_fails(void)
{
	reset_test_state();
	fail_allocation_at = 2U;
	TEST_ASSERT_NULL(htQueueCreate(2U, sizeof(int)));
}

static void test_mutex_behaves_as_bounded_binary_token(void)
{
	reset_test_state();
	SemaphoreHandle_t mutex = htSemaphoreCreateMutex();

	TEST_ASSERT_NOT_NULL(mutex);
	TEST_ASSERT_EQUAL(1, htSemaphoreGetCount(mutex));
	TEST_ASSERT_EQUAL(htPASS, htSemaphoreTakeMutex(mutex, 0U));
	TEST_ASSERT_EQUAL(0, htSemaphoreGetCount(mutex));
	TEST_ASSERT_EQUAL(htFAIL, htSemaphoreTakeMutex(mutex, 0U));
	TEST_ASSERT_EQUAL(htPASS, htSemaphoreGiveMutex(mutex));
	TEST_ASSERT_EQUAL(1, htSemaphoreGetCount(mutex));
	TEST_ASSERT_EQUAL(htFAIL, htSemaphoreGiveMutex(mutex));
	TEST_ASSERT_EQUAL(1, htSemaphoreGetCount(mutex));
	htSemaphoreDelete(mutex);
}

static void test_full_queue_finite_wait_expires_and_unlinks_task(void)
{
	reset_test_state();
	QueueHandle_t queue = htQueueCreate(1U, sizeof(int));
	htTCB_t task = {0};
	int first = 11;
	int second = 22;

	TEST_ASSERT_NOT_NULL(queue);
	initialise_current_task(&task, 1U);
	TEST_ASSERT_EQUAL(htPASS, htQueueSend(queue, &first, 0U));
	ticks_advanced_on_yield = 5U;

	TEST_ASSERT_EQUAL(htFAIL, htQueueSend(queue, &second, 5U));
	TEST_ASSERT_EQUAL(5U, xTickCount);
	TEST_ASSERT_EQUAL(HT_TASK_READY, task.uxTaskState);
	TEST_ASSERT_NULL(task.xEventListItem.pxContainer);
	TEST_ASSERT(task.xStateListItem.pxContainer == &pxReadyTasksLists[1]);
	TEST_ASSERT_EQUAL(0, uxCriticalNesting);
	htQueueDelete(queue);
}

static void test_single_item_queue_overwrite_keeps_latest_value(void)
{
	reset_test_state();
	QueueHandle_t queue = htQueueCreate(1U, sizeof(int));
	int first = 11;
	int latest = 22;
	int received = 0;

	TEST_ASSERT_NOT_NULL(queue);
	TEST_ASSERT_EQUAL(1, htQueueSpacesAvailable(queue));
	TEST_ASSERT_EQUAL(htPASS, htQueueOverwrite(queue, &first));
	TEST_ASSERT_EQUAL(0, htQueueSpacesAvailable(queue));
	TEST_ASSERT_EQUAL(htPASS, htQueueOverwrite(queue, &latest));
	TEST_ASSERT_EQUAL(1, htQueueMessagesWaiting(queue));
	TEST_ASSERT_EQUAL(htPASS, htQueueReceive(queue, &received, 0U));
	TEST_ASSERT_EQUAL(latest, received);
	TEST_ASSERT_EQUAL(1, htQueueSpacesAvailable(queue));
	htQueueDelete(queue);
}

static void test_queue_overwrite_rejects_invalid_queue_shapes(void)
{
	reset_test_state();
	QueueHandle_t multi_item_queue = htQueueCreate(2U, sizeof(int));
	QueueHandle_t semaphore = htQueueCreate(1U, 0U);
	int value = 7;

	TEST_ASSERT_NOT_NULL(multi_item_queue);
	TEST_ASSERT_NOT_NULL(semaphore);
	TEST_ASSERT_EQUAL(htFAIL, htQueueOverwrite(multi_item_queue, &value));
	TEST_ASSERT_EQUAL(htFAIL, htQueueOverwrite(semaphore, &value));
	TEST_ASSERT_EQUAL(0, htQueueSpacesAvailable(NULL));
	htQueueDelete(multi_item_queue);
	htQueueDelete(semaphore);
}

int main(void)
{
	UnityBegin("test_queue.c");
	RUN_TEST(test_queue_is_fifo_and_full_send_balances_critical_section);
	RUN_TEST(test_zero_sized_queue_honors_maximum_count);
	RUN_TEST(test_queue_create_returns_null_when_storage_allocation_fails);
	RUN_TEST(test_mutex_behaves_as_bounded_binary_token);
	RUN_TEST(test_full_queue_finite_wait_expires_and_unlinks_task);
	RUN_TEST(test_single_item_queue_overwrite_keeps_latest_value);
	RUN_TEST(test_queue_overwrite_rejects_invalid_queue_shapes);
	return UnityEnd();
}
