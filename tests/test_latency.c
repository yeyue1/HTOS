/**
 * @file test_latency.c
 * @brief 实时性仪表的主机单元测试。
 *
 * 主机上没有真实中断，也没有 DWT 硬件，但 tests/stubs/core_cm3.h 把 DWT->CYCCNT 落到了
 * 一块普通内存上：用例自己写这个计数器，就能在没有时钟的情况下构造出确定的周期差，
 * 从而验证"结算、取峰值、记调用点、分档、复位"这些纯逻辑。
 *
 * @note 用例顺序有意义：test_latency_init_configures_dwt 必须第一个跑，它是唯一一个
 *       在 s_ucLatencyReady 仍为 0 时观察 DWT 配置动作的用例；其余用例依赖已就绪。
 */
#include "unity.h"

#include "htlatency.h"
#include "htscheduler.h"

#include "core_cm3.h"

/** 本仓库的 unity.h 是精简版，没有"不等"与"直接通过"宏，这里补两条。 */
#define TEST_ASSERT_NOT_EQUAL(expected, actual) TEST_ASSERT((expected) != (actual))
#define TEST_ASSERT_PASS() TEST_ASSERT(1)

/** @brief 被测模块用到的临界区嵌套计数；主机侧由本文件提供定义。 */
volatile UBaseType_t uxCriticalNesting = 0U;

/** 一个微秒对应的周期数，与仪表内部换算一致。 */
#define TEST_CYCLES_PER_US ((uint32_t)HT_LATENCY_CYCLES_PER_US)

/**
 * @brief 构造一段"耗时 n 微秒"的临界区。
 *
 * @param[in] ulMicroseconds 期望的耗时。
 * @param[in] ulCaller      伪装的调用点地址。
 * @note 主机侧 cpsid/cpsie 是空操作，嵌套计数由用例亲手维护。
 */
static void prvRunCriticalSection(uint32_t ulMicroseconds, uint32_t ulCaller)
{
	uint32_t ulStart = htHostDwt.CYCCNT;
	uint32_t ulEnd;

	/* 仪表初始化是幂等的；首个用例之外的用例靠这一行保证 ready。 */
	htLatencyInit();
	uxCriticalNesting = 1U;
	htLatencyCriticalEnter(ulCaller);
	ulEnd = ulStart + (ulMicroseconds * TEST_CYCLES_PER_US);
	htHostDwt.CYCCNT = ulEnd;
	htLatencyCriticalExit();
	uxCriticalNesting = 0U;
}

/** @brief 每个用例前把计数器、嵌套计数与统计一起恢复到已知状态。 */
static void setup(void)
{
	/* 计数器可能停在上一个用例的末尾，先归零再清统计，避免跨用例串味。 */
	htHostDwt.CYCCNT = 0U;
	uxCriticalNesting = 0U;
	htLatencyReset();
}

/**
 * @brief 初始化要打开跟踪总开关、使能周期计数器并把计数器清零。
 */
static void test_latency_init_configures_dwt(void)
{
	/* 回到"从未初始化"的状态：清掉寄存器，让本次调用真的走一遍配置路径。 */
	htHostCoreDebug.DEMCR = 0U;
	htHostDwt.CTRL = 0U;
	htHostDwt.CYCCNT = 0xDEADBEEFUL;

	htLatencyInit();

	TEST_ASSERT_NOT_EQUAL(0U, (htHostCoreDebug.DEMCR & CoreDebug_DEMCR_TRCENA_Msk));
	TEST_ASSERT_NOT_EQUAL(0U, (htHostDwt.CTRL & DWT_CTRL_CYCCNTENA_Msk));
	TEST_ASSERT_EQUAL(0U, htHostDwt.CYCCNT);

	/* 第二次调用必须原样返回，不能把已经跑起来的计数器再次清零。 */
	htHostDwt.CYCCNT = 1234U;
	htLatencyInit();
	TEST_ASSERT_EQUAL(1234U, htHostDwt.CYCCNT);
}

/**
 * @brief 一次最外层临界区：次数、总时长、峰值、调用点、分档都要对上。
 */
