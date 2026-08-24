#include "htsemaphore.h"
#include "htmem.h"
#include "htqueue.h"
#include "htscheduler.h"
#include "httask.h"

#define htMUTEX_NO_INHERITED_PRIORITY UINT32_MAX

static void prvMoveReadyTaskToPriority(htTCB_t *pxTask, UBaseType_t uxPriority)
{
	if (pxTask == NULL || uxPriority >= configMAX_PRIORITIES) {
		return;
	}
	htList_t *pxEventList = (htList_t *)pxTask->xEventListItem.pxContainer;
	const BaseType_t xTaskIsReady =
			pxTask->uxTaskState == HT_TASK_READY || pxTask->uxTaskState == HT_TASK_RUNNING ? htTRUE : htFALSE;
	if (xTaskIsReady == htTRUE) {
		htListRemove(&(pxTask->xStateListItem));
	}
	if (pxEventList != NULL) {
		htListRemove(&(pxTask->xEventListItem));
	}
	pxTask->uxPriority = uxPriority;
	htListSetItemValue(&(pxTask->xAllListItem), configMAX_PRIORITIES - uxPriority);
	htListSetItemValue(&(pxTask->xEventListItem), configMAX_PRIORITIES - uxPriority);
	if (xTaskIsReady == htTRUE) {
		htListSetItemValue(&(pxTask->xStateListItem), configMAX_PRIORITIES - uxPriority);
		htListInsertEnd(&(pxReadyTasksLists[uxPriority]), &(pxTask->xStateListItem));
	}
	if (pxEventList != NULL) {
		htListInsert(pxEventList, &(pxTask->xEventListItem));
	}
}

static void prvRestoreMutexOwnerPriority(htMutexHolder_t *pxMutexHolder)
{
	if (pxMutexHolder == NULL || pxMutexHolder->xTaskHandle == NULL ||
			pxMutexHolder->uxOriginalPriority == htMUTEX_NO_INHERITED_PRIORITY) {
		return;
	}

	prvMoveReadyTaskToPriority(pxMutexHolder->xTaskHandle, pxMutexHolder->uxOriginalPriority);
	pxMutexHolder->uxOriginalPriority = htMUTEX_NO_INHERITED_PRIORITY;
}

static void prvInheritMutexOwnerPriority(htMutexHolder_t *pxMutexHolder)
{
	if (pxMutexHolder == NULL || pxMutexHolder->xTaskHandle == NULL || pxCurrentTCB == NULL) {
		return;
	}
	if (pxCurrentTCB->uxPriority <= pxMutexHolder->xTaskHandle->uxPriority) {
		return;
	}

	if (pxMutexHolder->uxOriginalPriority == htMUTEX_NO_INHERITED_PRIORITY) {
		pxMutexHolder->uxOriginalPriority = pxMutexHolder->xTaskHandle->uxPriority;
	}
	prvMoveReadyTaskToPriority(pxMutexHolder->xTaskHandle, pxCurrentTCB->uxPriority);
}

static BaseType_t prvRestoreRecursiveMutexAfterTimeout(QueueHandle_t xSemaphore, htQUEUE_t *pxQueue)
{
	htMutexHolder_t xMutexHolder;

	if (pxQueue == NULL || pxQueue->xTasksWaitingToReceive.uxNumberOfItems != 0U) {
		return htFAIL;
	}
	if (htQueueReceive(xSemaphore, &xMutexHolder, 0U) != htPASS) {
		return htFAIL;
	}

	prvRestoreMutexOwnerPriority(&xMutexHolder);
	(void)htQueueSend(xSemaphore, &xMutexHolder, 0U);
	return htPASS;
}

SemaphoreHandle_t htSemaphoreCreateBinary(void)
{
	return htQueueCreate(1U, 0U);
}

SemaphoreHandle_t htSemaphoreCreateBinaryStatic(htQUEUE_t *pxQueueBuffer)
{
	return htQueueCreateStatic(1U, 0U, NULL, pxQueueBuffer);
}

SemaphoreHandle_t htSemaphoreCreateCounting(UBaseType_t uxMaxCount, UBaseType_t uxInitialCount)
{
	QueueHandle_t xSemaphore;
	htQUEUE_t *pxQueue;

	if (uxMaxCount == 0U || uxInitialCount > uxMaxCount) {
		return NULL;
	}

	xSemaphore = htQueueCreate(uxMaxCount, 0U);
	if (xSemaphore != NULL) {
		pxQueue = (htQUEUE_t *)xSemaphore;
		pxQueue->u.xSemaphore.uxSemaphoreCount = uxInitialCount;
		pxQueue->uxMessagesWaiting = uxInitialCount;
	}
	return xSemaphore;
}

