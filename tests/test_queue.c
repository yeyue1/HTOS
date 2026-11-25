/**
 * @file test_queue.c
 * @brief 队列与信号量的主机侧 Unity 单元测试。
 *
 * 本套件在 PC 上由 MinGW gcc 编译运行，不依赖目标板：文件顶部用桩函数接管
 * htPortMalloc()/htPortFree()/htEnterCritical()/htExitCritical()/__disable_irq()/
 * __enable_irq()/htTaskYield()，并自行定义 pxCurrentTCB、xTickCount、
 * pxReadyTasksLists、pxDelayedTaskList 等内核全局量，使 kernel/htqueue.c 与
 * kernel/htsemaphore.c 可以脱离调度器单独链接。
 *
 * 覆盖范围：队列的 FIFO 顺序与满队列发送失败、uxItemSize == 0 时的计数上限、
 * 存储区分配失败后的清理、二值/计数/递归互斥量的计数与持有者深度、
 * 满队列有限等待超时后任务重新就绪、单元素队列的覆盖写、静态创建不产生堆操作、
 * FromISR 唤醒更高优先级任务、htQueuePeek()/htQueueReset() 之后队列仍然一致、
 * 删除时精确释放动态存储。
 *
 * @note 断言统一用十进制字面量比较计数；改动桩函数会同时影响本文件全部用例，
 *       因此每个用例开头都要先调用 reset_test_state() 复位内核全局量。
 */

#include "unity.h"

#include <stdlib.h>
#include <string.h>

#include "htqueue.h"
#include "htscheduler.h"
#include "htsemaphore.h"
#include "htkernel_internal.h"

/** 当前任务指针：内核 API 靠它判断调用者身份，用例按需赋值。 */
/* ------------------------------------------------------------------ */
/* 1. 内核全局量                                                      */
/* ------------------------------------------------------------------ */
htTCB_t *pxCurrentTCB = NULL;
/** 系统节拍计数：htTaskYield() 桩会按 ticks_advanced_on_yield 推进它。 */
TickType_t xTickCount = 0U;
/** 就绪链表数组，下标即优先级：唤醒后断言任务挂在哪个桶就靠它。 */
htList_t pxReadyTasksLists[configMAX_PRIORITIES];
/** 普通延时链表实体，供 pxDelayedTaskList 指向。 */
static htList_t delayed_task_list;
/** 延时溢出链表实体，节拍回绕后到期的任务挂在这里。 */
static htList_t overflow_delayed_task_list;
/** 内核使用的延时链表指针，指向上面的实体。 */
htList_t *pxDelayedTaskList = &delayed_task_list;
/** 内核使用的延时溢出链表指针。 */
htList_t *pxOverflowDelayedTaskList = &overflow_delayed_task_list;
TickType_t xNextTaskUnblockTime = (TickType_t)htBLOCKED_INDEFINITELY;
/** 临界区嵌套计数：内核退出临界区时会读它，桩函数必须如实维护。 */
volatile UBaseType_t uxCriticalNesting = 0U;

/** 累计分配次数；静态创建用例断言它保持 0。 */
static size_t allocation_count;
/** 累计释放次数；删除类用例用它精确断言释放了几块。 */
static size_t free_count;
/** 第几次分配强制失败（从 1 开始计数），0 表示不注入失败。 */
static size_t fail_allocation_at;
/** 每次主动让出推进的节拍数，0 表示只让出、不推进时间。 */
static TickType_t ticks_advanced_on_yield;
/** 记录 htTaskYield() 调用次数，用于验证抢占式唤醒。 */
static UBaseType_t yield_count;
/** 记录中断临界区嵌套次数，验证 FromISR 临界区配对。 */
static UBaseType_t isr_critical_nesting;

/**
 * @brief 替换内核的动态分配：统计次数，并可注入分配失败。
 *
 * @param[in] size 请求的字节数。
 * @return 分配到的内存；命中 fail_allocation_at 时返回 NULL。
 * @note 故意不做尺寸与对齐校验，避免掩盖内核自身的容量计算问题。
 */
/* ------------------------------------------------------------------ */
/* 2. 内核依赖桩函数                                                  */
/* ------------------------------------------------------------------ */
void *htPortMalloc(size_t size)
{
	allocation_count++;
	if (fail_allocation_at != 0U && allocation_count == fail_allocation_at) {
		return NULL;
	}
	return malloc(size);
}

/**
 * @brief 替换内核的释放函数，只统计非空释放次数。
 *
 * @param[in] pointer 待释放指针；传 NULL 时不计数，与真实堆的行为一致。
 */
void htPortFree(void *pointer)
{
	if (pointer != NULL) {
		free_count++;
		free(pointer);
	}
}

/**
 * @brief 模拟进入临界区，只把嵌套计数加一。
 *
 * @note 主机侧没有 CPSID I，"关中断"的效果完全由 uxCriticalNesting 体现：
 *       内核若少退出一次，用例末尾的 TEST_ASSERT_EQUAL(0, uxCriticalNesting) 就会失败。
 */
void htEnterCritical(void)
{
	uxCriticalNesting++;
}

/**
 * @brief 模拟退出临界区，嵌套计数减一并在 0 处钳位。
 *
 * @note 与 htEnterCritical() 成对使用；多余的开中断调用被静默吞掉，
 *       真正的泄漏（少一次退出）由用例末尾的计数断言反向捕获。
 */
void htExitCritical(void)
{
	if (uxCriticalNesting > 0U) {
		uxCriticalNesting--;
	}
}

UBaseType_t htEnterCriticalFromISR(void)
{
	isr_critical_nesting++;
	return isr_critical_nesting;
}

void htExitCriticalFromISR(UBaseType_t uxSavedInterruptStatus)
{
	(void)uxSavedInterruptStatus;
	if (isr_critical_nesting > 0U) {
		isr_critical_nesting--;
	}
}

void htSchedNotifyTaskEnqueued(htTCB_t *pxTask)
{
	(void)pxTask;
}

void htSchedRecordReadyPriority(UBaseType_t uxPriority)
{
	(void)uxPriority;
}

void htSchedClearReadyPriorityIfEmpty(UBaseType_t uxPriority)
{
	(void)uxPriority;
}

/**
 * @brief 模拟关中断，与 htEnterCritical() 共用同一个嵌套计数。
 *
 * @note 真实移植层里 __disable_irq() 与 htEnterCritical() 操作同一套嵌套层次，
 *       这里刻意共用 uxCriticalNesting，任何不配对的开关都会让最终计数不为 0。
 */
void __disable_irq(void)
{
	uxCriticalNesting++;
}

/**
 * @brief 模拟开中断，把共享的嵌套计数减一。
 *
 * @warning 调用次数多于 __disable_irq() 时会被钳到 0；真实硬件上这等价于
 *          临界区被提前打开，本测试只检查最终计数而不检查时序。
 */
void __enable_irq(void)
{
	if (uxCriticalNesting > 0U) {
		uxCriticalNesting--;
	}
}

/**
 * @brief 替换内核的主动让出：把当前任务放回就绪链表，并按配置推进节拍。
 *
 * @note 只有 ticks_advanced_on_yield 非 0 时才推进 xTickCount 并处理延时链表，
 *       用来区分"等到超时"与"立即返回"两种结果；pxCurrentTCB 为 NULL 时直接返回。
 * @note 本桩不切换任务，只让状态与链表的变化可以被断言。
 */
void htTaskYield(void)
{
	yield_count++;
	if (pxCurrentTCB == NULL || ticks_advanced_on_yield == 0U) {
		return;
	}

	xTickCount += ticks_advanced_on_yield;
	ticks_advanced_on_yield = 0U;
	htListRemove(&(pxCurrentTCB->xStateListItem));
	htListRemove(&(pxCurrentTCB->xEventListItem));
	pxCurrentTCB->uxTaskState = HT_TASK_READY;
	htListInsertEnd(&(pxReadyTasksLists[pxCurrentTCB->uxPriority]), &(pxCurrentTCB->xStateListItem));
}

