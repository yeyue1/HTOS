#include "htsemaphore.h"
#include "htqueue.h"
#include "httask.h"
#include "htmem.h"
#include "htscheduler.h"
#include <stdio.h>



/**
 * 创建二值信号量
 */
SemaphoreHandle_t htSemaphoreCreateBinary(void)
{
    QueueHandle_t xSemaphore;
    
    /* 创建长度为1的队列，不存储实际数据 */
    xSemaphore = htQueueCreate(1, 0);
    
    /* 默认创建为空（不可获取）状态 */
    /* 注意：如果需要创建后立即可用，需要调用htSemaphoreGive() */
    
    return xSemaphore;
}

/**
 * 创建计数信号量
 */
SemaphoreHandle_t htSemaphoreCreateCounting(UBaseType_t uxMaxCount, UBaseType_t uxInitialCount)
{
    QueueHandle_t xSemaphore;
    htQUEUE_t *pxQueue;
    
    /* 检查参数有效性 */
    if (uxMaxCount == 0 || uxInitialCount > uxMaxCount)
    {
        return NULL;
    }
    
    /* 创建长度为uxMaxCount的队列，不存储实际数据 */
    xSemaphore = htQueueCreate(uxMaxCount, 0);
    
    if (xSemaphore != NULL)
    {
        pxQueue = (htQUEUE_t *)xSemaphore;
        
        /* 设置初始计数值 */
        pxQueue->u.xSemaphore.uxSemaphoreCount = uxInitialCount;
        pxQueue->uxMessagesWaiting = uxInitialCount;
    }
    
    return xSemaphore;
}

/**
 * 创建互斥量
 */
SemaphoreHandle_t htSemaphoreCreateMutex(void)
{
    QueueHandle_t xSemaphore;
    htQUEUE_t *pxQueue;
    
    /* 创建长度为1的队列，不存储实际数据 */
    xSemaphore = htQueueCreate(1, 0);
    
    if (xSemaphore != NULL)
    {
        pxQueue = (htQUEUE_t *)xSemaphore;
        
        /* 互斥量初始为满状态 */
        pxQueue->u.xSemaphore.uxSemaphoreCount = 1;
        pxQueue->uxMessagesWaiting = 1;
    }
    
    return xSemaphore;
}

/**
 * 创建递归互斥量
 */
SemaphoreHandle_t htSemaphoreCreateRecursiveMutex(void)
{
    /* 仅在configUSE_RECURSIVE_MUTEXES启用时可用 */
#if configUSE_RECURSIVE_MUTEXES == 1
    QueueHandle_t xSemaphore;
    //htQUEUE_t *pxQueue;
    htMutexHolder_t *pxMutexHolder;
    
    /* 创建长度为1的队列，存储互斥量持有者信息 */
    xSemaphore = htQueueCreate(1, sizeof(htMutexHolder_t));
    
    if (xSemaphore != NULL)
    {
        //pxQueue = (htQUEUE_t *)xSemaphore;
        
        /* 分配互斥量持有者结构 */
        pxMutexHolder = (htMutexHolder_t *)htPortMalloc(sizeof(htMutexHolder_t));
        
        if (pxMutexHolder != NULL)
        {
            /* 初始化互斥量持有者信息 */
            pxMutexHolder->xTaskHandle = NULL;
            pxMutexHolder->uxRecursiveCallCount = 0;
            
            /* 将持有者信息存入队列 */
            if (htQueueSend(xSemaphore, pxMutexHolder, 0) != htPASS)
            {
                /* 发送失败，释放内存 */
                htPortFree(pxMutexHolder);
                htQueueDelete(xSemaphore);
                xSemaphore = NULL;
            }
            
            /* 不再需要临时持有者结构 */
            htPortFree(pxMutexHolder);
        }
        else
        {
            /* 内存分配失败，释放队列 */
            htQueueDelete(xSemaphore);
            xSemaphore = NULL;
        }
    }
    
    return xSemaphore;
#else
    /* 递归互斥量未启用，返回NULL */
    return NULL;
#endif
}

/**
 * 删除信号量
 */
void htSemaphoreDelete(SemaphoreHandle_t xSemaphore)
{
    /* 直接使用队列删除函数 */
    htQueueDelete(xSemaphore);
}

/**
 * 获取信号量计数值
 */
