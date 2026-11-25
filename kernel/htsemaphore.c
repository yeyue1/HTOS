/**
 * @file htsemaphore.c
 * @brief 二值信号量、计数信号量、互斥量与递归互斥量的实现。
 *
 * 全部实现都复用队列内核，没有独立的数据结构：
 * - 队列项大小（uxItemSize）为 0 时，队列只维护 uxMessagesWaiting 计数、不拷贝数据，
 *   于是“长度为 1 的空队列”就是二值信号量，“长度为 uxMaxCount 的空队列”就是计数信号量。
 * - 互斥量是计数上限为 1、初始计数为 1 的计数信号量，创建后即处于未上锁状态。
 *   它没有持有者记录，因此不校验“谁获取谁释放”，也不支持递归和优先级继承。
 * - 递归互斥量把队列项设为 htMutexHolder_t，队列里始终保存一条“锁状态”记录：
 *   无人持有时 xTaskHandle 为 NULL，被持有时记录持有者、递归计数和被继承前的原始优先级。
 *   优先级继承与撤销都作用在这条记录上，因此它天然跟随队列一起受临界区保护。
 */
#include "htsemaphore.h"
#include "htmem.h"
#include "htqueue.h"
#include "htscheduler.h"
#include "htschedpolicy.h"
#include "httask.h"
#include "htkernel_internal.h"

#if (configUSE_RECURSIVE_MUTEXES == 1)

/** 持有者 uxOriginalPriority 的哨兵值：表示该持有者当前没有被继承提升过优先级。 */
#define htMUTEX_NO_INHERITED_PRIORITY UINT32_MAX

/**
 * @brief 把任务的优先级改成指定值，并同步修好它在就绪链表和事件链表中的位置。
 */
static void prvMoveReadyTaskToPriority(htTCB_t *pxTask, UBaseType_t uxPriority)
{
	if (pxTask == NULL || uxPriority >= configMAX_PRIORITIES) {
		return;
	}
	const UBaseType_t uxOldPriority = pxTask->uxPriority;
	htList_t *pxEventList = pxTask->xEventListItem.pxContainer;
	const BaseType_t xTaskIsReady =
			pxTask->uxTaskState == HT_TASK_READY || pxTask->uxTaskState == HT_TASK_RUNNING ? htTRUE : htFALSE;
	if (xTaskIsReady == htTRUE) {
		htListRemove(&(pxTask->xStateListItem));
		htSchedClearReadyPriorityIfEmpty(uxOldPriority);
	}
	if (pxEventList != NULL) {
		htListRemove(&(pxTask->xEventListItem));
	}
	pxTask->uxPriority = (uint8_t)uxPriority;
#if (configUSE_TRACE_FACILITY == 1)
	htListSetItemValue(&(pxTask->xAllListItem), configMAX_PRIORITIES - uxPriority);
#endif
	htListSetItemValue(&(pxTask->xEventListItem), configMAX_PRIORITIES - uxPriority);
	if (xTaskIsReady == htTRUE) {
		htListSetItemValue(&(pxTask->xStateListItem), configMAX_PRIORITIES - uxPriority);
		htListInsertEnd(&(pxReadyTasksLists[uxPriority]), &(pxTask->xStateListItem));
		htSchedRecordReadyPriority(uxPriority);
	}
	if (pxEventList != NULL) {
		htListInsert(pxEventList, &(pxTask->xEventListItem));
	}
}

/**
 * @brief 撤销一次优先级继承，把持有者恢复到被提升之前的优先级。
 *
 * @param[in,out] pxMutexHolder 队列中那条持有者记录（指针指向队列存储区）。
 *
 * 记录里的 uxOriginalPriority 为 htMUTEX_NO_INHERITED_PRIORITY 时说明从未被提升，
 * 直接返回；否则把任务优先级改回原值，并重新置回哨兵，保证重复调用是幂等的。
 *
 * @note 必须持临界区调用，且应只在递归计数归零、真正解锁时调用一次。
 */