static void test_latency_single_critical_section(void)
{
	htLatencySnapshot_t xSnapshot;

	prvRunCriticalSection(5U, 0x08001234UL);
	htLatencyGetSnapshot(&xSnapshot);

	TEST_ASSERT_EQUAL(1U, xSnapshot.ulCriticalCount);
	TEST_ASSERT_EQUAL((5U * TEST_CYCLES_PER_US), xSnapshot.ulCriticalMaxCycles);
	TEST_ASSERT_EQUAL((5U * TEST_CYCLES_PER_US), xSnapshot.ullCriticalTotalCycles);
	TEST_ASSERT_EQUAL(0x08001234UL, xSnapshot.ulCriticalCaller);
	TEST_ASSERT_EQUAL(0U, xSnapshot.aulCriticalBuckets[2]); /* <= 4 us */
	TEST_ASSERT_EQUAL(1U, xSnapshot.aulCriticalBuckets[3]); /* <= 8 us */
}

/**
 * @brief 嵌套临界区只算最外层：内层入口不能覆盖起点，内层出口不能提前结算。
 */
static void test_latency_nested_sections_count_once(void)
{
	htLatencySnapshot_t xSnapshot;

	uxCriticalNesting = 1U;
	htHostDwt.CYCCNT = 0U;
	htLatencyCriticalEnter(0xAAU);

	uxCriticalNesting = 2U;
	htHostDwt.CYCCNT = 3U * TEST_CYCLES_PER_US; /* 内层入口若覆盖起点，总时长会变短 */
	htLatencyCriticalEnter(0xBBU);

	htHostDwt.CYCCNT = 10U * TEST_CYCLES_PER_US;
	htLatencyCriticalExit(); /* nesting == 2，忽略 */

	uxCriticalNesting = 1U;
	htLatencyCriticalExit(); /* 最外层，结算 */
	uxCriticalNesting = 0U;

	htLatencyGetSnapshot(&xSnapshot);
	TEST_ASSERT_EQUAL(1U, xSnapshot.ulCriticalCount);
	TEST_ASSERT_EQUAL((10U * TEST_CYCLES_PER_US), xSnapshot.ulCriticalMaxCycles);
	TEST_ASSERT_EQUAL(0xAAU, xSnapshot.ulCriticalCaller);
}

/**
 * @brief 峰值与调用点必须成对更新：较短的一段不能覆盖峰值那次的地址。
 */
static void test_latency_peak_keeps_its_call_site(void)
{
	htLatencySnapshot_t xSnapshot;

	prvRunCriticalSection(2U, 0x1000U);
	prvRunCriticalSection(20U, 0x2000U);
	prvRunCriticalSection(3U, 0x3000U);

	htLatencyGetSnapshot(&xSnapshot);
	TEST_ASSERT_EQUAL(3U, xSnapshot.ulCriticalCount);
	TEST_ASSERT_EQUAL((20U * TEST_CYCLES_PER_US), xSnapshot.ulCriticalMaxCycles);
	TEST_ASSERT_EQUAL(0x2000U, xSnapshot.ulCriticalCaller);
	TEST_ASSERT_EQUAL((25U * TEST_CYCLES_PER_US), xSnapshot.ullCriticalTotalCycles);
}

/**
 * @brief 分档边界：1 us 落第一档，超过 64 us 落溢出档。
 */
static void test_latency_buckets_bound_and_overflow(void)
{
	htLatencySnapshot_t xSnapshot;

	prvRunCriticalSection(1U, 0x4000U);
	prvRunCriticalSection(100U, 0x5000U);

	htLatencyGetSnapshot(&xSnapshot);
	TEST_ASSERT_EQUAL(1U, xSnapshot.aulCriticalBuckets[0]); /* <= 1 us */
	TEST_ASSERT_EQUAL(0U, xSnapshot.aulCriticalBuckets[1]); /* <= 2 us */
	TEST_ASSERT_EQUAL(1U, xSnapshot.aulCriticalBuckets[HT_LATENCY_BUCKET_COUNT - 1U]);
}

/**
 * @brief 滴答中断窗口：次数与总时长累加，峰值只升不降。
 */
