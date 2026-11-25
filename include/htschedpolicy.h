/**
 * @file htschedpolicy.h
 * @brief 可插拔调度策略接口：内核与具体调度算法之间的抽象层。
 *
 * 调度器只认本文件里的操作表，不认识"固定优先级""公平分享""最早死期"中的
 * 任何一个算法。新增一种策略只需写一个新的 const 实例并在启动前登记，不必
 * 修改 Scheduling 核心 —— 对扩展开放、对修改关闭（开闭原则）。
 *
 * 五个操作按"何时被调用"拆开，策略只需实现自己关心的那些（接口隔离）：
 *   vInitialise    策略被登记或重新启用时调用一次，重置策略自己的记账状态；
 *   vTaskCreated   任务初始化完成后调用，给新任务写策略私有字段；
 *   vTick          每个滴答在关中断状态下调用，做策略记账；
 *   pxSelectNext   从就绪任务里选出下一个该跑的，返回 NULL 表示无候选；
 *   xShouldPreempt 滴答里判断是否需要立即切换。
 *
 * 内核调用前都会判空，所以策略可以把用不到的操作留成 NULL，不必写空壳函数。
 */
#ifndef HT_SCHED_POLICY_H
#define HT_SCHED_POLICY_H

#include "httypes.h"
#include "httask.h"

/**
 * @brief 调度策略的操作表；一个 const 实例就是一个策略对象。
 */
typedef struct htSchedPolicy {
	const char *pcName; /**< 策略名，仅用于诊断输出 */
	void (*vInitialise)(void); /**< 策略被选中时的初始化，可为 NULL */
	void (*vTaskCreated)(htTCB_t *pxTask); /**< 新任务初始化完成后的回调，可为 NULL */
	void (*vTick)(htTCB_t *pxCurrent); /**< 滴答记账，可为 NULL */
	htTCB_t *(*pxSelectNext)(void); /**< 选择下一个任务，不允许为 NULL */
	BaseType_t (*xShouldPreempt)(htTCB_t *pxCurrent); /**< 无副作用抢占判断，不允许为 NULL */
	void (*vTaskEnqueued)(htTCB_t *pxTask); /**< 任务重新就绪（唤醒/恢复）时的回调，可为 NULL */
} htSchedPolicy_t;

/**
 * @brief 切换调度策略：启动前是登记，启动后是热切换。
 *
 * @param[in] pxPolicy 目标策略对象，必须提供 pxSelectNext 与 xShouldPreempt。
 * @return 成功返回 htPASS；空指针或缺少必需操作返回 htFAIL。
 *
 * @note 两种调用时机语义一致：换掉策略指针，再调用新策略的 vInitialise() 让它接管
 *       已有任务。启动后切换还会立即让出一次 CPU，使新策略马上参与选择，不必等
 *       下一个滴答。整个过程在临界区内完成，滴答中断不会看到半新半旧的状态。
 * @note 重复登记同一个对象是空操作，不会清掉策略已经积累的运行状态。
 * @note 热切换会重置策略的记账状态（虚拟时间、当前任务视图），但保留调用方用
 *       htSchedSetWeight() / htSchedSetDeadline() 显式设置的任务参数。切到 EDF 时
 *       需要按新策略重新登记死期。
 * @note 在中断服务程序里调用也会生效，但不会主动让出 CPU：切换由中断退出时的
 *       PendSV 完成。正常用法是任务上下文。
 */
BaseType_t htSchedSetPolicy(const htSchedPolicy_t *pxPolicy);

/** @brief 取当前生效的策略对象，永不为 NULL。 */
const htSchedPolicy_t *htSchedGetPolicy(void);

/**
 * @brief 取当前就绪任务中优先级最高的那一档，供策略实现复用（O(1) 位图 + CLZ 实现）。
 *
 * @return 最高非空就绪链表的优先级；全部为空时返回 0（空闲任务所在档）。
 */
UBaseType_t htSchedGetTopReadyPriority(void);

/**
 * @brief 通知策略"有新任务创建完成"，由内核在任务初始化末尾调用。
 * @param[in] pxTask 新任务的 TCB。
 */
void htSchedNotifyTaskCreated(htTCB_t *pxTask);