UBaseType_t htSemaphoreGetCount(SemaphoreHandle_t xSemaphore)
{
    htQUEUE_t *pxQueue = (htQUEUE_t *)xSemaphore;
    UBaseType_t uxReturn = 0;
    
    if (pxQueue != NULL)
    {
        htEnterCritical();
        uxReturn = pxQueue->uxMessagesWaiting;
        htExitCritical();
    }
    
    return uxReturn;
}

/**
 * 获取信号量
 */
BaseType_t htSemaphoreTake(SemaphoreHandle_t xSemaphore, TickType_t xTicksToWait)
{
    BaseType_t xReturn;
    uint8_t ucBuffer[4]; /* 临时缓冲区，实际不使用 */
    
    /* 使用队列接收实现信号量获取 */
    xReturn = htQueueReceive(xSemaphore, ucBuffer, xTicksToWait);
    
    return xReturn;
}

/**
 * 释放信号量
 */
BaseType_t htSemaphoreGive(SemaphoreHandle_t xSemaphore)
{
    BaseType_t xReturn;
    uint8_t ucDummy = 0; /* 占位数据，实际内容无关紧要 */
    
    /* 使用队列发送实现信号量释放 */
    xReturn = htQueueSend(xSemaphore, &ucDummy, 0);
    
    return xReturn;
}

/**
 * 从ISR中获取信号量
 */
BaseType_t htSemaphoreTakeFromISR(SemaphoreHandle_t xSemaphore, BaseType_t *pxHigherPriorityTaskWoken)
{
    BaseType_t xReturn;
    uint8_t ucBuffer[4]; /* 临时缓冲区，实际不使用 */
    
    /* 使用队列从ISR接收实现信号量获取 */
    xReturn = htQueueReceiveFromISR(xSemaphore, ucBuffer, pxHigherPriorityTaskWoken);
    
    return xReturn;
}

/**
 * 从ISR中释放信号量
 */
BaseType_t htSemaphoreGiveFromISR(SemaphoreHandle_t xSemaphore, BaseType_t *pxHigherPriorityTaskWoken)
{
    BaseType_t xReturn;
    uint8_t ucDummy = 0; /* 占位数据，实际内容无关紧要 */
    
    /* 使用队列从ISR发送实现信号量释放 */
    xReturn = htQueueSendFromISR(xSemaphore, &ucDummy, pxHigherPriorityTaskWoken);
    
    return xReturn;
}

/**
 * 互斥量获取
 */

BaseType_t htSemaphoreTakeMutex(SemaphoreHandle_t xSemaphore, TickType_t xTicksToWait)
{
    BaseType_t xReturn;
    uint8_t ucBuffer[4]; /* 临时缓冲区，实际不使用 */
    //检查参数有效性
    if (xSemaphore == NULL)
    {
        return htFAIL;
    }
    /* 禁用中断 */
    htEnterCritical();
    /* 互斥量不可用，且当前任务不是持有者，则需要等待 */
    htMutexHolder_t xMutexHolder;
    if (htQueuePeek(xSemaphore, &xMutexHolder, 0) == htPASS)
    {
        if (xMutexHolder.xTaskHandle != NULL && xMutexHolder.xTaskHandle != pxCurrentTCB)
        {
            //优先级继承
            if (xMutexHolder.xTaskHandle != NULL && xMutexHolder.xTaskHandle != pxCurrentTCB)
            {
                if (pxCurrentTCB->uxPriority > xMutexHolder.xTaskHandle->uxPriority)
                {
                    //记录原始优先级
                    if (xMutexHolder.uxOriginalPriority == 0)
                    {
                        xMutexHolder.uxOriginalPriority = xMutexHolder.xTaskHandle->uxPriority;
                    }
                    xMutexHolder.xTaskHandle->uxPriority = pxCurrentTCB->uxPriority;
                    //重新排序就绪列表
                    htListRemove(&(xMutexHolder.xTaskHandle->xStateListItem));
                    htListInsertEnd(&(pxReadyTasksLists[xMutexHolder.xTaskHandle->uxPriority]), &(xMutexHolder.xTaskHandle->xStateListItem));
                }
            }
            /* 互斥量被其他任务持有，需要等待 */
            if (xTicksToWait > 0)
            {
                /* 从就绪列表中移除当前任务 */
                htListRemove(&(pxCurrentTCB->xStateListItem));
                
                /* 设置唤醒时间 */
                TickType_t xTimeToWake = 0;
                if (xTicksToWait != 0xFFFFFFFF) /* 0xFFFFFFFF表示永久等待 */
                {
                    xTimeToWake = xTickCount + xTicksToWait;
                    htListSetItemValue(&(pxCurrentTCB->xEventListItem), xTimeToWake);
                }
                
                /* 添加到接收等待队列 */
                htListInsertEnd(&(((htQUEUE_t *)xSemaphore)->xTasksWaitingToReceive), &(pxCurrentTCB->xEventListItem));
                
                /* 标记任务为阻塞状态 */
                pxCurrentTCB->uxTaskState = HT_TASK_BLOCKED;
                
                /* 退出临界区并进行任务切换 */
                htExitCritical();
                htTaskYield();
                
                /* 任务被唤醒后继续执行，再次尝试获取 */
                return htSemaphoreTakeMutex(xSemaphore, 0);
            }
            else
            {
                /* 不等待，直接失败 */
                htExitCritical();
                return htFAIL;
            }
        }
    }
    /* 使用队列接收实现互斥量获取 */
    xReturn = htQueueReceive(xSemaphore, ucBuffer, xTicksToWait);

    /* 释放中断 */
    htExitCritical();

    return xReturn;
}





