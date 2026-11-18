#include "unity.h"

#include <stdlib.h>
#include <string.h>

#include "htlist.h"
#include "httask.h"

static UBaseType_t critical_nesting;
static UBaseType_t yield_count;
static UBaseType_t free_count;
static UBaseType_t allocation_count;
static TickType_t ticks_advanced_on_yield;

#if (configCHECK_FOR_STACK_OVERFLOW > 0)
/* 强符号：接管内核的弱符号默认停机，让测试能断言钩子被调用。 */
static UBaseType_t overflow_calls;
static TaskHandle_t overflowing_task;
static char overflowing_name[configMAX_TASK_NAME_LEN];

void htStackOverflowHook(TaskHandle_t xTask, const char *pcTaskName)
{
	size_t length = 0U;
	overflow_calls++;
	overflowing_task = xTask;
	while (length < (sizeof(overflowing_name) - 1U) && pcTaskName[length] != '\0') {
		overflowing_name[length] = pcTaskName[length];
		length++;
	}
	overflowing_name[length] = '\0';
}
#endif

extern void prvInitialiseTaskLists(void);
extern void htTaskCheckDelayedTasks(void);

void *htPortMalloc(size_t size)
{
	allocation_count++;
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
	critical_nesting++;
}

void htExitCritical(void)
{
	if (critical_nesting > 0U) {
		critical_nesting--;
	}
}

BaseType_t htSchedulerNeedsSwitch(void)
{
	return htFALSE;
}

void vPortStartFirstTask(void) {}

void htStartScheduler(void) {}

void htHostTestYieldHook(void)
{
	yield_count++;
	if (ticks_advanced_on_yield > 0U) {
		xTickCount += ticks_advanced_on_yield;
		ticks_advanced_on_yield = 0U;
		htTaskCheckDelayedTasks();
	}
}

static void reset_test_state(void)
{
	prvInitialiseTaskLists();
	pxCurrentTCB = NULL;
	uxCurrentNumberOfTasks = 0U;
	xTickCount = 0U;
	critical_nesting = 0U;
	yield_count = 0U;
	free_count = 0U;
	allocation_count = 0U;
	ticks_advanced_on_yield = 0U;
}

static void dummy_task(void *parameters)
{
	(void)parameters;
}

static void initialise_task(htTCB_t *task, const char *name, UBaseType_t priority)
{
	memset(task, 0, sizeof(*task));
	htListItemInit(&(task->xStateListItem));
	htListItemInit(&(task->xEventListItem));
	htListItemInit(&(task->xAllListItem));
	htListSetItemOwner(&(task->xStateListItem), task);
	htListSetItemOwner(&(task->xEventListItem), task);
	htListSetItemOwner(&(task->xAllListItem), task);
	task->uxPriority = priority;
	task->uxBasePriority = priority;
	task->uxTaskState = HT_TASK_READY;
	size_t name_length = 0U;
	while (name_length < configMAX_TASK_NAME_LEN - 1U && name[name_length] != '\0') {
		name_length++;
	}
	memcpy(task->pcTaskName, name, name_length);
	task->pcTaskName[name_length] = '\0';
	htListInsertEnd(&(pxReadyTasksLists[priority]), &(task->xStateListItem));
	htListInsertEnd(&pxAllocatedTasksList, &(task->xAllListItem));
}

static void test_task_query_and_priority_change_move_ready_item(void)
{
	reset_test_state();
	htTCB_t current;
	htTCB_t worker;
	initialise_task(&current, "current", 3U);
	initialise_task(&worker, "worker", 1U);
	pxCurrentTCB = &current;

	TEST_ASSERT_EQUAL(3, htTaskPriorityGet(NULL));
	TEST_ASSERT_EQUAL(1, htTaskPriorityGet(&worker));
	TEST_ASSERT(htTaskGetCurrentTaskHandle() == &current);
	TEST_ASSERT(strcmp("worker", htTaskGetName(&worker)) == 0);
	TEST_ASSERT_EQUAL(HT_TASK_READY, htTaskGetState(&worker));

	htTaskPrioritySet(&worker, 2U);
	TEST_ASSERT_EQUAL(2, worker.uxPriority);
	TEST_ASSERT_EQUAL(2, worker.uxBasePriority);
	TEST_ASSERT(worker.xStateListItem.pxContainer == &pxReadyTasksLists[2]);
	TEST_ASSERT_EQUAL(0, htListGetCurrentNumberOfItems(&pxReadyTasksLists[1]));
	TEST_ASSERT_EQUAL(0, yield_count);
	TEST_ASSERT_EQUAL(0, critical_nesting);
}

