#include "unity.h"

#include <stdlib.h>
#include <string.h>

#include "htlist.h"
#include "httask.h"

static UBaseType_t critical_nesting;
static UBaseType_t yield_count;
static UBaseType_t free_count;

extern void prvInitialiseTaskLists(void);
extern void htTaskCheckDelayedTasks(void);

void *htPortMalloc(size_t size)
{
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

int main(void)
{
	UnityBegin("test_task.c");
	RUN_TEST(test_task_query_and_priority_change_move_ready_item);
	RUN_TEST(test_suspend_and_resume_non_current_task);
	RUN_TEST(test_resume_from_isr_reports_higher_priority_task);
	RUN_TEST(test_delay_until_blocks_until_absolute_deadline);
	RUN_TEST(test_delay_until_reports_missed_deadline);
	RUN_TEST(test_tick_wrap_delays_task_until_overflow_list_expires);
	RUN_TEST(test_delete_non_current_task_releases_stack_and_tcb);
	return UnityEnd();
}
