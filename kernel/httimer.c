/**
 * @file httimer.c
 * @brief 软件定时器守护任务、双链表回绕安全调度与命令队列实现。
 */
#include <string.h>

#include "httimer.h"
#include "htmem.h"
#include "htqueue.h"
#include "htscheduler.h"
#include "httask.h"

#if (configUSE_TIMERS == 1)

#if (configTIMER_TASK_PRIORITY >= configMAX_PRIORITIES)
#error "configTIMER_TASK_PRIORITY must be less than configMAX_PRIORITIES"
#endif

#if (configTIMER_QUEUE_LENGTH < 1U)
#error "configTIMER_QUEUE_LENGTH must be at least 1"
#endif

typedef enum {
	HT_TIMER_CMD_START = 1U,
	HT_TIMER_CMD_RESET = 2U,
	HT_TIMER_CMD_STOP = 3U,
	HT_TIMER_CMD_CHANGE_PERIOD = 4U,
	HT_TIMER_CMD_DELETE = 5U
} htTimerCommandId_t;

typedef struct htTimerCommand {
	TickType_t xMessageValue; /**< Start/Reset 存命令时刻；ChangePeriod 存新周期 */
	htTimer_t *pxTimer;
	uint8_t ucCommand;
} htTimerCommand_t;

static htList_t s_xActiveTimerList1;
static htList_t s_xActiveTimerList2;
static htList_t *s_pxCurrentTimerList = NULL;
static htList_t *s_pxOverflowTimerList = NULL;
static QueueHandle_t s_xTimerQueue = NULL;
static TaskHandle_t s_xTimerTaskHandle = NULL;
static TickType_t s_xLastProcessedTick = 0U;
static uint8_t s_ucTimerServiceInit = 0U;

#if (configSUPPORT_STATIC_ALLOCATION == 1)
static htQUEUE_t s_xTimerQueueStruct;
static uint8_t s_aucTimerQueueStorage[configTIMER_QUEUE_LENGTH * sizeof(htTimerCommand_t)];
#if (configTOTAL_HEAP_SIZE == 0U)
static StackType_t s_axTimerTaskStack[configTIMER_TASK_STACK_DEPTH];
static htTCB_t s_xTimerTaskTCB;
#endif
#endif

static BaseType_t prvInitTimerStruct(htTimer_t *pxTimer, const char *pcTimerName, TickType_t xTimerPeriodInTicks,
		BaseType_t xAutoReload, void *pvTimerID, htTimerCallback_t pxCallbackFunction, uint8_t ucStatic)
{
	if (pxTimer == NULL || pxCallbackFunction == NULL || xTimerPeriodInTicks == 0U ||
			xTimerPeriodInTicks > (TickType_t)INT32_MAX) {
		return htFAIL;
	}

	memset(pxTimer, 0, sizeof(*pxTimer));
	htListItemInit(&(pxTimer->xTimerListItem));
	htListSetItemOwner(&(pxTimer->xTimerListItem), pxTimer);
	pxTimer->xTimerPeriodInTicks = xTimerPeriodInTicks;
	pxTimer->pxCallback = pxCallbackFunction;
	pxTimer->pvTimerID = pvTimerID;
	pxTimer->ucAutoReload = (xAutoReload != htFALSE) ? 1U : 0U;
	pxTimer->ucActive = 0U;
	pxTimer->ucStaticallyAllocated = ucStatic;

	if (pcTimerName != NULL) {
		size_t uxLen = 0U;
		while (uxLen < (configMAX_TASK_NAME_LEN - 1U) && pcTimerName[uxLen] != '\0') {
			pxTimer->pcTimerName[uxLen] = pcTimerName[uxLen];
			uxLen++;
		}
		pxTimer->pcTimerName[uxLen] = '\0';
	}

	return htPASS;
}

