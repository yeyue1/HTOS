/**
 * @file htqueue.c
 * @brief 队列与计数信号量的实现，含阻塞等待/唤醒逻辑。
 *
 * 同一个结构 htQUEUE_t 靠 uxItemSize 是否为 0 在两种形态间切换：为 0 时走
 * union 里的 uxSemaphoreCount，只做计数加减，不做 memcpy。消息存储区是
 * 环形缓冲，写指针与读指针越过 pcTail 后各自绕回 pcHead 处。
 */
#include <string.h>
#include "htqueue.h"
#include "httask.h"
#include "htmem.h"
#include "htscheduler.h"

/**
 * @brief 初始化一块已分配的队列结构，动态创建与静态创建共用。
 *
 * @param[in] uxQueueLength 队列深度，即可容纳的消息条数。
 * @param[in] uxItemSize 单条消息字节数；为 0 表示这是计数信号量。
 * @param[in] pcQueueStorage 消息存储区首地址；信号量形态下可为 NULL。
 * @param[in,out] pxNewQueue 待初始化的队列结构。
 * @param[in] ucStaticallyAllocated 非 0 表示存储区是静态的，删除时不释放。
 *
 * @return 初始化成功返回 htPASS，参数非法返回 htFAIL。
 *
 * @note 先用 memset 把整块结构清零，再逐个字段置初值，避免残留脏值。
 */
static BaseType_t prvInitialiseNewQueue(UBaseType_t uxQueueLength, UBaseType_t uxItemSize, int8_t *pcQueueStorage,
		htQUEUE_t *pxNewQueue, uint8_t ucStaticallyAllocated)
{
	if (pxNewQueue != NULL && (pcQueueStorage != NULL || uxItemSize == 0U)) {
		memset(pxNewQueue, 0, sizeof(*pxNewQueue));
		pxNewQueue->pcHead = pcQueueStorage;
		/* 逐个初始化队列成员：深度、项宽、写指针、消息计数与收发锁计数。 */
		pxNewQueue->uxLength = uxQueueLength;
		pxNewQueue->uxItemSize = uxItemSize;
		pxNewQueue->pcWriteTo = pxNewQueue->pcHead;
		pxNewQueue->uxMessagesWaiting = 0;
		pxNewQueue->cRxLock = htQUEUE_UNLOCKED;
		pxNewQueue->cTxLock = htQUEUE_UNLOCKED;
		pxNewQueue->ucStaticallyAllocated = ucStaticallyAllocated;

		/* 按 uxItemSize 是否为 0 区分队列与信号量两种形态。 */
		if (uxItemSize > 0) {
			/* 队列形态：pcTail 指向存储区末尾之后，读写指针都从头部开始。 */
			pxNewQueue->u.xQueue.pcTail = pxNewQueue->pcHead + (uxQueueLength * uxItemSize);
			pxNewQueue->u.xQueue.pcReadFrom = pxNewQueue->pcHead;
		} else {
			/* 信号量形态：初始计数为 0。 */
			pxNewQueue->u.xSemaphore.uxSemaphoreCount = 0;
		}

		/* 两张等待链表：因队列满而阻塞的发送者、因队列空而阻塞的接收者各一张。 */
		htListInit(&(pxNewQueue->xTasksWaitingToSend));
		htListInit(&(pxNewQueue->xTasksWaitingToReceive));
		return htPASS;
	}

	return htFAIL;
}

/**
 * @brief 动态创建队列：分别分配队列结构与消息存储区。
 *
 * @param[in] uxQueueLength 队列深度，至少为 1。
 * @param[in] uxItemSize 单条消息字节数；为 0 表示创建计数信号量。
 *
 * @return 成功返回队列句柄，失败返回 NULL。
 *
 * @note 深度与项宽的乘积在相乘前用 SIZE_MAX / uxItemSize 检查上溢；
 *       初始化失败时会回滚已分配的两块内存。
 */
