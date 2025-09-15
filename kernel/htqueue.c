#include <string.h>
#include "htqueue.h"
#include "httask.h"
#include "htmem.h"
#include "htscheduler.h"

/**
 * 初始化队列
 */
static void prvInitialiseNewQueue(UBaseType_t uxQueueLength, UBaseType_t uxItemSize, htQUEUE_t *pxNewQueue)
{
    /* 队列结构已分配，现在创建队列存储区 */
    if (uxItemSize == 0)
    {
        /* 二值信号量或计数信号量不需要存储区 */
        pxNewQueue->pcHead = NULL;
    }
    else
    {
        /* 分配队列存储区 */
        pxNewQueue->pcHead = (int8_t *)htPortMalloc(uxQueueLength * uxItemSize);
    }

    if (pxNewQueue->pcHead != NULL || uxItemSize == 0)
    {
        /* 初始化队列成员 */
        pxNewQueue->uxLength = uxQueueLength;
        pxNewQueue->uxItemSize = uxItemSize;
        pxNewQueue->pcWriteTo = pxNewQueue->pcHead;
        pxNewQueue->uxMessagesWaiting = 0;
        pxNewQueue->cRxLock = htQUEUE_UNLOCKED;
        pxNewQueue->cTxLock = htQUEUE_UNLOCKED;

        /* 初始化指针 */
        if (uxItemSize > 0)
        {
            /* 队列指针初始化 */
            pxNewQueue->u.xQueue.pcTail = pxNewQueue->pcHead + (uxQueueLength * uxItemSize);
            pxNewQueue->u.xQueue.pcReadFrom = pxNewQueue->pcHead;
        }
        else
        {
            /* 信号量初始化 - 初始计数为0 */
            pxNewQueue->u.xSemaphore.uxSemaphoreCount = 0;
        }

        /* 初始化任务等待列表 */
        htListInit(&(pxNewQueue->xTasksWaitingToSend));
        htListInit(&(pxNewQueue->xTasksWaitingToReceive));
    }
    else
    {
        /* 如果存储区分配失败，释放队列结构 */
        htPortFree(pxNewQueue);
    }
}

/**
 * 创建队列
 */
QueueHandle_t htQueueCreate(UBaseType_t uxQueueLength, UBaseType_t uxItemSize)
{
    htQUEUE_t *pxNewQueue;

    /* 至少有一个空间 */
    if (uxQueueLength < 1)
    {
        return NULL;
    }

    /* 分配队列结构内存 */
    pxNewQueue = (htQUEUE_t *)htPortMalloc(sizeof(htQUEUE_t));

    if (pxNewQueue != NULL)
    {
        prvInitialiseNewQueue(uxQueueLength, uxItemSize, pxNewQueue);
    }

    return pxNewQueue;
}

/**
 * 复制数据到队列
 */
static void prvCopyDataToQueue(htQUEUE_t *pxQueue, const void *pvItemToQueue)
{
    if (pxQueue->uxItemSize == 0)
    {
        /* 如果是信号量，只增加计数 */
        pxQueue->u.xSemaphore.uxSemaphoreCount++;
    }
    else
    {
        /* 复制数据到队列 */
        memcpy((void *)pxQueue->pcWriteTo, pvItemToQueue, pxQueue->uxItemSize);
        
        /* 更新写指针 */
        pxQueue->pcWriteTo += pxQueue->uxItemSize;
        if (pxQueue->pcWriteTo >= pxQueue->u.xQueue.pcTail)
        {
            pxQueue->pcWriteTo = pxQueue->pcHead;
        }
    }
    
    /* 更新消息计数 */
    pxQueue->uxMessagesWaiting++;
}

/**
 * 从队列复制数据
 */
static void prvCopyDataFromQueue(htQUEUE_t *pxQueue, void *pvBuffer)
{
    if (pxQueue->uxItemSize != 0)
    {
        /* 复制数据到用户缓冲区 */
        memcpy(pvBuffer, (void *)pxQueue->u.xQueue.pcReadFrom, pxQueue->uxItemSize);
        
        /* 更新读指针 */
        pxQueue->u.xQueue.pcReadFrom += pxQueue->uxItemSize;
        if (pxQueue->u.xQueue.pcReadFrom >= pxQueue->u.xQueue.pcTail)
        {
            pxQueue->u.xQueue.pcReadFrom = pxQueue->pcHead;
        }
    }
    
    /* 更新消息计数 */
    pxQueue->uxMessagesWaiting--;
}

/**
 * 解锁队列
 */