static void prvInsertTimerInActiveList(htTimer_t *pxTimer, TickType_t xExpiryTime, TickType_t xNow)
{
	htListRemove(&(pxTimer->xTimerListItem));
	htListSetItemValue(&(pxTimer->xTimerListItem), xExpiryTime);
	pxTimer->ucActive = 1U;

	/* 未来到期且跨越 32 位回绕边界的定时器放进溢出链表，其余放进当前链表。 */
	if ((xExpiryTime < xNow) && ((int32_t)(xExpiryTime - xNow) > 0)) {
		htListInsert(s_pxOverflowTimerList, &(pxTimer->xTimerListItem));
	} else {
		htListInsert(s_pxCurrentTimerList, &(pxTimer->xTimerListItem));
	}
}

static void prvFireTimer(htTimer_t *pxTimer, TickType_t xExpiryTime, TickType_t xNow)
{
	htListRemove(&(pxTimer->xTimerListItem));
	if (pxTimer->ucAutoReload != 0U) {
		TickType_t xNextExpiry = xExpiryTime + pxTimer->xTimerPeriodInTicks;
		if ((int32_t)(xNextExpiry - xNow) <= 0) {
			/* 严重滞后时对齐到当前时刻重新排期，防止单次步进陷入无限补发死循环。 */
			xNextExpiry = xNow + pxTimer->xTimerPeriodInTicks;
		}
		prvInsertTimerInActiveList(pxTimer, xNextExpiry, xNow);
	} else {
		pxTimer->ucActive = 0U;
	}

	if (pxTimer->pxCallback != NULL) {
		pxTimer->pxCallback((TimerHandle_t)pxTimer);
	}
}

static void prvProcessCommand(const htTimerCommand_t *pxCmd, TickType_t xNow)
{
	htTimer_t *pxTimer = pxCmd->pxTimer;
	if (pxTimer == NULL) {
		return;
	}

	switch ((htTimerCommandId_t)pxCmd->ucCommand) {
		case HT_TIMER_CMD_START:
		case HT_TIMER_CMD_RESET: {
			const TickType_t xExpiry = pxCmd->xMessageValue + pxTimer->xTimerPeriodInTicks;
			if ((int32_t)(xExpiry - xNow) <= 0) {
				prvFireTimer(pxTimer, xExpiry, xNow);
			} else {
				prvInsertTimerInActiveList(pxTimer, xExpiry, xNow);
			}
			break;
		}
		case HT_TIMER_CMD_STOP:
			htListRemove(&(pxTimer->xTimerListItem));
			pxTimer->ucActive = 0U;
			break;
		case HT_TIMER_CMD_CHANGE_PERIOD:
			if (pxCmd->xMessageValue > 0U && pxCmd->xMessageValue <= (TickType_t)INT32_MAX) {
				pxTimer->xTimerPeriodInTicks = pxCmd->xMessageValue;
				prvInsertTimerInActiveList(pxTimer, xNow + pxTimer->xTimerPeriodInTicks, xNow);
			}
			break;
		case HT_TIMER_CMD_DELETE:
			htListRemove(&(pxTimer->xTimerListItem));
			pxTimer->ucActive = 0U;
			if (pxTimer->ucStaticallyAllocated == 0U) {
				htPortFree(pxTimer);
			}
			break;
		default:
			break;
	}
}

static BaseType_t prvSendCommand(
		htTimerCommandId_t xCmdId, htTimer_t *pxTimer, TickType_t xValue, TickType_t xTicksToWait)
{
	if (pxTimer == NULL) {
		return htFAIL;
	}
	if (s_ucTimerServiceInit == 0U && htTimerInit() != htPASS) {
		return htFAIL;
	}

	htTimerCommand_t xCmd;
	xCmd.xMessageValue = xValue;
	xCmd.pxTimer = pxTimer;
	xCmd.ucCommand = (uint8_t)xCmdId;
	return htQueueSend(s_xTimerQueue, &xCmd, xTicksToWait);
}