SemaphoreHandle_t htSemaphoreCreateCountingStatic(
		UBaseType_t uxMaxCount, UBaseType_t uxInitialCount, htQUEUE_t *pxQueueBuffer)
{
	if (pxQueueBuffer == NULL || uxMaxCount == 0U || uxInitialCount > uxMaxCount) {
		return NULL;
	}

	QueueHandle_t xSemaphore = htQueueCreateStatic(uxMaxCount, 0U, NULL, pxQueueBuffer);
	if (xSemaphore != NULL) {
		pxQueueBuffer->u.xSemaphore.uxSemaphoreCount = uxInitialCount;
		pxQueueBuffer->uxMessagesWaiting = uxInitialCount;
	}
	return xSemaphore;
}

SemaphoreHandle_t htSemaphoreCreateMutex(void)
{
	return htSemaphoreCreateCounting(1U, 1U);
}

SemaphoreHandle_t htSemaphoreCreateMutexStatic(htQUEUE_t *pxQueueBuffer)
{
	return htSemaphoreCreateCountingStatic(1U, 1U, pxQueueBuffer);
}

SemaphoreHandle_t htSemaphoreCreateRecursiveMutex(void)
{
#if configUSE_RECURSIVE_MUTEXES == 1
	QueueHandle_t xSemaphore;
	htMutexHolder_t xMutexHolder = {NULL, 0U, htMUTEX_NO_INHERITED_PRIORITY};

	xSemaphore = htQueueCreate(1U, sizeof(htMutexHolder_t));
	if (xSemaphore == NULL) {
		return NULL;
	}
	if (htQueueSend(xSemaphore, &xMutexHolder, 0U) != htPASS) {
		htQueueDelete(xSemaphore);
		return NULL;
	}
	return xSemaphore;
#else
	return NULL;
#endif
}

void htSemaphoreDelete(SemaphoreHandle_t xSemaphore)
{
	htQueueDelete(xSemaphore);
}

UBaseType_t htSemaphoreGetCount(SemaphoreHandle_t xSemaphore)
{
	htQUEUE_t *pxQueue = (htQUEUE_t *)xSemaphore;
	UBaseType_t uxReturn = 0U;

	if (pxQueue != NULL) {
		htEnterCritical();
		uxReturn = pxQueue->uxMessagesWaiting;
		htExitCritical();
	}
	return uxReturn;
}

BaseType_t htSemaphoreTake(SemaphoreHandle_t xSemaphore, TickType_t xTicksToWait)
{
	BaseType_t xReturn;
	uint8_t ucBuffer[4];

	xReturn = htQueueReceive(xSemaphore, ucBuffer, xTicksToWait);
	return xReturn;
}

BaseType_t htSemaphoreGive(SemaphoreHandle_t xSemaphore)
{
	BaseType_t xReturn;
	uint8_t ucDummy = 0U;

	xReturn = htQueueSend(xSemaphore, &ucDummy, 0U);
	return xReturn;
}

BaseType_t htSemaphoreTakeFromISR(SemaphoreHandle_t xSemaphore, BaseType_t *pxHigherPriorityTaskWoken)
{
	BaseType_t xReturn;
	uint8_t ucBuffer[4];

	xReturn = htQueueReceiveFromISR(xSemaphore, ucBuffer, pxHigherPriorityTaskWoken);
	return xReturn;
}

BaseType_t htSemaphoreGiveFromISR(SemaphoreHandle_t xSemaphore, BaseType_t *pxHigherPriorityTaskWoken)
{
	BaseType_t xReturn;
	uint8_t ucDummy = 0U;

	xReturn = htQueueSendFromISR(xSemaphore, &ucDummy, pxHigherPriorityTaskWoken);
	return xReturn;
}

BaseType_t htSemaphoreTakeMutex(SemaphoreHandle_t xSemaphore, TickType_t xTicksToWait)
{
	return htSemaphoreTake(xSemaphore, xTicksToWait);
}

