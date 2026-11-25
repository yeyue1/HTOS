/**
 * @file htutils.h
 * @brief 时间换算、堆栈分析与系统统计接口。
 *
 * 这里的函数面向应用层：把毫秒与 tick 互相换算、查询任务栈的剩余余量，
 * 以及 CPU 占用统计。统计类接口在 configGENERATE_RUN_TIME_STATS 为 1 时返回实测值，
 * 为 0 时恒返回 0（表示"没有这项数据"，而不是"很空闲"）。
 */
#ifndef HT_UTILS_H
#define HT_UTILS_H

#include "httypes.h"

/**
 * @brief 以毫秒为单位安全延时的便捷宏。
 *
 * 展开为 htTaskDelay(htTimeToTicks(ms))。
 *
 * @param[in] ms 延时毫秒数，内部向上取整到整数个 tick。
 * @warning 仅在 RTOS 调度器启动之后才可以安全使用；启动前没有任务上下文，调用会破坏内核状态。
 */
#define OS_SAFE_DELAY(ms) htTaskDelay(htTimeToTicks(ms))

/* ------------------------------------------------------------------ */
/* 1. 系统统计                                                          */
/* ------------------------------------------------------------------ */

#if (configGENERATE_RUN_TIME_STATS == 1)
/**
 * @brief 采样一次运行时间，把上一个 tick 记到当前任务头上。
 *
 * @note 由 htTaskTickInc() 在关中断状态下每毫秒调用一次，应用不要直接调用。
 */
void htRunTimeSample(void);
#endif

/**
 * @brief 读取空闲任务的运行时间百分比。
 *
 * @return 0~100 的百分比，是"自启动以来的平均值"；统计关闭时恒返回 0。
 * @note 实现在 kernel/httask.c：统计跟着滴答采样，并直接累加到任务控制块上。
 */
uint32_t htGetIdleRunTimePercent(void);
/**
 * @brief 读取 CPU 使用率。
 *
 * @return 0~100 的百分比，等于 100 减去空闲占比；统计关闭时恒返回 0。
 */
uint32_t htGetCPUUsage(void);
/**
 * @brief 读取系统累计运行时间计数。
 *
 * @return 64 位周期计数；72 MHz 下除以 72000 即得毫秒数。统计关闭时恒返回 0。
 */
uint64_t htGetRunTimeCounter(void);

/* ------------------------------------------------------------------ */
/* 2. 堆栈分析                                                          */
/* ------------------------------------------------------------------ */

/**
 * @brief 读取任务栈中尚未被使用过的余量。
 *
 * 实现方式是从栈底开始统计仍等于 htTASK_STACK_FILL_VALUE 的栈单元个数，
 * 因此返回值是"从未被写过的栈字数"，不是曾经达到过的最大使用量。
 *
 * @param[in] xTask 任务句柄，传 NULL 表示当前任务。
 * @return 未被触碰的栈单元数（word）；句柄无效时返回 0。
 * @note 栈单元为 StackType_t（32 位），想换算成字节请乘以 sizeof(StackType_t)。
 */
UBaseType_t htGetTaskStackHighWaterMark(TaskHandle_t xTask);

/* ------------------------------------------------------------------ */
/* 3. 时间换算                                                          */
/* ------------------------------------------------------------------ */

/**
 * @brief 把毫秒换算成系统 tick 数。
 *
 * @param[in] milliseconds 毫秒数；为 0 时直接返回 0。
 * @return 对应的 tick 数，向上取整；结果超出 uint32_t 范围时饱和到 0xFFFFFFFF。
 */
uint32_t htTimeToTicks(uint32_t milliseconds);
/**
 * @brief 把系统 tick 数换算回毫秒。
 *
 * @param[in] ticks 系统节拍数。
 * @return 对应的毫秒数，向下取整；结果超出 uint32_t 范围时饱和到 0xFFFFFFFF。
 */
uint32_t htTicksToTime(TickType_t ticks);

#endif /* HT_UTILS_H */