static BaseType_t prvSendCommandFromISR(
		htTimerCommandId_t xCmdId, htTimer_t *pxTimer, TickType_t xValue, BaseType_t *pxHigherPriorityTaskWoken)
{
	if (pxTimer == NULL || s_xTimerQueue == NULL) {
		if (pxHigherPriorityTaskWoken != NULL) {
			*pxHigherPriorityTaskWoken = htFALSE;
		}
		return htFAIL;
	}

	htTimerCommand_t xCmd;
	xCmd.xMessageValue = xValue;
	xCmd.pxTimer = pxTimer;
	xCmd.ucCommand = (uint8_t)xCmdId;
	return htQueueSendFromISR(s_xTimerQueue, &xCmd, pxHigherPriorityTaskWoken);
}

static void prvTimerDaemonTask(void *pvParameters)
{
	(void)pvParameters;

	for (;;) {
		const TickType_t xWaitTicks = htTimerProcessStep(xTickCount);
		htTimerCommand_t xCmd;
		if (htQueueReceive(s_xTimerQueue, &xCmd, xWaitTicks) == htPASS) {
			prvProcessCommand(&xCmd, xTickCount);
		}
	}
}

BaseType_t htTimerInit(void)
{
	if (s_ucTimerServiceInit != 0U) {
		return htPASS;
	}

	htListInit(&s_xActiveTimerList1);
	htListInit(&s_xActiveTimerList2);
	s_pxCurrentTimerList = &s_xActiveTimerList1;
	s_pxOverflowTimerList = &s_xActiveTimerList2;
	s_xLastProcessedTick = xTickCount;

#if (configSUPPORT_STATIC_ALLOCATION == 1)
	s_xTimerQueue = htQueueCreateStatic(
			configTIMER_QUEUE_LENGTH, sizeof(htTimerCommand_t), s_aucTimerQueueStorage, &s_xTimerQueueStruct);
#else
	s_xTimerQueue = htQueueCreate(configTIMER_QUEUE_LENGTH, sizeof(htTimerCommand_t));
#endif
	if (s_xTimerQueue == NULL) {
		return htFAIL;
	}

	s_ucTimerServiceInit = 1U;
	return htPASS;
}

BaseType_t htTimerCreateDaemonTask(void)
{
	if (htTimerInit() != htPASS) {
		return htFAIL;
	}
	if (s_xTimerTaskHandle != NULL) {
		return htPASS;
	}

#if (configTOTAL_HEAP_SIZE > 0U)
	return htTaskCreate(prvTimerDaemonTask, "TmrSvc", (uint16_t)configTIMER_TASK_STACK_DEPTH, NULL,
			(UBaseType_t)configTIMER_TASK_PRIORITY, &s_xTimerTaskHandle);
#elif (configSUPPORT_STATIC_ALLOCATION == 1)
	s_xTimerTaskHandle = htTaskCreateStatic(prvTimerDaemonTask, "TmrSvc", (uint16_t)configTIMER_TASK_STACK_DEPTH, NULL,
			(UBaseType_t)configTIMER_TASK_PRIORITY, s_axTimerTaskStack, &s_xTimerTaskTCB);
	return (s_xTimerTaskHandle != NULL) ? htPASS : htFAIL;
#else
	return htFAIL;
#endif
}

TimerHandle_t htTimerCreate(const char *pcTimerName, TickType_t xTimerPeriodInTicks, BaseType_t xAutoReload,
		void *pvTimerID, htTimerCallback_t pxCallbackFunction)
{
	if (pxCallbackFunction == NULL || xTimerPeriodInTicks == 0U || xTimerPeriodInTicks > (TickType_t)INT32_MAX) {
		return NULL;
	}

	htTimer_t *pxNewTimer = (htTimer_t *)htPortMalloc(sizeof(htTimer_t));
	if (pxNewTimer == NULL) {
		return NULL;
	}

	if (prvInitTimerStruct(pxNewTimer, pcTimerName, xTimerPeriodInTicks, xAutoReload, pvTimerID, pxCallbackFunction,
				0U) != htPASS) {
		htPortFree(pxNewTimer);
		return NULL;
	}

	return (TimerHandle_t)pxNewTimer;
}

