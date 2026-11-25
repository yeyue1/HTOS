/**
 * @file hteventgroup.c
 * @brief 24 位事件标志组与原子广播唤醒实现。
 */
#include <string.h>

#include "hteventgroup.h"
#include "htmem.h"
#include "htscheduler.h"
#include "htschedpolicy.h"
#include "httask.h"
#include "htkernel_internal.h"

#if (configUSE_EVENT_GROUPS == 1)

/* 复用等待任务 xEventListItem.xItemValue 的高 8 位存放等待模式与唤醒标记，TCB 零额外开销。 */
#define HT_EVENT_CONTROL_CLEAR_ON_EXIT ((EventBits_t)0x01000000UL)
#define HT_EVENT_CONTROL_WAIT_FOR_ALL ((EventBits_t)0x02000000UL)
#define HT_EVENT_CONTROL_UNBLOCKED ((EventBits_t)0x04000000UL)

static BaseType_t prvTestWaitCondition(
		EventBits_t uxCurrentBits, EventBits_t uxBitsToWaitFor, BaseType_t xWaitForAllBits)
{
	if (xWaitForAllBits != htFALSE) {
		return ((uxCurrentBits & uxBitsToWaitFor) == uxBitsToWaitFor) ? htTRUE : htFALSE;
	}
	return ((uxCurrentBits & uxBitsToWaitFor) != 0U) ? htTRUE : htFALSE;
}

static BaseType_t prvSetBitsLocked(htEventGroup_t *pxGroup, EventBits_t uxBitsToSet, EventBits_t *puxResultBits)
{
	BaseType_t xHigherPriorityWoken = htFALSE;
	EventBits_t uxBitsToClear = 0U;

	pxGroup->uxEventBits |= (uxBitsToSet & HT_EVENT_BITS_MASK);
	const EventBits_t uxSnapshotBits = pxGroup->uxEventBits;

	htListItem_t *const pxEnd = htListGetEndMarker(&(pxGroup->xTasksWaitingForBits));
	htListItem_t *pxItem = htListGetHead(&(pxGroup->xTasksWaitingForBits));

	while (pxItem != NULL && pxItem != pxEnd) {
		htListItem_t *pxNext = htListGetNext(pxItem);
		const EventBits_t uxControl = htListGetItemValue(pxItem);
		const EventBits_t uxWaitBits = uxControl & HT_EVENT_BITS_MASK;
		const BaseType_t xWaitAll = ((uxControl & HT_EVENT_CONTROL_WAIT_FOR_ALL) != 0U) ? htTRUE : htFALSE;

		if (prvTestWaitCondition(uxSnapshotBits, uxWaitBits, xWaitAll) == htTRUE) {
			if ((uxControl & HT_EVENT_CONTROL_CLEAR_ON_EXIT) != 0U) {
				uxBitsToClear |= uxWaitBits;
			}

			htTCB_t *pxTCB = (htTCB_t *)htListGetItemOwner(pxItem);
			htListRemove(pxItem);
			htListSetItemValue(pxItem, uxSnapshotBits | HT_EVENT_CONTROL_UNBLOCKED);

			if (pxTCB != NULL && pxTCB->uxPriority < configMAX_PRIORITIES) {
				htListRemove(&(pxTCB->xStateListItem));
				htListSetItemValue(&(pxTCB->xStateListItem), configMAX_PRIORITIES - pxTCB->uxPriority);
				pxTCB->uxTaskState = HT_TASK_READY;
				htListInsertEnd(&(pxReadyTasksLists[pxTCB->uxPriority]), &(pxTCB->xStateListItem));
				htSchedRecordReadyPriority(pxTCB->uxPriority);
				htSchedNotifyTaskEnqueued(pxTCB);

				if (pxCurrentTCB == NULL || pxTCB->uxPriority > pxCurrentTCB->uxPriority) {
					xHigherPriorityWoken = htTRUE;
				}
			}
		}

		pxItem = pxNext;
	}

	pxGroup->uxEventBits &= ~uxBitsToClear;
	if (puxResultBits != NULL) {
		*puxResultBits = pxGroup->uxEventBits;
	}
	return xHigherPriorityWoken;
}

EventGroupHandle_t htEventGroupCreate(void)
{
	htEventGroup_t *pxGroup = (htEventGroup_t *)htPortMalloc(sizeof(htEventGroup_t));
	if (pxGroup == NULL) {
		return NULL;
	}

	memset(pxGroup, 0, sizeof(*pxGroup));
	pxGroup->uxEventBits = 0U;
	pxGroup->ucStaticallyAllocated = 0U;
	htListInit(&(pxGroup->xTasksWaitingForBits));
	return pxGroup;
}