static void prvRestoreMutexOwnerPriority(htMutexHolder_t *pxMutexHolder)
{
	if (pxMutexHolder == NULL || pxMutexHolder->xTaskHandle == NULL ||
			pxMutexHolder->uxOriginalPriority == htMUTEX_NO_INHERITED_PRIORITY) {
		return;
	}

	prvMoveReadyTaskToPriority(pxMutexHolder->xTaskHandle, pxMutexHolder->uxOriginalPriority);
	pxMutexHolder->uxOriginalPriority = htMUTEX_NO_INHERITED_PRIORITY;
}

/**
 * @brief 按优先级继承规则提升互斥量持有者的优先级。
 *
 * @param[in,out] pxMutexHolder 队列中那条持有者记录。
 *
 * 只有“当前任务优先级严格高于持有者”时才需要提升，否则直接返回。
 * 第一次提升时把持有者原来的优先级存进 uxOriginalPriority，再把它抬到与当前任务
 * 相同的优先级，让持有者尽快跑完临界区并释放锁，缩短高优先级任务的阻塞时间。
 *
 * @note 只在第一次提升时记录原优先级，因此多个任务叠加继承后也只回退一次
 *       就能回到初始值，不会被中间状态污染。
 * @note pxCurrentTCB 为 NULL（中断上下文）时直接返回：中断里不做优先级继承。
 * @note 必须持临界区调用。
 */
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

/**
 * @brief 递归互斥量超时退出前的收尾：按剩余最高优先级等待者重新计算或撤销优先级继承。
 *
 * @param[in,out] pxQueue 递归互斥量控制块，调用方已持有内核临界区。
 */
static void prvDisinheritRecursiveMutexAfterTimeout(htQUEUE_t *pxQueue)
{
	htMutexHolder_t *pxHolder;
	htListItem_t *pxHead;

	if (pxQueue == NULL || pxQueue->ucQueueType != htQUEUE_TYPE_RECURSIVE_MUTEX) {
		return;
	}
	pxHolder = &(pxQueue->u.xMutex);
	if (pxHolder->xTaskHandle == NULL || pxHolder->uxOriginalPriority == htMUTEX_NO_INHERITED_PRIORITY) {
		return;
	}

	pxHead = htListGetHead(&(pxQueue->xTasksWaitingToReceive));
	if (pxHead == NULL) {
		prvRestoreMutexOwnerPriority(pxHolder);
		return;
	}

	/* 仍有其他等待者：将持有者降到 max(原始优先级, 剩余最高等待者优先级)。 */
	htTCB_t *pxTopWaiter = (htTCB_t *)htListGetItemOwner(pxHead);
	UBaseType_t uxTargetPriority = pxHolder->uxOriginalPriority;
	if (pxTopWaiter != NULL && pxTopWaiter->uxPriority > uxTargetPriority) {
		uxTargetPriority = pxTopWaiter->uxPriority;
	}
	if (pxHolder->xTaskHandle->uxPriority != uxTargetPriority) {
		prvMoveReadyTaskToPriority(pxHolder->xTaskHandle, uxTargetPriority);
	}
	if (uxTargetPriority == pxHolder->uxOriginalPriority) {
		pxHolder->uxOriginalPriority = htMUTEX_NO_INHERITED_PRIORITY;
	}
}

/**
 * @brief 从递归互斥量等待链表唤醒优先级最高的等待者（调用方需持有临界区）。
 */
static htTCB_t *prvUnblockMutexWaiter(htList_t *pxEventList)
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

	htListRemove(&(pxTCB->xStateListItem));
	htListSetItemValue(&(pxTCB->xStateListItem), configMAX_PRIORITIES - pxTCB->uxPriority);
	pxTCB->uxTaskState = HT_TASK_READY;
	htListInsertEnd(&(pxReadyTasksLists[pxTCB->uxPriority]), &(pxTCB->xStateListItem));
	htSchedRecordReadyPriority(pxTCB->uxPriority);
	htSchedNotifyTaskEnqueued(pxTCB);
	return pxTCB;
}

/**
 * @brief 初始化递归互斥量控制块的内嵌锁字段。
 */
static void prvInitRecursiveMutexFields(htQUEUE_t *pxQueue)
{
	pxQueue->ucQueueType = htQUEUE_TYPE_RECURSIVE_MUTEX;
	pxQueue->uxMessagesWaiting = 1U;
	pxQueue->u.xMutex.xTaskHandle = NULL;
	pxQueue->u.xMutex.uxRecursiveCallCount = 0U;
	pxQueue->u.xMutex.uxOriginalPriority = htMUTEX_NO_INHERITED_PRIORITY;
}
#endif /* configUSE_RECURSIVE_MUTEXES */