static void reset_test_state(void)
{
	allocation_count = 0U;
	free_count = 0U;
	fail_allocation_at = 0U;
	ticks_advanced_on_yield = 0U;
	yield_count = 0U;
	isr_critical_nesting = 0U;
	uxCriticalNesting = 0U;
	xTickCount = 0U;
	xNextTaskUnblockTime = (TickType_t)htBLOCKED_INDEFINITELY;
	pxCurrentTCB = NULL;
	htListInit(pxDelayedTaskList);
	htListInit(pxOverflowDelayedTaskList);
	for (UBaseType_t priority = 0U; priority < configMAX_PRIORITIES; priority++) {
		htListInit(&pxReadyTasksLists[priority]);
	}
}

/**
 * @brief 在调用者提供的 TCB 上构造"已就绪"的任务，并把它设为当前任务。
 *
 * @param[in,out] task 调用者提供的 TCB 存储。
 * @param[in] priority 优先级，同时写入 uxPriority 与 uxBasePriority。
 * @note 链表节点必须先 htListItemInit() 再 htListSetItemOwner()，
 *       否则 htListRemove() 会解引用未初始化的 pxContainer。
 */
static void initialise_current_task(htTCB_t *task, UBaseType_t priority)
{
	memset(task, 0, sizeof(*task));
	htListItemInit(&(task->xStateListItem));
	htListItemInit(&(task->xEventListItem));
	htListSetItemOwner(&(task->xStateListItem), task);
	htListSetItemOwner(&(task->xEventListItem), task);
	task->uxPriority = priority;
	task->uxBasePriority = priority;
	task->uxTaskState = HT_TASK_READY;
	htListInsertEnd(&(pxReadyTasksLists[priority]), &(task->xStateListItem));
	pxCurrentTCB = task;
}

/**
 * @brief 队列按 FIFO 出队；满队列发送必须失败且不留下未退出的临界区。
 *
 * 断言点：连续发送两次成功，第三次返回 htFAIL；随后 uxCriticalNesting 必须回到 0 ——
 * 这条断言专门捕获"满队列提前 return 却漏掉 htExitCritical()"的缺陷。
 *
 * @note Unity 的 TEST_ASSERT_EQUAL 宏展开后实参可能出现多次，
 *       因此断言里只写纯函数调用，不要放入带副作用的表达式。
 */
/* ------------------------------------------------------------------ */
/* 4. 测试用例                                                        */
/* ------------------------------------------------------------------ */
static void test_queue_is_fifo_and_full_send_balances_critical_section(void)
{
	reset_test_state();
	QueueHandle_t queue = htQueueCreate(2U, sizeof(int));
	int first = 11;
	int second = 22;
	int third = 33;
	int received = 0;

	TEST_ASSERT_NOT_NULL(queue);
	TEST_ASSERT_EQUAL(htPASS, htQueueSend(queue, &first, 0U));
	TEST_ASSERT_EQUAL(htPASS, htQueueSend(queue, &second, 0U));
	TEST_ASSERT_EQUAL(htFAIL, htQueueSend(queue, &third, 0U));
	TEST_ASSERT_EQUAL(0, uxCriticalNesting);
	TEST_ASSERT_EQUAL(2, htQueueMessagesWaiting(queue));
	TEST_ASSERT_EQUAL(htPASS, htQueueReceive(queue, &received, 0U));
	TEST_ASSERT_EQUAL(first, received);
	TEST_ASSERT_EQUAL(htPASS, htQueueReceive(queue, &received, 0U));
	TEST_ASSERT_EQUAL(second, received);
	htQueueDelete(queue);
}

/**
 * @brief uxItemSize == 0（信号量模式）的队列同样受 uxLength 限制。
 *
 * 断言点：第一次发送成功、第二次返回 htFAIL，等待中的消息数保持 1；
 * 用例结束时临界区计数为 0。
 * @note 该模式不拷贝元素、也不分配存储区，容量判断只能依赖计数器。
 */
static void test_zero_sized_queue_honors_maximum_count(void)
{
	reset_test_state();
	QueueHandle_t semaphore = htQueueCreate(1U, 0U);
	unsigned char dummy = 0U;

	TEST_ASSERT_NOT_NULL(semaphore);
	TEST_ASSERT_EQUAL(htPASS, htQueueSend(semaphore, &dummy, 0U));
	TEST_ASSERT_EQUAL(htFAIL, htQueueSend(semaphore, &dummy, 0U));
	TEST_ASSERT_EQUAL(1, htQueueMessagesWaiting(semaphore));
	TEST_ASSERT_EQUAL(0, uxCriticalNesting);
	htQueueDelete(semaphore);
}

/**
 * @brief 存储区分配失败时 htQueueCreate() 必须返回 NULL。
 *
 * fail_allocation_at = 2 让第 2 次分配（存储区）失败，
 * 第 1 次分配出来的队列结构体应被回滚释放。
 * @note 这里只断言返回值；释放次数由后面的删除用例覆盖。
 */
static void test_queue_create_returns_null_when_storage_allocation_fails(void)
{
	reset_test_state();
	fail_allocation_at = 2U;
	TEST_ASSERT_NULL(htQueueCreate(2U, sizeof(int)));
}

/**
 * @brief 互斥量表现为有界的二值令牌：取走后不能再取，归还后不能再归还。
 *
 * 断言点：初始计数为 1；take 后为 0，再次 take 返回 htFAIL；
 * give 后回到 1，再次 give 因达到上限返回 htFAIL。
 * @note 普通互斥量没有持有者记录，本用例只验证计数语义；
 *       持有者与递归深度见下面的递归互斥量用例。
 */
static void test_mutex_behaves_as_bounded_binary_token(void)
{
	reset_test_state();
	SemaphoreHandle_t mutex = htSemaphoreCreateMutex();

	TEST_ASSERT_NOT_NULL(mutex);
	TEST_ASSERT_EQUAL(1, htSemaphoreGetCount(mutex));
	TEST_ASSERT_EQUAL(htPASS, htSemaphoreTakeMutex(mutex, 0U));
	TEST_ASSERT_EQUAL(0, htSemaphoreGetCount(mutex));
	TEST_ASSERT_EQUAL(htFAIL, htSemaphoreTakeMutex(mutex, 0U));
	TEST_ASSERT_EQUAL(htPASS, htSemaphoreGiveMutex(mutex));
	TEST_ASSERT_EQUAL(1, htSemaphoreGetCount(mutex));
	TEST_ASSERT_EQUAL(htFAIL, htSemaphoreGiveMutex(mutex));
	TEST_ASSERT_EQUAL(1, htSemaphoreGetCount(mutex));
	htSemaphoreDelete(mutex);
}

/**
 * @brief 计数信号量遵守创建时的初始值与最大计数。
 *
 * 断言点：初始为 2；take 后为 1；give 后回到 2 并可再给到 3；
 * 达到上限 3 后再 give 必须返回 htFAIL 且计数保持不变。
 */
