#ifndef HT_UTILS_H
#define HT_UTILS_H

#include "httypes.h"


/* 仅在RTOS启动后可以安全使用的函数 */
#define OS_SAFE_DELAY(ms) htTaskDelay((ms * configTICK_RATE_HZ) / 1000 + 1)

/* 系统统计函数 */
uint32_t htGetIdleRunTimePercent(void); /* 获取空闲任务运行时间百分比 */
uint32_t htGetCPUUsage(void);           /* 获取CPU使用率 */
uint64_t htGetRunTimeCounter(void);     /* 获取系统总运行时间计数 */

/* 堆栈分析函数 */
UBaseType_t htGetTaskStackHighWaterMark(TaskHandle_t xTask); /* 获取任务堆栈使用高水位标记 */

/* 任务调试信息 */
void htListTasks(void);                 /* 列出系统中所有任务及其状态 */

/* 系统时间计算 */
uint32_t htTimeToTicks(uint32_t milliseconds); /* 将毫秒转换为系统滴答 */
uint32_t htTicksToTime(TickType_t ticks);      /* 将系统滴答转换为毫秒 */

#endif /* HT_UTILS_H */