#if (configUSE_COUNTING_SEMAPHORES == 1) || (configUSE_MUTEXES == 1)
static SemaphoreHandle_t prvCreateSemaphoreWithInitialCount(
		UBaseType_t uxMaxCount, UBaseType_t uxInitialCount, uint8_t ucQueueType)
{
	QueueHandle_t xSemaphore;

	if (uxMaxCount == 0U || uxInitialCount > uxMaxCount) {
		return NULL;
	}

	xSemaphore = htQueueCreate(uxMaxCount, 0U);
	if (xSemaphore != NULL) {
		htQUEUE_t *pxQueue = (htQUEUE_t *)xSemaphore;
		pxQueue->uxMessagesWaiting = uxInitialCount;
		pxQueue->ucQueueType = ucQueueType;
	}
	return xSemaphore;
}

static SemaphoreHandle_t prvCreateSemaphoreWithInitialCountStatic(
		UBaseType_t uxMaxCount, UBaseType_t uxInitialCount, uint8_t ucQueueType, htQUEUE_t *pxQueueBuffer)
{
	QueueHandle_t xSemaphore;

	if (pxQueueBuffer == NULL || uxMaxCount == 0U || uxInitialCount > uxMaxCount) {
		return NULL;
	}

	xSemaphore = htQueueCreateStatic(uxMaxCount, 0U, NULL, pxQueueBuffer);
	if (xSemaphore != NULL) {
		pxQueueBuffer->uxMessagesWaiting = uxInitialCount;
		pxQueueBuffer->ucQueueType = ucQueueType;
	}
	return xSemaphore;
}
#endif /* configUSE_COUNTING_SEMAPHORES || configUSE_MUTEXES */

/**
 * @brief 创建二值信号量。
 */
SemaphoreHandle_t htSemaphoreCreateBinary(void)
{
	QueueHandle_t xSemaphore = htQueueCreate(1U, 0U);
	if (xSemaphore != NULL) {
		((htQUEUE_t *)xSemaphore)->ucQueueType = htQUEUE_TYPE_BINARY_SEMAPHORE;
	}
	return xSemaphore;
}

/**
 * @brief 静态创建二值信号量，控制块由调用者提供。
 */
SemaphoreHandle_t htSemaphoreCreateBinaryStatic(htQUEUE_t *pxQueueBuffer)
{
	QueueHandle_t xSemaphore = htQueueCreateStatic(1U, 0U, NULL, pxQueueBuffer);
	if (xSemaphore != NULL) {
		pxQueueBuffer->ucQueueType = htQUEUE_TYPE_BINARY_SEMAPHORE;
	}
	return xSemaphore;
}

/**
 * @brief 创建计数信号量。
 */
SemaphoreHandle_t htSemaphoreCreateCounting(UBaseType_t uxMaxCount, UBaseType_t uxInitialCount)
{
#if (configUSE_COUNTING_SEMAPHORES == 1)
	return prvCreateSemaphoreWithInitialCount(uxMaxCount, uxInitialCount, htQUEUE_TYPE_COUNTING_SEMAPHORE);
#else
	(void)uxMaxCount;
	(void)uxInitialCount;
	return NULL;
#endif
}

/**
 * @brief 静态创建计数信号量，控制块由调用者提供。
 */
SemaphoreHandle_t htSemaphoreCreateCountingStatic(
		UBaseType_t uxMaxCount, UBaseType_t uxInitialCount, htQUEUE_t *pxQueueBuffer)
{
#if (configUSE_COUNTING_SEMAPHORES == 1)
	return prvCreateSemaphoreWithInitialCountStatic(
			uxMaxCount, uxInitialCount, htQUEUE_TYPE_COUNTING_SEMAPHORE, pxQueueBuffer);
#else
	(void)uxMaxCount;
	(void)uxInitialCount;
	(void)pxQueueBuffer;
	return NULL;
#endif
}

/**
 * @brief 创建互斥量。
 */