static void test_counting_semaphore_honors_initial_and_maximum_counts(void)
{
	reset_test_state();
	SemaphoreHandle_t semaphore = htSemaphoreCreateCounting(3U, 2U);

	TEST_ASSERT_NOT_NULL(semaphore);
	TEST_ASSERT_EQUAL(2, htSemaphoreGetCount(semaphore));
	TEST_ASSERT_EQUAL(htPASS, htSemaphoreTake(semaphore, 0U));
	TEST_ASSERT_EQUAL(1, htSemaphoreGetCount(semaphore));
	TEST_ASSERT_EQUAL(htPASS, htSemaphoreGive(semaphore));
	TEST_ASSERT_EQUAL(2, htSemaphoreGetCount(semaphore));
	TEST_ASSERT_EQUAL(htPASS, htSemaphoreGive(semaphore));
	TEST_ASSERT_EQUAL(3, htSemaphoreGetCount(semaphore));
	TEST_ASSERT_EQUAL(htFAIL, htSemaphoreGive(semaphore));
	TEST_ASSERT_EQUAL(3, htSemaphoreGetCount(semaphore));
	htSemaphoreDelete(semaphore);
}

/**
 * @brief 递归互斥量记录持有者句柄与递归深度，逐层释放直到完全解锁。
 *
 * 断言点：两次 take 后 uxRecursiveCallCount 依次为 1、2；每次 give 递减；
 * 深度归零时内部记录的持有者必须被清成 NULL。
 * @note 内部状态通过 htQueuePeek() 读出 htMutexHolder_t 观察，
 *       因此本用例先把当前任务初始化成"已就绪"的任务。
 */
static void test_recursive_mutex_tracks_owner_depth_and_release(void)
{
	reset_test_state();
	htTCB_t owner = {0};
	SemaphoreHandle_t mutex = htSemaphoreCreateRecursiveMutex();

	TEST_ASSERT_NOT_NULL(mutex);
	TEST_ASSERT_EQUAL(1U, htSemaphoreGetCount(mutex));
	initialise_current_task(&owner, 2U);

	TEST_ASSERT_EQUAL(htPASS, htSemaphoreTakeRecursive(mutex, 0U));
	TEST_ASSERT(htSemaphoreGetMutexHolder(mutex) == &owner);
	TEST_ASSERT_EQUAL(1, mutex->u.xMutex.uxRecursiveCallCount);
	TEST_ASSERT_EQUAL(0U, htSemaphoreGetCount(mutex));

	TEST_ASSERT_EQUAL(htPASS, htSemaphoreTakeRecursive(mutex, 0U));
	TEST_ASSERT_EQUAL(2, mutex->u.xMutex.uxRecursiveCallCount);

	TEST_ASSERT_EQUAL(htPASS, htSemaphoreGiveRecursive(mutex));
	TEST_ASSERT_EQUAL(1, mutex->u.xMutex.uxRecursiveCallCount);
	TEST_ASSERT(htSemaphoreGetMutexHolder(mutex) == &owner);

	TEST_ASSERT_EQUAL(htPASS, htSemaphoreGiveRecursive(mutex));
	TEST_ASSERT_EQUAL(0, mutex->u.xMutex.uxRecursiveCallCount);
	TEST_ASSERT_NULL(htSemaphoreGetMutexHolder(mutex));
	TEST_ASSERT_EQUAL(1U, htSemaphoreGetCount(mutex));
	htSemaphoreDelete(mutex);
}

static void test_recursive_mutex_inherits_priority_and_restores_after_release(void)
{
	reset_test_state();
	htTCB_t owner = {0};
	htTCB_t waiter = {0};
	SemaphoreHandle_t mutex = htSemaphoreCreateRecursiveMutex();

	TEST_ASSERT_NOT_NULL(mutex);
	initialise_current_task(&owner, 0U);
	TEST_ASSERT_EQUAL(htPASS, htSemaphoreTakeRecursive(mutex, 0U));

	htListItemInit(&(waiter.xStateListItem));
	htListItemInit(&(waiter.xEventListItem));
	htListSetItemOwner(&(waiter.xStateListItem), &waiter);
	htListSetItemOwner(&(waiter.xEventListItem), &waiter);
	waiter.uxPriority = 3U;
	waiter.uxBasePriority = 3U;
	waiter.uxTaskState = HT_TASK_READY;
	htListInsertEnd(&(pxReadyTasksLists[3]), &(waiter.xStateListItem));
	pxCurrentTCB = &waiter;
	ticks_advanced_on_yield = 2U;

	TEST_ASSERT_EQUAL(htFAIL, htSemaphoreTakeRecursive(mutex, 2U));
	TEST_ASSERT_EQUAL(2U, xTickCount);
	TEST_ASSERT_EQUAL(0U, owner.uxBasePriority);
	TEST_ASSERT_EQUAL(0U, owner.uxPriority);

	pxCurrentTCB = &owner;
	TEST_ASSERT_EQUAL(UINT32_MAX, mutex->u.xMutex.uxOriginalPriority);
	TEST_ASSERT_EQUAL(htPASS, htSemaphoreGiveRecursive(mutex));
	TEST_ASSERT_EQUAL(0U, owner.uxPriority);
	htSemaphoreDelete(mutex);
}

/**
 * @brief 无等待（超时 0）的 take 失败时不得抬升持有者优先级。
 *
 * 断言点：优先级 3 的任务 try-take 失败后，持有者的 uxPriority 仍为 0。
 * @note 这条用例把"不阻塞就不继承"固化下来，
 *       防止有人把抬升逻辑挪到取锁快路径上。
 */
static void test_recursive_mutex_try_take_does_not_inherit_priority(void)
{
	reset_test_state();
	htTCB_t owner = {0};
	htTCB_t contender = {0};
	SemaphoreHandle_t mutex = htSemaphoreCreateRecursiveMutex();

	TEST_ASSERT_NOT_NULL(mutex);
	initialise_current_task(&owner, 0U);
	TEST_ASSERT_EQUAL(htPASS, htSemaphoreTakeRecursive(mutex, 0U));

	initialise_current_task(&contender, 3U);
	TEST_ASSERT_EQUAL(htFAIL, htSemaphoreTakeRecursive(mutex, 0U));
	TEST_ASSERT_EQUAL(0U, owner.uxPriority);

	pxCurrentTCB = &owner;
	TEST_ASSERT_EQUAL(htPASS, htSemaphoreGiveRecursive(mutex));
	htSemaphoreDelete(mutex);
}

/**
 * @brief 满队列的有限等待超时后，任务必须从等待链表摘除并重新变为就绪。
 *
 * 断言点：htQueueSend(..., 5) 返回 htFAIL 且节拍正好推进 5；
 * 任务状态回到 HT_TASK_READY，xEventListItem 已摘链（pxContainer 为 NULL），
 * xStateListItem 重新挂到 pxReadyTasksLists[1]；临界区计数为 0。
 * @note 这是队列侧唯一直接验证"超时真的发生"的用例，
 *       失败通常意味着等待链表与延时链表没有联动。
 */
static void test_full_queue_finite_wait_expires_and_unlinks_task(void)
{
	reset_test_state();
	QueueHandle_t queue = htQueueCreate(1U, sizeof(int));
	htTCB_t task = {0};
	int first = 11;
	int second = 22;

	TEST_ASSERT_NOT_NULL(queue);
	initialise_current_task(&task, 1U);
	TEST_ASSERT_EQUAL(htPASS, htQueueSend(queue, &first, 0U));
	ticks_advanced_on_yield = 5U;

	TEST_ASSERT_EQUAL(htFAIL, htQueueSend(queue, &second, 5U));
	TEST_ASSERT_EQUAL(5U, xTickCount);
	TEST_ASSERT_EQUAL(HT_TASK_READY, task.uxTaskState);
	TEST_ASSERT_NULL(task.xEventListItem.pxContainer);
	TEST_ASSERT(task.xStateListItem.pxContainer == &pxReadyTasksLists[1]);
	TEST_ASSERT_EQUAL(0, uxCriticalNesting);
	htQueueDelete(queue);
}

