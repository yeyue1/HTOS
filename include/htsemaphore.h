#ifndef HT_SEMAPHORE_H
#define HT_SEMAPHORE_H

#include "httypes.h"
#include "htqueue.h"
#include "httask.h"


/* 信号量句柄定义 */
typedef QueueHandle_t SemaphoreHandle_t;

/* 二值信号量创建函数 */
SemaphoreHandle_t htSemaphoreCreateBinary(void);

/* 计数信号量创建函数 */
SemaphoreHandle_t htSemaphoreCreateCounting(UBaseType_t uxMaxCount, UBaseType_t uxInitialCount);

/* 互斥量创建函数 */
SemaphoreHandle_t htSemaphoreCreateMutex(void);
SemaphoreHandle_t htSemaphoreCreateRecursiveMutex(void);

/* 信号量删除函数 */
void htSemaphoreDelete(SemaphoreHandle_t xSemaphore);

/* 信号量获取（take/获取） */
BaseType_t htSemaphoreTake(SemaphoreHandle_t xSemaphore, TickType_t xTicksToWait);
BaseType_t htSemaphoreTakeFromISR(SemaphoreHandle_t xSemaphore, BaseType_t *pxHigherPriorityTaskWoken);

/*互斥量获取*/
BaseType_t htSemaphoreTakeMutex(SemaphoreHandle_t xSemaphore, TickType_t xTicksToWait);


/* 递归互斥量获取 */
BaseType_t htSemaphoreTakeRecursive(SemaphoreHandle_t xSemaphore, TickType_t xTicksToWait);

/* 信号量释放（give/释放） */
BaseType_t htSemaphoreGive(SemaphoreHandle_t xSemaphore);
BaseType_t htSemaphoreGiveFromISR(SemaphoreHandle_t xSemaphore, BaseType_t *pxHigherPriorityTaskWoken);

/* 互斥量释放 */
BaseType_t htSemaphoreGiveMutex(SemaphoreHandle_t xSemaphore);

/* 递归互斥量释放 */
BaseType_t htSemaphoreGiveRecursive(SemaphoreHandle_t xSemaphore);

/* 获取信号量计数值 */
UBaseType_t htSemaphoreGetCount(SemaphoreHandle_t xSemaphore);

#endif /* HT_SEMAPHORE_H */
