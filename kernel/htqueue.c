#include <string.h>
#include "htqueue.h"
#include "httask.h"
#include "htmem.h"
#include "htscheduler.h"

/**
 * 初始化队列
 */
static BaseType_t prvInitialiseNewQueue(UBaseType_t uxQueueLength, UBaseType_t uxItemSize, htQUEUE_t *pxNewQueue)
{
	/* 队列结构已分配，现在创建队列存储区 */
	if (uxItemSize == 0) {
		/* 二值信号量或计数信号量不需要存储区 */
		pxNewQueue->pcHead = NULL;
	} else {
		/* 分配队列存储区 */
		pxNewQueue->pcHead = (int8_t *)htPortMalloc(uxQueueLength * uxItemSize);
	}

	if (pxNewQueue->pcHead != NULL || uxItemSize == 0) {
		/* 初始化队列成员 */
		pxNewQueue->uxLength = uxQueueLength;
		pxNewQueue->uxItemSize = uxItemSize;
		pxNewQueue->pcWriteTo = pxNewQueue->pcHead;
		pxNewQueue->uxMessagesWaiting = 0;
		pxNewQueue->cRxLock = htQUEUE_UNLOCKED;
		pxNewQueue->cTxLock = htQUEUE_UNLOCKED;

		/* 初始化指针 */
		if (uxItemSize > 0) {
			/* 队列指针初始化 */
			pxNewQueue->u.xQueue.pcTail = pxNewQueue->pcHead + (uxQueueLength * uxItemSize);
			pxNewQueue->u.xQueue.pcReadFrom = pxNewQueue->pcHead;
		} else {
			/* 信号量初始化 - 初始计数为0 */
			pxNewQueue->u.xSemaphore.uxSemaphoreCount = 0;
		}

		/* 初始化任务等待列表 */
		htListInit(&(pxNewQueue->xTasksWaitingToSend));
		htListInit(&(pxNewQueue->xTasksWaitingToReceive));
		return htPASS;
	}

	return htFAIL;
}

/**
 * 创建队列
 */
QueueHandle_t htQueueCreate(UBaseType_t uxQueueLength, UBaseType_t uxItemSize)
{
	htQUEUE_t *pxNewQueue;

	/* 至少有一个空间 */
	if (uxQueueLength < 1U || (uxItemSize != 0U && uxQueueLength > SIZE_MAX / uxItemSize)) {
		return NULL;
	}

	/* 分配队列结构内存 */
	pxNewQueue = (htQUEUE_t *)htPortMalloc(sizeof(htQUEUE_t));

	if (pxNewQueue != NULL) {
		if (prvInitialiseNewQueue(uxQueueLength, uxItemSize, pxNewQueue) != htPASS) {
			htPortFree(pxNewQueue);
			pxNewQueue = NULL;
		}
	}

	return pxNewQueue;
}

/**
 * 复制数据到队列
 */