/**
 * @brief 单元素队列的覆盖写保留最新值，可用空间随之变化。
 *
 * 断言点：覆盖写前可用空间为 1，写后为 0；
 * 第二次覆盖写后队列中仍只有 1 个元素，接收到的就是最后写入的值。
 */
static void test_single_item_queue_overwrite_keeps_latest_value(void)
{
	reset_test_state();
	QueueHandle_t queue = htQueueCreate(1U, sizeof(int));
	int first = 11;
	int latest = 22;
	int received = 0;

	TEST_ASSERT_NOT_NULL(queue);
	TEST_ASSERT_EQUAL(1, htQueueSpacesAvailable(queue));
	TEST_ASSERT_EQUAL(htPASS, htQueueOverwrite(queue, &first));
	TEST_ASSERT_EQUAL(0, htQueueSpacesAvailable(queue));
	TEST_ASSERT_EQUAL(htPASS, htQueueOverwrite(queue, &latest));
	TEST_ASSERT_EQUAL(1, htQueueMessagesWaiting(queue));
	TEST_ASSERT_EQUAL(htPASS, htQueueReceive(queue, &received, 0U));
	TEST_ASSERT_EQUAL(latest, received);
	TEST_ASSERT_EQUAL(1, htQueueSpacesAvailable(queue));
	htQueueDelete(queue);
}

/**
 * @brief htQueueOverwrite() 只对长度为 1 的队列合法，其余形状必须拒绝。
 *
 * 断言点：多元素队列与信号量模式队列都返回 htFAIL；
 * htQueueSpacesAvailable(NULL) 返回 0 而不是解引用空指针；队列内容不受影响。
 */
static void test_queue_overwrite_rejects_invalid_queue_shapes(void)
{
	reset_test_state();
	QueueHandle_t multi_item_queue = htQueueCreate(2U, sizeof(int));
	QueueHandle_t semaphore = htQueueCreate(1U, 0U);
	int value = 7;

	TEST_ASSERT_NOT_NULL(multi_item_queue);
	TEST_ASSERT_NOT_NULL(semaphore);
	TEST_ASSERT_EQUAL(htFAIL, htQueueOverwrite(multi_item_queue, &value));
	TEST_ASSERT_EQUAL(htFAIL, htQueueOverwrite(semaphore, &value));
	TEST_ASSERT_EQUAL(0, htQueueSpacesAvailable(NULL));
	htQueueDelete(multi_item_queue);
	htQueueDelete(semaphore);
}

/**
 * @brief 静态创建使用调用者提供的结构体与存储区，全程不触碰堆。
 *
 * 断言点：句柄等于传入的 htQUEUE_t 缓冲；allocation_count 保持 0；
 * ucStaticallyAllocated 置位；删除后 free_count 仍为 0，且结构体被清空（uxLength 归零）。
 * @note 最后一行验证存储区指针为 NULL 时 htQueueCreateStatic() 返回 NULL。
 */
static void test_static_queue_uses_caller_storage_without_heap_operations(void)
{
	reset_test_state();
	htQUEUE_t queue_buffer;
	uint8_t storage[2U * sizeof(int)];
	int first = 11;
	int second = 22;
	int received = 0;

	QueueHandle_t queue = htQueueCreateStatic(2U, sizeof(int), storage, &queue_buffer);
	TEST_ASSERT(queue == &queue_buffer);
	TEST_ASSERT_EQUAL(0, allocation_count);
	TEST_ASSERT_EQUAL(1, queue_buffer.ucStaticallyAllocated);
	TEST_ASSERT_EQUAL(htPASS, htQueueSend(queue, &first, 0U));
	TEST_ASSERT_EQUAL(htPASS, htQueueSend(queue, &second, 0U));
	TEST_ASSERT_EQUAL(htPASS, htQueueReceive(queue, &received, 0U));
	TEST_ASSERT_EQUAL(first, received);

	htQueueDelete(queue);
	TEST_ASSERT_EQUAL(0, free_count);
	TEST_ASSERT_EQUAL(0, queue_buffer.uxLength);
	TEST_ASSERT_NULL(htQueueCreateStatic(2U, sizeof(int), NULL, &queue_buffer));
}

/**
 * @brief 静态二值信号量不需要调用者提供存储区（uxItemSize == 0）。
 *
 * 断言点：句柄等于传入缓冲；无堆操作；give/take 各成功一次；删除不产生释放。
 */
static void test_static_binary_semaphore_needs_no_storage(void)
{
	reset_test_state();
	htQUEUE_t semaphore_buffer;
	SemaphoreHandle_t semaphore = htSemaphoreCreateBinaryStatic(&semaphore_buffer);

	TEST_ASSERT(semaphore == &semaphore_buffer);
	TEST_ASSERT_EQUAL(0, allocation_count);
	TEST_ASSERT_EQUAL(htPASS, htSemaphoreGive(semaphore));
	TEST_ASSERT_EQUAL(htPASS, htSemaphoreTake(semaphore, 0U));
	htSemaphoreDelete(semaphore);
	TEST_ASSERT_EQUAL(0, free_count);
}

/**
 * @brief 静态计数信号量与静态互斥量同样不产生任何堆操作。
 *
 * 断言点：allocation_count 与 free_count 均为 0；计数信号量的初始值与递减正确；
 * 互斥量的 take/give 成对成功、重复 give 失败；
 * 初始计数大于最大计数时 htSemaphoreCreateCountingStatic() 返回 NULL。
 */
static void test_static_counting_semaphore_and_mutex_use_no_heap(void)
{
	reset_test_state();
	htQUEUE_t counting_buffer;
	htQUEUE_t mutex_buffer;
	SemaphoreHandle_t counting = htSemaphoreCreateCountingStatic(3U, 2U, &counting_buffer);
	SemaphoreHandle_t mutex = htSemaphoreCreateMutexStatic(&mutex_buffer);

	TEST_ASSERT_NOT_NULL(counting);
	TEST_ASSERT_NOT_NULL(mutex);
	TEST_ASSERT_EQUAL(0, allocation_count);
	TEST_ASSERT_EQUAL(2, htSemaphoreGetCount(counting));
	TEST_ASSERT_EQUAL(htPASS, htSemaphoreTake(counting, 0U));
	TEST_ASSERT_EQUAL(1, htSemaphoreGetCount(counting));
	TEST_ASSERT_EQUAL(htPASS, htSemaphoreTakeMutex(mutex, 0U));
	TEST_ASSERT_EQUAL(htFAIL, htSemaphoreTakeMutex(mutex, 0U));
	TEST_ASSERT_EQUAL(htPASS, htSemaphoreGiveMutex(mutex));
	TEST_ASSERT_EQUAL(htFAIL, htSemaphoreGiveMutex(mutex));

	htSemaphoreDelete(counting);
	htSemaphoreDelete(mutex);
	TEST_ASSERT_EQUAL(0, free_count);
	TEST_ASSERT_NULL(htSemaphoreCreateCountingStatic(2U, 3U, &counting_buffer));
}

/**
 * @brief 手工把任务挂到队列等待链表上，模拟它已经阻塞。
 *
 * @param[in,out] list 队列的等待链表（挂的是 xEventListItem）。
 * @param[in,out] task 调用者提供的 TCB 存储，先清零再初始化链表节点。
 * @param[in] priority 任务优先级，同时写入 uxPriority 与 uxBasePriority。
 * @note 只把 xEventListItem 挂链并绑定归属，xStateListItem 故意不挂链，
 *       这样 FromISR 唤醒后"状态项被挂进就绪链表"的断言才有意义。
 */
