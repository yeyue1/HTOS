/**
 * @file htlatency.c
 * @brief 实时性仪表实现：最坏情况临界区与滴答中断耗时。
 *
 * 全部代码受 configUSE_LATENCY_STATS 保护，默认关闭时这份文件不产生任何目标代码。
 * 计时基准是 DWT->CYCCNT：32 位自由运行计数器，72 MHz 下 13.9 ns 一跳、59.6 s 回绕，
 * 用无符号减法跨回绕，因此单次测量只要短于 59.6 s 就是精确的（本模块测的是微秒级）。
 */
#include "htconfig.h"

#if (configUSE_LATENCY_STATS == 1)

#include "htlatency.h"
#include "htscheduler.h"

/* 设备头：真机上由 CMSIS 提供 IRQn_Type 与 __NVIC_PRIO_BITS，主机上由 tests/stubs 转发。 */
#include "stm32f1xx.h"

#include <string.h>

/** 直方图的边界个数：最后一个档是 "> 64 us" 的溢出档，没有上界。 */
#define HT_LATENCY_BOUND_COUNT (HT_LATENCY_BUCKET_COUNT - 1U)

/**
 * @brief 直方图分档上界，单位是 DWT 周期。
 *
 * 档位是 1/2/4/8/16/32/64 微秒：小于 1 微秒的临界区对 1 kHz 滴答几乎无感，
 * 而超过 64 微秒的临界区已经开始吃掉中断响应预算，因此单列一档重点观察。
 */
static const uint32_t s_aulBucketBounds[HT_LATENCY_BOUND_COUNT] = {
		1U * HT_LATENCY_CYCLES_PER_US,
		2U * HT_LATENCY_CYCLES_PER_US,
		4U * HT_LATENCY_CYCLES_PER_US,
		8U * HT_LATENCY_CYCLES_PER_US,
		16U * HT_LATENCY_CYCLES_PER_US,
		32U * HT_LATENCY_CYCLES_PER_US,
		64U * HT_LATENCY_CYCLES_PER_US,
};

/** 累计统计量；快照就是把这个结构体整体拷出去。 */
static htLatencySnapshot_t s_xStats;

/** 当前这次最外层临界区的起始周期数与调用点（等结算时取用）。 */
static uint32_t s_ulCriticalStart;
static uint32_t s_ulPendingCaller;

/** 当前这次滴答中断的起始周期数。 */
static uint32_t s_ulTickStart;

/** 1 = DWT 已配置好；0 时所有打点直接跳过，避免在初始化前读到无意义的计数器。 */
static uint8_t s_ucLatencyReady;

/**
 * @brief 把一段耗时归入直方图档位。
 *
 * @param[in] ulCycles 耗时，单位周期。
 * @return 档位下标，0 表示 <= 1 us，HT_LATENCY_BUCKET_COUNT-1 表示 > 64 us。
 */
static uint32_t prvBucketOf(uint32_t ulCycles)
{
	uint32_t uxIndex;

	for (uxIndex = 0U; uxIndex < HT_LATENCY_BOUND_COUNT; uxIndex++) {
		if (ulCycles <= s_aulBucketBounds[uxIndex]) {
			return uxIndex;
		}
	}

	return (uint32_t)(HT_LATENCY_BUCKET_COUNT - 1U);
}

void htLatencyInit(void)
{
	if (s_ucLatencyReady != 0U) {
		return;
	}

	/* 写 DWT 之前必须打开跟踪总开关，否则所有写入都会被硬件忽略。 */
	CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
	DWT->CYCCNT = 0UL;
	DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
	s_ucLatencyReady = 1U;
}

void htLatencyCriticalEnter(uint32_t ulCaller)
{
	/*
	 * 只统计最外层：嵌套的第一层已经覆盖了内层的全部时间，重复计时会把
	 * "一次关中断的时长"重复计成多次，直方图立刻失真。
	 */
	if ((s_ucLatencyReady == 0U) || (uxCriticalNesting != 1U)) {
		return;
	}

	s_ulCriticalStart = DWT->CYCCNT;
	s_ulPendingCaller = ulCaller;
}

void htLatencyCriticalExit(void)
{
	uint32_t ulElapsed;

	/* 本函数在 uxCriticalNesting 递减之前调用，因此此刻仍是 1 表示这是最外层出口。 */
	if ((s_ucLatencyReady == 0U) || (uxCriticalNesting != 1U)) {
		return;
	}

	ulElapsed = DWT->CYCCNT - s_ulCriticalStart;

	s_xStats.ulCriticalCount++;
	s_xStats.ulCriticalTotalCycles += ulElapsed;
	if (ulElapsed > s_xStats.ulCriticalMaxCycles) {
		s_xStats.ulCriticalMaxCycles = ulElapsed;
		s_xStats.ulCriticalCaller = s_ulPendingCaller; /* 只记峰值那次的调用点 */
	}
	s_xStats.aulCriticalBuckets[prvBucketOf(ulElapsed)]++;
}

void htLatencyTickBegin(void)
{
	if (s_ucLatencyReady == 0U) {
		return;
	}

	s_ulTickStart = DWT->CYCCNT;
}

void htLatencyTickEnd(void)
{
	uint32_t ulElapsed;

	if (s_ucLatencyReady == 0U) {
		return;
	}

	ulElapsed = DWT->CYCCNT - s_ulTickStart;

	s_xStats.ulTickCount++;
	s_xStats.ulTickTotalCycles += ulElapsed;
	if (ulElapsed > s_xStats.ulTickMaxCycles) {
		s_xStats.ulTickMaxCycles = ulElapsed;
	}
}

void htLatencyGetSnapshot(htLatencySnapshot_t *pxSnapshot)
{
	uint32_t ulPrimask;

	if (pxSnapshot == NULL) {
		return;
	}

	/*
	 * 这里刻意不用 htEnterCritical()：那会改动 uxCriticalNesting，
	 * 而本模块正是靠这个计数判断"最外层"。用 PRIMASK 原语关一下即可。
	 */
	ulPrimask = __get_PRIMASK();
	__disable_irq();
	*pxSnapshot = s_xStats;
	__set_PRIMASK(ulPrimask);
}

void htLatencyReset(void)
{
	uint32_t ulPrimask;

	ulPrimask = __get_PRIMASK();
	__disable_irq();
	(void)memset(&s_xStats, 0, sizeof(s_xStats));
	s_ulCriticalStart = 0U;
	s_ulPendingCaller = 0U;
	s_ulTickStart = 0U;
	__set_PRIMASK(ulPrimask);
}

uint32_t htLatencyCyclesToUs(uint32_t ulCycles)
{
	const uint32_t ulCyclesPerUs = (uint32_t)HT_LATENCY_CYCLES_PER_US;

	if (ulCyclesPerUs == 0U) {
		return 0U;
	}

	/* 四舍五入，避免 0.5 us 以下的抖动全被截成 0。 */
	return (ulCycles + (ulCyclesPerUs / 2U)) / ulCyclesPerUs;
}

#else /* configUSE_LATENCY_STATS == 0 */

/* 关闭时本文件是空翻译单元；留一个类型定义，免得严格模式下报警"空翻译单元"。 */
typedef int htLatencyDisabled_t;

#endif /* configUSE_LATENCY_STATS */