EventGroupHandle_t htEventGroupCreateStatic(htEventGroup_t *pxEventGroupBuffer)
{
#if (configSUPPORT_STATIC_ALLOCATION == 1)
	if (pxEventGroupBuffer == NULL) {
		return NULL;
	}

	memset(pxEventGroupBuffer, 0, sizeof(*pxEventGroupBuffer));
	pxEventGroupBuffer->uxEventBits = 0U;
	pxEventGroupBuffer->ucStaticallyAllocated = 1U;
	htListInit(&(pxEventGroupBuffer->xTasksWaitingForBits));
	return pxEventGroupBuffer;
#else
	(void)pxEventGroupBuffer;
	return NULL;
#endif
}

EventBits_t htEventGroupWaitBits(EventGroupHandle_t xEventGroup, EventBits_t uxBitsToWaitFor, BaseType_t xClearOnExit,
		BaseType_t xWaitForAllBits, TickType_t xTicksToWait)
{
	htEventGroup_t *pxGroup = (htEventGroup_t *)xEventGroup;
	EventBits_t uxReturn = 0U;

	if (pxGroup == NULL || uxBitsToWaitFor == 0U || (uxBitsToWaitFor & ~HT_EVENT_BITS_MASK) != 0U) {
		return 0U;
	}

	htEnterCritical();
	const EventBits_t uxCurrentBits = pxGroup->uxEventBits;
	if (prvTestWaitCondition(uxCurrentBits, uxBitsToWaitFor, xWaitForAllBits) == htTRUE) {
		uxReturn = uxCurrentBits;
		if (xClearOnExit != htFALSE) {
			pxGroup->uxEventBits &= ~uxBitsToWaitFor;
		}
		htExitCritical();
		return uxReturn;
	}

	if (xTicksToWait == 0U || pxCurrentTCB == NULL ||
			(xTicksToWait != htBLOCKED_INDEFINITELY &&
					(pxDelayedTaskList == NULL || pxOverflowDelayedTaskList == NULL))) {
		uxReturn = uxCurrentBits;
		htExitCritical();
		return uxReturn;
	}

	EventBits_t uxControl = uxBitsToWaitFor;
	if (xClearOnExit != htFALSE) {
		uxControl |= HT_EVENT_CONTROL_CLEAR_ON_EXIT;
	}
	if (xWaitForAllBits != htFALSE) {
		uxControl |= HT_EVENT_CONTROL_WAIT_FOR_ALL;
	}

	htListRemove(&(pxCurrentTCB->xStateListItem));
	htSchedClearReadyPriorityIfEmpty(pxCurrentTCB->uxPriority);
	htListRemove(&(pxCurrentTCB->xEventListItem));
	htListSetItemValue(&(pxCurrentTCB->xEventListItem), uxControl);
	htListInsertEnd(&(pxGroup->xTasksWaitingForBits), &(pxCurrentTCB->xEventListItem));

	if (xTicksToWait != htBLOCKED_INDEFINITELY) {
		const TickType_t xTimeToWake = xTickCount + xTicksToWait;
		htListSetItemValue(&(pxCurrentTCB->xStateListItem), xTimeToWake);
		if (xTimeToWake > xTickCount) {
			htListInsert(pxDelayedTaskList, &(pxCurrentTCB->xStateListItem));
			if (xTimeToWake < xNextTaskUnblockTime) {
				xNextTaskUnblockTime = xTimeToWake;
			}
		} else {
			htListInsert(pxOverflowDelayedTaskList, &(pxCurrentTCB->xStateListItem));
		}
	}

	pxCurrentTCB->uxTaskState = HT_TASK_BLOCKED;
	htExitCritical();
	htTaskYield();

	htEnterCritical();
	const EventBits_t uxEventValue = htListGetItemValue(&(pxCurrentTCB->xEventListItem));
	if ((uxEventValue & HT_EVENT_CONTROL_UNBLOCKED) != 0U) {
		uxReturn = uxEventValue & HT_EVENT_BITS_MASK;
	} else {
		/* 超时唤醒：从等待链表摘除后再复检一次最新事件位。 */
		htListRemove(&(pxCurrentTCB->xEventListItem));
		uxReturn = pxGroup->uxEventBits;
		if (prvTestWaitCondition(uxReturn, uxBitsToWaitFor, xWaitForAllBits) == htTRUE && xClearOnExit != htFALSE) {
			pxGroup->uxEventBits &= ~uxBitsToWaitFor;
		}
	}
	htListSetItemValue(&(pxCurrentTCB->xEventListItem), configMAX_PRIORITIES - pxCurrentTCB->uxPriority);
	htExitCritical();
	return uxReturn;
}