static void block_task_on(htList_t *list, htTCB_t *task, UBaseType_t priority)
{
	memset(task, 0, sizeof(*task));
	htListItemInit(&(task->xStateListItem));
	htListItemInit(&(task->xEventListItem));
	htListSetItemOwner(&(task->xEventListItem), task);
	task->uxPriority = priority;
	task->uxBasePriority = priority;
	task->uxTaskState = HT_TASK_BLOCKED;
	htListInsertEnd(list, &(task->xEventListItem));
}

/**
 * @brief FromISR 发送会唤醒阻塞在接收上的更高优先级任务。
 *
 * 断言点：返回 htPASS 且 *pxHigherPriorityTaskWoken 为 htTRUE；
 * 接收者从 HT_TASK_BLOCKED 回到 HT_TASK_READY 并挂入 pxReadyTasksLists[3]，
 * 其 xEventListItem 已摘链；等待中的消息数为 1。
 * @note 第二段：没有阻塞接收者时不得报告唤醒；
 *       第三段：队列满且唤醒指针为 NULL 时也必须安全失败。
 */
static void test_queue_isr_send_wakes_a_higher_priority_receiver(void)
{
	reset_test_state();
	QueueHandle_t queue = htQueueCreate(2U, sizeof(int));
	htTCB_t current = {0};
	htTCB_t receiver;
	int value = 42;
	BaseType_t woken = htFALSE;
	BaseType_t rc;

	TEST_ASSERT_NOT_NULL(queue);
	initialise_current_task(&current, 0U);
	block_task_on(&queue->xTasksWaitingToReceive, &receiver, 3U);

	rc = htQueueSendFromISR(queue, &value, &woken);
	TEST_ASSERT_EQUAL(htPASS, rc);
	TEST_ASSERT_EQUAL(htTRUE, woken);
	TEST_ASSERT_EQUAL(1, htQueueMessagesWaiting(queue));
	TEST_ASSERT_EQUAL(HT_TASK_READY, receiver.uxTaskState);
	TEST_ASSERT(receiver.xStateListItem.pxContainer == &pxReadyTasksLists[3]);
	TEST_ASSERT_NULL(receiver.xEventListItem.pxContainer);

	/* 没有阻塞接收者时不应报告唤醒 */
	woken = htTRUE;
	rc = htQueueSendFromISR(queue, &value, &woken);
	TEST_ASSERT_EQUAL(htPASS, rc);
	TEST_ASSERT_EQUAL(htFALSE, woken);

	/* 队列满 + NULL 唤醒指针也必须安全失败 */
	rc = htQueueSendFromISR(queue, &value, NULL);
	TEST_ASSERT_EQUAL(htFAIL, rc);
	TEST_ASSERT_EQUAL(2, htQueueMessagesWaiting(queue));
	htQueueDelete(queue);
}

/**
 * @brief FromISR 接收会唤醒因队列满而阻塞的更高优先级发送者。
 *
 * 断言点：接收到的就是先前写入的值，*pxHigherPriorityTaskWoken 为 htTRUE；
 * 阻塞的发送者回到就绪态并挂入 pxReadyTasksLists[2]。
 * @note 第二段：空队列接收必须失败，且不得把唤醒标志置为 htTRUE。
 */
static void test_queue_isr_receive_wakes_a_higher_priority_sender(void)
{
	reset_test_state();
	QueueHandle_t queue = htQueueCreate(1U, sizeof(int));
	htTCB_t current = {0};
	htTCB_t blocked_sender;
	int value = 7;
	int out = 0;
	BaseType_t woken = htFALSE;
	BaseType_t rc;

	TEST_ASSERT_NOT_NULL(queue);
	initialise_current_task(&current, 0U);
	TEST_ASSERT_EQUAL(htPASS, htQueueSend(queue, &value, 0U) == htPASS ? htPASS : htFAIL);
	block_task_on(&queue->xTasksWaitingToSend, &blocked_sender, 2U);

	rc = htQueueReceiveFromISR(queue, &out, &woken);
	TEST_ASSERT_EQUAL(htPASS, rc);
	TEST_ASSERT_EQUAL(7, out);
	TEST_ASSERT_EQUAL(htTRUE, woken);
	TEST_ASSERT_EQUAL(HT_TASK_READY, blocked_sender.uxTaskState);
	TEST_ASSERT(blocked_sender.xStateListItem.pxContainer == &pxReadyTasksLists[2]);

	/* 空队列接收必须失败且不报告唤醒 */
	woken = htTRUE;
	rc = htQueueReceiveFromISR(queue, &out, &woken);
	TEST_ASSERT_EQUAL(htFAIL, rc);
	TEST_ASSERT_EQUAL(htFALSE, woken);
	htQueueDelete(queue);
}

/**
 * @brief htQueuePeek() 只读不取，htQueueReset() 之后队列仍然可用。
 *
 * 断言点：peek 得到队首元素且消息数不变；接收一次后消息数递减；
 * reset 后消息数为 0、可用空间恢复为 3，
 * 随后发送/接收拿到的顺序从缓冲区起点重新开始。
 */
static void test_queue_reset_and_peek_keep_the_queue_consistent(void)
{
	reset_test_state();
	QueueHandle_t queue = htQueueCreate(3U, sizeof(int));
	const int values[3] = {5, 6, 7};
	int peeked = 0;
	int received = 0;
	BaseType_t rc;

	TEST_ASSERT_NOT_NULL(queue);
	for (UBaseType_t i = 0U; i < 3U; i++) {
		rc = htQueueSend(queue, &values[i], 0U);
		TEST_ASSERT_EQUAL(htPASS, rc);
	}
	TEST_ASSERT_EQUAL(3, htQueueMessagesWaiting(queue));
	TEST_ASSERT_EQUAL(0, htQueueSpacesAvailable(queue));

	rc = htQueuePeek(queue, &peeked, 0U);
	TEST_ASSERT_EQUAL(htPASS, rc);
	TEST_ASSERT_EQUAL(5, peeked);
	TEST_ASSERT_EQUAL(3, htQueueMessagesWaiting(queue));

	rc = htQueueReceive(queue, &received, 0U);
	TEST_ASSERT_EQUAL(htPASS, rc);
	TEST_ASSERT_EQUAL(5, received);

	rc = htQueueReset(queue);
	TEST_ASSERT_EQUAL(htPASS, rc);
	TEST_ASSERT_EQUAL(0, htQueueMessagesWaiting(queue));
	TEST_ASSERT_EQUAL(3, htQueueSpacesAvailable(queue));

	/* reset 之后队列仍然可用：读写位置从头开始 */
	rc = htQueueSend(queue, &values[1], 0U);
	TEST_ASSERT_EQUAL(htPASS, rc);
	rc = htQueueReceive(queue, &received, 0U);
	TEST_ASSERT_EQUAL(htPASS, rc);
	TEST_ASSERT_EQUAL(6, received);
	htQueueDelete(queue);
}

/**
 * @brief 删除时按"结构体 + 存储区"精确释放动态内存。
 *
 * 断言点：动态队列释放 2 块；信号量（uxItemSize == 0，无存储区）只释放结构体这 1 块；
 * 重复删除 NULL 安全且不产生额外释放。
 */
static void test_delete_releases_dynamic_storage(void)
{
	reset_test_state();
	QueueHandle_t queue = htQueueCreate(2U, sizeof(int));
	SemaphoreHandle_t counting = htSemaphoreCreateCounting(3U, 1U);
	size_t frees_before;

	TEST_ASSERT_NOT_NULL(queue);
	TEST_ASSERT_NOT_NULL(counting);
	TEST_ASSERT_EQUAL(1, htSemaphoreGetCount(counting));
	TEST_ASSERT_EQUAL(htPASS, htSemaphoreGive(counting));
	TEST_ASSERT_EQUAL(2, htSemaphoreGetCount(counting));

	/* 动态队列 = 结构体 + 存储区 两次释放 */
	frees_before = free_count;
	htQueueDelete(queue);
	TEST_ASSERT(free_count == frees_before + 2U);

	/* 信号量只有结构体（itemSize == 0 不分配存储区） */
	frees_before = free_count;
	htSemaphoreDelete(counting);
	TEST_ASSERT(free_count == frees_before + 1U);

	/* 重复删除 NULL 是安全的，且不产生释放 */
	htQueueDelete(NULL);
	htSemaphoreDelete(NULL);
	TEST_ASSERT(free_count == frees_before + 1U);
}
/* ------------------------------------------------------------------ */
/* 4. 队列集                                                          */
/* ------------------------------------------------------------------ */

