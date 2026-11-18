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
#include "httask.h"

/** 持有者 uxOriginalPriority 的哨兵值：表示该持有者当前没有被继承提升过优先级。 */
#define htMUTEX_NO_INHERITED_PRIORITY UINT32_MAX

/**
 * @brief 把任务的优先级改成指定值，并同步修好它在就绪链表和事件链表中的位置。
 *
 * @param[in] pxTask 目标任务；为 NULL 或目标优先级越界（>= configMAX_PRIORITIES）时直接返回。
 * @param[in] uxPriority 新的优先级。
 *
 * 步骤：
 * 1. 任务处于 READY/RUNNING 时先把它从就绪链表摘下，事件表项也从所在事件链表摘下；
 * 2. 更新 uxPriority，并把两个表项的排序键改成 configMAX_PRIORITIES - uxPriority
 *    （htListInsert() 按值升序插入，键越小越靠前，所以高优先级任务的键更小）；
 * 3. 恢复挂载：就绪任务挂到新优先级就绪链表的尾部，仍在等待事件的任务挂回原事件链表。
 *
 * @note 必须在内核临界区内调用；中途被中断打断会让任务短暂同时挂在两个链表上。
 * @note 就绪链表用 htListInsertEnd() 而不是按值插入，是为了让同优先级任务保持
 *       “先来先跑”，改优先级不会打乱时间片轮转的顺序。
 * @note 事件链表指针取自 xEventListItem.pxContainer，摘链前先保存，摘链后仍可回溯。
 */
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
 * @brief 递归互斥量超时退出前的收尾：撤销本次等待期间施加的优先级继承。
 *
 * @param[in] xSemaphore 递归互斥量句柄。
 * @param[in] pxQueue 该句柄对应的队列控制块，调用者已经转换好，避免重复强转。
 * @return htPASS 已完成收尾；htFAIL 表示当前不应或无法撤销（还有等待者、队列为空）。
 *
 * 等待者在 htSemaphoreTakeRecursive() 中阻塞时，持有者记录是放回队列里的；
 * 如果最终因超时返回失败，这次等待期间施加的优先级提升必须撤回，否则持有者会永远
 * 停在被抬高的优先级上（优先级继承“泄漏”，会干扰后续的调度公平性）。
 *
 * @note 仅当 xTasksWaitingToReceive 为空时才撤销：还有任务在等这把锁说明锁仍被争用，
 *       保留提升可以避免下一次竞争重新出现优先级反转窗口。
 * @note 撤销方式为“取出记录 - 恢复原优先级 - 再放回”，记录本身的所有权语义不变，
 *       也不会把锁从持有者手里抢走。
 * @note 返回值只做诊断用，调用方无论如何都会返回 htFAIL。
 */
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

/**
 * @brief 创建二值信号量。
 *
 * 内部创建一个长度为 1、项大小为 0 的队列：项大小为 0 时队列只维护计数、不拷贝数据。
 *
 * @return 信号量句柄；内存不足时返回 NULL。
 * @note 初始计数为 0（不可用），需要先释放一次才能获取成功。
 */
SemaphoreHandle_t htSemaphoreCreateBinary(void)
{
	return htQueueCreate(1U, 0U);
}

/**
 * @brief 静态创建二值信号量，控制块由调用者提供。
 *
 * @param[in] pxQueueBuffer 调用者提供的队列控制块存储，通常为 static 变量。
 * @return 信号量句柄；参数非法、空间不足或未启用 configSUPPORT_STATIC_ALLOCATION 时返回 NULL。
 * @note 项大小为 0，不需要额外的消息存储区，一个 htQUEUE_t 就够。
 * @warning pxQueueBuffer 的生命周期必须覆盖信号量的整个使用期，不能是任务栈上的局部变量。
 */