static void prvCopyDataToQueue(htQUEUE_t *pxQueue, const void *pvItemToQueue)
{
	if (pxQueue->uxItemSize == 0) {
		/* 如果是信号量，只增加计数 */
		pxQueue->u.xSemaphore.uxSemaphoreCount++;
	} else {
		/* 复制数据到队列 */
		memcpy((void *)pxQueue->pcWriteTo, pvItemToQueue, pxQueue->uxItemSize);

		/* 更新写指针 */
		pxQueue->pcWriteTo += pxQueue->uxItemSize;
		if (pxQueue->pcWriteTo >= pxQueue->u.xQueue.pcTail) {
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
	if (pxQueue->uxItemSize != 0) {
		/* 复制数据到用户缓冲区 */
		memcpy(pvBuffer, (void *)pxQueue->u.xQueue.pcReadFrom, pxQueue->uxItemSize);

		/* 更新读指针 */
		pxQueue->u.xQueue.pcReadFrom += pxQueue->uxItemSize;
		if (pxQueue->u.xQueue.pcReadFrom >= pxQueue->u.xQueue.pcTail) {
			pxQueue->u.xQueue.pcReadFrom = pxQueue->pcHead;
		}
	} else if (pxQueue->u.xSemaphore.uxSemaphoreCount > 0U) {
		pxQueue->u.xSemaphore.uxSemaphoreCount--;
	}

	/* 更新消息计数 */
	pxQueue->uxMessagesWaiting--;
}

/* Must be called with the kernel critical section held. */
static BaseType_t prvBlockCurrentTask(htList_t *pxEventList, TickType_t xTicksToWait)
{
	if (pxCurrentTCB == NULL) {
		return htFAIL;
	}
	if (xTicksToWait != htBLOCKED_INDEFINITELY && (pxDelayedTaskList == NULL || pxOverflowDelayedTaskList == NULL)) {
		return htFAIL;
	}

	htListRemove(&(pxCurrentTCB->xStateListItem));
	htListRemove(&(pxCurrentTCB->xEventListItem));

	htListSetItemValue(&(pxCurrentTCB->xEventListItem), configMAX_PRIORITIES - pxCurrentTCB->uxPriority);
	htListInsert(pxEventList, &(pxCurrentTCB->xEventListItem));

	if (xTicksToWait != htBLOCKED_INDEFINITELY) {
		const TickType_t xTimeToWake = xTickCount + xTicksToWait;
		htListSetItemValue(&(pxCurrentTCB->xStateListItem), xTimeToWake);
		if (xTimeToWake > xTickCount) {
			htListInsert(pxDelayedTaskList, &(pxCurrentTCB->xStateListItem));
		} else {
			htListInsert(pxOverflowDelayedTaskList, &(pxCurrentTCB->xStateListItem));
		}
	}

	pxCurrentTCB->uxTaskState = HT_TASK_BLOCKED;
	return htPASS;
}

/* Must be called while queue/list state is protected. */
static htTCB_t *prvUnblockFirstTask(htList_t *pxEventList)
{
	htListItem_t *pxItem = htListGetHead(pxEventList);
	if (pxItem == NULL) {
		return NULL;
	}

	htTCB_t *pxTCB = (htTCB_t *)htListGetItemOwner(pxItem);
	htListRemove(pxItem);
	if (pxTCB == NULL || pxTCB->uxPriority >= configMAX_PRIORITIES) {
		return NULL;
	}

	/* An event wake-up cancels the task's timeout entry. */
	htListRemove(&(pxTCB->xStateListItem));
	htListSetItemValue(&(pxTCB->xStateListItem), configMAX_PRIORITIES - pxTCB->uxPriority);
	pxTCB->uxTaskState = HT_TASK_READY;
	htListInsertEnd(&(pxReadyTasksLists[pxTCB->uxPriority]), &(pxTCB->xStateListItem));
	return pxTCB;
}

/**
 * 发送数据到队列
 */
BaseType_t htQueueSend(QueueHandle_t xQueue, const void *pvItemToQueue, TickType_t xTicksToWait)
{
	htQUEUE_t *pxQueue = (htQUEUE_t *)xQueue;
	const TickType_t xStartTime = xTickCount;
	TickType_t xRemainingTicks = xTicksToWait;

	/* 检查参数有效性 */
	if (pxQueue == NULL || (pxQueue->uxItemSize != 0U && pvItemToQueue == NULL)) {
		return htFAIL;
	}

	for (;;) {
		htEnterCritical();
		if (pxQueue->uxMessagesWaiting < pxQueue->uxLength) {
			prvCopyDataToQueue(pxQueue, pvItemToQueue);
			(void)prvUnblockFirstTask(&(pxQueue->xTasksWaitingToReceive));
			htExitCritical();
			return htPASS;
		}

		if (xRemainingTicks == 0U || prvBlockCurrentTask(&(pxQueue->xTasksWaitingToSend), xRemainingTicks) != htPASS) {
			htExitCritical();
			return htFAIL;
		}
		htExitCritical();
		htTaskYield();

		if (xTicksToWait != htBLOCKED_INDEFINITELY) {
			const TickType_t xElapsed = xTickCount - xStartTime;
			if (xElapsed >= xTicksToWait) {
				return htFAIL;
			}
			xRemainingTicks = xTicksToWait - xElapsed;
		}
	}
}

/**
 * 从队列接收数据
 */
BaseType_t htQueueReceive(QueueHandle_t xQueue, void *pvBuffer, TickType_t xTicksToWait)
{
	htQUEUE_t *pxQueue = (htQUEUE_t *)xQueue;
	const TickType_t xStartTime = xTickCount;
	TickType_t xRemainingTicks = xTicksToWait;
	/* 检查参数有效性 */
	if (pxQueue == NULL || (pxQueue->uxItemSize != 0U && pvBuffer == NULL)) {
		return htFAIL;
	}

	for (;;) {
		htEnterCritical();
		if (pxQueue->uxMessagesWaiting > 0U) {
			prvCopyDataFromQueue(pxQueue, pvBuffer);
			(void)prvUnblockFirstTask(&(pxQueue->xTasksWaitingToSend));
			htExitCritical();
			return htPASS;
		}

		if (xRemainingTicks == 0U ||
				prvBlockCurrentTask(&(pxQueue->xTasksWaitingToReceive), xRemainingTicks) != htPASS) {
			htExitCritical();
			return htFAIL;
		}
		htExitCritical();
		htTaskYield();

		if (xTicksToWait != htBLOCKED_INDEFINITELY) {
			const TickType_t xElapsed = xTickCount - xStartTime;
			if (xElapsed >= xTicksToWait) {
				return htFAIL;
			}
			xRemainingTicks = xTicksToWait - xElapsed;
		}
	}
}

/**
 * 从ISR中发送数据到队列
 */
BaseType_t htQueueSendFromISR(QueueHandle_t xQueue, const void *pvItemToQueue, BaseType_t *pxHigherPriorityTaskWoken)
{
	BaseType_t xReturn = htFAIL;
	htQUEUE_t *pxQueue = (htQUEUE_t *)xQueue;

	/* 检查参数有效性 */
	if (pxQueue == NULL || (pxQueue->uxItemSize != 0U && pvItemToQueue == NULL)) {
		return htFAIL;
	}

	/* 默认没有更高优先级任务被唤醒 */
	if (pxHigherPriorityTaskWoken != NULL) {
		*pxHigherPriorityTaskWoken = htFALSE;
	}

	/* 检查是否有空间 */
	if (pxQueue->uxMessagesWaiting < pxQueue->uxLength) {
		/* 有空间，直接复制数据 */
		prvCopyDataToQueue(pxQueue, pvItemToQueue);

		htTCB_t *pxUnblockedTCB = prvUnblockFirstTask(&(pxQueue->xTasksWaitingToReceive));
		if (pxUnblockedTCB != NULL && (pxCurrentTCB == NULL || pxUnblockedTCB->uxPriority > pxCurrentTCB->uxPriority) &&
				pxHigherPriorityTaskWoken != NULL) {
			*pxHigherPriorityTaskWoken = htTRUE;
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
	if (pxQueue == NULL || (pxQueue->uxItemSize != 0U && pvBuffer == NULL)) {
		return htFAIL;
	}

	/* 默认没有更高优先级任务被唤醒 */
	if (pxHigherPriorityTaskWoken != NULL) {
		*pxHigherPriorityTaskWoken = htFALSE;
	}

	/* 检查是否有消息 */
	if (pxQueue->uxMessagesWaiting > 0) {
		/* 有消息，直接接收 */
		prvCopyDataFromQueue(pxQueue, pvBuffer);

		htTCB_t *pxUnblockedTCB = prvUnblockFirstTask(&(pxQueue->xTasksWaitingToSend));
		if (pxUnblockedTCB != NULL && (pxCurrentTCB == NULL || pxUnblockedTCB->uxPriority > pxCurrentTCB->uxPriority) &&
				pxHigherPriorityTaskWoken != NULL) {
			*pxHigherPriorityTaskWoken = htTRUE;
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

	if (pxQueue == NULL) {
		return 0;
	}

	htEnterCritical();
	uxReturn = pxQueue->uxMessagesWaiting;
	htExitCritical();

	return uxReturn;
}

/**
 * 查询队列剩余空间。
 */
UBaseType_t htQueueSpacesAvailable(QueueHandle_t xQueue)
{
	htQUEUE_t *pxQueue = (htQUEUE_t *)xQueue;
	UBaseType_t uxReturn;

	if (pxQueue == NULL) {
		return 0U;
	}

	htEnterCritical();
	uxReturn = pxQueue->uxMessagesWaiting < pxQueue->uxLength ? pxQueue->uxLength - pxQueue->uxMessagesWaiting : 0U;
	htExitCritical();
	return uxReturn;
}

/**
 * 写入长度为 1 的数据队列；队列已满时覆盖旧值。
 */
BaseType_t htQueueOverwrite(QueueHandle_t xQueue, const void *pvItemToQueue)
{
	htQUEUE_t *pxQueue = (htQUEUE_t *)xQueue;
	if (pxQueue == NULL || pvItemToQueue == NULL || pxQueue->uxLength != 1U || pxQueue->uxItemSize == 0U) {
		return htFAIL;
	}

	htEnterCritical();
	if (pxQueue->uxMessagesWaiting == 0U) {
		prvCopyDataToQueue(pxQueue, pvItemToQueue);
		(void)prvUnblockFirstTask(&(pxQueue->xTasksWaitingToReceive));
	} else {
		memcpy(pxQueue->pcHead, pvItemToQueue, pxQueue->uxItemSize);
		pxQueue->pcWriteTo = pxQueue->pcHead;
		pxQueue->u.xQueue.pcReadFrom = pxQueue->pcHead;
	}
	htExitCritical();
	return htPASS;
}

/**
 * 重置队列
 */
BaseType_t htQueueReset(QueueHandle_t xQueue)
{
	htQUEUE_t *pxQueue = (htQUEUE_t *)xQueue;

	if (pxQueue == NULL) {
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
	if (pxQueue->uxItemSize == 0) {
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

	if (pxQueue != NULL) {
		/* 释放队列存储区 */
		if (pxQueue->pcHead != NULL) {
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
	htQUEUE_t *pxQueue = (htQUEUE_t *)xQueue;
	const TickType_t xStartTime = xTickCount;
	TickType_t xRemainingTicks = xTicksToWait;

	/* 检查参数有效性 */
	if (pxQueue == NULL || (pxQueue->uxItemSize != 0U && pvBuffer == NULL)) {
		return htFAIL;
	}

	for (;;) {
		htEnterCritical();
		if (pxQueue->uxMessagesWaiting > 0U) {
			if (pxQueue->uxItemSize != 0U) {
				memcpy(pvBuffer, (void *)pxQueue->u.xQueue.pcReadFrom, pxQueue->uxItemSize);
			}
			htExitCritical();
			return htPASS;
		}

		if (xRemainingTicks == 0U ||
				prvBlockCurrentTask(&(pxQueue->xTasksWaitingToReceive), xRemainingTicks) != htPASS) {
			htExitCritical();
			return htFAIL;
		}
		htExitCritical();
		htTaskYield();

		if (xTicksToWait != htBLOCKED_INDEFINITELY) {
			const TickType_t xElapsed = xTickCount - xStartTime;
			if (xElapsed >= xTicksToWait) {
				return htFAIL;
			}
			xRemainingTicks = xTicksToWait - xElapsed;
		}
	}
}