/**
 * @brief 集合只报告"哪个成员有数据"，数据本身留在成员里。
 *
 * 断言点：空集合立即返回 NULL；发送后 select 返回该成员；令牌取走后再次 select
 * 仍返回 NULL（说明一次入队只产生一个令牌），而成员里的数据可以正常取出；
 * 对普通队列调用 select 必须被拒绝，而不是把队列内容误当成成员句柄。
 */
static void test_queue_set_select_reports_the_member_holding_data(void)
{
	reset_test_state();
	htQUEUE_t *set = htQueueSetCreate(4U);
	htQUEUE_t *first = htQueueCreate(2U, sizeof(int));
	htQUEUE_t *second = htQueueCreate(2U, sizeof(int));
	int value = 7;
	int received = 0;

	TEST_ASSERT_NOT_NULL(set);
	TEST_ASSERT_NOT_NULL(first);
	TEST_ASSERT_NOT_NULL(second);
	TEST_ASSERT_EQUAL(htPASS, htQueueAddToSet(first, set));
	TEST_ASSERT_EQUAL(htPASS, htQueueAddToSet(second, set));

	TEST_ASSERT_NULL(htQueueSelectFromSet(set, 0U));
	TEST_ASSERT_EQUAL(htPASS, htQueueSend(first, &value, 0U));
	TEST_ASSERT(first == htQueueSelectFromSet(set, 0U));
	TEST_ASSERT_NULL(htQueueSelectFromSet(set, 0U));

	TEST_ASSERT_EQUAL(htPASS, htQueueReceive(first, &received, 0U));
	TEST_ASSERT_EQUAL(value, received);

	/* 普通队列没有集合标记，不能被当作集合使用。 */
	TEST_ASSERT_NULL(htQueueSelectFromSet(second, 0U));
	TEST_ASSERT_EQUAL(0U, uxCriticalNesting);

	TEST_ASSERT_EQUAL(htPASS, htQueueRemoveFromSet(first, set));
	TEST_ASSERT_EQUAL(htPASS, htQueueRemoveFromSet(second, set));
	htQueueDelete(set);
	htQueueDelete(first);
	htQueueDelete(second);
}

/**
 * @brief 每成功入队一次就产生一个令牌，令牌数不会超过入队次数。
 *
 * 断言点：向长度为 2 的成员连发两条消息后可 select 两次，第三次返回 NULL；
 * 两次接收顺序仍是 FIFO，证明 select 没有从成员里取走数据。
 */
static void test_queue_set_emits_one_token_per_sent_item(void)
{
	reset_test_state();
	htQUEUE_t *set = htQueueSetCreate(4U);
	htQUEUE_t *member = htQueueCreate(2U, sizeof(int));
	int value = 1;
	int received = 0;

	TEST_ASSERT_NOT_NULL(set);
	TEST_ASSERT_NOT_NULL(member);
	TEST_ASSERT_EQUAL(htPASS, htQueueAddToSet(member, set));

	TEST_ASSERT_EQUAL(htPASS, htQueueSend(member, &value, 0U));
	value = 2;
	TEST_ASSERT_EQUAL(htPASS, htQueueSend(member, &value, 0U));

	TEST_ASSERT(member == htQueueSelectFromSet(set, 0U));
	TEST_ASSERT(member == htQueueSelectFromSet(set, 0U));
	TEST_ASSERT_NULL(htQueueSelectFromSet(set, 0U));

	TEST_ASSERT_EQUAL(htPASS, htQueueReceive(member, &received, 0U));
	TEST_ASSERT_EQUAL(1, received);
	TEST_ASSERT_EQUAL(htPASS, htQueueReceive(member, &received, 0U));
	TEST_ASSERT_EQUAL(2, received);
	TEST_ASSERT_EQUAL(0U, uxCriticalNesting);

	TEST_ASSERT_EQUAL(htPASS, htQueueRemoveFromSet(member, set));
	htQueueDelete(member);
	htQueueDelete(set);
}

/**
 * @brief 集合空时的有限等待会超时，并把任务重新挂回就绪链表。
 *
 * 断言点：select(..., 5) 返回 NULL 且节拍正好推进 5；任务回到 HT_TASK_READY，
 * 事件项已摘链、状态项挂回 pxReadyTasksLists[1]，临界区计数归零。
 */
static void test_queue_set_select_times_out_and_relinks_task(void)
{
	reset_test_state();
	htQUEUE_t *set = htQueueSetCreate(2U);
	htQUEUE_t *member = htQueueCreate(2U, sizeof(int));
	htTCB_t task = {0};

	TEST_ASSERT_NOT_NULL(set);
	TEST_ASSERT_NOT_NULL(member);
	TEST_ASSERT_EQUAL(htPASS, htQueueAddToSet(member, set));

	initialise_current_task(&task, 1U);
	ticks_advanced_on_yield = 5U;
	TEST_ASSERT_NULL(htQueueSelectFromSet(set, 5U));
	TEST_ASSERT_EQUAL(5U, xTickCount);
	TEST_ASSERT_EQUAL(HT_TASK_READY, task.uxTaskState);
	TEST_ASSERT_NULL(task.xEventListItem.pxContainer);
	TEST_ASSERT(task.xStateListItem.pxContainer == &pxReadyTasksLists[1]);
	TEST_ASSERT_EQUAL(0U, uxCriticalNesting);

	TEST_ASSERT_EQUAL(htPASS, htQueueRemoveFromSet(member, set));
	htQueueDelete(member);
	htQueueDelete(set);
}

/**
 * @brief 中断侧发送产生令牌，中断侧 select 能取到同一个成员。
 *
 * 断言点：发送成功且没有等待者时 pxHigherPriorityTaskWoken 保持 htFALSE；
 * 令牌只产生一次；数据仍可正常接收。
 */
static void test_queue_set_from_isr_reports_the_member(void)
{
	reset_test_state();
	htQUEUE_t *set = htQueueSetCreate(2U);
	htQUEUE_t *member = htQueueCreate(2U, sizeof(int));
	int value = 42;
	int received = 0;
	BaseType_t woken = htTRUE;

	TEST_ASSERT_NOT_NULL(set);
	TEST_ASSERT_NOT_NULL(member);
	TEST_ASSERT_EQUAL(htPASS, htQueueAddToSet(member, set));

	TEST_ASSERT_EQUAL(htPASS, htQueueSendFromISR(member, &value, &woken));
	TEST_ASSERT_EQUAL(htFALSE, woken);

	TEST_ASSERT(member == htQueueSelectFromSetFromISR(set));
	TEST_ASSERT_NULL(htQueueSelectFromSetFromISR(set));

	TEST_ASSERT_EQUAL(htPASS, htQueueReceiveFromISR(member, &received, NULL));
	TEST_ASSERT_EQUAL(value, received);
	TEST_ASSERT_EQUAL(0U, uxCriticalNesting);

	TEST_ASSERT_EQUAL(htPASS, htQueueRemoveFromSet(member, set));
	htQueueDelete(member);
	htQueueDelete(set);
}