TimerHandle_t htTimerCreateStatic(const char *pcTimerName, TickType_t xTimerPeriodInTicks, BaseType_t xAutoReload,
		void *pvTimerID, htTimerCallback_t pxCallbackFunction, htTimer_t *pxTimerBuffer)
{
#if (configSUPPORT_STATIC_ALLOCATION == 1)
	if (prvInitTimerStruct(pxTimerBuffer, pcTimerName, xTimerPeriodInTicks, xAutoReload, pvTimerID, pxCallbackFunction,
				1U) != htPASS) {
		return NULL;
	}
	return (TimerHandle_t)pxTimerBuffer;
#else
	(void)pcTimerName;
	(void)xTimerPeriodInTicks;
	(void)xAutoReload;
	(void)pvTimerID;
	(void)pxCallbackFunction;
	(void)pxTimerBuffer;
	return NULL;
#endif
}

BaseType_t htTimerStart(TimerHandle_t xTimer, TickType_t xTicksToWait)
{
	return prvSendCommand(HT_TIMER_CMD_START, (htTimer_t *)xTimer, xTickCount, xTicksToWait);
}

BaseType_t htTimerStartFromISR(TimerHandle_t xTimer, BaseType_t *pxHigherPriorityTaskWoken)
{
	return prvSendCommandFromISR(HT_TIMER_CMD_START, (htTimer_t *)xTimer, xTickCount, pxHigherPriorityTaskWoken);
}

BaseType_t htTimerStop(TimerHandle_t xTimer, TickType_t xTicksToWait)
{
	return prvSendCommand(HT_TIMER_CMD_STOP, (htTimer_t *)xTimer, 0U, xTicksToWait);
}

BaseType_t htTimerStopFromISR(TimerHandle_t xTimer, BaseType_t *pxHigherPriorityTaskWoken)
{
	return prvSendCommandFromISR(HT_TIMER_CMD_STOP, (htTimer_t *)xTimer, 0U, pxHigherPriorityTaskWoken);
}

BaseType_t htTimerReset(TimerHandle_t xTimer, TickType_t xTicksToWait)
{
	return prvSendCommand(HT_TIMER_CMD_RESET, (htTimer_t *)xTimer, xTickCount, xTicksToWait);
}

BaseType_t htTimerResetFromISR(TimerHandle_t xTimer, BaseType_t *pxHigherPriorityTaskWoken)
{
	return prvSendCommandFromISR(HT_TIMER_CMD_RESET, (htTimer_t *)xTimer, xTickCount, pxHigherPriorityTaskWoken);
}

BaseType_t htTimerChangePeriod(TimerHandle_t xTimer, TickType_t xNewPeriod, TickType_t xTicksToWait)
{
	if (xNewPeriod == 0U || xNewPeriod > (TickType_t)INT32_MAX) {
		return htFAIL;
	}
	return prvSendCommand(HT_TIMER_CMD_CHANGE_PERIOD, (htTimer_t *)xTimer, xNewPeriod, xTicksToWait);
}

BaseType_t htTimerChangePeriodFromISR(
		TimerHandle_t xTimer, TickType_t xNewPeriod, BaseType_t *pxHigherPriorityTaskWoken)
{
	if (xNewPeriod == 0U || xNewPeriod > (TickType_t)INT32_MAX) {
		if (pxHigherPriorityTaskWoken != NULL) {
			*pxHigherPriorityTaskWoken = htFALSE;
		}
		return htFAIL;
	}
	return prvSendCommandFromISR(
			HT_TIMER_CMD_CHANGE_PERIOD, (htTimer_t *)xTimer, xNewPeriod, pxHigherPriorityTaskWoken);
}

BaseType_t htTimerDelete(TimerHandle_t xTimer, TickType_t xTicksToWait)
{
	return prvSendCommand(HT_TIMER_CMD_DELETE, (htTimer_t *)xTimer, 0U, xTicksToWait);
}

