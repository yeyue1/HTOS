/**
 * @file kernel/htwatchdog.c
 * @brief 固定容量任务心跳注册表与窗口检查。
 */
#include "htwatchdog.h"
#include "htscheduler.h"

#include <stddef.h>

typedef struct {
	TaskHandle_t xTask;
	uint32_t ulCount;
	uint32_t ulCheckedCount;
} htWatchdogEntry_t;

static htWatchdogEntry_t xEntries[configWATCHDOG_MAX_TASKS];
static UBaseType_t uxEntryCount;
static BaseType_t xBaselineReady;

BaseType_t htWatchdogRegister(TaskHandle_t xTask)
{
	UBaseType_t uxIndex;

	if (xTask == NULL) {
		return htFAIL;
	}
	htEnterCritical();
	for (uxIndex = 0U; uxIndex < uxEntryCount; uxIndex++) {
		if (xEntries[uxIndex].xTask == xTask) {
			htExitCritical();
			return htFAIL;
		}
	}
	if (uxEntryCount >= (UBaseType_t)configWATCHDOG_MAX_TASKS) {
		htExitCritical();
		return htFAIL;
	}
	xEntries[uxEntryCount].xTask = xTask;
	xEntries[uxEntryCount].ulCount = 0U;
	xEntries[uxEntryCount].ulCheckedCount = 0U;
	uxEntryCount++;
	xBaselineReady = htFALSE;
	htExitCritical();
	return htPASS;
}

BaseType_t htWatchdogUnregister(TaskHandle_t xTask)
{
	UBaseType_t uxIndex;
	htEnterCritical();
	for (uxIndex = 0U; uxIndex < uxEntryCount; uxIndex++) {
		if (xEntries[uxIndex].xTask == xTask) {
			uxEntryCount--;
			xEntries[uxIndex] = xEntries[uxEntryCount];
			xBaselineReady = htFALSE;
			htExitCritical();
			return htPASS;
		}
	}
	htExitCritical();
	return htFAIL;
}

BaseType_t htWatchdogHeartbeat(void)
{
	TaskHandle_t xCurrent = htTaskGetCurrentTaskHandle();
	UBaseType_t uxIndex;

	if (xCurrent == NULL) {
		return htFAIL;
	}
	htEnterCritical();
	for (uxIndex = 0U; uxIndex < uxEntryCount; uxIndex++) {
		if (xEntries[uxIndex].xTask == xCurrent) {
			xEntries[uxIndex].ulCount++;
			htExitCritical();
			return htPASS;
		}
	}
	htExitCritical();
	return htFAIL;
}

BaseType_t htWatchdogCheck(void)
{
	UBaseType_t uxIndex;
	BaseType_t xHealthy = htTRUE;

	htEnterCritical();
	if (uxEntryCount == 0U) {
		xBaselineReady = htFALSE;
		htExitCritical();
		return htFALSE;
	}
	if (xBaselineReady == htFALSE) {
		for (uxIndex = 0U; uxIndex < uxEntryCount; uxIndex++) {
			xEntries[uxIndex].ulCheckedCount = xEntries[uxIndex].ulCount;
		}
		xBaselineReady = htTRUE;
		htExitCritical();
		return htFALSE;
	}
	for (uxIndex = 0U; uxIndex < uxEntryCount; uxIndex++) {
		if (xEntries[uxIndex].ulCount == xEntries[uxIndex].ulCheckedCount) {
			xHealthy = htFALSE;
		}
	}
	if (xHealthy == htTRUE) {
		for (uxIndex = 0U; uxIndex < uxEntryCount; uxIndex++) {
			xEntries[uxIndex].ulCheckedCount = xEntries[uxIndex].ulCount;
		}
	}
	htExitCritical();
	return xHealthy;
}

void htWatchdogReset(void)
{
	UBaseType_t uxIndex;

	htEnterCritical();
	for (uxIndex = 0U; uxIndex < (UBaseType_t)configWATCHDOG_MAX_TASKS; uxIndex++) {
		xEntries[uxIndex].xTask = NULL;
		xEntries[uxIndex].ulCount = 0U;
		xEntries[uxIndex].ulCheckedCount = 0U;
	}
	uxEntryCount = 0U;
	xBaselineReady = htFALSE;
	htExitCritical();
}
