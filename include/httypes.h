/**
 * @file httypes.h
 * @brief htOS基本类型定义
 */
#ifndef HT_TYPES_H
#define HT_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include "htconfig.h"

/* 基本类型定义 */
typedef int BaseType_t;
typedef unsigned int UBaseType_t;
typedef uint32_t TickType_t;
typedef void* TaskHandle_t;
typedef uint32_t StackType_t;

/* 定时器回调函数类型 */
typedef void (*htTimerCallback_t)(void);

/* 常量定义 */
#define htPASS     0
#define htFAIL     -1
#define htTRUE     1
#define htFALSE    0
#define htNULL     NULL

/* 任务状态枚举 - 添加此定义以供httask.h使用 */
enum htTaskStatus {
    HT_TASK_READY = 0,      /* 任务就绪 */
    HT_TASK_RUNNING,        /* 任务运行中 */
    HT_TASK_BLOCKED,        /* 任务阻塞 */
    HT_TASK_SUSPENDED,      /* 任务挂起 */
};
typedef enum htTaskStatus htTaskStatus_t;

/* 调度器状态枚举 */
enum htSchedulerState {
    HT_SCHEDULER_NOT_STARTED = 0,
    HT_SCHEDULER_RUNNING,
    HT_SCHEDULER_SUSPENDED
};
typedef enum htSchedulerState htSchedulerState_t;

/* 任务优先级枚举 */
enum htTaskPriority {
    HT_IDLE_TASK = 0,
    HT_LOW_TASK,
<<<<<<< HEAD
<<<<<<< HEAD
    HT_NORMAL_TASK,  // 修复拼写错误：HHT_NORMAL_TASK -> HT_NORMAL_TASK
=======
    HT_NORMAL_TASK,  
>>>>>>> db6a41e (change)
=======
    HT_NORMAL_TASK,  
>>>>>>> main/main
    HT_HIGH_TASK
};

/* 队列相关定义 */
#define htQUEUE_TYPE_BASE                      0U
#define htQUEUE_TYPE_MUTEX                     1U
#define htQUEUE_TYPE_COUNTING_SEMAPHORE        2U
#define htQUEUE_TYPE_BINARY_SEMAPHORE          3U
#define htQUEUE_TYPE_RECURSIVE_MUTEX           4U

/* 队列状态标志 */
#define htBLOCKED_INDEFINITELY                 (0xFFFFFFFF)

/* 列表相关结构体定义 */
struct htListItem {
    TickType_t xItemValue;
    struct htListItem *pxNext;
    struct htListItem *pxPrevious;
    struct htList *pvOwner;
    struct htList *pxContainer;
};

struct htList {
    UBaseType_t uxNumberOfItems;
    struct htListItem *pxHead;
    struct htListItem *pxIndex;
    struct htListItem xListEnd;
};

/* 定义别名宏 */
#define htListItem_t struct htListItem
#define htList_t struct htList

#endif /* HT_TYPES_H */