SemaphoreHandle_t htSemaphoreCreateMutex(void)
{
#if (configUSE_MUTEXES == 1)
	return prvCreateSemaphoreWithInitialCount(1U, 1U, htQUEUE_TYPE_MUTEX);
#else
	return NULL;
#endif
}

/**
 * @brief 静态创建互斥量，控制块由调用者提供。
 */
SemaphoreHandle_t htSemaphoreCreateMutexStatic(htQUEUE_t *pxQueueBuffer)
{
#if (configUSE_MUTEXES == 1)
	return prvCreateSemaphoreWithInitialCountStatic(1U, 1U, htQUEUE_TYPE_MUTEX, pxQueueBuffer);
#else
	(void)pxQueueBuffer;
	return NULL;
#endif
}

/**
 * @brief 创建递归互斥量。
 *
 * 创建一个长度为 1、项大小为 sizeof(htMutexHolder_t) 的队列，并预先塞入一条
 * “无人持有”的记录：xTaskHandle 为 NULL、递归计数 0、原始优先级为哨兵值。
 * 这把锁的持有者、递归计数和优先级继承状态全部保存在这条队列记录里。
 *
 * @return 互斥量句柄；内存不足、首次入队失败或未启用 configUSE_RECURSIVE_MUTEXES 时返回 NULL。
 * @note 未启用递归互斥量时函数体退化为 return NULL，接口仍然保留，便于上层无条件链接。
 * @note 首次入队失败时会先删除刚创建的队列再返回，避免内存泄漏。
 */
SemaphoreHandle_t htSemaphoreCreateRecursiveMutex(void)
{
#if configUSE_RECURSIVE_MUTEXES == 1
	QueueHandle_t xSemaphore = htQueueCreate(1U, 0U);
	if (xSemaphore != NULL) {
		prvInitRecursiveMutexFields((htQUEUE_t *)xSemaphore);
	}
	return xSemaphore;
#else
	return NULL;
#endif
}

SemaphoreHandle_t htSemaphoreCreateRecursiveMutexStatic(htQUEUE_t *pxQueueBuffer)
{
#if (configUSE_RECURSIVE_MUTEXES == 1) && (configSUPPORT_STATIC_ALLOCATION == 1)
	QueueHandle_t xSemaphore = htQueueCreateStatic(1U, 0U, NULL, pxQueueBuffer);
	if (xSemaphore != NULL) {
		prvInitRecursiveMutexFields((htQUEUE_t *)xSemaphore);
	}
	return xSemaphore;
#else
	(void)pxQueueBuffer;
	return NULL;
#endif
}

TaskHandle_t htSemaphoreGetMutexHolder(SemaphoreHandle_t xMutex)
{
#if configUSE_RECURSIVE_MUTEXES == 1
	htQUEUE_t *pxQueue = (htQUEUE_t *)xMutex;
	TaskHandle_t xHolder = NULL;

	if (pxQueue != NULL && pxQueue->ucQueueType == htQUEUE_TYPE_RECURSIVE_MUTEX) {
		htEnterCritical();
		xHolder = (TaskHandle_t)pxQueue->u.xMutex.xTaskHandle;
		htExitCritical();
	}
	return xHolder;
#else
	(void)xMutex;
	return NULL;
#endif
}

/**
 * @brief 删除信号量/互斥量，释放其占用的内存。
 *
 * @param[in] xSemaphore 要删除的句柄，可为 NULL（此时直接返回）。
 * @note 静态创建的信号量不会释放内存，只会把控制块里的长度、项大小和计数清零。
 * @warning 删除时若还有任务阻塞在该信号量上，这些任务不会被唤醒，行为未定义；
 *          请先确认没有等待者再删除。
 * @warning 删除后原句柄立即失效，其他任务再使用会访问已释放内存。
 */
void htSemaphoreDelete(SemaphoreHandle_t xSemaphore)
{
	htQueueDelete(xSemaphore);
}

/**
 * @brief 读取信号量的当前计数值。
 *
 * @param[in] xSemaphore 信号量句柄。
 * @return 当前可用计数；句柄为 NULL 时返回 0。
 * @note 在临界区内读取 uxMessagesWaiting，保证拿到的是自洽快照；但返回后计数可能
 *       立刻被其他任务或中断改变，不应用它做同步判断。
 */
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