BaseType_t htSemaphoreTakeRecursive(SemaphoreHandle_t xSemaphore, TickType_t xTicksToWait)
{
#if configUSE_RECURSIVE_MUTEXES == 1
	const TickType_t xStartTime = xTickCount;
	TickType_t xRemainingTicks = xTicksToWait;
	htQUEUE_t *pxQueue = (htQUEUE_t *)xSemaphore;
	htMutexHolder_t xMutexHolder;

	if (pxQueue == NULL || pxCurrentTCB == NULL) {
		return htFAIL;
	}

	for (;;) {
		htEnterCritical();
		if (htQueueReceive(xSemaphore, &xMutexHolder, 0U) != htPASS) {
			htExitCritical();
			return htFAIL;
		}

		if (xMutexHolder.xTaskHandle == NULL || xMutexHolder.xTaskHandle == pxCurrentTCB) {
			if (xMutexHolder.xTaskHandle == NULL) {
				xMutexHolder.xTaskHandle = pxCurrentTCB;
				xMutexHolder.uxRecursiveCallCount = 1U;
				xMutexHolder.uxOriginalPriority = htMUTEX_NO_INHERITED_PRIORITY;
			} else {
				xMutexHolder.uxRecursiveCallCount++;
			}
			(void)htQueueSend(xSemaphore, &xMutexHolder, 0U);
			htExitCritical();
			return htPASS;
		}

		if (xRemainingTicks == 0U) {
			(void)htQueueSend(xSemaphore, &xMutexHolder, 0U);
			htExitCritical();
			return htFAIL;
		}

		prvInheritMutexOwnerPriority(&xMutexHolder);
		(void)htQueueSend(xSemaphore, &xMutexHolder, 0U);

		htListRemove(&(pxCurrentTCB->xStateListItem));
		htListRemove(&(pxCurrentTCB->xEventListItem));
		htListSetItemValue(&(pxCurrentTCB->xEventListItem), configMAX_PRIORITIES - pxCurrentTCB->uxPriority);
		htListInsert(&(pxQueue->xTasksWaitingToReceive), &(pxCurrentTCB->xEventListItem));

		if (xRemainingTicks != htBLOCKED_INDEFINITELY) {
			const TickType_t xTimeToWake = xTickCount + xRemainingTicks;
			htListSetItemValue(&(pxCurrentTCB->xStateListItem), xTimeToWake);
			if (xTimeToWake > xTickCount) {
				htListInsert(pxDelayedTaskList, &(pxCurrentTCB->xStateListItem));
			} else {
				htListInsert(pxOverflowDelayedTaskList, &(pxCurrentTCB->xStateListItem));
			}
		}

		pxCurrentTCB->uxTaskState = HT_TASK_BLOCKED;
		htExitCritical();
		htTaskYield();

		if (xTicksToWait != htBLOCKED_INDEFINITELY) {
			const TickType_t xElapsed = xTickCount - xStartTime;
			if (xElapsed >= xTicksToWait) {
				htEnterCritical();
				(void)prvRestoreRecursiveMutexAfterTimeout(xSemaphore, pxQueue);
				htExitCritical();
				return htFAIL;
			}
			xRemainingTicks = xTicksToWait - xElapsed;
		}
	}
#else
	(void)xSemaphore;
	(void)xTicksToWait;
	return htFAIL;
#endif
}

BaseType_t htSemaphoreGiveMutex(SemaphoreHandle_t xSemaphore)
{
	return htSemaphoreGive(xSemaphore);
}

BaseType_t htSemaphoreGiveRecursive(SemaphoreHandle_t xSemaphore)
{
#if configUSE_RECURSIVE_MUTEXES == 1
	BaseType_t xReturn = htFAIL;
	htQUEUE_t *pxQueue = (htQUEUE_t *)xSemaphore;
	htMutexHolder_t xMutexHolder;

	if (pxQueue == NULL || pxCurrentTCB == NULL) {
		return htFAIL;
	}

	htEnterCritical();
	if (htQueueReceive(xSemaphore, &xMutexHolder, 0U) == htPASS) {
		if (xMutexHolder.xTaskHandle == pxCurrentTCB && xMutexHolder.uxRecursiveCallCount > 0U) {
			xMutexHolder.uxRecursiveCallCount--;
			if (xMutexHolder.uxRecursiveCallCount == 0U) {
				prvRestoreMutexOwnerPriority(&xMutexHolder);
				xMutexHolder.xTaskHandle = NULL;
			}
			xReturn = htPASS;
		}
		(void)htQueueSend(xSemaphore, &xMutexHolder, 0U);
	}
	htExitCritical();

	return xReturn;
#else
	(void)xSemaphore;
	return htFAIL;
#endif
}