/**
 * 递归互斥量获取
 */
BaseType_t htSemaphoreTakeRecursive(SemaphoreHandle_t xSemaphore, TickType_t xTicksToWait)
{
#if configUSE_RECURSIVE_MUTEXES == 1
    BaseType_t xReturn = htFAIL;
    htQUEUE_t *pxQueue = (htQUEUE_t *)xSemaphore;
    htMutexHolder_t xMutexHolder;
    
    /* 检查参数有效性 */
    if (pxQueue == NULL)
    {
        return htFAIL;
    }
    
    /* 禁用中断 */
    htEnterCritical();
    
    /* 检查互斥量是否可用或当前任务是否持有者 */
    if (htQueuePeek(xSemaphore, &xMutexHolder, 0) == htPASS)
    {
        //如果当前任务优先级高于持有者任务优先级，则优先级继承
        if (xMutexHolder.xTaskHandle != NULL && xMutexHolder.xTaskHandle != pxCurrentTCB)
        {
            if (pxCurrentTCB->uxPriority > xMutexHolder.xTaskHandle->uxPriority)
            {
                //记录原始优先级
                if (xMutexHolder.uxOriginalPriority == 0)
                {
                    xMutexHolder.uxOriginalPriority = xMutexHolder.xTaskHandle->uxPriority;
                }
                xMutexHolder.xTaskHandle->uxPriority = pxCurrentTCB->uxPriority;
                //重新排序就绪列表
                htListRemove(&(xMutexHolder.xTaskHandle->xStateListItem));
                htListInsertEnd(&(pxReadyTasksLists[xMutexHolder.xTaskHandle->uxPriority]), &(xMutexHolder.xTaskHandle->xStateListItem));
            }
        }
        /* 互斥量可用，检查是否已被当前任务持有 */
        if (xMutexHolder.xTaskHandle == NULL || xMutexHolder.xTaskHandle == pxCurrentTCB)
        {
            /* 更新互斥量持有者信息 */
            htQueueReceive(xSemaphore, &xMutexHolder, 0);
            
            if (xMutexHolder.xTaskHandle == NULL)
            {
                /* 首次获取 */
                xMutexHolder.xTaskHandle = pxCurrentTCB;
                xMutexHolder.uxRecursiveCallCount = 1;
            }
            else
            {
                /* 递归获取 */
                xMutexHolder.uxRecursiveCallCount++;
            }
            
            /* 将更新后的持有者信息存回互斥量 */
            htQueueSend(xSemaphore, &xMutexHolder, 0);
            
            xReturn = htPASS;
        }
        else if (xTicksToWait > 0)
        {
            /* 互斥量被其他任务持有，需要等待 */
            /* 禁用中断 */
            htEnterCritical();
            
            /* 从就绪列表中移除当前任务 */
            htListRemove(&(pxCurrentTCB->xStateListItem));
            
            /* 设置唤醒时间 */
            TickType_t xTimeToWake = 0;
            if (xTicksToWait != 0xFFFFFFFF) /* 0xFFFFFFFF表示永久等待 */
            {
                xTimeToWake = xTickCount + xTicksToWait;
                htListSetItemValue(&(pxCurrentTCB->xEventListItem), xTimeToWake);
            }
            
            /* 添加到接收等待队列 */
            htListInsertEnd(&(pxQueue->xTasksWaitingToReceive), &(pxCurrentTCB->xEventListItem));
            
            /* 标记任务为阻塞状态 */
            pxCurrentTCB->uxTaskState = HT_TASK_BLOCKED;
            
            /* 退出临界区并进行任务切换 */
            htExitCritical();
            htTaskYield();
            
            /* 任务被唤醒后继续执行，再次尝试获取 */
            return htSemaphoreTakeRecursive(xSemaphore, 0);
        }
    }
    
    /* 退出临界区 */
    htExitCritical();
    
    return xReturn;
#else
    /* 递归互斥量未启用 */
    (void)xSemaphore;
    (void)xTicksToWait;
    return htFAIL;
#endif
}


