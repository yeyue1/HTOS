#include "htutils.h"
#include "htos.h"
#include "httask.h"
#include "stm32f1xx_hal.h" // 添加此行以解决HAL_Delay未声明警告


/**
 * 将毫秒转换为系统滴答
 * @param milliseconds 毫秒数
 * @return 对应的系统滴答数
 */
uint32_t htTimeToTicks(uint32_t milliseconds)
{
    return (uint32_t)((milliseconds * configTICK_RATE_HZ) / 1000);
}

/**
 * 将系统滴答转换为毫秒
 * @param ticks 系统滴答数
 * @return 对应的毫秒数
 */
uint32_t htTicksToTime(TickType_t ticks)
{
    return (uint32_t)((ticks * 1000) / configTICK_RATE_HZ);
}


/**
 * 获取任务堆栈使用高水位标记
 * @param xTask 要检查的任务句柄
 * @return 堆栈使用高水位标记（以字为单位）
 */
UBaseType_t htGetTaskStackHighWaterMark(TaskHandle_t xTask)
{
    /* 防止未使用参数警告 */
    (void)xTask;
    
    /* 简化实现，实际应该检查堆栈模式 */
    return 0;
}

/**
 * 获取CPU使用率
 * @return CPU使用百分比（0-100）
 */
uint32_t htGetCPUUsage(void)
{
    /* 需要基于空闲任务运行时间计算 */
    return 100 - htGetIdleRunTimePercent();
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