static void test_resume_from_isr_reports_higher_priority_task(void)
{
	reset_test_state();
	htTCB_t current;
	htTCB_t worker;
	initialise_task(&current, "current", 1U);
	initialise_task(&worker, "worker", 3U);
	pxCurrentTCB = &current;

	htTaskSuspend(&worker);
	TEST_ASSERT_EQUAL(htTRUE, htTaskResumeFromISR(&worker));
	TEST_ASSERT_EQUAL(HT_TASK_READY, worker.uxTaskState);
	TEST_ASSERT(worker.xStateListItem.pxContainer == &pxReadyTasksLists[3]);
	TEST_ASSERT_EQUAL(0, yield_count);
}

static void test_suspend_and_resume_non_current_task(void)
{
	reset_test_state();
	htTCB_t current;
	htTCB_t worker;
	initialise_task(&current, "current", 3U);
	initialise_task(&worker, "worker", 1U);
	pxCurrentTCB = &current;

	htTaskSuspend(&worker);
	TEST_ASSERT_EQUAL(HT_TASK_SUSPENDED, htTaskGetState(&worker));
	TEST_ASSERT_EQUAL(0, htListGetCurrentNumberOfItems(&pxReadyTasksLists[1]));
	htTaskResume(&worker);
	TEST_ASSERT_EQUAL(HT_TASK_READY, htTaskGetState(&worker));
	TEST_ASSERT(worker.xStateListItem.pxContainer == &pxReadyTasksLists[1]);
	TEST_ASSERT_EQUAL(0, yield_count);
	TEST_ASSERT_EQUAL(0, critical_nesting);
}

static void test_delay_until_blocks_until_absolute_deadline(void)
{
	reset_test_state();
	htTCB_t task;
	TickType_t previous_wake = 10U;
	initialise_task(&task, "periodic", 1U);
	pxCurrentTCB = &task;
	xTickCount = 10U;

	TEST_ASSERT_EQUAL(htPASS, htTaskDelayUntil(&previous_wake, 5U));
	TEST_ASSERT_EQUAL(15, previous_wake);
	TEST_ASSERT_EQUAL(HT_TASK_BLOCKED, task.uxTaskState);
	TEST_ASSERT_EQUAL(15, htListGetItemValue(&task.xStateListItem));
	TEST_ASSERT_EQUAL(1, yield_count);

	xTickCount = 15U;
	htTaskCheckDelayedTasks();
	TEST_ASSERT_EQUAL(HT_TASK_READY, task.uxTaskState);
	TEST_ASSERT(task.xStateListItem.pxContainer == &pxReadyTasksLists[1]);
}

static void test_delay_until_reports_missed_deadline(void)
{
	reset_test_state();
	htTCB_t task;
	TickType_t previous_wake = 10U;
	initialise_task(&task, "late", 1U);
	pxCurrentTCB = &task;
	xTickCount = 20U;

	TEST_ASSERT_EQUAL(htFAIL, htTaskDelayUntil(&previous_wake, 5U));
	TEST_ASSERT_EQUAL(15, previous_wake);
	TEST_ASSERT_EQUAL(HT_TASK_READY, task.uxTaskState);
	TEST_ASSERT_EQUAL(0, yield_count);
}

static void test_tick_wrap_delays_task_until_overflow_list_expires(void)
{
	reset_test_state();
	htTCB_t task;
	initialise_task(&task, "wrap", 1U);
	pxCurrentTCB = &task;
	xTickCount = UINT32_MAX - 1U;

	htTaskDelay(3U);
	TEST_ASSERT_EQUAL(HT_TASK_BLOCKED, task.uxTaskState);
	TEST_ASSERT(task.xStateListItem.pxContainer == pxOverflowDelayedTaskList);
	htTaskTickInc();
	htTaskTickInc();
	TEST_ASSERT_EQUAL(HT_TASK_BLOCKED, task.uxTaskState);
	htTaskTickInc();
	TEST_ASSERT_EQUAL(HT_TASK_READY, task.uxTaskState);
	TEST_ASSERT(task.xStateListItem.pxContainer == &pxReadyTasksLists[1]);
}