SemaphoreHandle_t htSemaphoreCreateBinaryStatic(htQUEUE_t *pxQueueBuffer)
{
	return htQueueCreateStatic(1U, 0U, NULL, pxQueueBuffer);
}

/**
 * @brief 创建计数信号量。
 *
 * @param[in] uxMaxCount 计数上限，同时决定队列长度。
 * @param[in] uxInitialCount 初始计数，必须不大于 uxMaxCount。
 * @return 信号量句柄；uxMaxCount 为 0、初始计数越界或内存不足时返回 NULL。
 * @note 队列创建时计数为 0，这里再把 uxSemaphoreCount 与 uxMessagesWaiting 一起改成
 *       uxInitialCount；两个字段必须保持一致，前者给上层查询，后者是队列逻辑判断依据。
 */
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

/**
 * @brief 静态创建计数信号量，控制块由调用者提供。
 *
 * @param[in] uxMaxCount 计数上限。
 * @param[in] uxInitialCount 初始计数，必须不大于 uxMaxCount。
 * @param[in] pxQueueBuffer 调用者提供的队列控制块存储。
 * @return 信号量句柄；参数非法、控制块为空或静态分配未启用时返回 NULL。
 * @note 与动态版本一样，创建后会把两个计数字段一起改成 uxInitialCount。
 * @warning pxQueueBuffer 必须是长生命周期的静态存储，不能放在任务栈上。
 */
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

/**
 * @brief 创建互斥量。
 *
 * @return 互斥量句柄；内存不足时返回 NULL。
 * @note 等价于“计数上限 1、初始计数 1”的计数信号量，创建后即为未上锁状态。
 * @note 没有持有者记录，因此不校验谁获取谁释放，也不支持递归获取和优先级继承；
 *       需要递归语义请使用 htSemaphoreCreateRecursiveMutex()。
 */
SemaphoreHandle_t htSemaphoreCreateMutex(void)
{
	return htSemaphoreCreateCounting(1U, 1U);
}

/**
 * @brief 静态创建互斥量，控制块由调用者提供。
 *
 * @param[in] pxQueueBuffer 调用者提供的队列控制块存储。
 * @return 互斥量句柄；参数非法或静态分配未启用时返回 NULL。
 * @note 语义与 htSemaphoreCreateMutex() 相同，只是控制块来自调用者。
 */