static void prvUnlockQueue(htQUEUE_t *pxQueue)
{
    /* 该函数应在临界区内调用 */
    
    /* 如果有发送锁定 */
    if (pxQueue->cTxLock != htQUEUE_UNLOCKED)
    {
        pxQueue->cTxLock = htQUEUE_UNLOCKED;
    }
    
    /* 如果有接收锁定 */
    if (pxQueue->cRxLock != htQUEUE_UNLOCKED)
    {
        pxQueue->cRxLock = htQUEUE_UNLOCKED;
    }
}

/**
 * 发送数据到队列
 */
BaseType_t htQueueSend(QueueHandle_t xQueue, const void *pvItemToQueue, TickType_t xTicksToWait)
{
    BaseType_t xReturn = htFAIL;
    htQUEUE_t *pxQueue = (htQUEUE_t *)xQueue;
		TickType_t xTimeToWake =0;
    
    /* 检查参数有效性 */
    if (pxQueue == NULL)
    {
        return htFAIL;
    }
    
    /* 禁用中断 */
    htEnterCritical();
    
    /* 检查是否有空间 */
    if (pxQueue->uxMessagesWaiting < pxQueue->uxLength || pxQueue->uxItemSize == 0)
    {
        /* 有空间，直接复制数据 */
        prvCopyDataToQueue(pxQueue, pvItemToQueue);
        
        /* 如果有任务在等待读取，唤醒一个优先级最高的任务 */
        if (htListGetCurrentNumberOfItems(&(pxQueue->xTasksWaitingToReceive)) > 0)
        {
            /* 获取等待列表中的第一个任务(优先级最高) */
            htListItem_t *pxItem = htListGetHead(&(pxQueue->xTasksWaitingToReceive));
            if (pxItem != NULL)
            {
                /* 获取任务TCB并移出等待列表 */
                htTCB_t *pxTCB = (htTCB_t *)htListGetItemOwner(pxItem);
                htListRemove(pxItem);
                
                /* 添加到就绪列表 */
                pxTCB->uxTaskState = HT_TASK_READY;
                htListInsertEnd(&(pxReadyTasksLists[pxTCB->uxPriority]), &(pxTCB->xStateListItem));
            }
        }
        
        xReturn = htPASS;
    }
		else if( pxQueue->uxMessagesWaiting == pxQueue->uxLength)
		{
			return xReturn;
		}
    else if (xTicksToWait > 0) 
    {
				__disable_irq();
				if (pxCurrentTCB == NULL) {
        __enable_irq();
        return xReturn;
        }       
        /* 从就绪列表中删除当前任务 */
        htListRemove(&(pxCurrentTCB->xStateListItem));
        
        /* 设置唤醒时间 */
        if (xTicksToWait != 0xFFFFFFFF) /* 0xFFFFFFFF表示永久等待 */
        {
            xTimeToWake = xTickCount + xTicksToWait;
            htListSetItemValue(&(pxCurrentTCB->xEventListItem), xTimeToWake);
        }
        
        /* 添加到发送等待队列 */
        htListInsertEnd(&(pxQueue->xTasksWaitingToSend), &(pxCurrentTCB->xEventListItem));
        
        /* 标记任务为阻塞状态 */
        pxCurrentTCB->uxTaskState = HT_TASK_BLOCKED;
        
        /* 退出临界区并进行任务切换 */
				__enable_irq();
        htTaskYield();
        
        /* 任务被唤醒后继续执行，再次尝试发送 */
        return htQueueSend(xQueue, pvItemToQueue, xTicksToWait);
    }
    
    /* 退出临界区 */
    htExitCritical();
    
    return xReturn;
}

/**
 * 从队列接收数据
 */
