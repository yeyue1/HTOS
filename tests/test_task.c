/**
 * @file test_task.c
 * @brief 任务管理、延时、通知与栈哨兵的主机侧 Unity 单元测试。
 *
 * 本套件在 PC 上由 MinGW gcc 编译运行，不依赖目标板：文件里用桩函数接管
 * htPortMalloc()/htPortFree()/htEnterCritical()/htExitCritical()/
 * htSchedulerNeedsSwitch()/vPortStartFirstTask()/htStartScheduler()/htHostTestYieldHook()，
 * 并直接调用内核内部的 prvInitialiseTaskLists() 与 htTaskCheckDelayedTasks()，
 * 使 kernel/httask.c 可以脱离启动流程与真实节拍中断被逐条验证。
 *
 * 覆盖范围：任务句柄/名字/优先级查询与改优先级后的就绪桶迁移、挂起与恢复、
 * FromISR 恢复的返回值、htTaskDelayUntil() 的绝对截止时间与错过截止时间、
 * 节拍回绕后延时链表迁移、任务通知（置位/覆盖/不覆盖/自增/计数式取走/FromISR）、
 * 静态创建不产生堆操作、删除任务精确释放栈与 TCB，以及在
 * configCHECK_FOR_STACK_OVERFLOW > 0 时的栈哨兵用例。
 *
 * @note 断言统一用十进制字面量比较；每个用例开头先调用 reset_test_state() 复位
 *       内核全局量与全部链表，否则上一个用例的残留会影响就绪桶计数。
 * @note Unity 的 TEST_ASSERT_EQUAL 宏展开后实参可能出现多次，因此断言里只写纯
 *       函数调用（如 htTaskPriorityGet(NULL)），不要写入自增、赋值等带副作用的表达式。
 */

#include "unity.h"

#include <stdlib.h>
#include <string.h>

#include "core_cm3.h" /* 主机侧桩：提供 DWT->CYCCNT，用来构造可控的时间差 */
#include "htlist.h"
#include "htschedpolicy.h"
#include "httask.h"
#include "htutils.h"
#include "htkernel_internal.h"

/* ------------------------------------------------------------------ */
/* 1. 测试状态与计数器                                                */
/* ------------------------------------------------------------------ */

/** 临界区嵌套计数：内核每次进出临界区都必须配对，用例末尾断言它回到 0。 */
static UBaseType_t critical_nesting;
/** ISR 临界区嵌套计数，验证 FromISR 接口配对保护。 */
static UBaseType_t isr_critical_nesting;
/** 可控的调度器启动标志，便于验证运行期建任务抢占。 */
static BaseType_t scheduler_started;
/** 调度器挂起嵌套计数，用于验证 htTaskList() 走挂起调度器而非全程关中断。 */
static UBaseType_t scheduler_suspend_nesting;
/** 让出钩子被调用的次数：断言某条路径 "没有触发调度" 时就靠它为 0。 */
static UBaseType_t yield_count;
/** 累计释放次数；删除类用例用它精确断言释放了几块内存。 */
static UBaseType_t free_count;
/** 累计分配次数；静态创建用例断言它保持 0。 */
static UBaseType_t allocation_count;
/** 让出钩子每次推进的节拍数，非 0 时只生效一次，用来模拟有限等待到期。 */
static TickType_t ticks_advanced_on_yield;

/* 下面 3 个记录器只在 configCHECK_FOR_STACK_OVERFLOW > 0 时才存在。 */
#if (configCHECK_FOR_STACK_OVERFLOW > 0)
/**
 * @brief 强符号实现：接管内核的弱符号默认停机版本，把栈溢出变成可断言的记录。
 *
 * @param[in] xTask 触发溢出的任务句柄，原样记入 overflowing_task。
 * @param[in] pcTaskName 任务名，逐字符拷贝进 overflowing_name 并以 '\0' 结尾。
 * @note 不用 strncpy()：任务名的定长字段未必以 '\0' 结尾，逐字符拷贝并强制截断
 *       更安全，也避免在主机侧读到缓冲区之外。
 * @warning 主机侧没有真实栈，本钩子只做记录，不会像目标板那样停机。
 */
/** 钩子被调用的次数；每个栈哨兵用例先调用 reset_overflow_recorder() 清零。 */
static UBaseType_t overflow_calls;
/** 最近一次溢出报告的 TCB 指针，用来断言报告的是哪个任务。 */
static TaskHandle_t overflowing_task;
/** 最近一次溢出报告的任务名副本，按 configMAX_TASK_NAME_LEN 截断。 */
static char overflowing_name[configMAX_TASK_NAME_LEN];

void htStackOverflowHook(TaskHandle_t xTask, const char *pcTaskName)
{
	size_t length = 0U;
	overflow_calls++;
	overflowing_task = xTask;
	while (length < (sizeof(overflowing_name) - 1U) && pcTaskName[length] != '\0') {
		overflowing_name[length] = pcTaskName[length];
		length++;
	}
	overflowing_name[length] = '\0';
}
#endif

/* ------------------------------------------------------------------ */
/* 2. 内核依赖桩函数                                                  */
/* ------------------------------------------------------------------ */

/**
 * @brief 替换内核的动态分配：统计次数后转发给 malloc()。
 *
 * @param[in] size 请求的字节数。
 * @return 分配到的内存，失败时返回 NULL。
 */
void *htPortMalloc(size_t size)
{
	allocation_count++;
	return malloc(size);
}

/**
 * @brief 替换内核的释放函数，只对非空指针计数后转发给 free()。
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
 * @brief 模拟进入临界区，只把 critical_nesting 加一。
 *
 * @note 主机侧没有 CPSID I，关中断的效果完全由计数器体现：内核若少退出一次，
 *       用例末尾的 TEST_ASSERT_EQUAL(0, critical_nesting) 就会失败。
 */
void htEnterCritical(void)
{
	critical_nesting++;
}

