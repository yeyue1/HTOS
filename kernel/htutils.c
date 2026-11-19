/**
 * @file htutils.c
 * @brief 毫秒/滴答换算、任务栈高水位与运行时间统计等辅助接口的实现。
 *
 * 这里的函数都是轻量的换算或查询，本身不阻塞、不切换任务，
 * 可以在任务上下文里直接调用。
 */
#include "htutils.h"
#include "httask.h"

/**
 * @brief 将毫秒换算成系统滴答数。
 *
 * @param[in] milliseconds 毫秒数，调用者传入 0 时直接返回 0。
 * @return 换算得到的滴答数；超过 UINT32_MAX 时饱和到 UINT32_MAX。
 * @note 采用向上取整，保证非零时延至少等待 1 个滴答，不会因截断变成“不等待”。
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
 * @brief 将系统滴答数换算成毫秒数。
 *
 * @param[in] ticks 系统滴答数。
 * @return 对应的毫秒数；超过 UINT32_MAX 时饱和到 UINT32_MAX。
 * @note 采用整除向下取整，不足 1 毫秒的余数被丢弃；默认配置下 1 滴答恰好等于 1 毫秒，
 *       只有在 configTICK_RATE_HZ 小于 1000 时才可能出现精度损失。
 */
uint32_t htTicksToTime(TickType_t ticks)
{
	const uint64_t milliseconds = ((uint64_t)ticks * 1000U) / configTICK_RATE_HZ;
	return milliseconds > UINT32_MAX ? UINT32_MAX : (uint32_t)milliseconds;
}

/**
 * @brief 获取任务栈的高水位标记（栈底连续未被写过的字数）。
 *
 * @param[in] xTask 要检查的任务句柄；传 NULL 表示当前任务。
 * @return 从栈底开始连续保持 htTASK_STACK_FILL_VALUE 的字数；句柄或栈信息无效时返回 0。
 * @note 从栈底（低地址）向上扫描，遇到第一个被写过的字就停止，因此返回值反映的是
 *       “栈底还剩多少余量”，不能发现栈中部被零星使用而栈底已填满的情况。
 * @note 该统计依赖创建任务时把整段栈初始化为 htTASK_STACK_FILL_VALUE。
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

/*
 * 运行时间统计（htGetCPUUsage / htGetIdleRunTimePercent / htGetRunTimeCounter）
 * 的实现在 kernel/httask.c：它必须跟着 SysTick 采样走，也要直接读当前任务的 TCB，
 * 放在这里只会平白多一条跨文件依赖。本文件只保留与本模块无关的换算与栈分析。
 */