BaseType_t htTimerIsActive(TimerHandle_t xTimer)
{
	htTimer_t *pxTimer = (htTimer_t *)xTimer;
	if (pxTimer == NULL) {
		return htFALSE;
	}
	htEnterCritical();
	const BaseType_t xActive = (pxTimer->ucActive != 0U) ? htTRUE : htFALSE;
	htExitCritical();
	return xActive;
}

TickType_t htTimerGetPeriod(TimerHandle_t xTimer)
{
	htTimer_t *pxTimer = (htTimer_t *)xTimer;
	return (pxTimer != NULL) ? pxTimer->xTimerPeriodInTicks : 0U;
}

void *htTimerGetTimerID(TimerHandle_t xTimer)
{
	htTimer_t *pxTimer = (htTimer_t *)xTimer;
	void *pvId = NULL;
	if (pxTimer != NULL) {
		htEnterCritical();
		pvId = pxTimer->pvTimerID;
		htExitCritical();
	}
	return pvId;
}

void htTimerSetTimerID(TimerHandle_t xTimer, void *pvNewID)
{
	htTimer_t *pxTimer = (htTimer_t *)xTimer;
	if (pxTimer != NULL) {
		htEnterCritical();
		pxTimer->pvTimerID = pvNewID;
		htExitCritical();
	}
}

const char *htTimerGetName(TimerHandle_t xTimer)
{
	htTimer_t *pxTimer = (htTimer_t *)xTimer;
	return (pxTimer != NULL) ? pxTimer->pcTimerName : NULL;
}

TaskHandle_t htTimerGetDaemonTaskHandle(void)
{
	return s_xTimerTaskHandle;
}

TickType_t htTimerProcessStep(TickType_t xNow)
{
	if (s_ucTimerServiceInit == 0U && htTimerInit() != htPASS) {
		return htBLOCKED_INDEFINITELY;
	}

	/* 检测到 32 位 tick 回绕：先清完旧周期残留到期项，再切换溢出链表为当前链表。 */
	if (xNow < s_xLastProcessedTick) {
		htListItem_t *pxHead;
		while ((pxHead = htListGetHead(s_pxCurrentTimerList)) != NULL) {
			htTimer_t *pxTimer = (htTimer_t *)htListGetItemOwner(pxHead);
			const TickType_t xExpiry = htListGetItemValue(pxHead);
			if (pxTimer != NULL) {
				prvFireTimer(pxTimer, xExpiry, xNow);
			} else {
				htListRemove(pxHead);
			}
		}

		htList_t *pxSwap = s_pxCurrentTimerList;
		s_pxCurrentTimerList = s_pxOverflowTimerList;
		s_pxOverflowTimerList = pxSwap;
	}
	s_xLastProcessedTick = xNow;

	/* 先排空当前命令队列，再触发所有到期定时器。 */
	htTimerCommand_t xCmd;
	while (htQueueReceive(s_xTimerQueue, &xCmd, 0U) == htPASS) {
		prvProcessCommand(&xCmd, xNow);
	}

	for (;;) {
		htListItem_t *pxHead = htListGetHead(s_pxCurrentTimerList);
		if (pxHead == NULL) {
			break;
		}
		const TickType_t xExpiry = htListGetItemValue(pxHead);
		if (xExpiry > xNow) {
			break;
		}
		htTimer_t *pxTimer = (htTimer_t *)htListGetItemOwner(pxHead);
		if (pxTimer != NULL) {
			prvFireTimer(pxTimer, xExpiry, xNow);
		} else {
			htListRemove(pxHead);
		}
	}

	htListItem_t *pxNext = htListGetHead(s_pxCurrentTimerList);
	if (pxNext != NULL) {
		return htListGetItemValue(pxNext) - xNow;
	}
	if (htListGetCurrentNumberOfItems(s_pxOverflowTimerList) > 0U) {
		/* 当前链表空但溢出链表非空：最多等到 32 位回绕点再切表。 */
		return (TickType_t)(UINT32_MAX - xNow) + 1U;
	}
	return htBLOCKED_INDEFINITELY;
}

#endif /* configUSE_TIMERS == 1 */
