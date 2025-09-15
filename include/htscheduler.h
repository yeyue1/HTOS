#ifndef HT_SCHEDULER_H
#define HT_SCHEDULER_H

#include "httypes.h"



/* 调度器初始化 */
void htSchedulerInit(void);

/* 暂停调度器 */
void htSuspendScheduler(void);

/* 恢复调度器 */
void htResumeScheduler(void);

/* 系统滴答处理函数 */
void htSchedulerTickHandler(void);

/* 内核进入临界区 */
void htEnterCritical(void);

/* 内核退出临界区 */
void htExitCritical(void);

/* 临界区嵌套计数器 */
extern volatile UBaseType_t uxCriticalNesting;
/* 任务调度 */
void htTaskSwitchContext(void);

#endif /* HT_SCHEDULER_H */