/**
 * @brief 获取信号量（阻塞式）。
 *
 * @param[in] xSemaphore 信号量句柄。
 * @param[in] xTicksToWait 等待超时，单位滴答；htBLOCKED_INDEFINITELY 表示永久等待。
 * @return htPASS 获取成功；htFAIL 参数非法或等待超时。
 * @note 信号量的 uxItemSize 为 0，内核不会向 ucBuffer 写任何数据，这个缓冲区只是
 *       为了满足队列接收接口的形参，信号量完全靠计数增减完成 take/give。
 * @warning 会阻塞当前任务，不能在中断服务程序里调用；ISR 请用 htSemaphoreTakeFromISR()。
 */
BaseType_t htSemaphoreTake(SemaphoreHandle_t xSemaphore, TickType_t xTicksToWait)
{
	BaseType_t xReturn;
	uint8_t ucBuffer[4];

	xReturn = htQueueReceive(xSemaphore, ucBuffer, xTicksToWait);
	return xReturn;
}

/**
 * @brief 释放信号量（非阻塞）。
 *
 * @param[in] xSemaphore 信号量句柄。
 * @return htPASS 释放成功；htFAIL 参数非法或计数已达上限（二值信号量重复释放）。
 * @note 传入的是 &ucDummy：项大小为 0 时队列不会解引用它，只为满足接口形参。
 * @note 释放成功后若已有任务在等待，会唤醒其中优先级最高的一个。
 * @warning 不能在中断服务程序里调用；ISR 请用 htSemaphoreGiveFromISR()。
 */
BaseType_t htSemaphoreGive(SemaphoreHandle_t xSemaphore)
{
	BaseType_t xReturn;
	uint8_t ucDummy = 0U;

	xReturn = htQueueSend(xSemaphore, &ucDummy, 0U);
	return xReturn;
}

/**
 * @brief 在中断中获取信号量（不阻塞）。
 *
 * @param[in] xSemaphore 信号量句柄。
 * @param[out] pxHigherPriorityTaskWoken 回填“是否有更高优先级任务被唤醒”，可为 NULL。
 * @return htPASS 获取成功；htFAIL 参数非法或当前没有可用计数。
 *
 * 函数入口先把 *pxHigherPriorityTaskWoken 置为 htFALSE；若本次接收腾出了空间、
 * 唤醒了阻塞在“发送”上的任务，且该任务优先级高于当前任务，则回填 htTRUE。
 * ISR 退出前据此决定是否触发一次 PendSV 任务切换。
 *
 * @note 只做计数增减，不会阻塞，因此可以在任意中断优先级下调用。
 * @warning 不要在 ISR 里对递归互斥量调用本函数，递归互斥量必须在任务上下文中使用。
 */
BaseType_t htSemaphoreTakeFromISR(SemaphoreHandle_t xSemaphore, BaseType_t *pxHigherPriorityTaskWoken)
{
	BaseType_t xReturn;
	uint8_t ucBuffer[4];

	xReturn = htQueueReceiveFromISR(xSemaphore, ucBuffer, pxHigherPriorityTaskWoken);
	return xReturn;
}

/**
 * @brief 在中断中释放信号量（不阻塞）。
 *
 * @param[in] xSemaphore 信号量句柄。
 * @param[out] pxHigherPriorityTaskWoken 回填“是否有更高优先级任务被唤醒”，可为 NULL。
 * @return htPASS 释放成功；htFAIL 参数非法或计数已满。
 *
 * 若本次入队唤醒了等待接收的任务且其优先级高于当前任务，则把
 * *pxHigherPriorityTaskWoken 置为 htTRUE，供 ISR 退出时触发任务切换。
 *
 * @note 常见的“中断置位、任务清零”二值信号量用法就是本函数 + 任务里的 take。
 * @warning 不要在 ISR 里对递归互斥量调用本函数。
 */
BaseType_t htSemaphoreGiveFromISR(SemaphoreHandle_t xSemaphore, BaseType_t *pxHigherPriorityTaskWoken)
{
	BaseType_t xReturn;
	uint8_t ucDummy = 0U;

	xReturn = htQueueSendFromISR(xSemaphore, &ucDummy, pxHigherPriorityTaskWoken);
	return xReturn;
}

