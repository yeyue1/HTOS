/**
 * @file htutils.h
 * @brief 时间换算、堆栈分析与系统统计接口。
 *
 * 这里的函数面向应用层：把毫秒与 tick 互相换算、查询任务栈的剩余余量，
 * 以及（预留的）CPU 占用统计。注意其中的统计类接口当前均为桩实现，恒返回 0。
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

/**
 * @brief 读取空闲任务的运行时间百分比。
 *
 * @return 百分比数值；当前为桩实现，恒返回 0（尚未接入运行时统计）。
 */
uint32_t htGetIdleRunTimePercent(void);
/**
 * @brief 读取 CPU 使用率。
 *
 * @return 使用率百分比；当前为桩实现，恒返回 0（尚未接入运行时统计）。
 */
uint32_t htGetCPUUsage(void);
/**
 * @brief 读取系统累计运行时间计数。
 *
 * @return 64 位计数值；当前为桩实现，恒返回 0（尚未接入运行时统计）。
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
