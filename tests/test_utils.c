#include "unity.h"

#include <stdint.h>
#include <string.h>

#include "httask.h"
#include "htutils.h"

htTCB_t *pxCurrentTCB = NULL;

static void test_stack_high_water_mark_counts_untouched_words(void)
{
	StackType_t stack[8];
	htTCB_t task;
	memset(&task, 0, sizeof(task));
	for (size_t index = 0U; index < 8U; index++) {
		stack[index] = htTASK_STACK_FILL_VALUE;
	}
	stack[5] = 0U;
	task.pxStack = stack;
	task.uxStackDepth = 8U;

	TEST_ASSERT_EQUAL(5, htGetTaskStackHighWaterMark(&task));
	pxCurrentTCB = &task;
	TEST_ASSERT_EQUAL(5, htGetTaskStackHighWaterMark(NULL));
}

static void test_stack_high_water_mark_rejects_invalid_task(void)
{
	htTCB_t task;
	memset(&task, 0, sizeof(task));
	pxCurrentTCB = NULL;

	TEST_ASSERT_EQUAL(0, htGetTaskStackHighWaterMark(NULL));
	TEST_ASSERT_EQUAL(0, htGetTaskStackHighWaterMark(&task));
}

static void test_time_conversions_do_not_overflow_intermediate_math(void)
{
	TEST_ASSERT_EQUAL(1, htTimeToTicks(1U));
	TEST_ASSERT_EQUAL(UINT32_MAX, htTimeToTicks(UINT32_MAX));
	TEST_ASSERT_EQUAL(UINT32_MAX, htTicksToTime(UINT32_MAX));
}

static void test_runtime_stats_report_unavailable_as_zero(void)
{
	TEST_ASSERT_EQUAL(0, htGetCPUUsage());
	TEST_ASSERT_EQUAL(0, htGetIdleRunTimePercent());
	TEST_ASSERT_EQUAL(0, htGetRunTimeCounter());
}

int main(void)
{
	UnityBegin("test_utils.c");
	RUN_TEST(test_stack_high_water_mark_counts_untouched_words);
	RUN_TEST(test_stack_high_water_mark_rejects_invalid_task);
	RUN_TEST(test_time_conversions_do_not_overflow_intermediate_math);
	RUN_TEST(test_runtime_stats_report_unavailable_as_zero);
	return UnityEnd();
}