static void test_task_notify_wait_returns_pending_value_and_clears_requested_bits(void)
{
	reset_test_state();
	htTCB_t current;
	uint32_t notified_value = 0U;

	initialise_task(&current, "notify", 2U);
	pxCurrentTCB = &current;

	TEST_ASSERT_EQUAL(htPASS, htTaskNotify(&current, 0x05U, HT_NOTIFY_SET_BITS));
	TEST_ASSERT_EQUAL(htPASS, htTaskNotify(&current, 0x02U, HT_NOTIFY_SET_BITS));
	TEST_ASSERT_EQUAL(htPASS, htTaskNotifyWait(0x00U, 0x04U, &notified_value, 0U));
	TEST_ASSERT_EQUAL(7, (int)notified_value);
	TEST_ASSERT_EQUAL(3, (int)current.ulNotifiedValue);
	TEST_ASSERT_EQUAL(0, yield_count);
}

static void test_task_notify_wait_times_out_and_unlinks_blocked_task(void)
{
	reset_test_state();
	htTCB_t current;
	uint32_t notified_value = 0xA5A5A5A5U;

	initialise_task(&current, "waiter", 1U);
	pxCurrentTCB = &current;
	xTickCount = 10U;
	ticks_advanced_on_yield = 3U;

	TEST_ASSERT_EQUAL(htFAIL, htTaskNotifyWait(0x00U, 0x00U, &notified_value, 3U));
	TEST_ASSERT_EQUAL(13U, xTickCount);
	TEST_ASSERT_EQUAL(HT_TASK_READY, current.uxTaskState);
	TEST_ASSERT_NULL(current.xEventListItem.pxContainer);
	TEST_ASSERT(current.xStateListItem.pxContainer == &pxReadyTasksLists[1]);
	TEST_ASSERT_EQUAL(0, (int)notified_value);
}

static void test_task_notify_from_isr_sets_value_without_waking_higher_priority_task(void)
{
	reset_test_state();
	htTCB_t current;
	uint32_t notified_value = 0U;
	BaseType_t higher_priority_woken = htFALSE;

	initialise_task(&current, "current", 1U);
	pxCurrentTCB = &current;

	TEST_ASSERT_EQUAL(htPASS, htTaskNotifyFromISR(&current, 0x08U, HT_NOTIFY_SET_BITS, &higher_priority_woken));
	TEST_ASSERT_EQUAL(htFALSE, higher_priority_woken);
	TEST_ASSERT_EQUAL(htPASS, htTaskNotifyWait(0x00U, 0xFFFFFFFFU, &notified_value, 0U));
	TEST_ASSERT_EQUAL(8, (int)notified_value);
}

static void test_task_notify_counting_take_preserves_or_clears_count(void)
{
	reset_test_state();
	htTCB_t current;
	initialise_task(&current, "counter", 2U);
	pxCurrentTCB = &current;

	TEST_ASSERT_EQUAL(htPASS, htTaskNotifyGive(&current));
	TEST_ASSERT_EQUAL(htPASS, htTaskNotifyGive(&current));
	TEST_ASSERT_EQUAL(2, (int)htTaskNotifyTake(htFALSE, 0U));
	TEST_ASSERT_EQUAL(1, (int)current.ulNotifiedValue);
	TEST_ASSERT_EQUAL(1, (int)htTaskNotifyTake(htTRUE, 0U));
	TEST_ASSERT_EQUAL(0, (int)current.ulNotifiedValue);
	TEST_ASSERT_EQUAL(0, (int)htTaskNotifyTake(htTRUE, 0U));
}

