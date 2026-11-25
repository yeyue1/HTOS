/**
 * @file tests/test_scheduler.c
 * @brief 调度器状态机（kernel/htscheduler.c）与内核入口（kernel/htos.c）的集成单元测试。
 *
 * 重点覆盖：
 * - htOSInit() 幂等性；
 * - htOSStart() 在无任务时安全拒绝启动；
 * - htSuspendScheduler() / htResumeScheduler() 嵌套计数、挂起期间阻断
 *   htTaskSwitchContext() 与 htSchedulerNeedsSwitch()，以及最外层恢复时补发 xYieldPending；
 * - htEnterCritical() / htExitCritical() 嵌套配对与下溢防护；
 * - htEnterCriticalFromISR() / htExitCriticalFromISR() 不干扰任务侧 uxCriticalNesting。
 */
#include "unity.h"

#include <string.h>

#include "htlist.h"
#include "htos.h"
#include "htschedpolicy.h"
#include "htscheduler.h"
#include "httask.h"
#include "htkernel_internal.h"

static UBaseType_t s_yield_count;
static UBaseType_t s_start_first_task_calls;
static UBaseType_t s_trace_calls;

/* MinGW PE-COFF 链接器不原生回退弱符号，测试侧提供强符号顺便计数断言。 */
void htTrace(const char *pcMessage)
{
	(void)pcMessage;
	s_trace_calls++;
}

void htTraceValue(const char *pcTag, uint32_t ulValue)
{
	(void)pcTag;
	(void)ulValue;
}

#if (configCHECK_FOR_STACK_OVERFLOW > 0)
void htStackOverflowHook(TaskHandle_t xTask, const char *pcTaskName)
{
	(void)xTask;
	(void)pcTaskName;
}
#endif

/* 捕获 htTaskYield() 与 vPortStartFirstTask()，避免主机侧触碰 Cortex-M 异常。 */
void htHostTestYieldHook(void)
{
	s_yield_count++;
}

void vPortStartFirstTask(void)
{
	s_start_first_task_calls++;
}

static void dummy_task_entry(void *pvParameters)
{
	(void)pvParameters;
	for (;;) {
	}
}

static void reset_scheduler_fixture(void)
{
	s_yield_count = 0U;
	s_start_first_task_calls = 0U;
	s_trace_calls = 0U;
	pxCurrentTCB = NULL;
	uxCurrentNumberOfTasks = 0U;
	xTickCount = 0U;
	htSchedulerInit();
	(void)htSchedSetPolicy(&htSchedPolicyFixedPriority);
}

static void test_os_init_is_idempotent_and_start_requires_task(void)
{
	reset_scheduler_fixture();

	TEST_ASSERT_EQUAL(0, htOSInit());
	TEST_ASSERT_EQUAL(0, htOSInit());
	TEST_ASSERT_EQUAL(htFALSE, htSchedulerIsStarted());

	/* 无任务时启动必须失败，不能把系统误标为运行中。 */
	TEST_ASSERT_EQUAL(-1, htOSStart());
	TEST_ASSERT_EQUAL(0U, s_start_first_task_calls);
	TEST_ASSERT_EQUAL(htFALSE, htSchedulerIsStarted());
}

static void test_critical_section_nesting_and_isr_isolation(void)
{
	reset_scheduler_fixture();

	TEST_ASSERT_EQUAL(0U, uxCriticalNesting);
	htEnterCritical();
	TEST_ASSERT_EQUAL(1U, uxCriticalNesting);
	htEnterCritical();
	TEST_ASSERT_EQUAL(2U, uxCriticalNesting);

	/* ISR 临界区不得修改任务侧嵌套计数。 */
	const UBaseType_t uxSaved = htEnterCriticalFromISR();
	TEST_ASSERT_EQUAL(2U, uxCriticalNesting);
	htExitCriticalFromISR(uxSaved);
	TEST_ASSERT_EQUAL(2U, uxCriticalNesting);

	htExitCritical();
	TEST_ASSERT_EQUAL(1U, uxCriticalNesting);
	htExitCritical();
	TEST_ASSERT_EQUAL(0U, uxCriticalNesting);

	/* 额外退出不得下溢。 */
	htExitCritical();
	TEST_ASSERT_EQUAL(0U, uxCriticalNesting);
}

static void test_suspend_scheduler_blocks_switch_and_replays_pending_yield(void)
{
	static StackType_t low_stack[configMINIMAL_STACK_SIZE];
	static StackType_t high_stack[configMINIMAL_STACK_SIZE];
	static htTCB_t low_tcb;
	static htTCB_t high_tcb;

	reset_scheduler_fixture();
	TaskHandle_t low =
			htTaskCreateStatic(dummy_task_entry, "low", configMINIMAL_STACK_SIZE, NULL, 1U, low_stack, &low_tcb);
	TaskHandle_t high =
			htTaskCreateStatic(dummy_task_entry, "high", configMINIMAL_STACK_SIZE, NULL, 3U, high_stack, &high_tcb);
	TEST_ASSERT_NOT_NULL(low);
	TEST_ASSERT_NOT_NULL(high);

	/* 启动调度器（主机桩 vPortStartFirstTask 会直接返回）：高优先级任务应被选为当前任务。 */
	htStartScheduler();
	TEST_ASSERT_EQUAL(1U, s_start_first_task_calls);
	TEST_ASSERT_EQUAL(htTRUE, htSchedulerIsStarted());
	TEST_ASSERT(pxCurrentTCB == &high_tcb);

	/* 模拟当前正在运行低优先级任务，同时高优先级任务处于就绪态。 */
	pxCurrentTCB = &low_tcb;
	s_yield_count = 0U;

	/* 嵌套挂起两层调度器。 */
	htSuspendScheduler();
	htSuspendScheduler();

	/* 挂起期间：htSchedulerNeedsSwitch() 必须返回 htFALSE，但要记录待补切换。 */
	TEST_ASSERT_EQUAL(htFALSE, htSchedulerNeedsSwitch());

	/* 挂起期间：显式 htTaskSwitchContext() 也必须拒绝切换当前任务。 */
	htTaskSwitchContext();
	TEST_ASSERT(pxCurrentTCB == &low_tcb);

	/* 仅恢复第一层：仍处于挂起态，不得触发补切。 */
	htResumeScheduler();
	TEST_ASSERT_EQUAL(0U, s_yield_count);
	htTaskSwitchContext();
	TEST_ASSERT(pxCurrentTCB == &low_tcb);

	/* 最外层恢复：必须补发一次 htTaskYield()，且恢复后可正常切到高优先级任务。 */
	htResumeScheduler();
	TEST_ASSERT_EQUAL(1U, s_yield_count);
	TEST_ASSERT_EQUAL(htTRUE, htSchedulerNeedsSwitch());
	htTaskSwitchContext();
	TEST_ASSERT(pxCurrentTCB == &high_tcb);
	TEST_ASSERT_EQUAL(0U, uxCriticalNesting);
}

int main(void)
{
	UnityBegin("test_scheduler.c");
	RUN_TEST(test_os_init_is_idempotent_and_start_requires_task);
	RUN_TEST(test_critical_section_nesting_and_isr_isolation);
	RUN_TEST(test_suspend_scheduler_blocks_switch_and_replays_pending_yield);
	return UnityEnd();
}