QueueHandle_t htQueueCreate(UBaseType_t uxQueueLength, UBaseType_t uxItemSize)
{
	htQUEUE_t *pxNewQueue;
	int8_t *pcQueueStorage = NULL;

	/* 深度至少为 1，且 深度 * 项宽 不得溢出。 */
	if (uxQueueLength < 1U || (uxItemSize != 0U && uxQueueLength > SIZE_MAX / uxItemSize)) {
		return NULL;
	}

	/* 先分配队列结构本身。 */
	pxNewQueue = (htQUEUE_t *)htPortMalloc(sizeof(htQUEUE_t));

	if (pxNewQueue != NULL) {
		if (uxItemSize != 0U) {
			pcQueueStorage = (int8_t *)htPortMalloc(uxQueueLength * uxItemSize);
		}
		if (prvInitialiseNewQueue(uxQueueLength, uxItemSize, pcQueueStorage, pxNewQueue, 0U) != htPASS) {
			htPortFree(pcQueueStorage);
			htPortFree(pxNewQueue);
			pxNewQueue = NULL;
		}
	}

	return pxNewQueue;
}

/**
 * @brief 静态创建队列：存储区与队列结构均由调用者提供，不占堆空间。
 *
 * @param[in] uxQueueLength 队列深度，至少为 1。
 * @param[in] uxItemSize 单条消息字节数；为 0 表示计数信号量。
 * @param[in] pucQueueStorage 消息存储区首地址，容量至少为 深度 * 项宽。
 * @param[in,out] pxQueueBuffer 由调用者提供的队列结构。
 *
 * @return 成功返回 pxQueueBuffer，失败返回 NULL。
 *
 * @note configSUPPORT_STATIC_ALLOCATION 为 0 时本函数只吞掉参数并返回 NULL。
 */
QueueHandle_t htQueueCreateStatic(
		UBaseType_t uxQueueLength, UBaseType_t uxItemSize, uint8_t *pucQueueStorage, htQUEUE_t *pxQueueBuffer)
{
#if configSUPPORT_STATIC_ALLOCATION == 1
	if (pxQueueBuffer == NULL || uxQueueLength < 1U ||
			(uxItemSize != 0U && (pucQueueStorage == NULL || uxQueueLength > SIZE_MAX / uxItemSize))) {
		return NULL;
	}

	if (prvInitialiseNewQueue(uxQueueLength, uxItemSize, (int8_t *)pucQueueStorage, pxQueueBuffer, 1U) != htPASS) {
		return NULL;
	}
	return pxQueueBuffer;
#else
	(void)uxQueueLength;
	(void)uxItemSize;
	(void)pucQueueStorage;
	(void)pxQueueBuffer;
	return NULL;
#endif
}

/**
 * @brief 把一条消息写入队列的写指针处，并推进写指针。
 *
 * @param[in,out] pxQueue 目标队列。
 * @param[in] pvItemToQueue 待写入消息的地址；信号量形态下不使用。
 *
 * @note 本函数不唤醒任务，唤醒动作由调用者在临界区内另行完成。
 * @note 写指针越过 pcTail 后绕回 pcHead，形成环形缓冲。
 */
static void prvCopyDataToQueue(htQUEUE_t *pxQueue, const void *pvItemToQueue)
{
	if (pxQueue->uxItemSize == 0) {
		/* 信号量形态：只把计数加一。 */
		pxQueue->u.xSemaphore.uxSemaphoreCount++;
	} else {
		/* 队列形态：按 uxItemSize 把消息拷到写指针处。 */
		memcpy((void *)pxQueue->pcWriteTo, pvItemToQueue, pxQueue->uxItemSize);

		/* 推进写指针；越过尾部就绕回头部。 */
		pxQueue->pcWriteTo += pxQueue->uxItemSize;
		if (pxQueue->pcWriteTo >= pxQueue->u.xQueue.pcTail) {
			pxQueue->pcWriteTo = pxQueue->pcHead;
		}
	}

	/* 消息计数加一。 */
	pxQueue->uxMessagesWaiting++;
}