static void test_task_notify_without_overwrite_rejects_pending_value(void)
{
	reset_test_state();
	htTCB_t current;
	uint32_t notified_value = 0U;
	initialise_task(&current, "overwrite", 2U);
	pxCurrentTCB = &current;

	TEST_ASSERT_EQUAL(htPASS, htTaskNotify(&current, 0x11U, HT_NOTIFY_SET_VALUE_WITHOUT_OVERWRITE));
	TEST_ASSERT_EQUAL(htFAIL, htTaskNotify(&current, 0x22U, HT_NOTIFY_SET_VALUE_WITHOUT_OVERWRITE));
	TEST_ASSERT_EQUAL(htPASS, htTaskNotifyWait(0U, UINT32_MAX, &notified_value, 0U));
	TEST_ASSERT_EQUAL(0x11, (int)notified_value);
}

static void test_task_notify_from_isr_wakes_higher_priority_waiter(void)
{
	reset_test_state();
	htTCB_t current;
	htTCB_t waiter;
	BaseType_t higher_priority_woken = htFALSE;
	initialise_task(&current, "current", 1U);
	initialise_task(&waiter, "waiter", 3U);
	pxCurrentTCB = &current;

	htListRemove(&(waiter.xStateListItem));
	waiter.uxTaskState = HT_TASK_BLOCKED;
	waiter.uxNotificationStatus = HT_NOTIFY_WAITING;

	TEST_ASSERT_EQUAL(htPASS, htTaskNotifyGiveFromISR(&waiter, &higher_priority_woken));
	TEST_ASSERT_EQUAL(htTRUE, higher_priority_woken);
	TEST_ASSERT_EQUAL(HT_TASK_READY, waiter.uxTaskState);
	TEST_ASSERT(waiter.xStateListItem.pxContainer == &pxReadyTasksLists[3]);
}

static void test_static_task_uses_caller_memory_and_is_not_freed(void)
{
	reset_test_state();
	htTCB_t current;
	htTCB_t task_buffer;
	StackType_t stack_buffer[configMINIMAL_STACK_SIZE];
	initialise_task(&current, "current", 3U);
	pxCurrentTCB = &current;
	uxCurrentNumberOfTasks = 1U;

	TaskHandle_t handle =
			htTaskCreateStatic(dummy_task, "static", configMINIMAL_STACK_SIZE, NULL, 1U, stack_buffer, &task_buffer);
	TEST_ASSERT(handle == &task_buffer);
	TEST_ASSERT_EQUAL(0, allocation_count);
	TEST_ASSERT_EQUAL(1, task_buffer.ucStaticallyAllocated);
	TEST_ASSERT(task_buffer.pxStack == stack_buffer);
	TEST_ASSERT_EQUAL(2, uxCurrentNumberOfTasks);

	htTaskDelete(handle);
	TEST_ASSERT_EQUAL(0, free_count);
	TEST_ASSERT_EQUAL(1, uxCurrentNumberOfTasks);
	TEST_ASSERT_EQUAL(HT_TASK_SUSPENDED, task_buffer.uxTaskState);
	TEST_ASSERT_NULL(task_buffer.xStateListItem.pxContainer);
	TEST_ASSERT_NULL(task_buffer.xAllListItem.pxContainer);
	TEST_ASSERT_NULL(htTaskCreateStatic(
			dummy_task, "small", configMINIMAL_STACK_SIZE - 1U, NULL, 1U, stack_buffer, &task_buffer));
}

static void test_delete_non_current_task_releases_stack_and_tcb(void)
{
	reset_test_state();
	htTCB_t current;
	initialise_task(&current, "current", 3U);
	pxCurrentTCB = &current;

	htTCB_t *worker = malloc(sizeof(*worker));
	TEST_ASSERT_NOT_NULL(worker);
	initialise_task(worker, "worker", 1U);
	worker->pxStack = malloc(32U * sizeof(StackType_t));
	TEST_ASSERT_NOT_NULL(worker->pxStack);
	uxCurrentNumberOfTasks = 2U;

	htTaskDelete(worker);
	TEST_ASSERT_EQUAL(2, free_count);
	TEST_ASSERT_EQUAL(1, uxCurrentNumberOfTasks);
	TEST_ASSERT_EQUAL(0, htListGetCurrentNumberOfItems(&pxReadyTasksLists[1]));
	TEST_ASSERT_EQUAL(1, htListGetCurrentNumberOfItems(&pxAllocatedTasksList));
}

