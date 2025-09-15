/**
 * @file htOS.c
 * @brief 自定义操作系统实现
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "htOS.h"
#include <stdio.h> 
#include "htos.h"
#include "httask.h"
#include "htscheduler.h"  
#include "htmem.h"
#include "htqueue.h"      // 添加队列模块
#include "htsemaphore.h"  // 添加信号量模块
#include "stm32f1xx.h"

/* 版本定义 */
#define HT_OS_VERSION_MAJOR    0
#define HT_OS_VERSION_MINOR    1
#define HT_OS_VERSION_BUILD    0


/* 系统滴答计数器 - 改为外部声明，避免重复定义 */
extern TickType_t xTickCount;

/* 中断嵌套计数 */
static volatile uint32_t uxCriticalNesting = 0;

// 操作系统状态
int gOSInitialized = 0;
int gOSRunning = 0;

/* 扩展函数声明 */
void htStartScheduler(void);  // 添加此声明以消除警告

/**
 * 操作系统初始化
 */
int htOSInit(void) {
    if (gOSInitialized) {
        return 0; // 已经初始化
    }
    
    
    // 初始化滴答计数器
    xTickCount = 0;
    
    // 初始化临界区嵌套计数
    uxCriticalNesting = 0;
    
    // 初始化任务调度器
    htSchedulerInit();
    
    gOSInitialized = 1;
		htMemInit();
    return 0;
}

/**
 * Start the OS scheduler - focus on debug and reliability
 */
int htOSStart(void) 
{
    if (!gOSInitialized) {
        printf("[htOS] System not initialized, initializing now...\r\n");
        if (htOSInit() != 0) {
            printf("[htOS] Initialization failed!\r\n");
            return -1;
        }
    }

    // Check if already running
    if (gOSRunning) {
        printf("[htOS] System already running!\r\n");
        return 0;
    }

    // Ensure there are tasks to run
    if (pxCurrentTCB == NULL) {
        printf("[htOS] ERROR: No tasks available to run!\r\n");
        return -1;
    }

    
    // Mark system as running
    gOSRunning = 1;
    

    
    // Start scheduler
    htStartScheduler();
    
    // If execution reaches here, startup failed
    gOSRunning = 0;
    
    return -1;
}

/**
 * 创建新任务- 简单包装httask.h中的函数
 */
TaskHandle_t htOSTaskCreate(const char* name, void (*function)(void*), void* param, 
                   uint32_t stackSize, uint8_t priority) {
    TaskHandle_t taskHandle = NULL;
    
    // 调用httask.h中定义的标准任务创建函数 - 修正参数顺序
    BaseType_t status = htTaskCreate(
                                    function,                       // pxTaskCode
                                    name,                          // pcName
                                    (uint16_t)(stackSize / sizeof(StackType_t)), // usStackDepth
                                    param,                         // pvParameters
                                    (UBaseType_t)priority,         // uxPriority
                                    &taskHandle);                  // pxCreatedTask
    
    if (status != 0) {
        return NULL; // 创建失败
    }
    
    return taskHandle;
}

/**
 * 删除任务- 简单包装httask.h中的函数
 */
int htOSTaskDelete(TaskHandle_t task) {
    if (task == NULL) {
        return -1; // 参数错误
    }
    
    htTaskDelete((TaskHandle_t)task);
    return 0; // 假设删除总是成功
}

/**
 * 创建队列（兼容旧API）- 直接调用htqueue.h中的函数
 */
QueueHandle_t htOSQueueCreate(uint32_t length, uint32_t itemSize) {
    return htQueueCreate((UBaseType_t)length, (UBaseType_t)itemSize);
}

/**
 * 发送数据到队列（兼容旧API）
 */
int htOSQueueSend(QueueHandle_t queue, const void* item, uint32_t timeout) {
    if (queue == NULL || item == NULL) {
        return -1; // 参数错误
    }
    
    // 转换超时时间从毫秒到系统滴答数
    TickType_t xTicksToWait = (timeout == 0xFFFFFFFF) ? 0xFFFFFFFF : 
                             ((TickType_t)(((timeout) * configTICK_RATE_HZ) / 1000) + 1);
    
    BaseType_t status = htQueueSend(queue, item, xTicksToWait);
    return (status == htPASS) ? 0 : -1;
}

/**
 * 从队列接收数据（兼容旧API）
 */
int htOSQueueReceive(QueueHandle_t queue, void* buffer, uint32_t timeout) {
    if (queue == NULL || buffer == NULL) {
        return -1; // 参数错误
    }
    
    // 转换超时时间从毫秒到系统滴答数
    TickType_t xTicksToWait = (timeout == 0xFFFFFFFF) ? 0xFFFFFFFF : 
                             ((TickType_t)(((timeout) * configTICK_RATE_HZ) / 1000) + 1);
    
    BaseType_t status = htQueueReceive(queue, buffer, xTicksToWait);
    return (status == htPASS) ? 0 : -1;
}

/**
 * 创建二值信号量（兼容旧API）
 */
SemaphoreHandle_t htOSSemaphoreCreateBinary(void) {
    return htSemaphoreCreateBinary();
}

/**
 * 创建互斥锁（兼容旧API）
 */
SemaphoreHandle_t htOSMutexCreate(void) {
    return htSemaphoreCreateMutex();
}

/**
 * 获取信号量/互斥锁（兼容旧API）
 */
int htOSSemaphoreTake(SemaphoreHandle_t sem, uint32_t timeout) {
    if (sem == NULL) {
        return -1; // 参数错误
    }
    
    // 转换超时时间从毫秒到系统滴答数
    TickType_t xTicksToWait = (timeout == 0xFFFFFFFF) ? 0xFFFFFFFF : 
                             ((TickType_t)(((timeout) * configTICK_RATE_HZ) / 1000) + 1);
    
    BaseType_t status = htSemaphoreTake(sem, xTicksToWait);
    return (status == htPASS) ? 0 : -1;
}

/**
 * 释放信号量/互斥锁（兼容旧API）
 */
int htOSSemaphoreGive(SemaphoreHandle_t sem) {
    if (sem == NULL) {
        return -1; // 参数错误
    }
    
    BaseType_t status = htSemaphoreGive(sem);
    return (status == htPASS) ? 0 : -1;
}
