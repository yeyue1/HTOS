/**
 * @file tests/test_watchdog.c
 * @brief 任务心跳监督器固定容量与窗口语义测试。
 */
#include "unity.h"
#include <string.h>
#include "htwatchdog.h"

static htTCB_t tasks[2];
htTCB_t *pxCurrentTCB;
static UBaseType_t critical_nesting;

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
TaskHandle_t htTaskGetCurrentTaskHandle(void)
{
	return pxCurrentTCB;
}

static void setup(void)
{
	memset(tasks, 0, sizeof(tasks));
	pxCurrentTCB = NULL;
	critical_nesting = 0U;
	htWatchdogReset();
}

static void test_empty_and_capacity(void)
{
	UBaseType_t i;
	setup();
	TEST_ASSERT_EQUAL(htFALSE, htWatchdogCheck());
	TEST_ASSERT_EQUAL(htFAIL, htWatchdogRegister(NULL));
	for (i = 0U; i < (UBaseType_t)configWATCHDOG_MAX_TASKS; i++) {
		TEST_ASSERT_EQUAL(htPASS, htWatchdogRegister((TaskHandle_t)(uintptr_t)(i + 1U)));
	}
	TEST_ASSERT_EQUAL(htFAIL, htWatchdogRegister((TaskHandle_t)(uintptr_t)99U));
	TEST_ASSERT_EQUAL(0, critical_nesting);
}

static void test_baseline_and_all_tasks_must_advance(void)
{
	setup();
	TEST_ASSERT_EQUAL(htPASS, htWatchdogRegister(&tasks[0]));
	TEST_ASSERT_EQUAL(htPASS, htWatchdogRegister(&tasks[1]));
	TEST_ASSERT_EQUAL(htFAIL, htWatchdogRegister(&tasks[0]));
	pxCurrentTCB = &tasks[0];
	TEST_ASSERT_EQUAL(htPASS, htWatchdogHeartbeat());
	pxCurrentTCB = &tasks[1];
	TEST_ASSERT_EQUAL(htPASS, htWatchdogHeartbeat());
	TEST_ASSERT_EQUAL(htFALSE, htWatchdogCheck());
	pxCurrentTCB = &tasks[0];
	TEST_ASSERT_EQUAL(htPASS, htWatchdogHeartbeat());
	pxCurrentTCB = &tasks[1];
	TEST_ASSERT_EQUAL(htPASS, htWatchdogHeartbeat());
	TEST_ASSERT_EQUAL(htTRUE, htWatchdogCheck());
	pxCurrentTCB = &tasks[0];
	TEST_ASSERT_EQUAL(htPASS, htWatchdogHeartbeat());
	TEST_ASSERT_EQUAL(htFALSE, htWatchdogCheck());
	pxCurrentTCB = &tasks[1];
	TEST_ASSERT_EQUAL(htPASS, htWatchdogHeartbeat());
	TEST_ASSERT_EQUAL(htTRUE, htWatchdogCheck());
	TEST_ASSERT_EQUAL(0, critical_nesting);
}

static void test_unregister_resets_baseline(void)
{
	setup();
	TEST_ASSERT_EQUAL(htPASS, htWatchdogRegister(&tasks[0]));
	TEST_ASSERT_EQUAL(htFALSE, htWatchdogCheck());
	pxCurrentTCB = &tasks[0];
	TEST_ASSERT_EQUAL(htPASS, htWatchdogHeartbeat());
	TEST_ASSERT_EQUAL(htTRUE, htWatchdogCheck());
	TEST_ASSERT_EQUAL(htPASS, htWatchdogUnregister(&tasks[0]));
	TEST_ASSERT_EQUAL(htFALSE, htWatchdogCheck());
	TEST_ASSERT_EQUAL(htFAIL, htWatchdogUnregister(&tasks[0]));
	TEST_ASSERT_EQUAL(0, critical_nesting);
}

int main(void)
{
	UnityBegin("test_watchdog.c");
	RUN_TEST(test_empty_and_capacity);
	RUN_TEST(test_baseline_and_all_tasks_must_advance);
	RUN_TEST(test_unregister_resets_baseline);
	return UnityEnd();
}