/**
 * @brief 从队列的读指针处拷出一条消息，并推进读指针。
 *
 * @param[in,out] pxQueue 源队列。
 * @param[out] pvBuffer 接收缓冲区；信号量形态下不使用。
 *
 * @note 读指针越过 pcTail 后同样绕回 pcHead。
 * @note 消息计数在这里减一；信号量形态下已先减过计数值。
 */
static void prvCopyDataFromQueue(htQUEUE_t *pxQueue, void *pvBuffer)
{
	if (pxQueue->uxItemSize != 0) {
		/* 队列形态：按 uxItemSize 拷到调用者提供的缓冲区。 */
		memcpy(pvBuffer, (void *)pxQueue->u.xQueue.pcReadFrom, pxQueue->uxItemSize);

		/* 推进读指针；越过尾部就绕回头部。 */
		pxQueue->u.xQueue.pcReadFrom += pxQueue->uxItemSize;
		if (pxQueue->u.xQueue.pcReadFrom >= pxQueue->u.xQueue.pcTail) {
			pxQueue->u.xQueue.pcReadFrom = pxQueue->pcHead;
		}
	} else if (pxQueue->u.xSemaphore.uxSemaphoreCount > 0U) {
		pxQueue->u.xSemaphore.uxSemaphoreCount--;
	}

	/* 消息计数减一。 */
	pxQueue->uxMessagesWaiting--;
}

/**
 * @brief 把当前任务阻塞到指定事件链表上，必要时同时挂入延时链表。
 *
 * @param[in,out] pxEventList 等待该队列/信号量的任务链表，按优先级排序。
 * @param[in] xTicksToWait 最长等待 tick 数；htBLOCKED_INDEFINITELY 表示无限等待。
 *
 * @return 阻塞成功返回 htPASS；无当前任务或延时链表未就绪返回 htFAIL。
 *
 * @warning 必须在持有内核临界区时调用，本函数自己不再关中断。
 * @note 事件项的值取 configMAX_PRIORITIES - uxPriority，使高优先级任务排在
 *       链表前部，唤醒方取表头即可。
 */
static BaseType_t prvBlockCurrentTask(htList_t *pxEventList, TickType_t xTicksToWait)
{
	if (pxCurrentTCB == NULL) {
		return htFAIL;
	}
	if (xTicksToWait != htBLOCKED_INDEFINITELY && (pxDelayedTaskList == NULL || pxOverflowDelayedTaskList == NULL)) {
		return htFAIL;
	}

	/* 先把任务从就绪链表与延时链表中摘出来，避免它同时挂在两处。 */
	htListRemove(&(pxCurrentTCB->xStateListItem));
	htListRemove(&(pxCurrentTCB->xEventListItem));

	/* 事件项值取优先级取反：高优先级任务排在链表前部，唤醒时取表头即可。 */
	htListSetItemValue(&(pxCurrentTCB->xEventListItem), configMAX_PRIORITIES - pxCurrentTCB->uxPriority);
	htListInsert(pxEventList, &(pxCurrentTCB->xEventListItem));

	/* 有限等待：把唤醒时刻写进状态项，按 tick 是否回绕放进当前或溢出延时链表。 */
	if (xTicksToWait != htBLOCKED_INDEFINITELY) {
		const TickType_t xTimeToWake = xTickCount + xTicksToWait;
		htListSetItemValue(&(pxCurrentTCB->xStateListItem), xTimeToWake);
		if (xTimeToWake > xTickCount) {
			htListInsert(pxDelayedTaskList, &(pxCurrentTCB->xStateListItem));
		} else {
			htListInsert(pxOverflowDelayedTaskList, &(pxCurrentTCB->xStateListItem));
		}
	}

	/* 标记为阻塞态，随后由调用者让出 CPU。 */
	pxCurrentTCB->uxTaskState = HT_TASK_BLOCKED;
	return htPASS;
}