/**
 * @brief 非法成员一律拒绝，且失败路径不残留临界区。
 *
 * 断言点：NULL 成员、NULL 集合、自身、以集合为成员、已属其它集合、非空成员
 * 全部返回 htFAIL；摘除非成员同样返回 htFAIL。
 */
static void test_queue_set_rejects_invalid_members(void)
{
	reset_test_state();
	htQUEUE_t *set = htQueueSetCreate(4U);
	htQUEUE_t *other_set = htQueueSetCreate(4U);
	htQUEUE_t *member = htQueueCreate(2U, sizeof(int));
	int value = 0;

	TEST_ASSERT_NOT_NULL(set);
	TEST_ASSERT_NOT_NULL(other_set);
	TEST_ASSERT_NOT_NULL(member);

	TEST_ASSERT_EQUAL(htFAIL, htQueueAddToSet(NULL, set));
	TEST_ASSERT_EQUAL(htFAIL, htQueueAddToSet(member, NULL));
	TEST_ASSERT_EQUAL(htFAIL, htQueueAddToSet(set, set));
	TEST_ASSERT_EQUAL(htFAIL, htQueueAddToSet(set, other_set));

	TEST_ASSERT_EQUAL(htPASS, htQueueAddToSet(member, set));
	TEST_ASSERT_EQUAL(htFAIL, htQueueAddToSet(member, other_set));
	TEST_ASSERT_EQUAL(htFAIL, htQueueAddToSet(member, set));

	TEST_ASSERT_EQUAL(htFAIL, htQueueRemoveFromSet(member, other_set));
	TEST_ASSERT_EQUAL(htFAIL, htQueueRemoveFromSet(member, NULL));

	/* 空成员才能加入：入队一次之后必须被拒绝。 */
	TEST_ASSERT_EQUAL(htPASS, htQueueRemoveFromSet(member, set));
	TEST_ASSERT_EQUAL(htPASS, htQueueSend(member, &value, 0U));
	TEST_ASSERT_EQUAL(htFAIL, htQueueAddToSet(member, set));
	TEST_ASSERT_EQUAL(0U, uxCriticalNesting);

	htQueueDelete(member);
	htQueueDelete(set);
	htQueueDelete(other_set);
}

/**
 * @brief 仍在集合里的成员、还有成员的集合都拒绝删除。
 *
 * 断言点：两次拒绝删除时释放次数不变且句柄仍可用；摘除后删除恢复正常的释放行为。
 */
static void test_queue_set_refuses_to_delete_live_participants(void)
{
	reset_test_state();
	htQUEUE_t *set = htQueueSetCreate(4U);
	htQUEUE_t *member = htQueueCreate(2U, sizeof(int));
	size_t frees_before;

	TEST_ASSERT_NOT_NULL(set);
	TEST_ASSERT_NOT_NULL(member);
	TEST_ASSERT_EQUAL(htPASS, htQueueAddToSet(member, set));

	frees_before = free_count;
	htQueueDelete(member);
	TEST_ASSERT_EQUAL(frees_before, free_count);
	TEST_ASSERT_EQUAL(2U, member->uxLength);

	htQueueDelete(set);
	TEST_ASSERT_EQUAL(frees_before, free_count);
	TEST_ASSERT_EQUAL(4U, set->uxLength);

	TEST_ASSERT_EQUAL(htPASS, htQueueRemoveFromSet(member, set));
	htQueueDelete(member);
	TEST_ASSERT(free_count > frees_before);

	frees_before = free_count;
	htQueueDelete(set);
	TEST_ASSERT(free_count > frees_before);
	TEST_ASSERT_EQUAL(0U, uxCriticalNesting);
}

/**
 * @brief 信号量可以作为集合成员：give 产生令牌，take 之后令牌不再重复。
 *
 * @note 信号量的 give 内部走 htQueueSend()，因此令牌生成路径与队列完全相同；
 *       这条用例把这个耦合固化为契约。
 */
static void test_queue_set_accepts_a_semaphore_member(void)
{
	reset_test_state();
	htQUEUE_t *set = htQueueSetCreate(2U);
	SemaphoreHandle_t semaphore = htSemaphoreCreateBinary();

	TEST_ASSERT_NOT_NULL(set);
	TEST_ASSERT_NOT_NULL(semaphore);
	TEST_ASSERT_EQUAL(htPASS, htQueueAddToSet(semaphore, set));
	TEST_ASSERT_NULL(htQueueSelectFromSet(set, 0U));

	TEST_ASSERT_EQUAL(htPASS, htSemaphoreGive(semaphore));
	TEST_ASSERT(semaphore == htQueueSelectFromSet(set, 0U));
	TEST_ASSERT_EQUAL(htPASS, htSemaphoreTake(semaphore, 0U));
	TEST_ASSERT_NULL(htQueueSelectFromSet(set, 0U));
	TEST_ASSERT_EQUAL(0U, uxCriticalNesting);

	TEST_ASSERT_EQUAL(htPASS, htQueueRemoveFromSet(semaphore, set));
	htSemaphoreDelete(semaphore);
	htQueueDelete(set);
}

/**
 * @brief 静态创建的集合与成员完全不碰堆，删除时也不释放调用方内存。
 *
 * 断言点：创建、加入、入队、select、接收全程 allocation_count 保持 0；
 * 删除前后 free_count 不变（静态对象只清字段）；容量为 0 时返回 NULL。
 * @note 纯静态固件（configTOTAL_HEAP_SIZE == 0）里这是队列集唯一可用的创建方式。
 */
static void test_queue_set_static_create_uses_caller_memory(void)
{
	reset_test_state();
	htQUEUE_t set_buffer = {0};
	uint8_t set_storage[4U * sizeof(QueueSetMemberHandle_t)];
	htQUEUE_t member_buffer = {0};
	uint8_t member_storage[2U * sizeof(int)];
	QueueSetHandle_t set;
	QueueHandle_t member;
	int value = 5;
	int received = 0;
	size_t frees_before;

	set = htQueueSetCreateStatic(4U, set_storage, &set_buffer);
	member = htQueueCreateStatic(2U, sizeof(int), member_storage, &member_buffer);
	TEST_ASSERT_NOT_NULL(set);
	TEST_ASSERT_NOT_NULL(member);
	TEST_ASSERT_EQUAL(0U, allocation_count);

	TEST_ASSERT_EQUAL(htPASS, htQueueAddToSet(member, set));
	TEST_ASSERT_EQUAL(htPASS, htQueueSend(member, &value, 0U));
	TEST_ASSERT(member == htQueueSelectFromSet(set, 0U));
	TEST_ASSERT_EQUAL(htPASS, htQueueReceive(member, &received, 0U));
	TEST_ASSERT_EQUAL(value, received);

	TEST_ASSERT_EQUAL(htPASS, htQueueRemoveFromSet(member, set));
	frees_before = free_count;
	htQueueDelete(member);
	htQueueDelete(set);
	TEST_ASSERT_EQUAL(frees_before, free_count);
	TEST_ASSERT_EQUAL(0U, uxCriticalNesting);

	TEST_ASSERT_NULL(htQueueSetCreateStatic(0U, set_storage, &set_buffer));
}

/**
 * @brief P0 回归：递归互斥量在被持有期间重入、竞争失败或误释放不得虚假唤醒等待者。
 */