/**
 * @brief 模拟退出临界区，把 critical_nesting 减一并在 0 处钳位。
 *
 * @warning 多余的开中断调用会被静默吞掉；真正的泄漏（少一次退出）由用例末尾的
 *           计数断言反向捕获。
 */
void htExitCritical(void)
{
	if (critical_nesting > 0U) {
		critical_nesting--;
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

BaseType_t htSchedulerNeedsSwitch(void)
{
	return htFALSE;
}

BaseType_t htSchedulerIsStarted(void)
{
	return scheduler_started;
}

void htSuspendScheduler(void)
{
	scheduler_suspend_nesting++;
}

void htResumeScheduler(void)
{
	if (scheduler_suspend_nesting > 0U) {
		scheduler_suspend_nesting--;
	}
}

/** @brief 空实现：主机侧没有启动汇编，仅用于满足链接。 */
void vPortStartFirstTask(void) {}

/** @brief 空实现：主机侧不真正启动调度器，仅用于满足链接。 */
void htStartScheduler(void) {}

/**
 * @brief 替换内核的让出钩子：计数，并按 ticks_advanced_on_yield 推进节拍。
 *
 * @note 只有 ticks_advanced_on_yield 非 0 时才推进 xTickCount、处理延时链表，
 *       并把该值清零（只生效一次），从而区分 "等到超时" 与 "立即返回" 两种结果。
 * @note 本桩不切换任务，只让链表与状态的变化可以被断言。
 */
void htHostTestYieldHook(void)
{
	yield_count++;
	if (ticks_advanced_on_yield > 0U) {
		xTickCount += ticks_advanced_on_yield;
		ticks_advanced_on_yield = 0U;
		htTaskCheckDelayedTasks();
	}
}

/* ------------------------------------------------------------------ */
/* 3. 测试辅助函数                                                    */
/* ------------------------------------------------------------------ */

/**
 * @brief 把内核全局量、链表与全部计数器恢复成干净状态。
 *
 * @note 每个用例开头都必须调用：重新初始化任务链表，清空当前任务、节拍、临界区
 *       计数与让出计数，并清零堆统计，防止用例之间互相污染。
 */
static void reset_test_state(void)
{
	prvInitialiseTaskLists();
	pxCurrentTCB = NULL;
	uxCurrentNumberOfTasks = 0U;
	xTickCount = 0U;
	critical_nesting = 0U;
	isr_critical_nesting = 0U;
	scheduler_started = htFALSE;
	scheduler_suspend_nesting = 0U;
	yield_count = 0U;
	free_count = 0U;
	allocation_count = 0U;
	ticks_advanced_on_yield = 0U;
}

/**
 * @brief 空任务体，只用来给 htTaskCreateStatic() 提供一个合法入口地址。
 *
 * @param[in] parameters 未使用的任务参数。
 */
static void dummy_task(void *parameters)
{
	(void)parameters;
}

/**
 * @brief 在调用者提供的 TCB 上构造一个 "已就绪" 的任务并挂进链表。
 *
 * @param[in,out] task 调用者提供的 TCB 存储，先整体清零再填充。
 * @param[in] name 任务名，按 configMAX_TASK_NAME_LEN 截断后逐字符拷贝。
 * @param[in] priority 优先级，同时写入 uxPriority 与 uxBasePriority。
 * @note 三个链表项都必须先 htListItemInit() 再 htListSetItemOwner()，
 *       否则 htListRemove() 会解引用未初始化的 pxContainer。
 * @note 任务会同时挂进 pxReadyTasksLists[priority] 和 pxAllocatedTasksList，
 *       后者用来验证删除时确实摘链。
 */
static void initialise_task(htTCB_t *task, const char *name, UBaseType_t priority)
{
	memset(task, 0, sizeof(*task));
	htListItemInit(&(task->xStateListItem));
	htListItemInit(&(task->xEventListItem));
	htListItemInit(&(task->xAllListItem));
	htListSetItemOwner(&(task->xStateListItem), task);
	htListSetItemOwner(&(task->xEventListItem), task);
	htListSetItemOwner(&(task->xAllListItem), task);
	task->uxPriority = priority;
	task->uxBasePriority = priority;
	task->uxTaskState = HT_TASK_READY;
	size_t name_length = 0U;
	while (name_length < configMAX_TASK_NAME_LEN - 1U && name[name_length] != '\0') {
		name_length++;
	}
	memcpy(task->pcTaskName, name, name_length);
	task->pcTaskName[name_length] = '\0';
	htListInsertEnd(&(pxReadyTasksLists[priority]), &(task->xStateListItem));
	htSchedRecordReadyPriority(priority);
	htListInsertEnd(&pxAllocatedTasksList, &(task->xAllListItem));
}

/* ------------------------------------------------------------------ */
/* 4. 测试用例                                                        */
/* ------------------------------------------------------------------ */

/**
 * @brief 查询接口返回当前任务/指定任务的信息；改优先级后状态链表项随之迁移。
 *
 * 断言点：htTaskPriorityGet(NULL) 取到当前任务的优先级 3；
 * htTaskGetCurrentTaskHandle() 与 htTaskGetName() 返回正确；改优先级为 2 后
 * uxPriority 与 uxBasePriority 同步更新，xStateListItem 从桶 1 迁到桶 2 且旧桶清空。
 * @note 该路径不应让出 CPU，也不应留下未退出的临界区，故同时断言 yield_count
 *       与 critical_nesting 为 0。
 */
static void test_task_query_and_priority_change_move_ready_item(void)
{
	reset_test_state();
	htTCB_t current;
	htTCB_t worker;
	initialise_task(&current, "current", 3U);
	initialise_task(&worker, "worker", 1U);
	pxCurrentTCB = &current;

	TEST_ASSERT_EQUAL(3, htTaskPriorityGet(NULL));
	TEST_ASSERT_EQUAL(1, htTaskPriorityGet(&worker));
	TEST_ASSERT(htTaskGetCurrentTaskHandle() == &current);
	TEST_ASSERT(strcmp("worker", htTaskGetName(&worker)) == 0);
	TEST_ASSERT_EQUAL(HT_TASK_READY, htTaskGetState(&worker));

	htTaskPrioritySet(&worker, 2U);
	TEST_ASSERT_EQUAL(2, worker.uxPriority);
	TEST_ASSERT_EQUAL(2, worker.uxBasePriority);
	TEST_ASSERT(worker.xStateListItem.pxContainer == &pxReadyTasksLists[2]);
	TEST_ASSERT_EQUAL(0, htListGetCurrentNumberOfItems(&pxReadyTasksLists[1]));
	TEST_ASSERT_EQUAL(0, yield_count);
	TEST_ASSERT_EQUAL(0, critical_nesting);
}

/**
 * @brief 从 ISR 恢复一个更高优先级任务时，必须把 "需要切换" 报告给调用者。
 *
 * 断言点：当前任务优先级 1，被挂起的任务优先级 3；htTaskResumeFromISR() 返回
 * htTRUE，任务回到 HT_TASK_READY 并挂进 pxReadyTasksLists[3]。
 * @note FromISR 版本自己不切换上下文，只通过返回值通知调用者；
 *       所以这里同时断言 yield_count 为 0。
 */
static void test_resume_from_isr_reports_higher_priority_task(void)
{
	reset_test_state();
	htTCB_t current;
	htTCB_t worker;
	initialise_task(&current, "current", 1U);
	initialise_task(&worker, "worker", 3U);
	pxCurrentTCB = &current;

	htTaskSuspend(&worker);
	TEST_ASSERT_EQUAL(htTRUE, htTaskResumeFromISR(&worker));
	TEST_ASSERT_EQUAL(HT_TASK_READY, worker.uxTaskState);
	TEST_ASSERT(worker.xStateListItem.pxContainer == &pxReadyTasksLists[3]);
	TEST_ASSERT_EQUAL(0, yield_count);
}

/**
 * @brief 挂起非当前任务时把它从就绪桶摘除，恢复时再挂回原优先级桶。
 *
 * 断言点：挂起后状态为 HT_TASK_SUSPENDED 且桶 1 为空；恢复后状态为
 * HT_TASK_READY 且 xStateListItem 回到 pxReadyTasksLists[1]；不让出、临界区归零。
 */
static void test_suspend_and_resume_non_current_task(void)
{
	reset_test_state();
	htTCB_t current;
	htTCB_t worker;
	initialise_task(&current, "current", 3U);
	initialise_task(&worker, "worker", 1U);
	pxCurrentTCB = &current;

	htTaskSuspend(&worker);
	TEST_ASSERT_EQUAL(HT_TASK_SUSPENDED, htTaskGetState(&worker));
	TEST_ASSERT_EQUAL(0, htListGetCurrentNumberOfItems(&pxReadyTasksLists[1]));
	htTaskResume(&worker);
	TEST_ASSERT_EQUAL(HT_TASK_READY, htTaskGetState(&worker));
	TEST_ASSERT(worker.xStateListItem.pxContainer == &pxReadyTasksLists[1]);
	TEST_ASSERT_EQUAL(0, yield_count);
	TEST_ASSERT_EQUAL(0, critical_nesting);
}

/**
 * @brief htTaskDelayUntil() 按绝对截止时间阻塞，到期后由延时链表唤醒。
 *
 * 断言点：previous_wake 为 10、周期 5 时返回 htPASS 并把它推进到 15；任务进入
 * HT_TASK_BLOCKED，xStateListItem 的 uxItemValue 正是唤醒节拍 15，且发生一次让出。
 * 把 xTickCount 推到 15 后调用 htTaskCheckDelayedTasks()，任务回到 READY 并挂回桶 1。
 * @note uxItemValue 就是唤醒节拍，这是判断 "tick 超时真的被实现" 的直接观测点。
 */
static void test_delay_until_blocks_until_absolute_deadline(void)
{
	reset_test_state();
	htTCB_t task;
	TickType_t previous_wake = 10U;
	initialise_task(&task, "periodic", 1U);
	pxCurrentTCB = &task;
	xTickCount = 10U;

	TEST_ASSERT_EQUAL(htPASS, htTaskDelayUntil(&previous_wake, 5U));
	TEST_ASSERT_EQUAL(15, previous_wake);
	TEST_ASSERT_EQUAL(HT_TASK_BLOCKED, task.uxTaskState);
	TEST_ASSERT_EQUAL(15, htListGetItemValue(&task.xStateListItem));
	TEST_ASSERT_EQUAL(1, yield_count);

	xTickCount = 15U;
	htTaskCheckDelayedTasks();
	TEST_ASSERT_EQUAL(HT_TASK_READY, task.uxTaskState);
	TEST_ASSERT(task.xStateListItem.pxContainer == &pxReadyTasksLists[1]);
}

/**
 * @brief 当前节拍已越过下一个截止时间时，htTaskDelayUntil() 必须报告错过并立即返回。
 *
 * 断言点：xTickCount 为 20 而目标截止为 15，返回 htFAIL；previous_wake 仍被推进到
 * 15（周期基准照常前移）；任务保持 HT_TASK_READY 且不让出。
 * @note 这是周期任务防止 "补跑" 的语义：错过就跳过，不累计欠账。
 */
static void test_delay_until_reports_missed_deadline(void)
{
	reset_test_state();
	htTCB_t task;
	TickType_t previous_wake = 10U;
	initialise_task(&task, "late", 1U);
	pxCurrentTCB = &task;
	xTickCount = 20U;

	TEST_ASSERT_EQUAL(htFAIL, htTaskDelayUntil(&previous_wake, 5U));
	TEST_ASSERT_EQUAL(15, previous_wake);
	TEST_ASSERT_EQUAL(HT_TASK_READY, task.uxTaskState);
	TEST_ASSERT_EQUAL(0, yield_count);
}

/**
 * @brief 节拍计数回绕时，延时任务迁移到溢出延时链表并在到期后正常唤醒。
 *
 * 断言点：xTickCount 为 UINT32_MAX-1 时延时 3 个节拍，任务挂在
 * pxOverflowDelayedTaskList 上；连续两次 htTaskTickInc() 仍是 BLOCKED，
 * 第三次到期后回到 READY 并挂进 pxReadyTasksLists[1]。
 */
static void test_tick_wrap_delays_task_until_overflow_list_expires(void)
{
	reset_test_state();
	htTCB_t task;
	initialise_task(&task, "wrap", 1U);
	pxCurrentTCB = &task;
	xTickCount = UINT32_MAX - 1U;

	htTaskDelay(3U);
	TEST_ASSERT_EQUAL(HT_TASK_BLOCKED, task.uxTaskState);
	TEST_ASSERT(task.xStateListItem.pxContainer == pxOverflowDelayedTaskList);
	htTaskTickInc();
	htTaskTickInc();
	TEST_ASSERT_EQUAL(HT_TASK_BLOCKED, task.uxTaskState);
	htTaskTickInc();
	TEST_ASSERT_EQUAL(HT_TASK_READY, task.uxTaskState);
	TEST_ASSERT(task.xStateListItem.pxContainer == &pxReadyTasksLists[1]);
}

/**
 * @brief htTaskNotifyWait() 返回挂起值，并按清位掩码清掉指定的位。
 *
 * 断言点：两次 SET_BITS 得到 0x05|0x02 = 0x07；等待返回 7，清位掩码 0x04 生效后
 * ulNotifiedValue 只剩 0x03。
 * @note Unity 的 TEST_ASSERT_EQUAL 宏展开后实参可能出现多次，
 *       因此断言里只写 ulNotifiedValue 这类纯读取，不要写带副作用的表达式。
 */
static void test_task_notify_wait_returns_pending_value_and_clears_requested_bits(void)
{
	reset_test_state();
	htTCB_t current;
	uint32_t notified_value = 0U;

	initialise_task(&current, "notify", 2U);
	pxCurrentTCB = &current;

	TEST_ASSERT_EQUAL(htPASS, htTaskNotify(&current, 0x05U, HT_NOTIFY_SET_BITS));
	TEST_ASSERT_EQUAL(htPASS, htTaskNotify(&current, 0x02U, HT_NOTIFY_SET_BITS));
	TEST_ASSERT_EQUAL(htPASS, htTaskNotifyWait(0x00U, 0x04U, &notified_value, 0U));
	TEST_ASSERT_EQUAL(7, (int)notified_value);
	TEST_ASSERT_EQUAL(3, (int)current.ulNotifiedValue);
	TEST_ASSERT_EQUAL(0, yield_count);
}

/**
 * @brief 有限等待通知超时后，任务必须从事件链表摘除并重新变为就绪。
 *
 * 断言点：等待 3 个节拍返回 htFAIL，xTickCount 从 10 推进到 13；任务回到
 * HT_TASK_READY，xEventListItem 已摘链（pxContainer 为 NULL），
 * xStateListItem 重新挂进 pxReadyTasksLists[1]，出参被清成 0。
 */
static void test_task_notify_wait_times_out_and_unlinks_blocked_task(void)
{
	reset_test_state();
	htTCB_t current;
	uint32_t notified_value = 0xA5A5A5A5U;

	initialise_task(&current, "waiter", 1U);
	pxCurrentTCB = &current;
	xTickCount = 10U;
	ticks_advanced_on_yield = 3U;

	TEST_ASSERT_EQUAL(htFAIL, htTaskNotifyWait(0x00U, 0x00U, &notified_value, 3U));
	TEST_ASSERT_EQUAL(13U, xTickCount);
	TEST_ASSERT_EQUAL(HT_TASK_READY, current.uxTaskState);
	TEST_ASSERT_NULL(current.xEventListItem.pxContainer);
	TEST_ASSERT(current.xStateListItem.pxContainer == &pxReadyTasksLists[1]);
	TEST_ASSERT_EQUAL(0, (int)notified_value);
}

/**
 * @brief 通知的目标就是当前任务本身时，FromISR 版本不应报告需要切换。
 *
 * 断言点：htTaskNotifyFromISR() 返回 htPASS，*pxHigherPriorityTaskWoken 仍为
 * htFALSE；随后 htTaskNotifyWait() 能取到 0x08。
 */
static void test_task_notify_from_isr_sets_value_without_waking_higher_priority_task(void)
{
	reset_test_state();
	htTCB_t current;
	uint32_t notified_value = 0U;
	BaseType_t higher_priority_woken = htFALSE;

	initialise_task(&current, "current", 1U);
	pxCurrentTCB = &current;

	TEST_ASSERT_EQUAL(htPASS, htTaskNotifyFromISR(&current, 0x08U, HT_NOTIFY_SET_BITS, &higher_priority_woken));
	TEST_ASSERT_EQUAL(htFALSE, higher_priority_woken);
	TEST_ASSERT_EQUAL(htPASS, htTaskNotifyWait(0x00U, 0xFFFFFFFFU, &notified_value, 0U));
	TEST_ASSERT_EQUAL(8, (int)notified_value);
}

/**
 * @brief 计数式取走：不清零时逐个递减，清零时一次取空。
 *
 * 断言点：Give 两次后计数为 2；htTaskNotifyTake(htFALSE, 0) 返回 2 并剩 1；
 * htTaskNotifyTake(htTRUE, 0) 返回 1 并把计数清零；再取一次返回 0。
 * @note 第二个参数是超时；此处传 0，确保用例不会真的阻塞。
 */
static void test_task_notify_counting_take_preserves_or_clears_count(void)
{
	reset_test_state();
	htTCB_t current;
	initialise_task(&current, "counter", 2U);
	pxCurrentTCB = &current;

	TEST_ASSERT_EQUAL(htPASS, htTaskNotifyGive(&current));
	TEST_ASSERT_EQUAL(htPASS, htTaskNotifyGive(&current));
	TEST_ASSERT_EQUAL(2, (int)htTaskNotifyTake(htFALSE, 0U));
	TEST_ASSERT_EQUAL(1, (int)current.ulNotifiedValue);
	TEST_ASSERT_EQUAL(1, (int)htTaskNotifyTake(htTRUE, 0U));
	TEST_ASSERT_EQUAL(0, (int)current.ulNotifiedValue);
	TEST_ASSERT_EQUAL(0, (int)htTaskNotifyTake(htTRUE, 0U));
}

/**
 * @brief SET_VALUE_WITHOUT_OVERWRITE 在已有未取走的值时必须失败且不改写。
 *
 * 断言点：第一次写入 0x11 成功；第二次写入 0x22 返回 htFAIL 且挂起值仍是 0x11；
 * 取走后得到 0x11。
 */
static void test_task_notify_without_overwrite_rejects_pending_value(void)
{
	reset_test_state();
	htTCB_t current;
	uint32_t notified_value = 0U;
	initialise_task(&current, "overwrite", 2U);
	pxCurrentTCB = &current;

	TEST_ASSERT_EQUAL(htPASS, htTaskNotify(&current, 0x11U, HT_NOTIFY_SET_VALUE_WITHOUT_OVERWRITE));
	TEST_ASSERT_EQUAL(htFAIL, htTaskNotify(&current, 0x22U, HT_NOTIFY_SET_VALUE_WITHOUT_OVERWRITE));
	TEST_ASSERT_EQUAL(htPASS, htTaskNotifyWait(0U, UINT32_MAX, &notified_value, 0U));
	TEST_ASSERT_EQUAL(0x11, (int)notified_value);
}

/**
 * @brief 从 ISR 给出通知时，阻塞等待的更高优先级任务被唤醒并挂回就绪桶。
 *
 * 断言点：手工把优先级 3 的 waiter 从就绪桶摘除、置为 HT_TASK_BLOCKED 且
 * uxNotificationStatus 为 HT_NOTIFY_WAITING；htTaskNotifyGiveFromISR() 返回 htPASS，
 * *pxHigherPriorityTaskWoken 为 htTRUE，waiter 回到 READY 并挂进 pxReadyTasksLists[3]。
 * @note 这里手工构造阻塞态而不调用 htTaskNotifyWait()，是为了让 "唤醒" 与 "进入等待"
 *       两个语义可以被分别断言。
 */
static void test_task_notify_from_isr_wakes_higher_priority_waiter(void)
{
	reset_test_state();
	htTCB_t current;
	htTCB_t waiter;
	BaseType_t higher_priority_woken = htFALSE;
	initialise_task(&current, "current", 1U);
	initialise_task(&waiter, "waiter", 3U);
	pxCurrentTCB = &current;

	htListRemove(&(waiter.xStateListItem));
	waiter.uxTaskState = HT_TASK_BLOCKED;
	waiter.uxNotificationStatus = HT_NOTIFY_WAITING;

	TEST_ASSERT_EQUAL(htPASS, htTaskNotifyGiveFromISR(&waiter, &higher_priority_woken));
	TEST_ASSERT_EQUAL(htTRUE, higher_priority_woken);
	TEST_ASSERT_EQUAL(HT_TASK_READY, waiter.uxTaskState);
	TEST_ASSERT(waiter.xStateListItem.pxContainer == &pxReadyTasksLists[3]);
}

/**
 * @brief 静态创建使用调用者提供的 TCB 与栈，全程不触碰堆；删除后也不释放。
 *
 * 断言点：句柄等于传入的 htTCB_t 缓冲，allocation_count 保持 0，
 * ucStaticallyAllocated 置位，pxStack 指向调用者栈，uxCurrentNumberOfTasks 递增；
 * 删除后 free_count 仍为 0，状态为 HT_TASK_SUSPENDED 且两个链表项都已摘链。
 * @note 最后一行验证栈深度小于 configMINIMAL_STACK_SIZE 时创建返回 NULL。
 */
static void test_static_task_uses_caller_memory_and_is_not_freed(void)
{
	reset_test_state();
	htTCB_t current;
	htTCB_t task_buffer;
	StackType_t stack_buffer[configMINIMAL_STACK_SIZE];
	initialise_task(&current, "current", 3U);
	pxCurrentTCB = &current;
	uxCurrentNumberOfTasks = 1U;

	TaskHandle_t handle =
			htTaskCreateStatic(dummy_task, "static", configMINIMAL_STACK_SIZE, NULL, 1U, stack_buffer, &task_buffer);
	TEST_ASSERT(handle == &task_buffer);
	TEST_ASSERT_EQUAL(0, allocation_count);
	TEST_ASSERT_EQUAL(1, task_buffer.ucStaticallyAllocated);
	TEST_ASSERT(task_buffer.pxStack == stack_buffer);
	TEST_ASSERT_EQUAL(2, uxCurrentNumberOfTasks);

	htTaskDelete(handle);
	TEST_ASSERT_EQUAL(0, free_count);
	TEST_ASSERT_EQUAL(1, uxCurrentNumberOfTasks);
	TEST_ASSERT_EQUAL(HT_TASK_SUSPENDED, task_buffer.uxTaskState);
	TEST_ASSERT_NULL(task_buffer.xStateListItem.pxContainer);
	TEST_ASSERT_NULL(task_buffer.xAllListItem.pxContainer);
	TEST_ASSERT_NULL(htTaskCreateStatic(
			dummy_task, "small", configMINIMAL_STACK_SIZE - 1U, NULL, 1U, stack_buffer, &task_buffer));
}

/**
 * @brief 删除动态创建的任务时，精确释放 "栈 + TCB" 两块内存并摘掉全部链表项。
 *
 * 断言点：free_count 恰好为 2；uxCurrentNumberOfTasks 递减；
 * pxReadyTasksLists[1] 清空，pxAllocatedTasksList 只剩当前任务。
 */
static void test_delete_non_current_task_releases_stack_and_tcb(void)
{
	reset_test_state();
	htTCB_t current;
	initialise_task(&current, "current", 3U);
	pxCurrentTCB = &current;

	htTCB_t *worker = malloc(sizeof(*worker));
	TEST_ASSERT_NOT_NULL(worker);
	initialise_task(worker, "worker", 1U);
	worker->pxStack = malloc(32U * sizeof(StackType_t));
	TEST_ASSERT_NOT_NULL(worker->pxStack);
	uxCurrentNumberOfTasks = 2U;

	htTaskDelete(worker);
	TEST_ASSERT_EQUAL(2, free_count);
	TEST_ASSERT_EQUAL(1, uxCurrentNumberOfTasks);
	TEST_ASSERT_EQUAL(0, htListGetCurrentNumberOfItems(&pxReadyTasksLists[1]));
	TEST_ASSERT_EQUAL(1, htListGetCurrentNumberOfItems(&pxAllocatedTasksList));
}

/* ------------------------------------------------------------------ */
/* 5. 栈溢出检查用例                                                  */
/* ------------------------------------------------------------------ */

/* 本节 3 个用例都依赖上面接管 htStackOverflowHook() 得到的记录器。 */
#if (configCHECK_FOR_STACK_OVERFLOW > 0)
/**
 * @brief 清空栈溢出钩子的调用次数、任务句柄与任务名记录。
 *
 * @note 每个栈哨兵用例检查前都要调用，否则前面用例的记录会让计数断言失效。
 */
static void reset_overflow_recorder(void)
{
	overflow_calls = 0U;
	overflowing_task = NULL;
	overflowing_name[0] = '\0';
}

/**
 * @brief 栈底 4 个字仍是填充值时，哨兵检查不得报告溢出。
 *
 * 断言点：填充值完好的 8 字栈通过 htTaskCheckStackSentinel() 后，
 * overflow_calls 为 0 且 overflowing_task 仍为 NULL。
 */
static void test_stack_sentinel_accepts_an_intact_stack(void)
{
	htTCB_t task;
	StackType_t stack[8];
	memset(&task, 0, sizeof(task));
	for (UBaseType_t index = 0U; index < 8U; index++) {
		stack[index] = htTASK_STACK_FILL_VALUE;
	}
	task.pxStack = stack;
	task.uxStackDepth = 8U;
	reset_overflow_recorder();

	htTaskCheckStackSentinel(&task);

	TEST_ASSERT_EQUAL(0, (int)overflow_calls);
	TEST_ASSERT_NULL(overflowing_task);
}

/**
 * @brief 栈底一个字被改写时，哨兵检查必须报出正确的任务与任务名。
 *
 * 断言点：stack[0] 被写成 0xDEADBEEF 后，钩子被调用 1 次，
 * overflowing_task 指向该 TCB，overflowing_name 与构造的任务名一致。
 */
static void test_stack_sentinel_reports_a_corrupted_stack(void)
{
	htTCB_t task;
	StackType_t stack[8];
	memset(&task, 0, sizeof(task));
	for (UBaseType_t index = 0U; index < 8U; index++) {
		stack[index] = htTASK_STACK_FILL_VALUE;
	}
	memcpy(task.pcTaskName, "overflow", 9U);
	task.pxStack = stack;
	task.uxStackDepth = 8U;
	stack[0] = 0xDEADBEEFU; /* 栈底被写掉一个字 */
	reset_overflow_recorder();

	htTaskCheckStackSentinel(&task);

	TEST_ASSERT_EQUAL(1, (int)overflow_calls);
	TEST_ASSERT(overflowing_task == &task);
	TEST_ASSERT(strcmp(overflowing_name, "overflow") == 0);
}

/**
 * @brief 缺少可用栈信息的任务必须被跳过，避免哨兵检查自己踩空指针。
 *
 * 断言点：pxStack 为 NULL、uxStackDepth 小于 4 这两种情况都不报溢出；
 * 第三个调用传 NULL 表示 "当前任务"（pxCurrentTCB），此时栈已被破坏，故恰好报 1 次。
 * @note 该用例把 "跳过" 与 "报告" 两种分支放在一起，断言的是总数 1 而不是 3。
 */
static void test_stack_sentinel_skips_tasks_without_usable_storage(void)
{
	htTCB_t task;
	StackType_t stack[8];
	for (UBaseType_t index = 0U; index < 8U; index++) {
		stack[index] = htTASK_STACK_FILL_VALUE;
	}
	stack[0] = 0U;
	reset_overflow_recorder();

	memset(&task, 0, sizeof(task));
	htTaskCheckStackSentinel(&task); /* pxStack == NULL */
	task.pxStack = stack;
	task.uxStackDepth = 3U; /* 太短，不能检查 */
	htTaskCheckStackSentinel(&task);
	pxCurrentTCB = &task; /* NULL 表示当前任务 */
	task.uxStackDepth = 8U;
	htTaskCheckStackSentinel(NULL);

	TEST_ASSERT_EQUAL(1, (int)overflow_calls);
}
#endif

/* ------------------------------------------------------------------ */
/* 6. 通知动作语义                                                    */
/* ------------------------------------------------------------------ */

/**
 * @brief 四种通知动作的完整语义：置位、自增、覆盖写、不覆盖写，以及非法动作拒绝。
 *
 * 断言点：SET_BITS 累加得到 0x05，INCREMENT 变 0x06，SET_VALUE_WITH_OVERWRITE
 * 覆盖为 0x80；随后 WITHOUT_OVERWRITE 返回 htFAIL 且值不变；非法 action 被拒；
 * INCREMENT 在 UINT32_MAX 处饱和而不是回绕为 0。
 * @note 该用例不涉及阻塞，主要用于固定通知寄存器的位运算与错误码语义。
 */
static void test_notify_actions_set_bits_increment_and_overwrite(void)
{
	reset_test_state();
	htTCB_t current;
	htTCB_t worker;
	BaseType_t rc;

	initialise_task(&current, "current", 1U);
	initialise_task(&worker, "worker", 2U);
	pxCurrentTCB = &current;

	rc = htTaskNotify(&worker, 0x01U, HT_NOTIFY_SET_BITS);
	TEST_ASSERT_EQUAL(htPASS, rc);
	rc = htTaskNotify(&worker, 0x04U, HT_NOTIFY_SET_BITS);
	TEST_ASSERT_EQUAL(htPASS, rc);
	TEST_ASSERT_EQUAL(0x05, worker.ulNotifiedValue);

	rc = htTaskNotify(&worker, 0U, HT_NOTIFY_INCREMENT);
	TEST_ASSERT_EQUAL(htPASS, rc);
	TEST_ASSERT_EQUAL(0x06, worker.ulNotifiedValue);

	rc = htTaskNotify(&worker, 0x80U, HT_NOTIFY_SET_VALUE_WITH_OVERWRITE);
	TEST_ASSERT_EQUAL(htPASS, rc);
	TEST_ASSERT_EQUAL(0x80, worker.ulNotifiedValue);

	/* 已有未取走的值时，WITHOUT_OVERWRITE 必须被拒绝且不改写挂起值 */
	rc = htTaskNotify(&worker, 0x40U, HT_NOTIFY_SET_VALUE_WITHOUT_OVERWRITE);
	TEST_ASSERT_EQUAL(htFAIL, rc);
	TEST_ASSERT_EQUAL(0x80, worker.ulNotifiedValue);

	/* 非法 action 被拒绝 */
	rc = htTaskNotify(&worker, 0x40U, (htNotifyAction_t)(HT_NOTIFY_SET_VALUE_WITHOUT_OVERWRITE + 1));
	TEST_ASSERT_EQUAL(htFAIL, rc);
	TEST_ASSERT_EQUAL(0x80, worker.ulNotifiedValue);

	/* INCREMENT 在 UINT32_MAX 处饱和 */
	worker.ulNotifiedValue = UINT32_MAX;
	rc = htTaskNotify(&worker, 0U, HT_NOTIFY_INCREMENT);
	TEST_ASSERT_EQUAL(htPASS, rc);
	TEST_ASSERT_EQUAL(UINT32_MAX, worker.ulNotifiedValue);

	TEST_ASSERT_EQUAL(0, critical_nesting);
}

/* ------------------------------------------------------------------ */
/* 7. 运行时间统计                                                     */
/* ------------------------------------------------------------------ */

/**
 * @brief 一次样本都没有时，三个统计接口必须返回 0 而不是 100。
 *
 * 断言意图：0 表示「没有数据」。若在无样本时返回 100% 空闲或 0% 使用率，
 * 上层会把「还没开始统计」误读成真实负载，因此把这个契约锁死。
 * @note 本用例必须排在累积用例前面：它依赖计数仍为 0。
 */
static void test_runtime_stats_report_no_data_as_zero(void)
{
	reset_test_state();
	TEST_ASSERT_EQUAL(0, htGetIdleRunTimePercent());
	TEST_ASSERT_EQUAL(0, htGetCPUUsage());
	TEST_ASSERT_EQUAL(0, htGetRunTimeCounter());
}

/**
 * @brief 两次采样之间只把增量记入统计，且回绕时用无符号减法得到正确差值。
 *
 * 断言意图：主机侧把 CYCCNT 当成普通内存，直接写值就能构造出确定的周期增量，
 * 因而可以脱离真实时钟验证「只有增量被累加」和「计数器回绕不算负数」。
 */
static void test_runtime_stats_accumulate_only_the_delta(void)
{
	uint64_t mark;

	reset_test_state();

	/* 先把基准钉在 0：若此前已有基准，这次采样增量为 0，不会污染结果。 */
	DWT->CYCCNT = 0U;
	htRunTimeSample();
	mark = htGetRunTimeCounter();

	/* 走 5000 个周期后采样，总量应恰好增加 5000。 */
	DWT->CYCCNT = 5000U;
	htRunTimeSample();
	TEST_ASSERT_EQUAL(5000, (int)(uint32_t)(htGetRunTimeCounter() - mark));

	/* 计数器回绕：从 UINT32_MAX 再走 1000 个周期，无符号减法仍得到正的 1000。 */
	DWT->CYCCNT = 0xFFFFFFFFU;
	htRunTimeSample();
	mark = htGetRunTimeCounter();
	DWT->CYCCNT = 999U; /* 0xFFFFFFFF + 1000 回绕后等于 999 */
	htRunTimeSample();
	TEST_ASSERT_EQUAL(1000, (int)(uint32_t)(htGetRunTimeCounter() - mark));
}

/* ------------------------------------------------------------------ */
/* 8. 任务表格式化                                                     */
/* ------------------------------------------------------------------ */

#if (configUSE_STATS_FORMATTING_FUNCTIONS == 1)
/**
 * @brief 任务表为每个任务写一行，内容含名字与状态字母。
 *
 * 断言意图：接口不经过 printf，只能靠输出内容验证；这里同时锁住「两个任务都在表里」
 * 与「整表以换行结尾」，因为上层通常是整块丢给串口。
 */
static void test_task_list_writes_one_line_per_task(void)
{
	htTCB_t alpha;
	htTCB_t beta;
	char buffer[128];
	size_t used;
	size_t length;

	reset_test_state();
	initialise_task(&alpha, "alpha", 2U);
	initialise_task(&beta, "beta", 1U);
	alpha.ullRunTimeCounter = 25U;
	beta.ullRunTimeCounter = 75U;

	used = htTaskList(buffer, sizeof(buffer));
	length = strlen(buffer);
	TEST_ASSERT(used > 0U);
	TEST_ASSERT_EQUAL((int)length, (int)used);
	TEST_ASSERT(strstr(buffer, "alpha") != NULL);
	TEST_ASSERT(strstr(buffer, "beta") != NULL);
	TEST_ASSERT_EQUAL('\n', (int)buffer[length - 1U]);
	TEST_ASSERT_EQUAL(0, critical_nesting);
}

/**
 * @brief 缓冲区不足时按容量截断，且末尾始终留一个结束符。
 *
 * 断言意图：这是无 printf 手写拼接最容易出错的地方——越界一格就会踩坏调用方栈，
 * 因此固定「容量 N 最多写 N-1 个字符 + '\0'」这一条契约。
 * 另外验证空指针与非法的过小容量返回 0 而不是崩溃。
 */
static void test_task_list_truncates_without_overflow(void)
{
	htTCB_t alpha;
	char small[16];
	size_t used;
	size_t index;

	reset_test_state();
	initialise_task(&alpha, "alpha", 2U);

	memset(small, 'x', sizeof(small));
	used = htTaskList(small, sizeof(small));
	TEST_ASSERT_EQUAL((int)(sizeof(small) - 1U), (int)used);
	TEST_ASSERT_EQUAL('\0', (int)small[sizeof(small) - 1U]);
	for (index = 0U; index < used; index++) {
		TEST_ASSERT(small[index] != 'x');
	}
	TEST_ASSERT_EQUAL(0, critical_nesting);

	TEST_ASSERT_EQUAL(0, (int)htTaskList(NULL, 32U));
	TEST_ASSERT_EQUAL(0, (int)htTaskList(small, 1U));
}

/**
 * @brief P0 回归：64 位任务运行周期计数跨越 59.6 s（UINT32_MAX）不回绕，且运行期创建高优任务立即抢占。
 */
static void test_runtime_create_yields_and_64bit_counter_no_wrap(void)
{
	static StackType_t high_stack[configMINIMAL_STACK_SIZE];
	static htTCB_t high_tcb;
	htTCB_t low_tcb;
	char buffer[160];

	reset_test_state();
	initialise_task(&low_tcb, "low", 1U);
	pxCurrentTCB = &low_tcb;
	scheduler_started = htTRUE;
	yield_count = 0U;

	TaskHandle_t high =
			htTaskCreateStatic(dummy_task, "high", configMINIMAL_STACK_SIZE, NULL, 3U, high_stack, &high_tcb);
	TEST_ASSERT_NOT_NULL(high);
	TEST_ASSERT_EQUAL(1U, yield_count);

	/* 累加超过 UINT32_MAX 周期，验证单任务计数不截断。 */
	DWT->CYCCNT = 0U;
	htRunTimeSample();
	DWT->CYCCNT = 0x80000000U;
	htRunTimeSample();
	DWT->CYCCNT = 0U;
	htRunTimeSample();
	DWT->CYCCNT = 0x80000000U;
	htRunTimeSample();
	TEST_ASSERT(low_tcb.ullRunTimeCounter > (uint64_t)UINT32_MAX);

	TEST_ASSERT(htTaskList(buffer, sizeof(buffer)) > 0U);
	TEST_ASSERT(strstr(buffer, "low") != NULL);

	/* 重复删除同一个已摘除的静态任务不得二次递减 uxCurrentNumberOfTasks。 */
	const UBaseType_t uxBeforeDelete = uxCurrentNumberOfTasks;
	htTaskDelete(high);
	TEST_ASSERT_EQUAL((int)(uxBeforeDelete - 1U), (int)uxCurrentNumberOfTasks);
	htTaskDelete(high);
	TEST_ASSERT_EQUAL((int)(uxBeforeDelete - 1U), (int)uxCurrentNumberOfTasks);
	TEST_ASSERT_EQUAL(0U, critical_nesting);
	TEST_ASSERT_EQUAL(0U, isr_critical_nesting);
}
#endif

int main(void)
{
	UnityBegin("test_task.c");
	RUN_TEST(test_task_query_and_priority_change_move_ready_item);
	RUN_TEST(test_suspend_and_resume_non_current_task);
	RUN_TEST(test_resume_from_isr_reports_higher_priority_task);
	RUN_TEST(test_delay_until_blocks_until_absolute_deadline);
	RUN_TEST(test_delay_until_reports_missed_deadline);
	RUN_TEST(test_tick_wrap_delays_task_until_overflow_list_expires);
	RUN_TEST(test_task_notify_wait_returns_pending_value_and_clears_requested_bits);
	RUN_TEST(test_task_notify_wait_times_out_and_unlinks_blocked_task);
	RUN_TEST(test_task_notify_from_isr_sets_value_without_waking_higher_priority_task);
	RUN_TEST(test_task_notify_counting_take_preserves_or_clears_count);
	RUN_TEST(test_task_notify_without_overwrite_rejects_pending_value);
	RUN_TEST(test_task_notify_from_isr_wakes_higher_priority_waiter);
	RUN_TEST(test_static_task_uses_caller_memory_and_is_not_freed);
	RUN_TEST(test_delete_non_current_task_releases_stack_and_tcb);
#if (configCHECK_FOR_STACK_OVERFLOW > 0)
	RUN_TEST(test_stack_sentinel_accepts_an_intact_stack);
	RUN_TEST(test_stack_sentinel_reports_a_corrupted_stack);
	RUN_TEST(test_stack_sentinel_skips_tasks_without_usable_storage);
#endif
	RUN_TEST(test_notify_actions_set_bits_increment_and_overwrite);
	RUN_TEST(test_runtime_stats_report_no_data_as_zero);
	RUN_TEST(test_runtime_stats_accumulate_only_the_delta);
#if (configUSE_STATS_FORMATTING_FUNCTIONS == 1)
	RUN_TEST(test_task_list_writes_one_line_per_task);
	RUN_TEST(test_task_list_truncates_without_overflow);
	RUN_TEST(test_runtime_create_yields_and_64bit_counter_no_wrap);
#endif
	return UnityEnd();
}