/**
 * @brief 从事件链表中取出排队最靠前的任务，并把它放回就绪链表。
 *
 * @param[in,out] pxEventList 等待该队列/信号量的任务链表。
 *
 * @return 被唤醒的任务控制块；链表为空或属主非法时返回 NULL。
 *
 * @warning 必须在队列/链表状态受保护时调用。
 * @note 取表头即可，因为阻塞时已按优先级倒序排好，表头就是最高优先级。
 */
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

	/* 被事件唤醒后必须撤销它的超时项，否则会在延时链表里留下悬空节点。 */
	htListRemove(&(pxTCB->xStateListItem));
	htListSetItemValue(&(pxTCB->xStateListItem), configMAX_PRIORITIES - pxTCB->uxPriority);
	pxTCB->uxTaskState = HT_TASK_READY;
	htListInsertEnd(&(pxReadyTasksLists[pxTCB->uxPriority]), &(pxTCB->xStateListItem));
	return pxTCB;
}

/**
 * @brief 向队列发送一条消息；队列满时可限时阻塞等待。
 *
 * @param[in] xQueue 目标队列句柄。
 * @param[in] pvItemToQueue 待发送消息的地址；信号量形态下可为 NULL。
 * @param[in] xTicksToWait 队列满时的最长等待 tick 数，0 表示不等待。
 *
 * @return 发送成功返回 htPASS；超时或参数错误返回 htFAIL。
 *
 * @note 每轮都用 htTaskYield() 主动让出 CPU，并用 xElapsed 重算剩余等待
 *       时间，避免被虚假唤醒后多等。
 */
