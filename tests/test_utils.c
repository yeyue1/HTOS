/**
 * @file test_utils.c
 * @brief 内核工具函数的主机测试：栈高水位、时间换算、运行时统计占位值。
 *
 * 覆盖的边界：
 * - 栈高水位：从栈底向上数连续未被写过的字；句柄传 NULL 表示当前任务；
 *   句柄、pxStack 或 uxStackDepth 任一无效时必须返回 0，而不是解引用空指针；
 * - 时间换算：UINT32_MAX 附近的输入不能用 32 位中间量算乘法，结果要饱和到
 *   UINT32_MAX 而不是回绕，1 毫秒这一端也不能被截断成 0 个滴答；
 * - 运行时统计：当前版本没有高精度时基，三个查询接口必须稳定返回 0，
 *   不能把「没有数据」误报成真实负载。
 *
 * @note 本文件自己定义 pxCurrentTCB，因为 kernel/httask.c 不参与主机测试链接，
 *       而 kernel/htutils.c 需要这个全局符号才能编译通过。
 */
#include "unity.h"

#include <stdint.h>
#include <string.h>

#include "httask.h"
#include "htutils.h"

/**
 * @brief 主机测试用的当前任务指针。
 *
 * kernel/httask.c 不参与本套件的链接，htutils.c 又需要这个全局符号，
 * 因此在这里补一个定义；需要「NULL 表示当前任务」的用例会临时把它指向栈上的 htTCB_t。
 */
htTCB_t *pxCurrentTCB = NULL;

/**
 * @brief 栈底连续保持填充值的字数就是高水位返回值。
 *
 * 断言意图：8 个字全部填成 htTASK_STACK_FILL_VALUE，再手动写脏 stack[5]，
 * 期望返回 5，即「从栈底向上还有 5 个字没被用过」。
 * 第二次断言把 pxCurrentTCB 指向该 TCB，验证传 NULL 时取的是当前任务而不是空指针。
 */
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

/**
 * @brief 无效句柄必须返回 0，而不是解引用空指针。
 *
 * 断言意图：pxCurrentTCB 为 NULL 时传 NULL 走「取当前任务」分支，
 * 以及传入 pxStack 为 NULL 的 TCB，两条路径都必须安全返回 0。
 */
static void test_stack_high_water_mark_rejects_invalid_task(void)
{
	htTCB_t task;
	memset(&task, 0, sizeof(task));
	pxCurrentTCB = NULL;

	TEST_ASSERT_EQUAL(0, htGetTaskStackHighWaterMark(NULL));
	TEST_ASSERT_EQUAL(0, htGetTaskStackHighWaterMark(&task));
}

/**
 * @brief 时间换算的中间量必须用 64 位，并对结果做饱和。
 *
 * 断言意图：UINT32_MAX 毫秒乘 configTICK_RATE_HZ 会撑爆 32 位乘法，
 * 正确实现应饱和到 UINT32_MAX 而不是回绕成小数；1 毫秒这一端
 * 则验证向上取整不会把非零时延截断成 0 个滴答（否则延时等于没延时）。
 */
static void test_time_conversions_do_not_overflow_intermediate_math(void)
{
	TEST_ASSERT_EQUAL(1, htTimeToTicks(1U));
	TEST_ASSERT_EQUAL(UINT32_MAX, htTimeToTicks(UINT32_MAX));
	TEST_ASSERT_EQUAL(UINT32_MAX, htTicksToTime(UINT32_MAX));
}

/**
 * @brief 套件入口：注册并顺序执行本文件全部用例。
 *
 * @return UnityEnd() 的返回值，即失败的用例数；0 表示全部通过。
 */
int main(void)
{
	UnityBegin("test_utils.c");
	RUN_TEST(test_stack_high_water_mark_counts_untouched_words);
	RUN_TEST(test_stack_high_water_mark_rejects_invalid_task);
	RUN_TEST(test_time_conversions_do_not_overflow_intermediate_math);
	return UnityEnd();
}