static void test_recursive_mutex_no_spurious_wakeup_while_held(void)
{
	reset_test_state();
	htQUEUE_t mutex_buf = {0};
	htTCB_t owner = {0};
	htTCB_t waiter_h1 = {0};
	htTCB_t contender_h2 = {0};

	SemaphoreHandle_t mutex = htSemaphoreCreateRecursiveMutexStatic(&mutex_buf);
	TEST_ASSERT_NOT_NULL(mutex);
	TEST_ASSERT_EQUAL(0U, allocation_count);

	initialise_current_task(&owner, 1U);
	TEST_ASSERT_EQUAL(htPASS, htSemaphoreTakeRecursive(mutex, 0U));

	/* 模拟 H1 已阻塞在递归互斥量上。 */
	block_task_on(&(mutex->xTasksWaitingToReceive), &waiter_h1, 4U);

	/* 1) owner 递归重入不得唤醒 H1。 */
	TEST_ASSERT_EQUAL(htPASS, htSemaphoreTakeRecursive(mutex, 0U));
	TEST_ASSERT_EQUAL(HT_TASK_BLOCKED, waiter_h1.uxTaskState);
	TEST_ASSERT(waiter_h1.xEventListItem.pxContainer == &(mutex->xTasksWaitingToReceive));

	/* 2) H2 try-take 失败或非持有者误调 GiveRecursive 不得唤醒 H1。 */
	initialise_current_task(&contender_h2, 4U);
	TEST_ASSERT_EQUAL(htFAIL, htSemaphoreTakeRecursive(mutex, 0U));
	TEST_ASSERT_EQUAL(htFAIL, htSemaphoreGiveRecursive(mutex));
	TEST_ASSERT_EQUAL(HT_TASK_BLOCKED, waiter_h1.uxTaskState);

	/* 3) H2 阻塞超时后，因为 H1(prio 4) 仍在等，owner 仍保留继承优先级，且 H1 仍阻塞。 */
	ticks_advanced_on_yield = 3U;
	TEST_ASSERT_EQUAL(htFAIL, htSemaphoreTakeRecursive(mutex, 3U));
	TEST_ASSERT_EQUAL(HT_TASK_BLOCKED, waiter_h1.uxTaskState);
	TEST_ASSERT_EQUAL(4U, owner.uxPriority);

	/* 4) owner 逐层释放：第一层不解锁不唤醒，第二层解锁后唤醒 H1 并触发 yield。 */
	pxCurrentTCB = &owner;
	yield_count = 0U;
	TEST_ASSERT_EQUAL(htPASS, htSemaphoreGiveRecursive(mutex));
	TEST_ASSERT_EQUAL(HT_TASK_BLOCKED, waiter_h1.uxTaskState);
	TEST_ASSERT_EQUAL(0U, yield_count);

	TEST_ASSERT_EQUAL(htPASS, htSemaphoreGiveRecursive(mutex));
	TEST_ASSERT_EQUAL(HT_TASK_READY, waiter_h1.uxTaskState);
	TEST_ASSERT_EQUAL(1U, owner.uxPriority);
	TEST_ASSERT_EQUAL(1U, yield_count);
	TEST_ASSERT_EQUAL(0U, uxCriticalNesting);
	htSemaphoreDelete(mutex);
}

/**
 * @brief P0 回归：队列 send/receive/overwrite 唤醒更高优先级任务必须立即触发抢占，且 overwrite 通知队列集。
 */
static void test_queue_preemption_and_overwrite_queue_set(void)
{
	reset_test_state();
	QueueHandle_t queue = htQueueCreate(1U, sizeof(int));
	QueueSetHandle_t set = htQueueSetCreate(2U);
	htTCB_t low_task = {0};
	htTCB_t high_receiver = {0};
	htTCB_t high_sender = {0};
	int value = 99;
	int out = 0;

	TEST_ASSERT_NOT_NULL(queue);
	TEST_ASSERT_NOT_NULL(set);
	TEST_ASSERT_EQUAL(htPASS, htQueueAddToSet(queue, set));

	initialise_current_task(&low_task, 1U);
	block_task_on(&(queue->xTasksWaitingToReceive), &high_receiver, 4U);

	/* Overwrite 空队列：唤醒高优先级接收者 + 触发 yield + 向队列集投令牌。 */
	yield_count = 0U;
	TEST_ASSERT_EQUAL(htPASS, htQueueOverwrite(queue, &value));
	TEST_ASSERT_EQUAL(1U, yield_count);
	TEST_ASSERT_EQUAL(HT_TASK_READY, high_receiver.uxTaskState);
	TEST_ASSERT(queue == htQueueSelectFromSet(set, 0U));

	/* Receive 腾出空位：唤醒高优先级发送者 + 触发 yield。 */
	block_task_on(&(queue->xTasksWaitingToSend), &high_sender, 4U);
	yield_count = 0U;
	TEST_ASSERT_EQUAL(htPASS, htQueueReceive(queue, &out, 0U));
	TEST_ASSERT_EQUAL(value, out);
	TEST_ASSERT_EQUAL(1U, yield_count);
	TEST_ASSERT_EQUAL(HT_TASK_READY, high_sender.uxTaskState);

	/* Send 唤醒高优先级接收者 + 触发 yield。 */
	block_task_on(&(queue->xTasksWaitingToReceive), &high_receiver, 4U);
	yield_count = 0U;
	TEST_ASSERT_EQUAL(htPASS, htQueueSend(queue, &value, 0U));
	TEST_ASSERT_EQUAL(1U, yield_count);
	TEST_ASSERT_EQUAL(0U, uxCriticalNesting);
	TEST_ASSERT_EQUAL(0U, isr_critical_nesting);

	TEST_ASSERT_EQUAL(htPASS, htQueueRemoveFromSet(queue, set));
	htQueueDelete(queue);
	htQueueDelete(set);
}

int main(void)
{
	UnityBegin("test_queue.c");
	RUN_TEST(test_queue_is_fifo_and_full_send_balances_critical_section);
	RUN_TEST(test_zero_sized_queue_honors_maximum_count);
	RUN_TEST(test_queue_create_returns_null_when_storage_allocation_fails);
	RUN_TEST(test_mutex_behaves_as_bounded_binary_token);
	RUN_TEST(test_counting_semaphore_honors_initial_and_maximum_counts);
	RUN_TEST(test_recursive_mutex_tracks_owner_depth_and_release);
	RUN_TEST(test_recursive_mutex_inherits_priority_and_restores_after_release);
	RUN_TEST(test_recursive_mutex_try_take_does_not_inherit_priority);
	RUN_TEST(test_full_queue_finite_wait_expires_and_unlinks_task);
	RUN_TEST(test_single_item_queue_overwrite_keeps_latest_value);
	RUN_TEST(test_queue_overwrite_rejects_invalid_queue_shapes);
	RUN_TEST(test_static_queue_uses_caller_storage_without_heap_operations);
	RUN_TEST(test_static_binary_semaphore_needs_no_storage);
	RUN_TEST(test_static_counting_semaphore_and_mutex_use_no_heap);
	RUN_TEST(test_queue_isr_send_wakes_a_higher_priority_receiver);
	RUN_TEST(test_queue_isr_receive_wakes_a_higher_priority_sender);
	RUN_TEST(test_queue_reset_and_peek_keep_the_queue_consistent);
	RUN_TEST(test_delete_releases_dynamic_storage);
	RUN_TEST(test_queue_set_select_reports_the_member_holding_data);
	RUN_TEST(test_queue_set_emits_one_token_per_sent_item);
	RUN_TEST(test_queue_set_select_times_out_and_relinks_task);
	RUN_TEST(test_queue_set_from_isr_reports_the_member);
	RUN_TEST(test_queue_set_rejects_invalid_members);
	RUN_TEST(test_queue_set_refuses_to_delete_live_participants);
	RUN_TEST(test_queue_set_accepts_a_semaphore_member);
	RUN_TEST(test_queue_set_static_create_uses_caller_memory);
	RUN_TEST(test_recursive_mutex_no_spurious_wakeup_while_held);
	RUN_TEST(test_queue_preemption_and_overwrite_queue_set);
	return UnityEnd();
}