SemaphoreHandle_t htSemaphoreCreateMutexStatic(htQUEUE_t *pxQueueBuffer)
{
	return htSemaphoreCreateCountingStatic(1U, 1U, pxQueueBuffer);
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
	return htSemaphoreTake(xSemaphore, xTicksToWait);
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
	htMutexHolder_t xMutexHolder;

	if (pxQueue == NULL || pxCurrentTCB == NULL) {
		return htFAIL;
	}

	for (;;) {
		/* 每一轮都在临界区内取出锁状态记录：拿到记录才谈得上判断能不能进入。 */
		htEnterCritical();
		if (htQueueReceive(xSemaphore, &xMutexHolder, 0U) != htPASS) {
			htExitCritical();
			return htFAIL;
		}

		/* 无人持有则直接持有，已属于自己则只是递归重入。 */
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

		/* 锁在别人手里且已经没有剩余等待时间：把记录原样放回后立即失败。 */
		if (xRemainingTicks == 0U) {
			(void)htQueueSend(xSemaphore, &xMutexHolder, 0U);
			htExitCritical();
			return htFAIL;
		}

		/* 锁被其他任务持有：先做优先级继承，再把记录放回队列（锁的所有权不变）。 */
		prvInheritMutexOwnerPriority(&xMutexHolder);
		(void)htQueueSend(xSemaphore, &xMutexHolder, 0U);

		/* 手工把当前任务挂到该队列的接收等待链表上，并按优先级排好事件链表项。 */
		htListRemove(&(pxCurrentTCB->xStateListItem));
		htListRemove(&(pxCurrentTCB->xEventListItem));
		htListSetItemValue(&(pxCurrentTCB->xEventListItem), configMAX_PRIORITIES - pxCurrentTCB->uxPriority);
		htListInsert(&(pxQueue->xTasksWaitingToReceive), &(pxCurrentTCB->xEventListItem));

		/* 有限等待时还要挂进延时链表；按唤醒时刻是否越过 xTickCount 决定放普通表还是溢出表。 */
		if (xRemainingTicks != htBLOCKED_INDEFINITELY) {
			const TickType_t xTimeToWake = xTickCount + xRemainingTicks;
			htListSetItemValue(&(pxCurrentTCB->xStateListItem), xTimeToWake);
			if (xTimeToWake > xTickCount) {
				htListInsert(pxDelayedTaskList, &(pxCurrentTCB->xStateListItem));
			} else {
				htListInsert(pxOverflowDelayedTaskList, &(pxCurrentTCB->xStateListItem));
			}
		}

		/* 标记阻塞后退出临界区并让出 CPU，直到有人释放锁或延时到期把本任务唤醒。 */
		pxCurrentTCB->uxTaskState = HT_TASK_BLOCKED;
		htExitCritical();
		htTaskYield();

		/* 被唤醒后按实际经过的滴答扣减超时：无符号差值在 tick 回绕时依然正确。 */
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

/**
 * @brief 释放互斥量（与 htSemaphoreGive() 等价）。
 *
 * @param[in] xSemaphore 互斥量句柄。
 * @return htPASS 释放成功；htFAIL 参数非法或互斥量本来就处于未上锁状态。
 * @note 不校验调用者是不是“持有者”——普通互斥量没有持有者记录，
 *       误用（例如未获取就释放）只能靠调用者自己保证。
 */
BaseType_t htSemaphoreGiveMutex(SemaphoreHandle_t xSemaphore)
{
	return htSemaphoreGive(xSemaphore);
}

/**
 * @brief 释放递归互斥量，只有递归计数归零时才真正解锁。
 *
 * @param[in] xSemaphore 递归互斥量句柄。
 * @return htPASS 释放成功（计数减 1）；htFAIL 参数非法、队列里没有记录，
 *         或当前任务并不是该锁的持有者。
 *
 * 处理过程：在临界区内取出锁状态记录，只有记录里的 xTaskHandle 等于当前任务
 * 且递归计数大于 0 时才把计数减 1；计数归零说明本轮是真正的解锁，此时撤销
 * 优先级继承、把持有者清成 NULL，最后无论成功与否都把记录放回队列，
 * 保证队列里始终保留一条锁状态记录。
 *
 * @note 计数未归零时不做任何调度动作，只是重入层数减少一层。
 * @note 解锁后由队列内核唤醒 xTasksWaitingToReceive 上优先级最高的等待者。
 * @warning 必须由持有者自己调用；其他任务调用会失败并返回 htFAIL。
 * @warning 必须在任务上下文调用，不能在中断中使用。
 */
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
		/* 只有持有者本人才能释放；递归计数未归零时不算真正解锁。 */
		if (xMutexHolder.xTaskHandle == pxCurrentTCB && xMutexHolder.uxRecursiveCallCount > 0U) {
			xMutexHolder.uxRecursiveCallCount--;
			if (xMutexHolder.uxRecursiveCallCount == 0U) {
				/* 最后一层释放：撤销优先级继承并清空持有者，锁真正变为可用。 */
				prvRestoreMutexOwnerPriority(&xMutexHolder);
				xMutexHolder.xTaskHandle = NULL;
			}
			xReturn = htPASS;
		}
		/* 无论释放是否成功都要把记录放回，否则队列里的锁状态会丢失。 */
		(void)htQueueSend(xSemaphore, &xMutexHolder, 0U);
	}
	htExitCritical();

	return xReturn;
#else
	(void)xSemaphore;
	return htFAIL;
#endif
}