/**
 * 互斥量释放
 */

BaseType_t htSemaphoreGiveMutex(SemaphoreHandle_t xSemaphore)
{
    BaseType_t xReturn;
    uint8_t ucDummy = 0; /* 占位数据，实际内容无关紧要 */
    //检查参数有效性
    if (xSemaphore == NULL)
    {
        return htFAIL;
    }
    /* 禁用中断 */
    htEnterCritical();
    /* 恢复原始优先级（如果有） */
    htMutexHolder_t xMutexHolder;
    if (htQueuePeek(xSemaphore, &xMutexHolder, 0) == htPASS)
    {
        if (xMutexHolder.xTaskHandle == pxCurrentTCB)
        {
            if (xMutexHolder.uxOriginalPriority != 0)
            {
                xMutexHolder.xTaskHandle->uxPriority = xMutexHolder.uxOriginalPriority;
                //重新排序就绪列表
                htListRemove(&(xMutexHolder.xTaskHandle->xStateListItem));
                htListInsertEnd(&(pxReadyTasksLists[xMutexHolder.xTaskHandle->uxPriority]), &(xMutexHolder.xTaskHandle->xStateListItem));
                
                xMutexHolder.uxOriginalPriority = 0; //重置原始优先级
            }
        }
    }
   

    /* 使用队列发送实现互斥量释放 */
    xReturn = htQueueSend(xSemaphore, &ucDummy, 0);
    
    /* 释放中断 */
    htExitCritical();
    
    return xReturn;
}



/**
 * 递归互斥量释放
 */
BaseType_t htSemaphoreGiveRecursive(SemaphoreHandle_t xSemaphore)
{
#if configUSE_RECURSIVE_MUTEXES == 1
    BaseType_t xReturn = htFAIL;
    htQUEUE_t *pxQueue = (htQUEUE_t *)xSemaphore;
    htMutexHolder_t xMutexHolder;
    
    /* 检查参数有效性 */
    if (pxQueue == NULL)
    {
        return htFAIL;
    }
    
    /* 禁用中断 */
    htEnterCritical();
    
    /* 检查互斥量是否由当前任务持有 */
    if (htQueuePeek(xSemaphore, &xMutexHolder, 0) == htPASS)
    {
        if (xMutexHolder.xTaskHandle == pxCurrentTCB)
        {
            /* 恢复原始优先级（如果有） */
            if (xMutexHolder.uxOriginalPriority != 0)
            {
                xMutexHolder.xTaskHandle->uxPriority = xMutexHolder.uxOriginalPriority;
                //重新排序就绪列表
                htListRemove(&(xMutexHolder.xTaskHandle->xStateListItem));
                htListInsertEnd(&(pxReadyTasksLists[xMutexHolder.xTaskHandle->uxPriority]), &(xMutexHolder.xTaskHandle->xStateListItem));
                
                xMutexHolder.uxOriginalPriority = 0; //重置原始优先级
            }
            /* 当前任务持有互斥量，更新持有者信息 */
            htQueueReceive(xSemaphore, &xMutexHolder, 0);
            
            /* 减少递归计数 */
            xMutexHolder.uxRecursiveCallCount--;
            
            if (xMutexHolder.uxRecursiveCallCount == 0)
            {
                /* 递归计数为0，互斥量完全释放 */
                xMutexHolder.xTaskHandle = NULL;
            }
            
            /* 将更新后的持有者信息存回互斥量 */
            htQueueSend(xSemaphore, &xMutexHolder, 0);
            
            xReturn = htPASS;
        }
    }
    
    /* 退出临界区 */
    htExitCritical();
    
    return xReturn;
#else
    /* 递归互斥量未启用 */
    (void)xSemaphore;
    return htFAIL;
#endif
}