/**
 * @brief 通知策略"刚过去一个滴答"，由内核在 SysTick 处理里调用，中断已关闭。
 * @param[in] pxCurrent 当前任务，可为 NULL（调度器尚未启动）。
 */
void htSchedNotifyTick(htTCB_t *pxCurrent);

/**
 * @brief 通知策略"有任务重新进入就绪表"（如延时到期、队列/通知唤醒、恢复挂起）。
 * @param[in] pxTask 重新就绪的任务 TCB，必须在临界区或关中断下调用。
 */
void htSchedNotifyTaskEnqueued(htTCB_t *pxTask);

/** @brief 固定优先级策略（默认）：优先级高者先跑，同级按时间片轮转。 */
extern const htSchedPolicy_t htSchedPolicyFixedPriority;

#if (configUSE_SCHED_POLICY_FAIR_SHARE == 1)
/** @brief 公平分享策略：优先级语义不变，同一优先级内按权重分配 CPU。 */
extern const htSchedPolicy_t htSchedPolicyFairShare;

/** @brief 默认权重；权重 1024 表示"标准份额"，每 tick 记一个步长 1024。 */
#define HT_SCHED_DEFAULT_WEIGHT 1024U
/** @brief 权重上限；权重 4096 对应最小步长 256，即 4 倍标准份额。 */
#define HT_SCHED_MAX_WEIGHT 4096U

/**
 * @brief 设置任务在公平分享策略下的权重。
 *
 * @param[in] xTask 任务句柄。
 * @param[in] uxWeight 权重，取值范围 1 ~ HT_SCHED_MAX_WEIGHT，1024 为标准份额。
 * @return 成功返回 htPASS；句柄为空或权重越界返回 htFAIL。
 *
 * @note 权重只在公平分享策略下有意义，其它策略下调用不会报错但也不产生效果。
 */
BaseType_t htSchedSetWeight(TaskHandle_t xTask, UBaseType_t uxWeight);
#endif /* configUSE_SCHED_POLICY_FAIR_SHARE */

#if (configUSE_SCHED_POLICY_EDF == 1)
/** @brief 最早死期优先策略：最高优先级档里死期最近的任务先跑。 */
extern const htSchedPolicy_t htSchedPolicyEdf;

/** @brief 未设置死期的标记值；绝对 tick 0 视为"没有死期"。 */
#define HT_SCHED_NO_DEADLINE ((TickType_t)0U)

/** @brief 取消任务死期的快捷入口（等价于将 ucHasDeadline 清零）。 */
BaseType_t htSchedClearDeadline(TaskHandle_t xTask);

/**
 * @brief 设置任务的绝对死期（单位 tick），允许传入任意 32 位值包括 0（tick 回绕安全）。
 *
 * @param[in] xTask 任务句柄。
 * @param[in] xDeadline 绝对死期，通常写成 xTickCount + 周期；
 *                     传 HT_SCHED_NO_DEADLINE 为兼容旧调用语义取消死期；
 *                     若需将绝对时刻 0 作为有效死期请使用 htSchedSetDeadlineAbsolute()。
 * @return 成功返回 htPASS；句柄为空返回 htFAIL。
 *
 * @note 设置死期会同时清掉该任务的"已漏期"标志。若调度器已启动且新死期早于当前任务，
 *       会立即触发一次重调度。
 */
BaseType_t htSchedSetDeadline(TaskHandle_t xTask, TickType_t xDeadline);

/**
 * @brief 设置任务的绝对死期（允许 0 作为合法回绕 tick 时刻）。
 *
 * @param[in] xTask 任务句柄。
 * @param[in] xDeadline 绝对死期（任意 32 位 tick 值）。
 * @return 成功返回 htPASS；句柄为空返回 htFAIL。
 */
BaseType_t htSchedSetDeadlineAbsolute(TaskHandle_t xTask, TickType_t xDeadline);

/** @brief 取累计漏期次数（每个任务每次死期只计一次）。 */
uint32_t htSchedEdfGetDeadlineMisses(void);

/** @brief 清零漏期计数。 */
void htSchedEdfClearDeadlineMisses(void);
#endif /* configUSE_SCHED_POLICY_EDF */

#endif /* HT_SCHED_POLICY_H */