/**
 * @brief 获取互斥量（与 htSemaphoreTake() 等价）。
 *
 * @param[in] xSemaphore 互斥量句柄。
 * @param[in] xTicksToWait 等待超时，单位滴答；htBLOCKED_INDEFINITELY 表示永久等待。
 * @return htPASS 获取成功；htFAIL 参数非法或等待超时。
 * @note 普通互斥量只是一个计数上限为 1 的二值令牌：没有持有者记录，
 *       既不检查“是否由自己持有”，也不支持递归获取。
 */
BaseType_t htSemaphoreTakeMutex(SemaphoreHandle_t xSemaphore, TickType_t xTicksToWait)
{
#if (configUSE_MUTEXES == 1)
	return htSemaphoreTake(xSemaphore, xTicksToWait);
#else
	(void)xSemaphore;
	(void)xTicksToWait;
	return htFAIL;
#endif
}

/**
 * @brief 获取递归互斥量（阻塞式，同一任务可重复获取）。
 *
 * @param[in] xSemaphore 递归互斥量句柄。
 * @param[in] xTicksToWait 等待超时，单位滴答；htBLOCKED_INDEFINITELY 表示永久等待。
 * @return htPASS 获取成功；htFAIL 参数非法、超时，或未启用 configUSE_RECURSIVE_MUTEXES。
 *
 * 锁状态保存在队列里那条 htMutexHolder_t 记录中，每轮循环先把它取出来判断：
 * - 记录无人持有：把持有者置为当前任务、递归计数置 1，记录放回队列即取得锁；
 * - 记录已属当前任务：递归计数加 1 即可重入，代价只有一次计数递增；
 * - 记录被其他任务持有：先按优先级继承抬高持有者优先级，再把记录放回队列，
 *   然后手工把当前任务挂到该队列的接收等待链表上阻塞（这里不用 htQueueReceive()
 *   的阻塞路径，因为要在等待期间保留“记录原样放回”的语义）。
 *
 * 阻塞前若为有限等待，还要把任务挂进延时链表，由 tick 中断负责超时唤醒；
 * 被唤醒后按 xTickCount - xStartTime 算出实际经过的滴答，扣减后带着剩余超时重试，
 * 这样即使中途被多个唤醒源打断也不会延长总等待时间。
 *
 * @note 每一轮都会重新获取一次锁状态，所以唤醒不一定代表拿到了锁——可能只是超时，
 *       也可能被别人抢先，必须回到循环顶部再试。
 * @note 有限等待超时退出时会调用 prvRestoreRecursiveMutexAfterTimeout() 撤销本次
 *       施加的优先级继承；永久等待则不会走到超时分支。
 * @note 阻塞时手工操作 xStateListItem / xEventListItem，事件链表项的排序键要按
 *       configMAX_PRIORITIES - uxPriority 重新设置，与内核其他等待链表保持一致。
 * @warning 必须在任务上下文调用，不能在中断中使用。
 */
