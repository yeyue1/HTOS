/**
 * @file htkernel_internal.h
 * @brief htOS 内核模块间私有声明，不作为对外公共应用 API。
 *
 * 将调度器、任务管理、队列/信号量与调度策略之间共享的内部链表与辅助函数
 * 集中收敛到此头文件，避免把内部实现细节暴露在公开头文件 httask.h /
 * htschedpolicy.h 中。
 */
#ifndef HT_KERNEL_INTERNAL_H
#define HT_KERNEL_INTERNAL_H

#include "httypes.h"
#include "htlist.h"
#include "httask.h"

/** @brief 各优先级的就绪任务链表数组，下标即优先级。 */
extern htList_t pxReadyTasksLists[];
/** @brief 两条延时链表实体，tick 回绕时交替充当当前/溢出链表。 */
extern htList_t xDelayedTaskList1;
extern htList_t xDelayedTaskList2;
/** @brief 当前延时链表指针（按唤醒 tick 升序）。 */
extern htList_t *pxDelayedTaskList;
/** @brief tick 溢出延时链表指针。 */
extern htList_t *pxOverflowDelayedTaskList;
/** @brief 当前延时链表头最早唤醒时刻缓存，供 SysTick O(1) 快速跳过。 */
extern TickType_t xNextTaskUnblockTime;

#if (configUSE_TRACE_FACILITY == 1)
/** @brief 全局已分配任务总表，供调试/统计/策略初始化遍历全部任务。 */
extern htList_t pxAllocatedTasksList;
#endif

/** @brief 初始化所有内核任务链表与 O(1) 就绪位图。 */
void prvInitialiseTaskLists(void);

/** @brief 扫描当前延时链表，唤醒所有已到期任务并刷新 xNextTaskUnblockTime。 */
void htTaskCheckDelayedTasks(void);

/** @brief 判断当前是否需要触发上下文切换（挂起态只登记 xYieldPending）。 */
BaseType_t htSchedulerNeedsSwitch(void);

/** @brief 在 32 位 O(1) 就绪位图中标记指定优先级非空。 */
void htSchedRecordReadyPriority(UBaseType_t uxPriority);

/** @brief 若指定优先级就绪链表为空，清除其在就绪位图中的对应位。 */
void htSchedClearReadyPriorityIfEmpty(UBaseType_t uxPriority);

/** @brief 清空 32 位就绪优先级位图。 */
void htSchedResetReadyBitmap(void);

#endif /* HT_KERNEL_INTERNAL_H */
