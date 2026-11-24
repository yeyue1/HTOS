/**
 * @file htlatency.h
 * @brief 实时性仪表：在线测量最坏情况临界区与滴答中断耗时。
 *
 * 硬实时系统的抖动主要由两处决定：关中断的最长一段（临界区），以及 SysTick 处理
 * 本身占用的时间。两者都无法靠静态分析得到可信上界，只能在目标板上实测。本模块用
 * Cortex-M3 的数据观察点单元（DWT->CYCCNT，72 MHz 下 13.9 ns 一个计数）给这两段
 * 时间打点，记录峰值、峰值出现的调用点、总次数以及一张按微秒分档的直方图。
 *
 * 与内核其余统计一样，本模块默认关闭（configUSE_LATENCY_STATS == 0）：此时下面的
 * 四个宏展开为空，htEnterCritical()/htExitCritical()/htTaskTickInc() 一个字节都不会
 * 变多；置 1 时内核需要接入这四个宏，代价约 80 字节 RAM 与几十字节 Flash。
 *
 * @note 测量本身也会抬高测量结果：每次进出临界区多两次计数器读取，约 10 个周期，
 *       折算约 0.14 us，相对被测区间可以忽略，但不等于零。
 * @note 分段是开区间语义：一个耗时恰好 8 us 的临界区落进"<= 8 us"这一档。
 */
#ifndef HT_LATENCY_H
#define HT_LATENCY_H

#include "htconfig.h"

#include <stdint.h>

/** 直方图档数：1/2/4/8/16/32/64 微秒七档，再加一个 "> 64 微秒" 溢出档。 */
#define HT_LATENCY_BUCKET_COUNT 8U

/** 每个微秒对应的周期数；72 MHz 时为 72。 */
#define HT_LATENCY_CYCLES_PER_US (configCPU_CLOCK_HZ / 1000000U)

/**
 * @brief 实时性快照：一次读走全部统计值。
 *
 * @note 字段单位都是 DWT 周期；打印前用 htLatencyCyclesToUs() 换算成微秒。
 */
typedef struct {
	uint32_t ulCriticalMaxCycles; /**< 观测到的最长临界区耗时。 */
	uint32_t ulCriticalCaller; /**< 该次临界区的 htEnterCritical() 返回地址。 */
	uint32_t ulCriticalCount; /**< 采样到的临界区次数（只统计最外层）。 */
	uint32_t ulCriticalTotalCycles; /**< 全部临界区耗时之和。 */
	uint32_t aulCriticalBuckets[HT_LATENCY_BUCKET_COUNT]; /**< 按耗时档累计的次数。 */
	uint32_t ulTickMaxCycles; /**< 观测到的最长滴答中断耗时。 */
	uint32_t ulTickCount; /**< 采样到的滴答中断次数。 */
	uint32_t ulTickTotalCycles; /**< 全部滴答中断耗时之和。 */
} htLatencySnapshot_t;

/**
 * @brief 初始化仪表：打开 DWT 周期计数器并清空统计。
 * @note 由 htOSInit() 自动调用，可重复调用。
 */
void htLatencyInit(void);

/**
 * @brief 记录一次临界区入口。
 * @param[in] ulCaller 调用方地址，宏会自动填成 htEnterCritical() 的返回地址。
 * @note 只在最外层（uxCriticalNesting == 1）生效，嵌套层不重复计时。
 */
void htLatencyCriticalEnter(uint32_t ulCaller);

/**
 * @brief 记录一次临界区出口并结算耗时。
 * @note 必须在 uxCriticalNesting 递减之前调用，否则最外层判定会失效。
 */
void htLatencyCriticalExit(void);

/** @brief 记录滴答中断开始。 */
void htLatencyTickBegin(void);

/** @brief 记录滴答中断结束并结算耗时。 */
void htLatencyTickEnd(void);

/**
 * @brief 取一份统计快照。
 * @param[out] pxSnapshot 目标结构体；传 NULL 时什么也不做。
 * @note 读取期间关中断，耗时约几十个周期。
 */
void htLatencyGetSnapshot(htLatencySnapshot_t *pxSnapshot);

/** @brief 清空全部统计值，保留 DWT 配置。 */
void htLatencyReset(void);

/**
 * @brief 把周期数换算成微秒。
 * @param[in] ulCycles 周期数。
 * @return 四舍五入后的微秒数。
 */
uint32_t htLatencyCyclesToUs(uint32_t ulCycles);

#if (configUSE_LATENCY_STATS == 1)

/**
 * @brief 临界区入口打点，自动带上调用点。
 * @note __builtin_return_address(0) 取的是 htEnterCritical() 的返回地址，即真正调用
 *       临界区的那行代码；实测到的地址可以用 arm-none-eabi-addr2line 反查源码行。
 */
#define HT_LATENCY_CRITICAL_ENTER() htLatencyCriticalEnter((uint32_t)(uintptr_t)__builtin_return_address(0U))

/** @brief 临界区出口打点。 */
#define HT_LATENCY_CRITICAL_EXIT() htLatencyCriticalExit()

/** @brief 滴答中断入口打点。 */
#define HT_LATENCY_TICK_BEGIN() htLatencyTickBegin()

/** @brief 滴答中断出口打点。 */
#define HT_LATENCY_TICK_END() htLatencyTickEnd()

#else /* configUSE_LATENCY_STATS == 0 */

/* 关闭时四个宏都是空语句：调用点保留在代码里，但生成的指令一条都没有。 */
#define HT_LATENCY_CRITICAL_ENTER() ((void)0)
#define HT_LATENCY_CRITICAL_EXIT() ((void)0)
#define HT_LATENCY_TICK_BEGIN() ((void)0)
#define HT_LATENCY_TICK_END() ((void)0)

#endif /* configUSE_LATENCY_STATS */

#endif /* HT_LATENCY_H */