BaseType_t htSemaphoreTakeRecursive(SemaphoreHandle_t xSemaphore, TickType_t xTicksToWait)
{
#if configUSE_RECURSIVE_MUTEXES == 1
	const TickType_t xStartTime = xTickCount;
	TickType_t xRemainingTicks = xTicksToWait;
	htQUEUE_t *pxQueue = (htQUEUE_t *)xSemaphore;

	if (pxQueue == NULL || pxCurrentTCB == NULL || pxQueue->ucQueueType != htQUEUE_TYPE_RECURSIVE_MUTEX) {
		return htFAIL;
	}

	for (;;) {
		/* 直接在临界区内操作内嵌的 u.xMutex，不再通过 htQueueReceive/Send 反复入队，
		 * 彻底消除持有期间放回记录导致等待者被虚假唤醒/活锁的缺陷。 */
		htEnterCritical();
		htMutexHolder_t *pxHolder = &(pxQueue->u.xMutex);

		if (pxHolder->xTaskHandle == NULL) {
			pxHolder->xTaskHandle = pxCurrentTCB;
			pxHolder->uxRecursiveCallCount = 1U;
			pxHolder->uxOriginalPriority = htMUTEX_NO_INHERITED_PRIORITY;
			pxQueue->uxMessagesWaiting = 0U;
			htExitCritical();
			return htPASS;
		}

		if (pxHolder->xTaskHandle == pxCurrentTCB) {
			if (pxHolder->uxRecursiveCallCount == UINT32_MAX) {
				htExitCritical();
				return htFAIL;
			}
			pxHolder->uxRecursiveCallCount++;
			htExitCritical();
			return htPASS;
		}

		/* 锁在别人手里且无剩余等待时间：不继承、不唤醒，立即失败。 */
		if (xRemainingTicks == 0U) {
			htExitCritical();
			return htFAIL;
		}
		if (xRemainingTicks != htBLOCKED_INDEFINITELY &&
				(pxDelayedTaskList == NULL || pxOverflowDelayedTaskList == NULL)) {
			htExitCritical();
			return htFAIL;
		}

		/* 锁被其他任务持有：提升持有者优先级并将当前任务挂入等待链表阻塞。 */
		prvInheritMutexOwnerPriority(pxHolder);

		htListRemove(&(pxCurrentTCB->xStateListItem));
		htSchedClearReadyPriorityIfEmpty(pxCurrentTCB->uxPriority);
		htListRemove(&(pxCurrentTCB->xEventListItem));
		htListSetItemValue(&(pxCurrentTCB->xEventListItem), configMAX_PRIORITIES - pxCurrentTCB->uxPriority);
		htListInsert(&(pxQueue->xTasksWaitingToReceive), &(pxCurrentTCB->xEventListItem));

		if (xRemainingTicks != htBLOCKED_INDEFINITELY) {
			const TickType_t xTimeToWake = xTickCount + xRemainingTicks;
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

		if (xTicksToWait != htBLOCKED_INDEFINITELY) {
			const TickType_t xElapsed = xTickCount - xStartTime;
			if (xElapsed >= xTicksToWait) {
				htEnterCritical();
				prvDisinheritRecursiveMutexAfterTimeout(pxQueue);
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
#if (configUSE_MUTEXES == 1)
	return htSemaphoreGive(xSemaphore);
#else
	(void)xSemaphore;
	return htFAIL;
#endif
}

BaseType_t htSemaphoreGiveRecursive(SemaphoreHandle_t xSemaphore)
{
#if configUSE_RECURSIVE_MUTEXES == 1
	BaseType_t xReturn = htFAIL;
	BaseType_t xYieldRequired = htFALSE;
	htQUEUE_t *pxQueue = (htQUEUE_t *)xSemaphore;

	if (pxQueue == NULL || pxCurrentTCB == NULL || pxQueue->ucQueueType != htQUEUE_TYPE_RECURSIVE_MUTEX) {
		return htFAIL;
	}

	htEnterCritical();
	htMutexHolder_t *pxHolder = &(pxQueue->u.xMutex);
	if (pxHolder->xTaskHandle == pxCurrentTCB && pxHolder->uxRecursiveCallCount > 0U) {
		pxHolder->uxRecursiveCallCount--;
		if (pxHolder->uxRecursiveCallCount == 0U) {
			const UBaseType_t uxPriorityBeforeRestore = pxCurrentTCB->uxPriority;
			/* 最后一层释放：撤销优先级继承、清空持有者并唤醒最高优先级等待者。 */
			prvRestoreMutexOwnerPriority(pxHolder);
			pxHolder->xTaskHandle = NULL;
			pxQueue->uxMessagesWaiting = 1U;

			htTCB_t *pxUnblocked = prvUnblockMutexWaiter(&(pxQueue->xTasksWaitingToReceive));
#if (configUSE_PREEMPTION == 1)
			if ((pxUnblocked != NULL && pxUnblocked->uxPriority > pxCurrentTCB->uxPriority) ||
					pxCurrentTCB->uxPriority < uxPriorityBeforeRestore) {
				xYieldRequired = htTRUE;
			}
#else
			(void)pxUnblocked;
			(void)uxPriorityBeforeRestore;
#endif
		}
		xReturn = htPASS;
	}
	htExitCritical();

	if (xYieldRequired == htTRUE) {
		htTaskYield();
	}
	return xReturn;
#else
	(void)xSemaphore;
	return htFAIL;
#endif
}
