#include "htutils.h"
#include "httask.h"

/**
 * 将毫秒转换为系统滴答
 * @param milliseconds 毫秒数
 * @return 对应的系统滴答数
 */
uint32_t htTimeToTicks(uint32_t milliseconds)
{
	if (milliseconds == 0U) {
		return 0U;
	}
	const uint64_t ticks = ((uint64_t)milliseconds * configTICK_RATE_HZ + 999U) / 1000U;
	return ticks > UINT32_MAX ? UINT32_MAX : (uint32_t)ticks;
}

/**
 * 将系统滴答转换为毫秒
 * @param ticks 系统滴答数
 * @return 对应的毫秒数
 */
uint32_t htTicksToTime(TickType_t ticks)
{
	const uint64_t milliseconds = ((uint64_t)ticks * 1000U) / configTICK_RATE_HZ;
	return milliseconds > UINT32_MAX ? UINT32_MAX : (uint32_t)milliseconds;
}

/**
 * 获取任务堆栈使用高水位标记
 * @param xTask 要检查的任务句柄
 * @return 堆栈使用高水位标记（以字为单位）
 */
UBaseType_t htGetTaskStackHighWaterMark(TaskHandle_t xTask)
{
	htTCB_t *pxTCB = xTask == NULL ? pxCurrentTCB : (htTCB_t *)xTask;
	if (pxTCB == NULL || pxTCB->pxStack == NULL || pxTCB->uxStackDepth == 0U) {
		return 0U;
	}

	UBaseType_t uxUntouchedWords = 0U;
	while (uxUntouchedWords < pxTCB->uxStackDepth && pxTCB->pxStack[uxUntouchedWords] == htTASK_STACK_FILL_VALUE) {
		uxUntouchedWords++;
	}
	return uxUntouchedWords;
}

/**
 * 获取CPU使用率
 * @return CPU使用百分比（0-100）
 */
uint32_t htGetCPUUsage(void)
{
	/* 未配置运行时计数器时返回 0，避免误报 100% 负载。 */
	return 0U;
}

/**
 * 获取空闲任务运行时间百分比
 * @return 空闲任务运行时间百分比（0-100）
 */
uint32_t htGetIdleRunTimePercent(void)
{
	/* 简化实现 */
	return 0;
}

/**
 * 获取系统总运行时间计数
 * @return 系统运行时间计数
 */
uint64_t htGetRunTimeCounter(void)
{
	return 0;
}