BaseType_t htQueueReceive(QueueHandle_t xQueue, void *pvBuffer, TickType_t xTicksToWait)
{
    BaseType_t xReturn = htFAIL;
    htQUEUE_t *pxQueue = (htQUEUE_t *)xQueue;
    TickType_t xTimeToWake= 0;
    /* 检查参数有效性 */
    if (pxQueue == NULL || pvBuffer == NULL)
    {
        return htFAIL;
    }
    
    /* 禁用中断 */
    htEnterCritical();
    
    /* 检查是否有消息 */
    if (pxQueue->uxMessagesWaiting > 0)
    {
        /* 有消息，直接接收 */
        prvCopyDataFromQueue(pxQueue, pvBuffer);
        
        /* 如果有任务在等待发送，唤醒一个优先级最高的任务 */
        if (htListGetCurrentNumberOfItems(&(pxQueue->xTasksWaitingToSend)) > 0)
        {
            /* 获取等待列表中的第一个任务(优先级最高) */
            htListItem_t *pxItem = htListGetHead(&(pxQueue->xTasksWaitingToSend));
            if (pxItem != NULL)
            {
                /* 获取任务TCB并移出等待列表 */
                htTCB_t *pxTCB = (htTCB_t *)htListGetItemOwner(pxItem);
                htListRemove(pxItem);
                
                /* 添加到就绪列表 */
                pxTCB->uxTaskState = HT_TASK_READY;
                htListInsertEnd(&(pxReadyTasksLists[pxTCB->uxPriority]), &(pxTCB->xStateListItem));
            }
        }
        
        xReturn = htPASS;
    }
		
    else if (xTicksToWait > 0)
    {
				__disable_irq();
				if (pxCurrentTCB == NULL) {
        __enable_irq();
        return xReturn;
        }     
        
        /* 将当前任务加入接收等待队列并阻塞 */
        htListItem_t *pxStateListItem = &(pxCurrentTCB->xStateListItem);
        
        /* 从就绪列表中删除当前任务 */
        htListRemove(pxStateListItem);
        
        /* 设置唤醒时间 */
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
				__enable_irq();
        htTaskYield();
				return htQueueReceive(xQueue, pvBuffer, xTicksToWait);
    }
    
    /* 退出临界区 */
    htExitCritical();
    
    return xReturn;
}

/**
 * 从ISR中发送数据到队列
 */
BaseType_t htQueueSendFromISR(QueueHandle_t xQueue, const void *pvItemToQueue, BaseType_t *pxHigherPriorityTaskWoken)
{
    BaseType_t xReturn = htFAIL;
    htQUEUE_t *pxQueue = (htQUEUE_t *)xQueue;
    
    /* 检查参数有效性 */
    if (pxQueue == NULL)
    {
        return htFAIL;
    }
    
    /* 默认没有更高优先级任务被唤醒 */
    if (pxHigherPriorityTaskWoken != NULL)
    {
        *pxHigherPriorityTaskWoken = htFALSE;
    }
    
    /* 检查是否有空间 */
    if (pxQueue->uxMessagesWaiting < pxQueue->uxLength || pxQueue->uxItemSize == 0)
    {
        /* 有空间，直接复制数据 */
        prvCopyDataToQueue(pxQueue, pvItemToQueue);
        
        /* 如果有任务在等待读取，唤醒一个优先级最高的任务 */
        if (htListGetCurrentNumberOfItems(&(pxQueue->xTasksWaitingToReceive)) > 0)
        {
            /* 获取等待列表中的第一个任务(优先级最高) */
            htListItem_t *pxItem = htListGetHead(&(pxQueue->xTasksWaitingToReceive));
            if (pxItem != NULL)
            {
                /* 获取任务TCB并移出等待列表 */
                htTCB_t *pxTCB = (htTCB_t *)htListGetItemOwner(pxItem);
                htListRemove(pxItem);
                
                /* 添加到就绪列表 */
                pxTCB->uxTaskState = HT_TASK_READY;
                htListInsertEnd(&(pxReadyTasksLists[pxTCB->uxPriority]), &(pxTCB->xStateListItem));
                
                /* 检查唤醒的任务优先级是否高于当前运行的任务 */
                if (pxTCB->uxPriority > pxCurrentTCB->uxPriority)
                {
                    if (pxHigherPriorityTaskWoken != NULL)
                    {
                        *pxHigherPriorityTaskWoken = htTRUE;
                    }
                }
            }
        }
        
        xReturn = htPASS;
    }
    
    return xReturn;
}

/**
 * 从ISR中从队列接收数据
 */