BaseType_t htQueueSend(QueueHandle_t xQueue, const void *pvItemToQueue, TickType_t xTicksToWait)
{
	htQUEUE_t *pxQueue = (htQUEUE_t *)xQueue;
	const TickType_t xStartTime = xTickCount;
	TickType_t xRemainingTicks = xTicksToWait;

	/* 检查参数有效性：队列非空；队列形态下消息地址也必须非空。 */
	if (pxQueue == NULL || (pxQueue->uxItemSize != 0U && pvItemToQueue == NULL)) {
		return htFAIL;
	}

	for (;;) {
		htEnterCritical();
		/* 队列未满：拷入消息，再从等待接收链表中唤醒一个任务。 */
		if (pxQueue->uxMessagesWaiting < pxQueue->uxLength) {
			prvCopyDataToQueue(pxQueue, pvItemToQueue);
			(void)prvUnblockFirstTask(&(pxQueue->xTasksWaitingToReceive));
			htExitCritical();
			return htPASS;
		}

		/* 队列已满：不等待则直接失败，否则把当前任务阻塞到发送等待链表。 */
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
 * @brief 从队列接收一条消息；队列空时可限时阻塞等待。
 *
 * @param[in] xQueue 源队列句柄。
 * @param[out] pvBuffer 接收缓冲区地址；信号量形态下可为 NULL。
 * @param[in] xTicksToWait 队列空时的最长等待 tick 数，0 表示不等待。
 *
 * @return 接收成功返回 htPASS；超时或参数错误返回 htFAIL。
 *
 * @note 取走消息后会尝试唤醒一个因队列满而阻塞的发送者。
 */
BaseType_t htQueueReceive(QueueHandle_t xQueue, void *pvBuffer, TickType_t xTicksToWait)
{
	htQUEUE_t *pxQueue = (htQUEUE_t *)xQueue;
	const TickType_t xStartTime = xTickCount;
	TickType_t xRemainingTicks = xTicksToWait;
	/* 检查参数有效性：队列非空；队列形态下接收缓冲区也必须非空。 */
	if (pxQueue == NULL || (pxQueue->uxItemSize != 0U && pvBuffer == NULL)) {
		return htFAIL;
	}

	for (;;) {
		htEnterCritical();
		/* 有消息：拷出并唤醒一个因队列满而阻塞的发送者。 */
		if (pxQueue->uxMessagesWaiting > 0U) {
			prvCopyDataFromQueue(pxQueue, pvBuffer);
			(void)prvUnblockFirstTask(&(pxQueue->xTasksWaitingToSend));
			htExitCritical();
			return htPASS;
		}

		/* 队列为空：不等待则直接失败，否则阻塞到接收等待链表。 */
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
 * @brief 在中断服务程序中向队列发送消息，不阻塞。
 *
 * @param[in] xQueue 目标队列句柄。
 * @param[in] pvItemToQueue 待发送消息的地址；信号量形态下可为 NULL。
 * @param[out] pxHigherPriorityTaskWoken 若唤醒了更高优先级任务则置 htTRUE，
 *             供中断退出前决定是否请求一次 PendSV 切换；可为 NULL。
 *
 * @return 入队成功返回 htPASS；队列满或参数错误返回 htFAIL。
 *
 * @warning 只能在中断上下文调用；本函数不进入临界区，靠中断屏蔽保证互斥。
 */
BaseType_t htQueueSendFromISR(QueueHandle_t xQueue, const void *pvItemToQueue, BaseType_t *pxHigherPriorityTaskWoken)
{
	BaseType_t xReturn = htFAIL;
	htQUEUE_t *pxQueue = (htQUEUE_t *)xQueue;

	/* 检查参数有效性。 */
	if (pxQueue == NULL || (pxQueue->uxItemSize != 0U && pvItemToQueue == NULL)) {
		return htFAIL;
	}

	/* 先假定没有唤醒更高优先级的任务。 */
	if (pxHigherPriorityTaskWoken != NULL) {
		*pxHigherPriorityTaskWoken = htFALSE;
	}

	/* 还有空位才能入队；ISR 中不做任何等待。 */
	if (pxQueue->uxMessagesWaiting < pxQueue->uxLength) {
		/* 拷贝消息，并尝试唤醒一个等待接收的任务。 */
		prvCopyDataToQueue(pxQueue, pvItemToQueue);

		/* 只有被唤醒任务的优先级高于当前任务时，才要求中断退出后切换。 */
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
 * @brief 在中断服务程序中从队列接收一条消息，不阻塞。
 *
 * @param[in] xQueue 源队列句柄。
 * @param[out] pvBuffer 接收缓冲区地址；信号量形态下可为 NULL。
 * @param[out] pxHigherPriorityTaskWoken 若唤醒了更高优先级任务则置 htTRUE；可为 NULL。
 *
 * @return 取到消息返回 htPASS；队列空或参数错误返回 htFAIL。
 *
 * @warning 只能在中断上下文调用，不会阻塞。
 */
BaseType_t htQueueReceiveFromISR(QueueHandle_t xQueue, void *pvBuffer, BaseType_t *pxHigherPriorityTaskWoken)
{
	BaseType_t xReturn = htFAIL;
	htQUEUE_t *pxQueue = (htQUEUE_t *)xQueue;

	/* 检查参数有效性。 */
	if (pxQueue == NULL || (pxQueue->uxItemSize != 0U && pvBuffer == NULL)) {
		return htFAIL;
	}

	/* 先假定没有唤醒更高优先级的任务。 */
	if (pxHigherPriorityTaskWoken != NULL) {
		*pxHigherPriorityTaskWoken = htFALSE;
	}

	/* 只有队列里确实有消息才能取走。 */
	if (pxQueue->uxMessagesWaiting > 0) {
		/* 拷出消息，并尝试唤醒一个因队列满而阻塞的发送者。 */
		prvCopyDataFromQueue(pxQueue, pvBuffer);

		/* 只有被唤醒任务的优先级高于当前任务时，才要求中断退出后切换。 */
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
 * @brief 查询队列中当前等待处理的消息条数。
 *
 * @param[in] xQueue 队列句柄，可为 NULL。
 *
 * @return 消息条数；句柄为 NULL 时返回 0。
 *
 * @note 读取时短暂进入临界区，保证拿到的是自洽快照。
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
 * @brief 查询队列还剩多少空间。
 *
 * @param[in] xQueue 队列句柄，可为 NULL。
 *
 * @return 还能再放多少条消息；句柄为 NULL 时返回 0。
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
 * @brief 覆盖写：只对长度为 1 的数据队列有效，已有数据时直接覆盖旧值。
 *
 * @param[in] xQueue 目标队列句柄。
 * @param[in] pvItemToQueue 新消息的地址。
 *
 * @return 成功返回 htPASS；队列长度不是 1、是信号量或参数非法返回 htFAIL。
 *
 * @note 覆盖时把读写指针一起复位到头部，队列里始终只保留这一条消息。
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
 * @brief 清空队列：复位消息计数与读写指针，并解锁收发计数。
 *
 * @param[in] xQueue 队列句柄。
 *
 * @return 成功返回 htPASS；句柄为 NULL 返回 htFAIL。
 *
 * @note 信号量形态下同时把计数清零；已在等待链表上的任务不受影响。
 */
BaseType_t htQueueReset(QueueHandle_t xQueue)
{
	htQUEUE_t *pxQueue = (htQUEUE_t *)xQueue;

	if (pxQueue == NULL) {
		return htFAIL;
	}

	htEnterCritical();

	/* 复位消息计数、读写指针与接收/发送锁计数。 */
	pxQueue->uxMessagesWaiting = 0;
	pxQueue->pcWriteTo = pxQueue->pcHead;
	pxQueue->u.xQueue.pcReadFrom = pxQueue->pcHead;
	pxQueue->cRxLock = htQUEUE_UNLOCKED;
	pxQueue->cTxLock = htQUEUE_UNLOCKED;

	/* 信号量形态：计数一并清零。 */
	if (pxQueue->uxItemSize == 0) {
		pxQueue->u.xSemaphore.uxSemaphoreCount = 0;
	}

	htExitCritical();

	return htPASS;
}

/**
 * @brief 删除队列并释放其占用的内存。
 *
 * @param[in] xQueue 队列句柄，可为 NULL。
 *
 * @note 静态创建的队列只清字段、不释放内存，避免去释放用户自己的缓冲。
 * @warning 删除后句柄立即失效，调用者不得再使用。
 */
void htQueueDelete(QueueHandle_t xQueue)
{
	htQUEUE_t *pxQueue = (htQUEUE_t *)xQueue;

	if (pxQueue != NULL) {
		if (pxQueue->ucStaticallyAllocated != 0U) {
			pxQueue->pcHead = NULL;
			pxQueue->pcWriteTo = NULL;
			pxQueue->uxMessagesWaiting = 0U;
			pxQueue->uxLength = 0U;
			pxQueue->uxItemSize = 0U;
			return;
		}

		/* 动态创建时先释放消息存储区，再释放队列结构。 */
		if (pxQueue->pcHead != NULL) {
			htPortFree(pxQueue->pcHead);
		}

		/* 释放队列结构本身。 */
		htPortFree(pxQueue);
	}
}

/**
 * @brief 查看队头消息但不移除；队列空时可限时阻塞等待。
 *
 * @param[in] xQueue 队列句柄。
 * @param[out] pvBuffer 接收缓冲区地址；信号量形态下可为 NULL。
 * @param[in] xTicksToWait 队列空时的最长等待 tick 数，0 表示不等待。
 *
 * @return 取到消息返回 htPASS；超时或参数错误返回 htFAIL。
 *
 * @note 只读 pcReadFrom 处的数据，不推进读指针、也不改消息计数。
 */
BaseType_t htQueuePeek(QueueHandle_t xQueue, void *pvBuffer, TickType_t xTicksToWait)
{
	htQUEUE_t *pxQueue = (htQUEUE_t *)xQueue;
	const TickType_t xStartTime = xTickCount;
	TickType_t xRemainingTicks = xTicksToWait;

	/* 检查参数有效性。 */
	if (pxQueue == NULL || (pxQueue->uxItemSize != 0U && pvBuffer == NULL)) {
		return htFAIL;
	}

	for (;;) {
		htEnterCritical();
		/* 有消息：直接从读指针处拷贝，但不推进读指针。 */
		if (pxQueue->uxMessagesWaiting > 0U) {
			if (pxQueue->uxItemSize != 0U) {
				memcpy(pvBuffer, (void *)pxQueue->u.xQueue.pcReadFrom, pxQueue->uxItemSize);
			}
			htExitCritical();
			return htPASS;
		}

		/* 队列为空：不等待则直接失败，否则阻塞到接收等待链表。 */
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