static void test_latency_tick_window(void)
{
	htLatencySnapshot_t xSnapshot;

	htLatencyInit();
	htHostDwt.CYCCNT = 0U;
	htLatencyTickBegin();
	htHostDwt.CYCCNT = 3U * TEST_CYCLES_PER_US;
	htLatencyTickEnd();

	htLatencyTickBegin();
	htHostDwt.CYCCNT = (3U + 1U) * TEST_CYCLES_PER_US;
	htLatencyTickEnd();

	htLatencyGetSnapshot(&xSnapshot);
	TEST_ASSERT_EQUAL(2U, xSnapshot.ulTickCount);
	TEST_ASSERT_EQUAL((3U * TEST_CYCLES_PER_US), xSnapshot.ulTickMaxCycles);
	TEST_ASSERT_EQUAL((4U * TEST_CYCLES_PER_US), xSnapshot.ullTickTotalCycles);
}

/**
 * @brief 复位清空全部统计，但仪表仍然可用（ready 不能跟着被清掉）。
 */
static void test_latency_reset_clears_stats(void)
{
	htLatencySnapshot_t xSnapshot;

	prvRunCriticalSection(6U, 0x6000U);
	htLatencyReset();
	htLatencyGetSnapshot(&xSnapshot);

	TEST_ASSERT_EQUAL(0U, xSnapshot.ulCriticalCount);
	TEST_ASSERT_EQUAL(0U, xSnapshot.ulCriticalMaxCycles);
	TEST_ASSERT_EQUAL(0U, xSnapshot.ulCriticalCaller);
	TEST_ASSERT_EQUAL(0U, xSnapshot.ulTickCount);

	/* 复位后继续采样，证明 DWT 配置没有被一起清掉。 */
	prvRunCriticalSection(2U, 0x7000U);
	htLatencyGetSnapshot(&xSnapshot);
	TEST_ASSERT_EQUAL(1U, xSnapshot.ulCriticalCount);
}

/** @brief 空指针快照必须安全返回，不能解引用。 */
static void test_latency_snapshot_null_is_ignored(void)
{
	prvRunCriticalSection(4U, 0x8000U);
	htLatencyGetSnapshot(NULL);
	TEST_ASSERT_PASS();
}

/** @brief 周期数换算：向下/向上都要四舍五入到最近的微秒。 */
static void test_latency_cycles_to_us_rounds(void)
{
	TEST_ASSERT_EQUAL(0U, htLatencyCyclesToUs(0U));
	TEST_ASSERT_EQUAL(0U, htLatencyCyclesToUs(35U)); /* 0.49 us */
	TEST_ASSERT_EQUAL(1U, htLatencyCyclesToUs(36U)); /* 0.50 us */
	TEST_ASSERT_EQUAL(1U, htLatencyCyclesToUs(TEST_CYCLES_PER_US));
	TEST_ASSERT_EQUAL(2U, htLatencyCyclesToUs(TEST_CYCLES_PER_US + (TEST_CYCLES_PER_US / 2U)));
}

/** @brief P0 回归：累计周期为 64 位，跨 UINT32_MAX 不溢出。 */
static void test_latency_64bit_totals_do_not_overflow(void)
{
	htLatencySnapshot_t xSnapshot;

	htLatencyInit();
	for (uint32_t i = 0U; i < 3U; i++) {
		htHostDwt.CYCCNT = 0U;
		htLatencyTickBegin();
		htHostDwt.CYCCNT = 0x80000000U;
		htLatencyTickEnd();
	}
	htLatencyGetSnapshot(&xSnapshot);
	TEST_ASSERT_EQUAL(3U, xSnapshot.ulTickCount);
	TEST_ASSERT(xSnapshot.ullTickTotalCycles == (3ULL * 0x80000000ULL));
}

int main(void)
{
	UnityBegin("test_latency.c");

	/* 顺序敏感：配置用例必须最先跑，见文件头注释。 */
	setup();
	RUN_TEST(test_latency_init_configures_dwt);
	setup();
	RUN_TEST(test_latency_single_critical_section);
	setup();
	RUN_TEST(test_latency_nested_sections_count_once);
	setup();
	RUN_TEST(test_latency_peak_keeps_its_call_site);
	setup();
	RUN_TEST(test_latency_buckets_bound_and_overflow);
	setup();
	RUN_TEST(test_latency_tick_window);
	setup();
	RUN_TEST(test_latency_reset_clears_stats);
	setup();
	RUN_TEST(test_latency_snapshot_null_is_ignored);
	setup();
	RUN_TEST(test_latency_cycles_to_us_rounds);
	setup();
	RUN_TEST(test_latency_64bit_totals_do_not_overflow);

	return UnityEnd();
}
