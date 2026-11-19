/**
 * @file htwatchdog.h
 * @brief 固定容量的任务心跳监督接口。
 */
#ifndef HT_WATCHDOG_H
#define HT_WATCHDOG_H

#include "httask.h"

#ifndef configWATCHDOG_MAX_TASKS
#define configWATCHDOG_MAX_TASKS 8U
#endif

/** 注册一个需要持续运行的任务；重复注册和容量耗尽返回 htFAIL。 */
BaseType_t htWatchdogRegister(TaskHandle_t xTask);
/** 注销任务；任务删除前必须先注销，成功返回 htPASS。 */
BaseType_t htWatchdogUnregister(TaskHandle_t xTask);
/** 当前任务上报一次进度；仅注册任务可上报。 */
BaseType_t htWatchdogHeartbeat(void);
/** 首次调用建立基线并返回 htFAIL；漏报不推进快照。 */
BaseType_t htWatchdogCheck(void);
/** 清空注册表，供内核初始化及测试使用。 */
void htWatchdogReset(void);

#endif /* HT_WATCHDOG_H */
