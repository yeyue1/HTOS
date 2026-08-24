#include "unity.h"

#include <stdlib.h>
#include <string.h>

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
static size_t free_count;
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
	if (pointer != NULL) {
		free_count++;
		free(pointer);
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
	free_count = 0U;
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
	memset(task, 0, sizeof(*task));
	htListItemInit(&(task->xStateListItem));
	htListItemInit(&(task->xEventListItem));
	htListSetItemOwner(&(task->xStateListItem), task);
	htListSetItemOwner(&(task->xEventListItem), task);
	task->uxPriority = priority;
	task->uxBasePriority = priority;
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

static void test_counting_semaphore_honors_initial_and_maximum_counts(void)
{
	reset_test_state();
	SemaphoreHandle_t semaphore = htSemaphoreCreateCounting(3U, 2U);

	TEST_ASSERT_NOT_NULL(semaphore);
	TEST_ASSERT_EQUAL(2, htSemaphoreGetCount(semaphore));
	TEST_ASSERT_EQUAL(htPASS, htSemaphoreTake(semaphore, 0U));
	TEST_ASSERT_EQUAL(1, htSemaphoreGetCount(semaphore));
	TEST_ASSERT_EQUAL(htPASS, htSemaphoreGive(semaphore));
	TEST_ASSERT_EQUAL(2, htSemaphoreGetCount(semaphore));
	TEST_ASSERT_EQUAL(htPASS, htSemaphoreGive(semaphore));
	TEST_ASSERT_EQUAL(3, htSemaphoreGetCount(semaphore));
	TEST_ASSERT_EQUAL(htFAIL, htSemaphoreGive(semaphore));
	TEST_ASSERT_EQUAL(3, htSemaphoreGetCount(semaphore));
	htSemaphoreDelete(semaphore);
}

static void test_recursive_mutex_tracks_owner_depth_and_release(void)
{
	reset_test_state();
	htTCB_t owner = {0};
	SemaphoreHandle_t mutex = htSemaphoreCreateRecursiveMutex();
	htMutexHolder_t holder = {0};

	TEST_ASSERT_NOT_NULL(mutex);
	initialise_current_task(&owner, 2U);

	TEST_ASSERT_EQUAL(htPASS, htSemaphoreTakeRecursive(mutex, 0U));
	TEST_ASSERT_EQUAL(htPASS, htQueuePeek(mutex, &holder, 0U));
	TEST_ASSERT(holder.xTaskHandle == &owner);
	TEST_ASSERT_EQUAL(1, holder.uxRecursiveCallCount);

	TEST_ASSERT_EQUAL(htPASS, htSemaphoreTakeRecursive(mutex, 0U));
	TEST_ASSERT_EQUAL(htPASS, htQueuePeek(mutex, &holder, 0U));
	TEST_ASSERT_EQUAL(2, holder.uxRecursiveCallCount);

	TEST_ASSERT_EQUAL(htPASS, htSemaphoreGiveRecursive(mutex));
	TEST_ASSERT_EQUAL(htPASS, htQueuePeek(mutex, &holder, 0U));
	TEST_ASSERT_EQUAL(1, holder.uxRecursiveCallCount);
	TEST_ASSERT(holder.xTaskHandle == &owner);

	TEST_ASSERT_EQUAL(htPASS, htSemaphoreGiveRecursive(mutex));
	TEST_ASSERT_EQUAL(htPASS, htQueuePeek(mutex, &holder, 0U));
	TEST_ASSERT_EQUAL(0, holder.uxRecursiveCallCount);
	TEST_ASSERT_NULL(holder.xTaskHandle);
	htSemaphoreDelete(mutex);
}

static void test_recursive_mutex_inherits_priority_and_restores_after_release(void)
{
	reset_test_state();
	htTCB_t owner = {0};
	htTCB_t waiter = {0};
	SemaphoreHandle_t mutex = htSemaphoreCreateRecursiveMutex();
	htMutexHolder_t holder = {0};

	TEST_ASSERT_NOT_NULL(mutex);
	initialise_current_task(&owner, 0U);
	TEST_ASSERT_EQUAL(htPASS, htSemaphoreTakeRecursive(mutex, 0U));

	htListItemInit(&(waiter.xStateListItem));
	htListItemInit(&(waiter.xEventListItem));
	htListSetItemOwner(&(waiter.xStateListItem), &waiter);
	htListSetItemOwner(&(waiter.xEventListItem), &waiter);
	waiter.uxPriority = 3U;
	waiter.uxBasePriority = 3U;
	waiter.uxTaskState = HT_TASK_READY;
	htListInsertEnd(&(pxReadyTasksLists[3]), &(waiter.xStateListItem));
	pxCurrentTCB = &waiter;
	ticks_advanced_on_yield = 2U;

	TEST_ASSERT_EQUAL(htFAIL, htSemaphoreTakeRecursive(mutex, 2U));
	TEST_ASSERT_EQUAL(2U, xTickCount);
	TEST_ASSERT_EQUAL(0U, owner.uxBasePriority);
	TEST_ASSERT_EQUAL(0U, owner.uxPriority);

	pxCurrentTCB = &owner;
	TEST_ASSERT_EQUAL(htPASS, htQueuePeek(mutex, &holder, 0U));
	TEST_ASSERT_EQUAL(UINT32_MAX, holder.uxOriginalPriority);
	TEST_ASSERT_EQUAL(htPASS, htSemaphoreGiveRecursive(mutex));
	TEST_ASSERT_EQUAL(0U, owner.uxPriority);
	htSemaphoreDelete(mutex);
}

static void test_recursive_mutex_try_take_does_not_inherit_priority(void)
{
	reset_test_state();
	htTCB_t owner = {0};
	htTCB_t contender = {0};
	SemaphoreHandle_t mutex = htSemaphoreCreateRecursiveMutex();

	TEST_ASSERT_NOT_NULL(mutex);
	initialise_current_task(&owner, 0U);
	TEST_ASSERT_EQUAL(htPASS, htSemaphoreTakeRecursive(mutex, 0U));

	initialise_current_task(&contender, 3U);
	TEST_ASSERT_EQUAL(htFAIL, htSemaphoreTakeRecursive(mutex, 0U));
	TEST_ASSERT_EQUAL(0U, owner.uxPriority);

	pxCurrentTCB = &owner;
	TEST_ASSERT_EQUAL(htPASS, htSemaphoreGiveRecursive(mutex));
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

static void test_static_queue_uses_caller_storage_without_heap_operations(void)
{
	reset_test_state();
	htQUEUE_t queue_buffer;
	uint8_t storage[2U * sizeof(int)];
	int first = 11;
	int second = 22;
	int received = 0;

	QueueHandle_t queue = htQueueCreateStatic(2U, sizeof(int), storage, &queue_buffer);
	TEST_ASSERT(queue == &queue_buffer);
	TEST_ASSERT_EQUAL(0, allocation_count);
	TEST_ASSERT_EQUAL(1, queue_buffer.ucStaticallyAllocated);
	TEST_ASSERT_EQUAL(htPASS, htQueueSend(queue, &first, 0U));
	TEST_ASSERT_EQUAL(htPASS, htQueueSend(queue, &second, 0U));
	TEST_ASSERT_EQUAL(htPASS, htQueueReceive(queue, &received, 0U));
	TEST_ASSERT_EQUAL(first, received);

	htQueueDelete(queue);
	TEST_ASSERT_EQUAL(0, free_count);
	TEST_ASSERT_EQUAL(0, queue_buffer.uxLength);
	TEST_ASSERT_NULL(htQueueCreateStatic(2U, sizeof(int), NULL, &queue_buffer));
}

static void test_static_binary_semaphore_needs_no_storage(void)
{
	reset_test_state();
	htQUEUE_t semaphore_buffer;
	SemaphoreHandle_t semaphore = htSemaphoreCreateBinaryStatic(&semaphore_buffer);

	TEST_ASSERT(semaphore == &semaphore_buffer);
	TEST_ASSERT_EQUAL(0, allocation_count);
	TEST_ASSERT_EQUAL(htPASS, htSemaphoreGive(semaphore));
	TEST_ASSERT_EQUAL(htPASS, htSemaphoreTake(semaphore, 0U));
	htSemaphoreDelete(semaphore);
	TEST_ASSERT_EQUAL(0, free_count);
}

static void test_static_counting_semaphore_and_mutex_use_no_heap(void)
{
	reset_test_state();
	htQUEUE_t counting_buffer;
	htQUEUE_t mutex_buffer;
	SemaphoreHandle_t counting = htSemaphoreCreateCountingStatic(3U, 2U, &counting_buffer);
	SemaphoreHandle_t mutex = htSemaphoreCreateMutexStatic(&mutex_buffer);

	TEST_ASSERT_NOT_NULL(counting);
	TEST_ASSERT_NOT_NULL(mutex);
	TEST_ASSERT_EQUAL(0, allocation_count);
	TEST_ASSERT_EQUAL(2, htSemaphoreGetCount(counting));
	TEST_ASSERT_EQUAL(htPASS, htSemaphoreTake(counting, 0U));
	TEST_ASSERT_EQUAL(1, htSemaphoreGetCount(counting));
	TEST_ASSERT_EQUAL(htPASS, htSemaphoreTakeMutex(mutex, 0U));
	TEST_ASSERT_EQUAL(htFAIL, htSemaphoreTakeMutex(mutex, 0U));
	TEST_ASSERT_EQUAL(htPASS, htSemaphoreGiveMutex(mutex));
	TEST_ASSERT_EQUAL(htFAIL, htSemaphoreGiveMutex(mutex));

	htSemaphoreDelete(counting);
	htSemaphoreDelete(mutex);
	TEST_ASSERT_EQUAL(0, free_count);
	TEST_ASSERT_NULL(htSemaphoreCreateCountingStatic(2U, 3U, &counting_buffer));
}

int main(void)
{
	UnityBegin("test_queue.c");
	RUN_TEST(test_queue_is_fifo_and_full_send_balances_critical_section);
	RUN_TEST(test_zero_sized_queue_honors_maximum_count);
	RUN_TEST(test_queue_create_returns_null_when_storage_allocation_fails);
	RUN_TEST(test_mutex_behaves_as_bounded_binary_token);
	RUN_TEST(test_counting_semaphore_honors_initial_and_maximum_counts);
	RUN_TEST(test_recursive_mutex_tracks_owner_depth_and_release);
	RUN_TEST(test_recursive_mutex_inherits_priority_and_restores_after_release);
	RUN_TEST(test_recursive_mutex_try_take_does_not_inherit_priority);
	RUN_TEST(test_full_queue_finite_wait_expires_and_unlinks_task);
	RUN_TEST(test_single_item_queue_overwrite_keeps_latest_value);
	RUN_TEST(test_queue_overwrite_rejects_invalid_queue_shapes);
	RUN_TEST(test_static_queue_uses_caller_storage_without_heap_operations);
	RUN_TEST(test_static_binary_semaphore_needs_no_storage);
	RUN_TEST(test_static_counting_semaphore_and_mutex_use_no_heap);
	return UnityEnd();
}
