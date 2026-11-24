/**
 * @file htschedpolicy.h
 * @brief 可插拔调度策略接口：内核与具体调度算法之间的抽象层。
 *
 * 调度器只认本文件里的操作表，不认识"固定优先级""公平分享""最早死期"中的
 * 任何一个算法。新增一种策略只需写一个新的 const 实例并在启动前登记，不必
 * 修改 Scheduling 核心 —— 对扩展开放、对修改关闭（开闭原则）。
 *
 * 五个操作按"何时被调用"拆开，策略只需实现自己关心的那些（接口隔离）：
 *   vInitialise    策略被选中时调用一次，清空策略自己的全局状态；
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
	BaseType_t (*xShouldPreempt)(htTCB_t *pxCurrent); /**< 是否需要切换，不允许为 NULL */
} htSchedPolicy_t;

/**
 * @brief 切换调度策略。
 *
 * @param[in] pxPolicy 目标策略对象，必须提供 pxSelectNext 与 xShouldPreempt。
 * @return 成功返回 htPASS；空指针、缺少必需操作或调度器已启动时返回 htFAIL。
 *
 * @note 必须在 htStartScheduler() 之前调用；已创建的任务会立即套用新策略。
 */
BaseType_t htSchedSetPolicy(const htSchedPolicy_t *pxPolicy);

/** @brief 取当前生效的策略对象，永不为 NULL。 */
const htSchedPolicy_t *htSchedGetPolicy(void);

/**
 * @brief 取当前就绪任务中优先级最高的那一档，供策略实现复用。
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

/**
 * @brief 设置任务的绝对死期（单位 tick）。
 *
 * @param[in] xTask 任务句柄。
 * @param[in] xDeadline 绝对死期，通常写成 htTaskGetTickCount() + 周期；
 *                     传 HT_SCHED_NO_DEADLINE 表示取消死期。
 * @return 成功返回 htPASS；句柄为空返回 htFAIL。
 *
 * @note 设置死期会同时清掉该任务的"已漏期"标志。周期任务每次醒来都要重新设置，
 *       否则下一次漏期不会被识别。
 */
BaseType_t htSchedSetDeadline(TaskHandle_t xTask, TickType_t xDeadline);

/** @brief 取累计漏期次数（每个任务每次死期只计一次）。 */
uint32_t htSchedEdfGetDeadlineMisses(void);

/** @brief 清零漏期计数。 */
void htSchedEdfClearDeadlineMisses(void);
#endif /* configUSE_SCHED_POLICY_EDF */

#endif /* HT_SCHED_POLICY_H */