EventBits_t htEventGroupSetBits(EventGroupHandle_t xEventGroup, EventBits_t uxBitsToSet)
{
	htEventGroup_t *pxGroup = (htEventGroup_t *)xEventGroup;
	EventBits_t uxResult = 0U;

	if (pxGroup == NULL || (uxBitsToSet & ~HT_EVENT_BITS_MASK) != 0U) {
		return 0U;
	}

	htEnterCritical();
	const BaseType_t xHigherPriorityWoken = prvSetBitsLocked(pxGroup, uxBitsToSet, &uxResult);
	htExitCritical();

#if (configUSE_PREEMPTION == 1)
	if (xHigherPriorityWoken == htTRUE) {
		htTaskYield();
	}
#else
	(void)xHigherPriorityWoken;
#endif
	return uxResult;
}

BaseType_t htEventGroupSetBitsFromISR(
		EventGroupHandle_t xEventGroup, EventBits_t uxBitsToSet, BaseType_t *pxHigherPriorityTaskWoken)
{
	htEventGroup_t *pxGroup = (htEventGroup_t *)xEventGroup;

	if (pxHigherPriorityTaskWoken != NULL) {
		*pxHigherPriorityTaskWoken = htFALSE;
	}
	if (pxGroup == NULL || (uxBitsToSet & ~HT_EVENT_BITS_MASK) != 0U) {
		return htFAIL;
	}

	const UBaseType_t uxSavedStatus = htEnterCriticalFromISR();
	const BaseType_t xWoken = prvSetBitsLocked(pxGroup, uxBitsToSet, NULL);
	htExitCriticalFromISR(uxSavedStatus);

	if (xWoken == htTRUE && pxHigherPriorityTaskWoken != NULL) {
		*pxHigherPriorityTaskWoken = htTRUE;
	}
	return htPASS;
}

EventBits_t htEventGroupClearBits(EventGroupHandle_t xEventGroup, EventBits_t uxBitsToClear)
{
	htEventGroup_t *pxGroup = (htEventGroup_t *)xEventGroup;
	EventBits_t uxPrevious = 0U;

	if (pxGroup == NULL || (uxBitsToClear & ~HT_EVENT_BITS_MASK) != 0U) {
		return 0U;
	}

	htEnterCritical();
	uxPrevious = pxGroup->uxEventBits;
	pxGroup->uxEventBits &= ~uxBitsToClear;
	htExitCritical();
	return uxPrevious;
}

EventBits_t htEventGroupClearBitsFromISR(EventGroupHandle_t xEventGroup, EventBits_t uxBitsToClear)
{
	htEventGroup_t *pxGroup = (htEventGroup_t *)xEventGroup;
	EventBits_t uxPrevious = 0U;

	if (pxGroup == NULL || (uxBitsToClear & ~HT_EVENT_BITS_MASK) != 0U) {
		return 0U;
	}

	const UBaseType_t uxSavedStatus = htEnterCriticalFromISR();
	uxPrevious = pxGroup->uxEventBits;
	pxGroup->uxEventBits &= ~uxBitsToClear;
	htExitCriticalFromISR(uxSavedStatus);
	return uxPrevious;
}

EventBits_t htEventGroupGetBits(EventGroupHandle_t xEventGroup)
{
	return htEventGroupClearBits(xEventGroup, 0U);
}

EventBits_t htEventGroupGetBitsFromISR(EventGroupHandle_t xEventGroup)
{
	return htEventGroupClearBitsFromISR(xEventGroup, 0U);
}

void htEventGroupDelete(EventGroupHandle_t xEventGroup)
{
	htEventGroup_t *pxGroup = (htEventGroup_t *)xEventGroup;
	if (pxGroup == NULL) {
		return;
	}

	if (pxGroup->ucStaticallyAllocated != 0U) {
		pxGroup->uxEventBits = 0U;
		return;
	}
	htPortFree(pxGroup);
}

#endif /* configUSE_EVENT_GROUPS == 1 */
