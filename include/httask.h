#ifndef HTTASK_H
#define HTTASK_H

#include "httypes.h"
#include "htlist.h"

#define htPortStartFirstTask vPortStartFirstTask

/* 任务函数类型定义 */
typedef void (*TaskFunction_t)(void *pvParameters);

/* 任务控制块(TCB)结构 */
typedef struct htTaskControlBlock
{
    StackType_t *pxTopOfStack;              /* 任务堆栈指针,注意任务堆栈必须在第一个，否则无法切换 */
    htListItem_t xStateListItem;            /* 用于将任务放入状态列表 */
    htListItem_t xEventListItem;            /* 用于将任务放入事件列表 */
    UBaseType_t uxPriority;                 /* 任务优先级 */
    StackType_t *pxStack;                   /* 任务堆栈起始地址 */
    char pcTaskName[configMAX_TASK_NAME_LEN]; /* 任务名称 */
    UBaseType_t uxTaskState;                /* 任务状态 */
    UBaseType_t uxBasePriority;             /* 任务基础优先级(用于优先级继承) */
    uint32_t ulNotifiedValue;               /* 任务通知值 */
    UBaseType_t uxNotificationStatus;       /* 任务通知状态 */
    uint32_t ulRunTimeCounter;              /* 任务运行时间计数 */
    UBaseType_t uxStackDepth;               /* 堆栈深度 */
 //   uint32_t ulDelayTime;                   /* 原始延时值，用于调试 */
    
} htTCB_t;

/* 全局变量声明 */
extern htTCB_t *pxCurrentTCB;        /* 当前运行的任务TCB */
extern UBaseType_t uxCurrentNumberOfTasks;  /* 当前任务数量 */
extern TickType_t xTickCount;        /* 系统tick计数 */

/* 就绪列表声明 */
extern htList_t pxReadyTasksLists[];

/* 任务管理相关函数声明 */
BaseType_t htTaskCreate(TaskFunction_t pxTaskCode,
                        const char * const pcName,
                        const uint16_t usStackDepth,
                        void * const pvParameters,
                        UBaseType_t uxPriority,
                        void ** const pxCreatedTask);
                        
void htTaskDelete(TaskHandle_t xTaskToDelete);
void htTaskDelay(TickType_t xTicksToDelay);
BaseType_t htTaskDelayUntil(TickType_t *pxPreviousWakeTime, TickType_t xTimeIncrement);
UBaseType_t htTaskPriorityGet(TaskHandle_t xTask);
void htTaskPrioritySet(TaskHandle_t xTask, UBaseType_t uxNewPriority);
void htTaskSuspend(TaskHandle_t xTaskToSuspend);
void htTaskResume(TaskHandle_t xTaskToResume);
void htTaskResumeFromISR(TaskHandle_t xTaskToResume);
TaskHandle_t htTaskGetCurrentTaskHandle(void);
char* htTaskGetName(TaskHandle_t xTaskToQuery);
htTaskStatus_t htTaskGetState(TaskHandle_t xTask);
void htTaskStartScheduler(void);
void htTaskEndScheduler(void);
void htTaskTickInc(void);
/* 任务调度相关函数声明 */
void htTaskYield(void);
void htTaskSwitchContext(void);

/* 空闲任务相关函数声明 */
static void htprvIdleTask(void *pvParameters);
BaseType_t htprvCreateIdleTask(void);

#endif /* HTTASK_H */