#if (configCHECK_FOR_STACK_OVERFLOW > 0)
static void reset_overflow_recorder(void)
{
	overflow_calls = 0U;
	overflowing_task = NULL;
	overflowing_name[0] = '\0';
}

static void test_stack_sentinel_accepts_an_intact_stack(void)
{
	htTCB_t task;
	StackType_t stack[8];
	memset(&task, 0, sizeof(task));
	for (UBaseType_t index = 0U; index < 8U; index++) {
		stack[index] = htTASK_STACK_FILL_VALUE;
	}
	task.pxStack = stack;
	task.uxStackDepth = 8U;
	reset_overflow_recorder();

	htTaskCheckStackSentinel(&task);

	TEST_ASSERT_EQUAL(0, (int)overflow_calls);
	TEST_ASSERT_NULL(overflowing_task);
}

static void test_stack_sentinel_reports_a_corrupted_stack(void)
{
	htTCB_t task;
	StackType_t stack[8];
	memset(&task, 0, sizeof(task));
	for (UBaseType_t index = 0U; index < 8U; index++) {
		stack[index] = htTASK_STACK_FILL_VALUE;
	}
	memcpy(task.pcTaskName, "overflow", 9U);
	task.pxStack = stack;
	task.uxStackDepth = 8U;
	stack[0] = 0xDEADBEEFU; /* 栈底被写掉一个字 */
	reset_overflow_recorder();

	htTaskCheckStackSentinel(&task);

	TEST_ASSERT_EQUAL(1, (int)overflow_calls);
	TEST_ASSERT(overflowing_task == &task);
	TEST_ASSERT(strcmp(overflowing_name, "overflow") == 0);
}

static void test_stack_sentinel_skips_tasks_without_usable_storage(void)
{
	htTCB_t task;
	StackType_t stack[8];
	for (UBaseType_t index = 0U; index < 8U; index++) {
		stack[index] = htTASK_STACK_FILL_VALUE;
	}
	stack[0] = 0U;
	reset_overflow_recorder();

	memset(&task, 0, sizeof(task));
	htTaskCheckStackSentinel(&task); /* pxStack == NULL */
	task.pxStack = stack;
	task.uxStackDepth = 3U; /* 太短，不能检查 */
	htTaskCheckStackSentinel(&task);
	pxCurrentTCB = &task; /* NULL 表示当前任务 */
	task.uxStackDepth = 8U;
	htTaskCheckStackSentinel(NULL);

	TEST_ASSERT_EQUAL(1, (int)overflow_calls);
}
#endif

int main(void)
{
	UnityBegin("test_task.c");
	RUN_TEST(test_task_query_and_priority_change_move_ready_item);
	RUN_TEST(test_suspend_and_resume_non_current_task);
	RUN_TEST(test_resume_from_isr_reports_higher_priority_task);
	RUN_TEST(test_delay_until_blocks_until_absolute_deadline);
	RUN_TEST(test_delay_until_reports_missed_deadline);
	RUN_TEST(test_tick_wrap_delays_task_until_overflow_list_expires);
	RUN_TEST(test_task_notify_wait_returns_pending_value_and_clears_requested_bits);
	RUN_TEST(test_task_notify_wait_times_out_and_unlinks_blocked_task);
	RUN_TEST(test_task_notify_from_isr_sets_value_without_waking_higher_priority_task);
	RUN_TEST(test_task_notify_counting_take_preserves_or_clears_count);
	RUN_TEST(test_task_notify_without_overwrite_rejects_pending_value);
	RUN_TEST(test_task_notify_from_isr_wakes_higher_priority_waiter);
	RUN_TEST(test_static_task_uses_caller_memory_and_is_not_freed);
	RUN_TEST(test_delete_non_current_task_releases_stack_and_tcb);
#if (configCHECK_FOR_STACK_OVERFLOW > 0)
	RUN_TEST(test_stack_sentinel_accepts_an_intact_stack);
	RUN_TEST(test_stack_sentinel_reports_a_corrupted_stack);
	RUN_TEST(test_stack_sentinel_skips_tasks_without_usable_storage);
#endif
	return UnityEnd();
}
