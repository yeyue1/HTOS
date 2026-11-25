/**
 * @file htscheduler.h
 * @brief 调度器与临界区接口。
 *
 * 负责就绪表的建立、任务的上下文切换、SysTick 与 PendSV 的配置，
 * 以及贯穿内核的临界区保护。临界区用嵌套计数器实现，可重入。
 */
#ifndef HT_SCHEDULER_H
#define HT_SCHEDULER_H

#include "httypes.h"

/**
 * @brief 初始化调度器。
 *
 * 建立各优先级的就绪表，把调度器状态置为未启动，清空挂起计数与临界区嵌套计数。
 * 由 htOSInit() 在系统启动时调用。
 */
void htSchedulerInit(void);
/**
 * @brief 启动调度器，开始按优先级调度任务。
 *
 * 会创建空闲任务、把 PendSV 设为最低优先级、SysTick 设为系统节拍优先级，
 * 然后切换到第一个就绪任务。正常启动后不返回。
 *
 * @note 调用前必须已创建至少一个任务，否则打印错误后返回。
 * @warning 启动前必须先执行一次上下文切换选出真正要运行的任务，
 *          否则 pxCurrentTCB 与实际运行的任务不一致，会因栈指针错乱触发 HardFault。
 */
void htStartScheduler(void);

/**
 * @brief 暂停调度器（支持嵌套计数），滴答中断与主动让出均只登记待补请求而不切换。
 * @note 与 htResumeScheduler() 严格配对使用；计数归零且期间存在切换请求时自动补切。
 */
void htSuspendScheduler(void);

/**
 * @brief 恢复被暂停的调度器（嵌套计数减一）。
 * @note 当最外层恢复且挂起期间曾产生上下文切换请求时，自动调用 htTaskYield() 补发切换。
 */
void htResumeScheduler(void);

/**
 * @brief SysTick 节拍处理函数，需要在滴答中断里调用。
 *
 * 递增系统 tick 计数，并检查是否有任务的延时到期需要唤醒。
 */
void htSchedulerTickHandler(void);

/**
 * @brief 调度器是否已经启动过。
 *
 * @return 已启动（运行中或已挂起）返回 htTRUE，从未启动返回 htFALSE。
 * @note 策略切换与任务创建路径据此决定是否需要立即请求一次重调度。
 */
BaseType_t htSchedulerIsStarted(void);

/**
 * @brief 进入内核临界区，会关闭中断并累加嵌套计数。
 *
 * @note 必须先关中断再累加计数，顺序不可调换：若先加计数后关中断，
 *       关中断前的窗口里一旦进 PendSV，计数将永远无法归零，
 *       PRIMASK 永久为 1，系统死锁。
 * @warning 必须与 htExitCritical() 严格配对，且不可在中断里长时间停留。
 */
void htEnterCritical(void);

/**
 * @brief 退出内核临界区，计数归零时才重新开中断。
 *
 * @note 支持嵌套：只有最外层退出（计数减到 0）才执行开中断。
 */
void htExitCritical(void);

/**
 * @brief 中断上下文进入临界区：保存当前中断屏蔽状态并屏蔽受管中断。
 *
 * @return 保存的中断屏蔽快照，退出时原样传回 htExitCriticalFromISR()。
 * @note 防止 Cortex-M 高优先级中断抢占正在操作内核链表的低优先级中断；
 *       不修改任务侧的 uxCriticalNesting。
 */
UBaseType_t htEnterCriticalFromISR(void);

/**
 * @brief 中断上下文退出临界区：按保存值恢复中断屏蔽状态。
 *
 * @param[in] uxSavedInterruptStatus 由配对的 htEnterCriticalFromISR() 返回的值。
 */
void htExitCriticalFromISR(UBaseType_t uxSavedInterruptStatus);

/** @brief 临界区嵌套计数器，0 表示当前不在临界区。 */
extern volatile UBaseType_t uxCriticalNesting;
/**
 * @brief 选出下一个该运行的任务并完成上下文切换。
 *
 * 挂起调度器时只置 xYieldPending 而不切换；否则先检查当前任务的栈哨兵，
 * 再从最高优先级向下找第一个非空就绪表，在该表内轮转选出下一个任务。
 * 由 PendSV 异常或显式让出 CPU 时调用。
 */
void htTaskSwitchContext(void);

#endif /* HT_SCHEDULER_H */
