#ifndef HT_QUEUE_H
#define HT_QUEUE_H

#include "httypes.h"
#include "htlist.h"
<<<<<<< HEAD
<<<<<<< HEAD
=======
#include "httask.h"
>>>>>>> db6a41e (change)
=======
#include "httask.h"
>>>>>>> main/main

/* 队列指针数据结构 */
typedef struct htQueuePointers
{
    int8_t *pcTail;                /* 指向队列存储区的尾部 */
    int8_t *pcReadFrom;           /* 指向下一个读取位置 */
} htQueuePointers_t;

/* 信号量数据结构 */
typedef struct htSemaphoreData
{
    UBaseType_t uxSemaphoreCount;  /* 信号量计数值 */
} htSemaphoreData_t;

<<<<<<< HEAD
<<<<<<< HEAD
=======
=======
>>>>>>> main/main
/* 记录递归互斥量所有者和递归计数的结构 */
typedef struct htMutexHolder
{
    htTCB_t* xTaskHandle;
    UBaseType_t uxRecursiveCallCount;
    //用于优先级继承
    UBaseType_t uxOriginalPriority;
} htMutexHolder_t;

<<<<<<< HEAD
>>>>>>> db6a41e (change)
=======
>>>>>>> main/main
/* 队列定义结构 */
typedef struct htQueueDefinition
{
    int8_t *pcHead;               /* 指向队列存储区开始位置 */
    int8_t *pcWriteTo;            /* 指向下一个写入位置 */

    union
    {
        htQueuePointers_t xQueue;     /* 队列特有数据 */
        htSemaphoreData_t xSemaphore; /* 信号量特有数据 */
    } u;

    htList_t xTasksWaitingToSend;     /* 等待发送的任务列表 */
    htList_t xTasksWaitingToReceive;  /* 等待接收的任务列表 */

    volatile UBaseType_t uxMessagesWaiting; /* 当前队列中的消息数量 */
    UBaseType_t uxLength;                   /* 队列长度（项目数量） */
    UBaseType_t uxItemSize;                 /* 每个项目的大小（字节） */

    volatile int8_t cRxLock;                /* 队列接收锁 */
    volatile int8_t cTxLock;                /* 队列发送锁 */

} htQUEUE_t;

/* 队列句柄类型 */
typedef htQUEUE_t *QueueHandle_t;

/* 常量定义 */
#define htQUEUE_UNLOCKED    ((int8_t) -1)
#define htQUEUE_LOCKED      ((int8_t) 0)

/* API函数原型 */
QueueHandle_t htQueueCreate(UBaseType_t uxQueueLength, UBaseType_t uxItemSize);
void htQueueDelete(QueueHandle_t xQueue);

BaseType_t htQueueSend(QueueHandle_t xQueue, const void *pvItemToQueue, TickType_t xTicksToWait);
BaseType_t htQueueSendFromISR(QueueHandle_t xQueue, const void *pvItemToQueue, BaseType_t *pxHigherPriorityTaskWoken);

BaseType_t htQueueReceive(QueueHandle_t xQueue, void *pvBuffer, TickType_t xTicksToWait);
BaseType_t htQueueReceiveFromISR(QueueHandle_t xQueue, void *pvBuffer, BaseType_t *pxHigherPriorityTaskWoken);

UBaseType_t htQueueMessagesWaiting(QueueHandle_t xQueue);
BaseType_t htQueueReset(QueueHandle_t xQueue);
BaseType_t htQueuePeek(QueueHandle_t xQueue, void *pvBuffer, TickType_t xTicksToWait);

#endif /* HT_QUEUE_H */