BaseType_t htQueueReceiveFromISR(QueueHandle_t xQueue, void *pvBuffer, BaseType_t *pxHigherPriorityTaskWoken)
{
    BaseType_t xReturn = htFAIL;
    htQUEUE_t *pxQueue = (htQUEUE_t *)xQueue;
    
    /* 检查参数有效性 */
    if (pxQueue == NULL || pvBuffer == NULL)
    {
        return htFAIL;
    }
    
    /* 默认没有更高优先级任务被唤醒 */
    if (pxHigherPriorityTaskWoken != NULL)
    {
        *pxHigherPriorityTaskWoken = htFALSE;
    }
    
    /* 检查是否有消息 */
    if (pxQueue->uxMessagesWaiting > 0)
    {
        /* 有消息，直接接收 */
        prvCopyDataFromQueue(pxQueue, pvBuffer);
        
        /* 如果有任务在等待发送，唤醒一个优先级最高的任务 */
        if (htListGetCurrentNumberOfItems(&(pxQueue->xTasksWaitingToSend)) > 0)
        {
            /* 获取等待列表中的第一个任务(优先级最高) */
            htListItem_t *pxItem = htListGetHead(&(pxQueue->xTasksWaitingToSend));
            if (pxItem != NULL)
            {
                /* 获取任务TCB并移出等待列表 */
                htTCB_t *pxTCB = (htTCB_t *)htListGetItemOwner(pxItem);
                htListRemove(pxItem);
                
                /* 添加到就绪列表 */
                pxTCB->uxTaskState = HT_TASK_READY;
                htListInsertEnd(&(pxReadyTasksLists[pxTCB->uxPriority]), &(pxTCB->xStateListItem));
                
                /* 检查唤醒的任务优先级是否高于当前运行的任务 */
                if (pxTCB->uxPriority > pxCurrentTCB->uxPriority)
                {
                    if (pxHigherPriorityTaskWoken != NULL)
                    {
                        *pxHigherPriorityTaskWoken = htTRUE;
                    }
                }
            }
        }
        
        xReturn = htPASS;
    }
    
    return xReturn;
}

/**
 * 查询队列中等待的消息数
 */
UBaseType_t htQueueMessagesWaiting(QueueHandle_t xQueue)
{
    htQUEUE_t *pxQueue = (htQUEUE_t *)xQueue;
    UBaseType_t uxReturn;
    
    if (pxQueue == NULL)
    {
        return 0;
    }
    
    htEnterCritical();
    uxReturn = pxQueue->uxMessagesWaiting;
    htExitCritical();
    
    return uxReturn;
}

/**
 * 重置队列
 */
BaseType_t htQueueReset(QueueHandle_t xQueue)
{
    htQUEUE_t *pxQueue = (htQUEUE_t *)xQueue;
    
    if (pxQueue == NULL)
    {
        return htFAIL;
    }
    
    htEnterCritical();
    
    /* 重置队列指针和计数 */
    pxQueue->uxMessagesWaiting = 0;
    pxQueue->pcWriteTo = pxQueue->pcHead;
    pxQueue->u.xQueue.pcReadFrom = pxQueue->pcHead;
    pxQueue->cRxLock = htQUEUE_UNLOCKED;
    pxQueue->cTxLock = htQUEUE_UNLOCKED;
    
    /* 如果是信号量，重置计数 */
    if (pxQueue->uxItemSize == 0)
    {
        pxQueue->u.xSemaphore.uxSemaphoreCount = 0;
    }
    
    htExitCritical();
    
    return htPASS;
}

/**
 * 删除队列
 */
void htQueueDelete(QueueHandle_t xQueue)
{
    htQUEUE_t *pxQueue = (htQUEUE_t *)xQueue;
    
    if (pxQueue != NULL)
    {
        /* 释放队列存储区 */
        if (pxQueue->pcHead != NULL)
        {
            htPortFree(pxQueue->pcHead);
        }
        
        /* 释放队列结构 */
        htPortFree(pxQueue);
    }
}

/**
 * 查看队列中的数据但不移除
 */
BaseType_t htQueuePeek(QueueHandle_t xQueue, void *pvBuffer, TickType_t xTicksToWait)
{
    BaseType_t xReturn = htFAIL;
    htQUEUE_t *pxQueue = (htQUEUE_t *)xQueue;
    
    /* 检查参数有效性 */
    if (pxQueue == NULL || pvBuffer == NULL)
    {
        return htFAIL;
    }
    
    /* 禁用中断 */
    htEnterCritical();
    
    /* 检查是否有消息 */
    if (pxQueue->uxMessagesWaiting > 0)
    {
        /* 有消息，复制但不移除 */
        if (pxQueue->uxItemSize != 0)
        {
            memcpy(pvBuffer, (void *)pxQueue->u.xQueue.pcReadFrom, pxQueue->uxItemSize);
        }
        
        xReturn = htPASS;
    }
    else if (xTicksToWait > 0)
    {
        /* 队列为空，需要等待 */
        /* 类似于htQueueReceive的等待逻辑 */
        htListItem_t *pxStateListItem = &(pxCurrentTCB->xStateListItem);
        
        /* 从就绪列表中删除当前任务 */
        htListRemove(pxStateListItem);
        
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
        
        /* 任务被唤醒后继续执行，再次尝试查看 */
        return htQueuePeek(xQueue, pvBuffer, 0);
    }
    
    /* 退出临界区 */
    htExitCritical();
    
    return xReturn;
}
